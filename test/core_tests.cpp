#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "composition.h"
#include "framework.h"
#include "occlusion.h"

namespace {

	// Records the on/off calls in order.
	class fake_notifyinjector final : public i_notifyinjector {
	public:
		void on() override { calls.push_back(true); active_ = true; }
		void off() override { calls.push_back(false); active_ = false; }
		bool active() const override { return active_; }

		std::vector<bool> calls;

	private:
		bool active_ = false;
	};

	class fake_injection final : public i_injection {
	public:
		explicit fake_injection(std::vector<int>& log, int id) : log(log), id(id) {}
		void on() override { log.push_back(id); }
		void off() override { log.push_back(-id); }

	private:
		std::vector<int>& log;
		int id;
	};

	HWND fake_hwnd(std::uintptr_t n) { return reinterpret_cast<HWND>(n); }

	// Pumps this thread's messages until done() holds or the time runs out.
	bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds limit = std::chrono::milliseconds(2000))
	{
		const auto until = std::chrono::steady_clock::now() + limit;
		MSG msg{};
		while (!done() && std::chrono::steady_clock::now() < until) {
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
			Sleep(1);
		}
		return done();
	}

	class fake_taskbar final : public i_taskbar {
	public:
		std::vector<HWND> windows() override { return open; }
		int overlay_size() override { return 16; }
		bool set_overlay(HWND window, HICON icon) override
		{
			if (icon) overlays[window] = icon;
			else      overlays.erase(window);
			++calls;
			return true;
		}

		std::vector<HWND> open;
		std::map<HWND, HICON> overlays;
		int calls = 0;
	};

	// Hands out copies of the arrow and remembers what was put in their place.
	class fake_cursors final : public i_cursors {
	public:
		~fake_cursors() override { for (auto& [id, c] : replaced) DestroyCursor(c); }

		HCURSOR copy(UINT) override { return CopyCursor(LoadCursorW(nullptr, IDC_ARROW)); }
		bool replace(UINT id, HCURSOR cursor) override
		{
			if (replaced.contains(id)) DestroyCursor(replaced[id]);
			replaced[id] = cursor;
			return true;
		}
		void restore() override { ++restores; }

		std::map<UINT, HCURSOR> replaced;
		int restores = 0;
	};

	class fake_desktop final : public i_desktop {
	public:
		std::optional<desktop_snapshot> snapshot() override { return snap; }
		void disconnect() override { ++disconnects; }

		std::optional<desktop_snapshot> snap;
		int disconnects = 0;
	};

	class fake_overlay final : public i_overlay {
	public:
		// The injection owns the fake; the test keeps a pointer to inspect it.
		static std::pair<std::unique_ptr<i_overlay>, fake_overlay*> make()
		{
			auto owned = std::make_unique<fake_overlay>();
			fake_overlay* seen = owned.get();
			return { std::move(owned), seen };
		}

	public:
		void show(HWND owner, const RECT& area, const std::vector<badge>& badges) override
		{
			last_owner = owner;
			last_area = area;
			last = badges;
			shown_ = true;
		}
		void show_groups(HWND owner, const RECT& area, const std::vector<badge_group>& groups) override
		{
			last_owner = owner;
			last_area = area;
			last_groups = groups;
			shown_ = true;
		}
		void hide() override { shown_ = false; }
		bool shown() const override { return shown_; }

		HWND last_owner = nullptr;
		RECT last_area{};
		std::vector<badge> last;
		std::vector<badge_group> last_groups;

	private:
		bool shown_ = false;
	};

	class fake_taskbar_items final : public i_taskbar_items {
	public:
		std::optional<tray_snapshot> snapshot() override { return snap; }
		bool visible_at(POINT pt) override { return !(pt.x >= covered.left && pt.x < covered.right); }
		void disconnect() override { ++disconnects; }

		std::optional<tray_snapshot> snap;
		int disconnects = 0;
		RECT covered{};   // x range where something sits above the taskbar
	};

	tray_snapshot two_tray_items()
	{
		tray_snapshot s;
		s.taskbar = fake_hwnd(0x20);
		s.bounds = { 0, 1528, 2560, 1600 };
		s.dpi = 144;
		s.items = {
			{ L"SystemTray.NormalButton#0", { 2145, 1528, 2193, 1600 } },
			{ L"SystemTray.OmniButton#0", { 2433, 1528, 2536, 1600 }, true },   // the clock
		};
		return s;
	}

	// Window icons kept in a map; "shown" falls back to a real icon so badges can be drawn.
	class fake_window_icons final : public i_window_icons {
	public:
		fake_window_icons() : fallback(CopyIcon(LoadIconW(nullptr, IDI_APPLICATION))) {}
		~fake_window_icons() override { DestroyIcon(fallback); }

		std::vector<HWND> windows() override { return open; }
		bool elevated(HWND w) override { return admin.contains(w); }
		HICON get(HWND w, icon_kind k) override { return current[{ w, k }]; }
		HICON shown(HWND w, icon_kind k) override { const HICON i = get(w, k); return i ? i : fallback; }
		bool set(HWND w, icon_kind k, HICON i) override { current[{ w, k }] = i; ++sets; return true; }
		int size(HWND, icon_kind k) override { return k == icon_kind::big ? 32 : 16; }

		std::vector<HWND> open;
		std::set<HWND> admin;
		std::map<std::pair<HWND, icon_kind>, HICON> current;
		HICON fallback;
		int sets = 0;
	};

	class fake_window_items final : public i_window_items {
	public:
		std::vector<HWND> candidates() override { return order; }
		std::optional<window_snapshot> query(HWND w) override
		{
			queried.push_back(w);
			if (delay.count() > 0) Sleep(static_cast<DWORD>(delay.count()));
			const auto it = snaps.find(w);
			if (it == snaps.end()) return std::nullopt;
			return it->second;
		}
		void disconnect() override { ++disconnects; }

		std::vector<HWND> order;
		std::map<HWND, window_snapshot> snaps;
		std::vector<HWND> queried;
		std::chrono::milliseconds delay{ 0 };   // pretend each query costs this long
		int disconnects = 0;
	};

	class fake_window_changes final : public i_window_changes {
	public:
		void start(std::function<void(HWND)> fn) override { changed = std::move(fn); }
		void stop() override { changed = nullptr; }
		std::function<void(HWND)> changed;
	};

	class fake_window_stack final : public i_window_stack {
	public:
		std::vector<stacked_window> front_to_back() override { return windows; }
		RECT screen() override { return { 0, 0, 2560, 1600 }; }
		std::vector<stacked_window> windows;
	};

	// One window at (100,100) with a close button and a details-view row.
	window_snapshot explorer_like(HWND w)
	{
		window_snapshot s;
		s.window = w;
		s.frame = { 100, 100, 1300, 900 };
		s.dpi = 144;
		s.items = {
			{ L"1.2.3", { 1200, 100, 1269, 145 } },
			{ L"1.2.4", { 400, 300, 1250, 330 } },
		};
		return s;
	}

	desktop_snapshot two_icons()
	{
		desktop_snapshot s;
		s.host = fake_hwnd(0x10);
		s.bounds = { 0, 0, 2560, 1600 };
		s.icons = {
			{ L"C:\\a.txt", { 40, 8, 112, 80 } },
			{ L"C:\\b.txt", { 40, 155, 112, 227 } },
		};
		return s;
	}

}

test::suite notifyinjector_suite()
{
	return { "notifyinjector", {
		{ "switches_every_method", [](test::context& t) {
			std::vector<int> log;
			std::vector<std::unique_ptr<i_injection>> methods;
			methods.push_back(std::make_unique<fake_injection>(log, 1));
			methods.push_back(std::make_unique<fake_injection>(log, 2));

			notifyinjector n(std::move(methods));
			CHECK(t, !n.active());
			n.on();
			CHECK(t, n.active());
			n.off();
			CHECK(t, !n.active());
			CHECK(t, (log == std::vector<int>{ 1, 2, -1, -2 }));
		} },
	} };
}

test::suite taskbar_injection_suite()
{
	return { "taskbar_injection", {
		{ "badges_every_window", [](test::context& t) {
			fake_taskbar bar;
			bar.open = { fake_hwnd(1), fake_hwnd(2) };

			taskbar_injection inj(bar);
			inj.on();
			CHECK(t, bar.overlays.size() == 2);
			CHECK(t, inj.badged() == 2);
			inj.off();
		} },
		{ "badges_new_windows_once", [](test::context& t) {
			fake_taskbar bar;
			bar.open = { fake_hwnd(1) };

			taskbar_injection inj(bar);
			inj.on();
			const int after_on = bar.calls;
			inj.refresh();
			CHECK(t, bar.calls == after_on);   // already badged, so untouched

			bar.open.push_back(fake_hwnd(2));
			inj.refresh();
			CHECK(t, bar.calls == after_on + 1);
			CHECK(t, bar.overlays.size() == 2);
			inj.off();
		} },
		{ "forgets_closed_windows", [](test::context& t) {
			fake_taskbar bar;
			bar.open = { fake_hwnd(1), fake_hwnd(2) };

			taskbar_injection inj(bar);
			inj.on();
			bar.open = { fake_hwnd(2) };
			inj.refresh();
			CHECK(t, inj.badged() == 1);
			inj.off();
		} },
		{ "off_clears_overlays", [](test::context& t) {
			fake_taskbar bar;
			bar.open = { fake_hwnd(1), fake_hwnd(2) };

			taskbar_injection inj(bar);
			inj.on();
			inj.off();
			CHECK(t, bar.overlays.empty());
			CHECK(t, inj.badged() == 0);
		} },
	} };
}

test::suite cursor_injection_suite()
{
	return { "cursor_injection", {
		{ "badge_sits_bottom_right", [](test::context& t) {
			const RECT r = cursor_injection::badge_rect(32, 32);
			CHECK(t, r.right == 32 && r.bottom == 32);
			CHECK(t, r.left > 0 && r.top > 0);
			CHECK(t, r.right - r.left == r.bottom - r.top);
		} },
		{ "keeps_size_and_hotspot", [](test::context& t) {
			const HCURSOR original = CopyCursor(LoadCursorW(nullptr, IDC_IBEAM));
			const HCURSOR badged = cursor_injection::with_badge(original, 5);
			CHECK(t, badged != nullptr);

			ICONINFO a{}, b{};
			GetIconInfo(original, &a);
			GetIconInfo(badged, &b);
			CHECK(t, a.xHotspot == b.xHotspot && a.yHotspot == b.yHotspot);
			CHECK(t, !b.fIcon);

			for (ICONINFO* i : { &a, &b }) {
				if (i->hbmColor) DeleteObject(i->hbmColor);
				if (i->hbmMask) DeleteObject(i->hbmMask);
			}
			DestroyCursor(original);
			DestroyCursor(badged);
		} },
		{ "replaces_every_target_and_restores", [](test::context& t) {
			fake_cursors cursors;
			cursor_injection inj(cursors);

			inj.on();
			CHECK(t, cursors.restores == 1);   // starts from the user's scheme
			CHECK(t, cursors.replaced.size() == cursor_injection::targets.size());

			inj.off();
			CHECK(t, cursors.restores == 2);
			inj.off();
			CHECK(t, cursors.restores == 2);   // nothing left to undo
		} },
	} };
}

test::suite tray_area_injection_suite()
{
	return { "tray_area_injection", {
		{ "place_on_icon", [](test::context& t) {
			const RECT item{ 2145, 1528, 2193, 1600 };
			const badge b = tray_area_injection::place(item, 144, 2, false);
			CHECK(t, b.number == 2);
			CHECK(t, b.x + b.diameter / 2 <= item.right && b.x > (item.left + item.right) / 2.0f);
			CHECK(t, b.y - b.diameter / 2 >= item.top && b.y < (item.top + item.bottom) / 2.0f);
		} },
		{ "clock_gets_the_corner", [](test::context& t) {
			const RECT clock{ 2433, 1528, 2536, 1600 };
			const badge b = tray_area_injection::place(clock, 144, 2, true);
			CHECK(t, b.x > clock.right - b.diameter * 2);   // near the right edge, clear of the time text
			CHECK(t, b.y - b.diameter / 2 >= clock.top);
		} },
		{ "badges_visible_items_with_taskbar_as_owner", [](test::context& t) {
			fake_taskbar_items items;
			auto [owned, layer] = fake_overlay::make();
			items.snap = two_tray_items();

			tray_area_injection inj(items, std::move(owned));
			inj.refresh();
			CHECK(t, layer->shown());
			CHECK(t, layer->last.size() == 2);
			CHECK(t, layer->last_owner == fake_hwnd(0x20));

			items.covered = { 2400, 0, 2560, 0 };
			inj.refresh();
			CHECK(t, layer->last.size() == 1);

			items.covered = { 0, 0, 10000, 0 };
			inj.refresh();
			CHECK(t, !layer->shown());
		} },
		{ "wide_bounds_while_animating_change_nothing", [](test::context& t) {
			// The chevron reports 78px for a moment while its arrow turns; that is not the clock.
			const badge normal = tray_area_injection::place({ 2193, 1528, 2241, 1600 }, 144, 1, false);
			const badge animating = tray_area_injection::place({ 2177, 1528, 2255, 1600 }, 144, 1, false);
			CHECK(t, std::abs(normal.y - animating.y) < 1.0f);
		} },
		{ "ignores_a_one_poll_glitch", [](test::context& t) {
			fake_taskbar_items items;
			auto [owned, layer] = fake_overlay::make();
			items.snap = two_tray_items();

			tray_area_injection inj(items, std::move(owned));
			inj.refresh();
			const badge before = layer->last[0];

			items.snap->items[0].rect = { 2177, 1528, 2255, 1600 };   // mid-animation
			inj.refresh();
			CHECK(t, layer->last[0] == before);

			items.snap->items[0].rect = { 2145, 1528, 2193, 1600 };   // back to normal
			inj.refresh();
			CHECK(t, layer->last[0] == before);
		} },
		{ "follows_a_real_move", [](test::context& t) {
			fake_taskbar_items items;
			auto [owned, layer] = fake_overlay::make();
			items.snap = two_tray_items();

			tray_area_injection inj(items, std::move(owned));
			inj.refresh();
			const badge before = layer->last[0];

			items.snap->items[0].rect = { 2097, 1528, 2145, 1600 };   // an icon was added to its right
			inj.refresh();
			inj.refresh();
			CHECK(t, layer->last[0].x < before.x);
		} },
		{ "numbers_stay_per_item", [](test::context& t) {
			fake_taskbar_items items;
			auto [owned, layer] = fake_overlay::make();
			items.snap = two_tray_items();

			tray_area_injection inj(items, std::move(owned));
			inj.refresh();
			const auto first = layer->last;
			for (int i = 0; i < 3; ++i) inj.refresh();
			CHECK(t, layer->last == first);
		} },
		{ "off_hides", [](test::context& t) {
			fake_taskbar_items items;
			auto [owned, layer] = fake_overlay::make();
			items.snap = two_tray_items();

			tray_area_injection inj(items, std::move(owned));
			inj.on();
			CHECK(t, pump_until([&] { return layer->shown(); }));
			inj.off();
			CHECK(t, !layer->shown());
			CHECK(t, items.disconnects == 1);   // the worker released its COM objects
		} },
	} };
}

test::suite window_icon_injection_suite()
{
	return { "window_icon_injection", {
		{ "with_badge_keeps_size", [](test::context& t) {
			const HICON source = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
			const HICON badged = window_icon_injection::with_badge(source, 24, 3);
			CHECK(t, badged != nullptr);

			ICONINFO info{};
			BITMAP bm{};
			GetIconInfo(badged, &info);
			GetObjectW(info.hbmColor, sizeof(bm), &bm);
			CHECK(t, bm.bmWidth == 24 && bm.bmHeight == 24);
			CHECK(t, info.fIcon);

			DeleteObject(info.hbmColor);
			DeleteObject(info.hbmMask);
			DestroyIcon(badged);
			DestroyIcon(source);
		} },
		{ "badges_both_sizes_and_restores_originals", [](test::context& t) {
			fake_window_icons icons;
			const HWND a = fake_hwnd(1), b = fake_hwnd(2);
			icons.open = { a, b };
			const HICON own = CopyIcon(LoadIconW(nullptr, IDI_INFORMATION));
			icons.current[{ a, icon_kind::big }] = own;   // a has its own big icon; everything else uses the class icon

			window_icon_injection inj(icons);
			inj.refresh();
			CHECK(t, inj.badged() == 2);
			CHECK(t, icons.get(a, icon_kind::big) != own);
			CHECK(t, icons.get(a, icon_kind::little) != nullptr);
			CHECK(t, icons.get(b, icon_kind::big) != nullptr);

			inj.off();
			CHECK(t, icons.get(a, icon_kind::big) == own);
			CHECK(t, icons.get(a, icon_kind::little) == nullptr);   // back to "use the class icon"
			CHECK(t, icons.get(b, icon_kind::big) == nullptr);
			CHECK(t, inj.badged() == 0);
			DestroyIcon(own);
		} },
		{ "skips_elevated_windows", [](test::context& t) {
			fake_window_icons icons;
			const HWND user = fake_hwnd(1), admin = fake_hwnd(2);
			icons.open = { user, admin };
			icons.admin = { admin };

			window_icon_injection inj(icons);
			inj.refresh();
			CHECK(t, inj.badged() == 1);
			CHECK(t, icons.get(admin, icon_kind::big) == nullptr);
			inj.off();
		} },
		{ "leaves_badged_windows_alone", [](test::context& t) {
			fake_window_icons icons;
			icons.open = { fake_hwnd(1) };

			window_icon_injection inj(icons);
			inj.refresh();
			const int sets = icons.sets;
			inj.refresh();
			CHECK(t, icons.sets == sets);
			inj.off();
		} },
		{ "rebadges_an_icon_the_app_replaced", [](test::context& t) {
			fake_window_icons icons;
			const HWND w = fake_hwnd(1);
			icons.open = { w };

			window_icon_injection inj(icons);
			inj.refresh();

			const HICON tab = CopyIcon(LoadIconW(nullptr, IDI_WARNING));   // e.g. a browser switching tabs
			icons.current[{ w, icon_kind::big }] = tab;
			inj.refresh();
			CHECK(t, icons.get(w, icon_kind::big) != tab);   // badged again

			inj.off();
			CHECK(t, icons.get(w, icon_kind::big) == tab);   // and the app's latest icon comes back
			DestroyIcon(tab);
		} },
		{ "keeps_an_icon_the_app_set_after_off_began", [](test::context& t) {
			fake_window_icons icons;
			const HWND w = fake_hwnd(1);
			icons.open = { w };

			window_icon_injection inj(icons);
			inj.refresh();
			const HICON fresh = CopyIcon(LoadIconW(nullptr, IDI_WARNING));
			icons.current[{ w, icon_kind::big }] = fresh;   // the app changed it and we haven't refreshed yet
			inj.off();
			CHECK(t, icons.get(w, icon_kind::big) == fresh);
			DestroyIcon(fresh);
		} },
		{ "forgets_closed_windows", [](test::context& t) {
			fake_window_icons icons;
			icons.open = { fake_hwnd(1), fake_hwnd(2) };

			window_icon_injection inj(icons);
			inj.refresh();
			icons.open = { fake_hwnd(2) };
			inj.refresh();
			CHECK(t, inj.badged() == 1);
			inj.off();
		} },
	} };
}

test::suite occlusion_suite()
{
	return { "occlusion", {
		{ "subtract_disjoint_keeps_all", [](test::context& t) {
			const auto r = occlusion::subtract({ 0, 0, 10, 10 }, { 20, 20, 30, 30 });
			const RECT whole{ 0, 0, 10, 10 };
			CHECK(t, r.size() == 1 && EqualRect(&r[0], &whole));
		} },
		{ "subtract_middle_leaves_a_frame", [](test::context& t) {
			const auto r = occlusion::subtract({ 0, 0, 30, 30 }, { 10, 10, 20, 20 });
			CHECK(t, r.size() == 4);
			long area = 0;
			for (const RECT& p : r) area += (p.right - p.left) * (p.bottom - p.top);
			CHECK(t, area == 900 - 100);
		} },
		{ "subtract_everything_leaves_nothing", [](test::context& t) {
			CHECK(t, occlusion::subtract({ 10, 10, 20, 20 }, { 0, 0, 30, 30 }).empty());
		} },
		{ "front_window_covers_the_one_behind", [](test::context& t) {
			const HWND front = fake_hwnd(1), back = fake_hwnd(2), hidden = fake_hwnd(3);
			const auto parts = occlusion::visible_parts({
				{ front, { 0, 0, 100, 100 }, true },
				{ back, { 50, 0, 150, 100 }, true },
				{ hidden, { 10, 10, 90, 90 }, true },
			});
			CHECK(t, parts.at(front).size() == 1);
			CHECK(t, parts.at(back).size() == 1 && parts.at(back)[0].left == 100);
			CHECK(t, !parts.contains(hidden));
		} },
		{ "see_through_windows_cover_nothing", [](test::context& t) {
			const HWND glass = fake_hwnd(1), app = fake_hwnd(2);
			const auto parts = occlusion::visible_parts({
				{ glass, { 0, 0, 2560, 1600 }, false },
				{ app, { 100, 100, 200, 200 }, true },
			});
			const RECT frame{ 100, 100, 200, 200 };
			CHECK(t, parts.contains(app) && EqualRect(&parts.at(app)[0], &frame));
		} },
	} };
}

test::suite window_item_injection_suite()
{
	// Builds the injection over fakes; the overlay is watched through layer.
	struct rig {
		fake_window_items items;
		fake_window_stack stack;
		fake_window_changes changes;
		fake_overlay* layer = nullptr;
		std::unique_ptr<window_item_injection> inj;

		rig()
		{
			auto [owned, seen] = fake_overlay::make();
			layer = seen;
			inj = std::make_unique<window_item_injection>(items, stack, changes, std::move(owned));
		}
	};

	return { "window_item_injection", {
		{ "compact_items_get_the_top_right", [](test::context& t) {
			const RECT button{ 1200, 100, 1269, 145 };
			const badge b = window_item_injection::place(button, 144, 1);
			CHECK(t, b.x > (button.left + button.right) / 2.0f && b.x + b.diameter / 2 <= button.right);
			CHECK(t, b.y - b.diameter / 2 >= button.top);
		} },
		{ "rows_get_it_near_their_icon", [](test::context& t) {
			const RECT row{ 400, 300, 1250, 330 };
			const badge b = window_item_injection::place(row, 144, 1);
			CHECK(t, b.x < row.left + 80);
			CHECK(t, b.x - b.diameter / 2 >= row.left);
		} },
		{ "anchors_follow_the_nearer_edges", [](test::context& t) {
			const badge close{ 1160, 20, 20, 1 };   // near the top-right of a 1200x800 window
			const auto a = window_item_injection::anchor(close, { 1200, 800 });
			CHECK(t, a.from_right && !a.from_bottom);

			const RECT maximized{ 0, 0, 2560, 1528 };
			const badge b = window_item_injection::resolve(a, maximized);
			CHECK(t, std::abs(b.x - (2560 - 40)) < 0.01f && std::abs(b.y - 20) < 0.01f);
		} },
		{ "queries_only_changed_or_overdue_windows", [](test::context& t) {
			rig r;
			r.items.order = { fake_hwnd(1), fake_hwnd(2), fake_hwnd(3) };

			r.inj->query_round();   // nothing seen yet: everything is overdue
			CHECK(t, (r.items.queried == std::vector<HWND>{ fake_hwnd(1), fake_hwnd(2), fake_hwnd(3) }));

			r.items.queried.clear();
			r.inj->query_round();   // nothing changed, nothing overdue
			CHECK(t, r.items.queried.empty());

			r.inj->changed(fake_hwnd(3));
			r.inj->query_round();
			CHECK(t, (r.items.queried == std::vector<HWND>{ fake_hwnd(3) }));
		} },
		{ "a_slow_noisy_window_is_paced_by_its_cost", [](test::context& t) {
			rig r;
			const HWND slow = fake_hwnd(1);
			r.items.order = { slow };
			r.items.snaps[slow] = explorer_like(slow);
			r.items.delay = std::chrono::milliseconds(30);

			r.inj->query_round();
			r.inj->changed(slow);
			r.items.queried.clear();
			r.inj->query_round();   // changed again at once, but its last query took 30ms
			CHECK(t, r.items.queried.empty());

			Sleep(30 * window_item_injection::cost_factor + 20);
			r.inj->query_round();   // still marked as changed, and now its turn has come
			CHECK(t, r.items.queried.size() == 1);
		} },
		{ "badges_follow_a_moved_window_without_a_new_query", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };

			r.inj->store(r.inj->query_round());
			r.inj->follow();
			CHECK(t, r.layer->shown() && r.layer->last_groups.size() == 1);
			CHECK(t, r.layer->last_owner == nullptr);
			const float before = r.layer->last_groups[0].badges[0].x;

			r.stack.windows[0].frame = { 300, 100, 1500, 900 };   // dragged 200px right
			r.inj->follow();
			CHECK(t, std::abs(r.layer->last_groups[0].badges[0].x - (before + 200)) < 0.01f);
			CHECK(t, r.items.queried.size() == 1);
		} },
		{ "a_resize_keeps_badges_by_their_edges_and_requeries_first", [](test::context& t) {
			rig r;
			const HWND front = fake_hwnd(1), w = fake_hwnd(2);
			r.items.order = { front, w };
			r.items.snaps[front] = explorer_like(front);
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };

			r.inj->store(r.inj->query_round());
			r.inj->follow();
			const float close_from_right = 1300 - r.layer->last_groups[0].badges[0].x;

			r.stack.windows[0].frame = { 0, 0, 2560, 1528 };   // maximized
			r.inj->follow();
			CHECK(t, r.layer->shown());
			CHECK(t, std::abs((2560 - r.layer->last_groups[0].badges[0].x) - close_from_right) < 0.01f);

			r.items.queried.clear();
			r.inj->query_round();
			CHECK(t, (r.items.queried == std::vector<HWND>{ w }));   // the resized one, without waiting its turn
		} },
		{ "covered_parts_are_clipped_away", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1), cover = fake_hwnd(2);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.inj->store(r.inj->query_round());

			r.stack.windows = { { cover, { 0, 0, 700, 1600 }, true }, { w, { 100, 100, 1300, 900 }, true } };
			r.inj->follow();
			CHECK(t, r.layer->last_groups.size() == 1);
			for (const RECT& v : r.layer->last_groups[0].visible) CHECK(t, v.left >= 700);

			r.stack.windows[0].frame = { 0, 0, 2560, 1600 };   // fully covered
			r.inj->follow();
			CHECK(t, !r.layer->shown());
		} },
		{ "minimized_or_gone_windows_drop_out", [](test::context& t) {
			rig r;
			const HWND a = fake_hwnd(1), b = fake_hwnd(2);
			r.items.order = { a, b };
			r.items.snaps[a] = explorer_like(a);
			r.items.snaps[b] = explorer_like(b);
			r.stack.windows = { { a, { 100, 100, 1300, 900 }, false }, { b, { 100, 100, 1300, 900 }, true } };

			r.inj->store(r.inj->query_round());
			r.inj->follow();
			CHECK(t, r.layer->last_groups.size() == 2);

			r.stack.windows.pop_back();   // b minimized: not on screen
			r.inj->follow();
			CHECK(t, r.layer->last_groups.size() == 1);

			r.items.order = { a };        // b closed: its layout goes
			r.inj->store(r.inj->query_round());
			r.stack.windows.push_back({ b, { 100, 100, 1300, 900 }, true });
			r.inj->follow();
			CHECK(t, r.layer->last_groups.size() == 1);
		} },
		{ "a_query_taken_mid_move_is_dropped_and_retried", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.items.snaps[w].settled = false;

			CHECK(t, r.inj->query_round().updated.empty());
			r.items.queried.clear();
			r.items.snaps[w].settled = true;
			CHECK(t, r.inj->query_round().updated.size() == 1);   // retried right away, not after a second
		} },
		{ "numbers_follow_the_element", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };

			r.inj->store(r.inj->query_round());
			r.inj->follow();
			const int row = r.layer->last_groups[0].badges[1].number;

			std::swap(r.items.snaps[w].items[0], r.items.snaps[w].items[1]);
			r.inj->changed(w);
			r.inj->store(r.inj->query_round());
			r.inj->follow();
			CHECK(t, r.layer->last_groups[0].badges[0].number == row);
		} },
		{ "change_events_reach_the_injection", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };

			r.inj->on();
			CHECK(t, static_cast<bool>(r.changes.changed));
			CHECK(t, pump_until([&] { return r.layer->shown(); }));
			r.inj->off();
			CHECK(t, !r.layer->shown());
			CHECK(t, !r.changes.changed);
			CHECK(t, r.items.disconnects == 1);
		} },
	} };
}

test::suite desktop_injection_suite()
{
	return { "desktop_injection", {
		{ "place_on_top_right", [](test::context& t) {
			const RECT icon{ 40, 8, 112, 80 };
			const badge b = desktop_injection::place(icon, 4);
			CHECK(t, b.number == 4);
			CHECK(t, b.diameter > 0 && b.diameter < 72);
			CHECK(t, b.x > (icon.left + icon.right) / 2.0f && b.x <= icon.right);
			CHECK(t, b.y < (icon.top + icon.bottom) / 2.0f && b.y >= icon.top);
		} },
		{ "badges_every_icon", [](test::context& t) {
			fake_desktop desk;
			auto [owned, layer] = fake_overlay::make();
			desk.snap = two_icons();

			desktop_injection inj(desk, std::move(owned));
			inj.refresh();
			CHECK(t, layer->shown());
			CHECK(t, layer->last.size() == 2);
			CHECK(t, layer->last_owner == fake_hwnd(0x10));
			for (const badge& b : layer->last) CHECK(t, b.number >= 1 && b.number <= 9);
		} },
		{ "numbers_stay_per_icon", [](test::context& t) {
			fake_desktop desk;
			auto [owned, layer] = fake_overlay::make();
			desk.snap = two_icons();

			desktop_injection inj(desk, std::move(owned));
			inj.refresh();
			const int first = layer->last[1].number;

			// Moving and reordering icons keeps each one's number.
			std::swap(desk.snap->icons[0], desk.snap->icons[1]);
			desk.snap->icons[0].rect = { 300, 300, 372, 372 };
			inj.refresh();
			CHECK(t, layer->last[0].number == first);
		} },
		{ "hides_without_icons", [](test::context& t) {
			fake_desktop desk;
			auto [owned, layer] = fake_overlay::make();
			desk.snap = two_icons();

			desktop_injection inj(desk, std::move(owned));
			inj.refresh();
			desk.snap.reset();
			inj.refresh();
			CHECK(t, !layer->shown());
		} },
		{ "off_hides", [](test::context& t) {
			fake_desktop desk;
			auto [owned, layer] = fake_overlay::make();
			desk.snap = two_icons();

			desktop_injection inj(desk, std::move(owned));
			inj.on();
			CHECK(t, pump_until([&] { return layer->shown(); }));
			inj.off();
			CHECK(t, !layer->shown());
			CHECK(t, desk.disconnects == 1);
		} },
	} };
}

test::suite tray_icon_suite()
{
	// run() needs a real window and tray, so only the state changes are tested.
	return { "tray_icon", {
		{ "visible_by_default", [](test::context& t) {
			fake_notifyinjector f;
			tray_icon tray(f);
			CHECK(t, tray.visible());
			CHECK(t, f.calls.empty());
		} },
		{ "toggle_switches_injector", [](test::context& t) {
			fake_notifyinjector f;
			tray_icon tray(f);

			std::vector<bool> published;
			auto sub = tray.on_visibility_changed().connect([&](bool v) { published.push_back(v); });

			tray.toggle();
			CHECK(t, !tray.visible());
			CHECK(t, !f.active());

			tray.toggle();
			CHECK(t, tray.visible());
			CHECK(t, f.active());

			CHECK(t, (f.calls == std::vector<bool>{ false, true }));
			CHECK(t, (published == std::vector<bool>{ false, true }));
		} },
	} };
}

test::suite composition_suite()
{
	// Only resolves the graph; switching it on would touch the real taskbar, cursors and desktop.
	return { "composition", {
		{ "resolves_singletons", [](test::context& t) {
			auto injector = composition::make_injector();
			auto& tray1 = injector.create<i_tray_icon&>();
			auto& tray2 = injector.create<i_tray_icon&>();
			auto& inj = injector.create<i_notifyinjector&>();
			const auto layer1 = injector.create<std::unique_ptr<i_overlay>>();
			const auto layer2 = injector.create<std::unique_ptr<i_overlay>>();

			CHECK(t, &tray1 == &tray2);
			CHECK(t, layer1 && layer2 && layer1 != layer2);   // each injection gets its own
			CHECK(t, dynamic_cast<tray_icon*>(&tray1) != nullptr);
			CHECK(t, dynamic_cast<notifyinjector*>(&inj) != nullptr);
			CHECK(t, !inj.active());
		} },
	} };
}
