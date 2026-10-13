#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "overlay.h"
#include "composition.h"
#include "drift.h"
#include "framework.h"
#include "occlusion.h"
#include "window_changes.h"

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

		void disconnect() override { ++disconnects; }

		std::vector<HWND> open;
		std::map<HWND, HICON> overlays;
		int calls = 0;
		int disconnects = 0;
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
		std::optional<desktop_snapshot> snapshot() override { ++snapshots; return snap; }
		HWND host() override { return snap ? snap->host : nullptr; }
		int snapshots = 0;
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

	// Both workers may call in at once; tests set it up before turning anything on.
	class fake_window_items final : public i_window_items {
	public:
		std::vector<HWND> candidates() override { return order; }
		HWND front() override { return focus; }
		std::optional<window_snapshot> query(HWND w) override
		{
			std::optional<window_snapshot> snap;
			std::chrono::milliseconds cost = delay;
			{
				std::lock_guard guard(lock);
				queried.push_back(w);
				if (const auto it = slow.find(w); it != slow.end()) cost = it->second;
				if (const auto it = snaps.find(w); it != snaps.end()) snap = it->second;
			}
			if (cost.count() > 0) Sleep(static_cast<DWORD>(cost.count()));
			return snap;
		}
		void watch(const window_snapshot& snap) override
		{
			std::lock_guard guard(lock);
			watched.insert(snap.window);
			shifted.erase(snap.window);
		}
		std::optional<bool> moved(HWND w) override
		{
			std::lock_guard guard(lock);
			++probes;
			if (!watched.contains(w)) return std::nullopt;
			return shifted.contains(w);
		}
		void disconnect() override
		{
			std::lock_guard guard(lock);
			++disconnects;
		}

		std::size_t asked(HWND w)
		{
			std::lock_guard guard(lock);
			return static_cast<std::size_t>(std::ranges::count(queried, w));
		}

		// Moves every item of w down by dy pixels, the way content scrolls.
		void scroll(HWND w, int dy)
		{
			std::lock_guard guard(lock);
			for (auto& item : snaps[w].items) OffsetRect(&item.rect, 0, dy);
			shifted.insert(w);
		}

		std::vector<HWND> order;
		HWND focus = nullptr;                        // the window in front, if any
		std::map<HWND, window_snapshot> snaps;
		std::vector<HWND> queried;
		std::chrono::milliseconds delay{ 0 };        // pretend each query costs this long
		std::map<HWND, std::chrono::milliseconds> slow;   // or this long for some windows
		std::set<HWND> watched, shifted;             // what moved() checks on, and what has moved since
		int probes = 0;
		int disconnects = 0;
		std::mutex lock;
	};

	class fake_window_changes final : public i_window_changes {
	public:
		void start() override { ++users; }
		void stop() override { --users; }
		void fire(HWND w, window_change kind) { changeEvent.publish(w, kind); }
		int users = 0;
	};

	class fake_window_stack final : public i_window_stack {
	public:
		std::vector<stacked_window> front_to_back() override { ++reads; return windows; }
		std::optional<RECT> frame_of(HWND w) override
		{
			++frame_reads;
			const auto it = std::ranges::find(windows, w, &stacked_window::hwnd);
			if (it == windows.end()) return std::nullopt;
			return it->frame;
		}
		int reads = 0;
		int frame_reads = 0;
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
			CHECK(t, pump_until([&] { return inj.badged() == 2; }));
			inj.off();
			CHECK(t, bar.overlays.empty());   // cleared by the worker as it ended
			CHECK(t, bar.disconnects == 1);
		} },
		{ "badges_new_windows_once", [](test::context& t) {
			fake_taskbar bar;
			bar.open = { fake_hwnd(1) };

			taskbar_injection inj(bar);
			inj.refresh();
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
			inj.refresh();
			bar.open = { fake_hwnd(2) };
			inj.refresh();
			CHECK(t, inj.badged() == 1);
			inj.off();
		} },
		{ "off_clears_overlays", [](test::context& t) {
			fake_taskbar bar;
			bar.open = { fake_hwnd(1), fake_hwnd(2) };

			taskbar_injection inj(bar);
			inj.refresh();
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

			fake_window_changes changes;
			tray_area_injection inj(items, changes, std::move(owned));
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

			fake_window_changes changes;
			tray_area_injection inj(items, changes, std::move(owned));
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

			fake_window_changes changes;
			tray_area_injection inj(items, changes, std::move(owned));
			inj.refresh();
			const badge before = layer->last[0];

			items.snap->items[0].rect = { 2097, 1528, 2145, 1600 };   // an icon was added to its right
			inj.refresh();
			inj.refresh();
			CHECK(t, layer->last[0].x < before.x);
		} },
		{ "queries_again_only_after_a_change", [](test::context& t) {
			fake_taskbar_items items;
			auto [owned, layer] = fake_overlay::make();
			items.snap = two_tray_items();
			fake_window_changes changes;
			tray_area_injection inj(items, changes, std::move(owned));

			CHECK(t, inj.compute().has_value());    // the first look
			CHECK(t, !inj.compute().has_value());   // nothing changed since
			inj.taskbar_changed();
			CHECK(t, inj.compute().has_value());
			CHECK(t, inj.compute().has_value());    // and for a while after, while animations settle
		} },
		{ "numbers_stay_per_item", [](test::context& t) {
			fake_taskbar_items items;
			auto [owned, layer] = fake_overlay::make();
			items.snap = two_tray_items();

			fake_window_changes changes;
			tray_area_injection inj(items, changes, std::move(owned));
			inj.refresh();
			const auto first = layer->last;
			for (int i = 0; i < 3; ++i) inj.refresh();
			CHECK(t, layer->last == first);
		} },
		{ "off_hides", [](test::context& t) {
			fake_taskbar_items items;
			auto [owned, layer] = fake_overlay::make();
			items.snap = two_tray_items();

			fake_window_changes changes;
			tray_area_injection inj(items, changes, std::move(owned));
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

			fake_window_changes changes;
			window_icon_injection inj(icons, changes);
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

			fake_window_changes changes;
			window_icon_injection inj(icons, changes);
			inj.refresh();
			CHECK(t, inj.badged() == 1);
			CHECK(t, icons.get(admin, icon_kind::big) == nullptr);
			inj.off();
		} },
		{ "leaves_badged_windows_alone", [](test::context& t) {
			fake_window_icons icons;
			icons.open = { fake_hwnd(1) };

			fake_window_changes changes;
			window_icon_injection inj(icons, changes);
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

			fake_window_changes changes;
			window_icon_injection inj(icons, changes);
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

			fake_window_changes changes;
			window_icon_injection inj(icons, changes);
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

			fake_window_changes changes;
			window_icon_injection inj(icons, changes);
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

test::suite badge_scene_suite()
{
	// A 20px badge at (50,50): its 22px picture spans 39..61 both ways.
	const badge b{ 50, 50, 20, 7 };
	const RECT whole{ 0, 0, 22, 22 };
	return { "badge_scene", {
		{ "pictures_leave_a_pixel_around_the_circle", [](test::context& t) {
			CHECK(t, badge_scene::sprite_size(20) == 22);
			CHECK(t, badge_scene::sprite_size(13.6f) == 16);
		} },
		{ "a_badge_in_the_open_shows_whole", [=](test::context& t) {
			const auto c = badge_scene::cut_out(b, { { 0, 0, 200, 200 } });
			const RECT sprite{ 39, 39, 61, 61 };
			CHECK(t, EqualRect(&c.sprite, &sprite));
			CHECK(t, c.pieces.size() == 1 && EqualRect(&c.pieces[0], &whole));
		} },
		{ "a_half_covered_badge_is_clipped", [=](test::context& t) {
			const auto c = badge_scene::cut_out(b, { { 0, 0, 50, 200 } });   // something covers x >= 50
			const RECT left_half{ 0, 0, 11, 22 };
			CHECK(t, c.pieces.size() == 1 && EqualRect(&c.pieces[0], &left_half));
		} },
		{ "a_badge_by_a_corner_shows_in_pieces", [=](test::context& t) {
			// Visible: a band above y=45 and a strip left of x=45; the corner below and right is covered.
			const auto c = badge_scene::cut_out(b, { { 0, 0, 200, 45 }, { 0, 45, 45, 200 } });
			const RECT top{ 0, 0, 22, 6 }, left{ 0, 6, 6, 22 };
			CHECK(t, c.pieces.size() == 2);
			CHECK(t, EqualRect(&c.pieces[0], &top) && EqualRect(&c.pieces[1], &left));
		} },
		{ "pieces_that_add_up_show_whole", [=](test::context& t) {
			const auto c = badge_scene::cut_out(b, { { 0, 0, 50, 200 }, { 50, 0, 200, 200 } });
			CHECK(t, c.pieces.size() == 1 && EqualRect(&c.pieces[0], &whole));
		} },
		{ "a_covered_badge_has_no_pieces", [=](test::context& t) {
			CHECK(t, badge_scene::cut_out(b, { { 100, 100, 200, 200 } }).pieces.empty());
			CHECK(t, badge_scene::cut_out(b, {}).pieces.empty());
		} },
	} };
}

test::suite drift_suite()
{
	using namespace std::chrono_literals;
	using spots = std::vector<drift::spot>;

	// A toolbar of two buttons that stays put, and a list of ten rows 40px apart that scrolls by `scrolled`.
	const auto page = [](long scrolled) {
		spots s{ { 1, { 30, 20 } }, { 2, { 70, 20 } } };
		for (std::size_t row = 0; row < 10; ++row)
			s.push_back({ 100 + row, { 40, 100 + static_cast<long>(row) * 40 - scrolled } });
		return s;
	};
	const drift::clock::time_point t0{};

	return { "drift", {
		{ "steady_scrolling_carries_on_between_layouts", [=](test::context& t) {
			const drift first = drift::track({}, page(0), t0, page(100), t0 + 500ms, 10);
			const drift second = drift::track(first, page(100), t0 + 500ms, page(200), t0 + 1000ms, 10);
			CHECK(t, !first.moving());   // one step could be anything
			CHECK(t, second.moving());
			CHECK(t, std::abs(second.vy + 200) < 1 && std::abs(second.vx) < 1);
			CHECK(t, second.keys.contains(100) && !second.keys.contains(1));   // the toolbar stays

			const auto quarter = second.offset(125ms);
			CHECK(t, quarter && quarter->x == 0 && quarter->y == -25);
			const auto late = second.offset(700ms);
			CHECK(t, late && late->y == -100);         // held where the next layout should have been
			CHECK(t, !second.offset(1100ms));          // and given up once it is long overdue
		} },
		{ "a_change_of_pace_starts_over", [=](test::context& t) {
			const drift first = drift::track({}, page(0), t0, page(100), t0 + 500ms, 10);
			const drift slower = drift::track(first, page(100), t0 + 500ms, page(120), t0 + 1000ms, 10);
			CHECK(t, slower.streak == 1 && !slower.moving());
		} },
		{ "a_few_strays_are_not_the_content", [=](test::context& t) {
			spots after = page(0);
			after[2].at.y += 30;   // two rows jump, the other eight stay
			after[3].at.y += 30;
			CHECK(t, drift::track({}, page(0), t0, after, t0 + 500ms, 10).streak == 0);
		} },
		{ "rows_scrolling_into_view_move_along", [=](test::context& t) {
			spots after = page(100);
			after.push_back({ 200, { 40, 420 } });   // a new row inside the list
			after.push_back({ 201, { 900, 20 } });   // a new button far away
			const drift d = drift::track({}, page(0), t0, after, t0 + 500ms, 10);
			CHECK(t, d.keys.contains(200) && !d.keys.contains(201));
		} },
	} };
}

test::suite window_changes_suite()
{
	const HWND root = fake_hwnd(1), child = fake_hwnd(2);
	return { "window_changes", {
		{ "a_whole_window_moving_is_a_frame_change", [=](test::context& t) {
			CHECK(t, window_changes::classify(EVENT_OBJECT_LOCATIONCHANGE, root, root, OBJID_WINDOW) == window_change::frame);
			CHECK(t, window_changes::classify(EVENT_SYSTEM_MOVESIZEEND, root, root, OBJID_WINDOW) == window_change::frame);
		} },
		{ "something_inside_moving_is_a_content_change", [=](test::context& t) {
			CHECK(t, window_changes::classify(EVENT_OBJECT_LOCATIONCHANGE, child, root, OBJID_CLIENT) == window_change::content);
			CHECK(t, window_changes::classify(EVENT_OBJECT_SHOW, child, root, OBJID_WINDOW) == window_change::content);
			CHECK(t, window_changes::classify(EVENT_SYSTEM_SCROLLINGEND, child, root, OBJID_VSCROLL) == window_change::content);
		} },
		{ "activation_and_visibility_are_stack_changes", [=](test::context& t) {
			CHECK(t, window_changes::classify(EVENT_SYSTEM_FOREGROUND, root, root, OBJID_WINDOW) == window_change::stack);
			CHECK(t, window_changes::classify(EVENT_SYSTEM_MINIMIZESTART, root, root, OBJID_WINDOW) == window_change::stack);
			CHECK(t, window_changes::classify(EVENT_OBJECT_HIDE, root, root, OBJID_WINDOW) == window_change::stack);
			CHECK(t, window_changes::classify(EVENT_OBJECT_CLOAKED, root, root, OBJID_WINDOW) == window_change::stack);
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

		// What the real event hook reports when a window moves, opens, closes or comes to the front.
		void stack_changed() { inj->changed(nullptr, window_change::stack); }

		// The UI thread looks at the screen, then the worker queries and the UI thread stores the result.
		void cycle()
		{
			stack_changed();
			inj->follow();
			inj->store(inj->query_round());
			inj->follow();
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
		{ "the_stack_is_read_only_after_a_change", [](test::context& t) {
			rig r;
			r.stack.windows = { { fake_hwnd(1), { 100, 100, 1300, 900 }, true } };
			r.inj->follow();
			const int first = r.stack.reads;
			r.inj->follow();
			r.inj->follow();
			CHECK(t, r.stack.reads == first);   // nothing happened: no walk over every window

			r.inj->changed(fake_hwnd(1), window_change::content);
			r.inj->follow();
			CHECK(t, r.stack.reads == first);   // content changes need a query, not a new stack

			r.inj->changed(fake_hwnd(1), window_change::frame);
			r.inj->follow();
			CHECK(t, r.stack.reads == first);   // a move re-reads that one frame only
			CHECK(t, r.stack.frame_reads == 1);

			r.inj->changed(fake_hwnd(1), window_change::stack);
			r.inj->follow();
			CHECK(t, r.stack.reads == first + 1);
		} },
		{ "frames_of_strangers_or_vanished_windows_need_the_whole_stack", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };
			r.inj->follow();
			const int first = r.stack.reads;

			r.inj->changed(fake_hwnd(9), window_change::frame);   // not on screen a moment ago
			r.inj->follow();
			CHECK(t, r.stack.reads == first + 1);

			r.stack.windows.clear();                               // w was hidden as it moved
			r.inj->changed(w, window_change::frame);
			r.inj->follow();
			CHECK(t, r.stack.reads == first + 2);
		} },
		{ "queries_only_changed_or_overdue_windows", [](test::context& t) {
			rig r;
			r.items.order = { fake_hwnd(1), fake_hwnd(2), fake_hwnd(3) };
			r.stack.windows = {
				{ fake_hwnd(1), { 0, 0, 500, 500 }, true },
				{ fake_hwnd(2), { 600, 0, 1100, 500 }, true },
				{ fake_hwnd(3), { 1200, 0, 1700, 500 }, true },
			};
			r.inj->follow();

			r.inj->query_round();   // nothing seen yet: everything is overdue
			CHECK(t, (r.items.queried == std::vector<HWND>{ fake_hwnd(1), fake_hwnd(2), fake_hwnd(3) }));

			r.items.queried.clear();
			r.inj->query_round();   // nothing changed, nothing overdue
			CHECK(t, r.items.queried.empty());

			r.inj->changed(fake_hwnd(3), window_change::content);
			Sleep(20);   // past the pacing of a query that cost next to nothing, even if it was preempted
			r.inj->query_round();
			CHECK(t, (r.items.queried == std::vector<HWND>{ fake_hwnd(3) }));
		} },
		{ "hidden_windows_wait_until_they_show", [](test::context& t) {
			rig r;
			const HWND front = fake_hwnd(1), behind = fake_hwnd(2);
			r.items.order = { front, behind };
			r.stack.windows = {
				{ front, { 0, 0, 2560, 1600 }, true },     // covers the whole screen
				{ behind, { 100, 100, 900, 700 }, true },
			};
			r.inj->follow();
			r.inj->query_round();
			CHECK(t, (r.items.queried == std::vector<HWND>{ front }));

			r.inj->changed(behind, window_change::content);
			r.items.queried.clear();
			r.inj->query_round();
			CHECK(t, r.items.queried.empty());   // changed, but nobody can see it

			r.stack.windows[0].frame = { 1000, 0, 2560, 1600 };   // uncovered
			r.stack_changed();
			r.inj->follow();
			r.inj->query_round();
			CHECK(t, (r.items.queried == std::vector<HWND>{ behind }));
		} },
		{ "badges_follow_a_moved_window_without_a_new_query", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };

			r.cycle();
			CHECK(t, r.layer->shown() && r.layer->last_groups.size() == 1);
			CHECK(t, r.layer->last_owner == nullptr);
			const badge_group before = r.layer->last_groups[0];
			CHECK(t, before.key == w && before.origin.x == 100 && before.origin.y == 100);

			r.stack.windows[0].frame = { 300, 100, 1500, 900 };   // dragged 200px right
			r.inj->changed(w, window_change::frame);
			r.inj->follow();
			const badge_group& after = r.layer->last_groups[0];
			CHECK(t, after.origin.x == 300 && after.origin.y == 100);
			CHECK(t, after.badges == before.badges);   // the same group, only moved
			CHECK(t, r.items.queried.size() == 1);
		} },
		{ "a_resize_keeps_badges_by_their_edges_and_requeries_once_settled", [](test::context& t) {
			rig r;
			const HWND front = fake_hwnd(1), w = fake_hwnd(2);
			r.items.order = { front, w };
			r.items.snaps[front] = explorer_like(front);
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true }, { front, { 1400, 0, 2500, 600 }, true } };

			r.cycle();
			const float close_from_right = 1200 - r.layer->last_groups[0].badges[0].x;   // the frame is 1200px wide

			r.stack.windows[0].frame = { 0, 0, 2560, 1528 };   // maximized
			r.inj->changed(w, window_change::frame);
			r.inj->follow();
			CHECK(t, r.layer->shown());
			CHECK(t, std::abs((2560 - r.layer->last_groups[0].badges[0].x) - close_from_right) < 0.01f);

			r.items.queried.clear();
			r.inj->query_round();
			CHECK(t, r.items.queried.empty());   // still settling: its app may be mid-layout

			Sleep(static_cast<DWORD>(window_item_injection::frame_quiet.count()) + 30);
			r.inj->query_round();
			CHECK(t, (r.items.queried == std::vector<HWND>{ w }));   // the resized one, without waiting its turn
		} },
		{ "a_moving_window_is_queried_once_it_stops", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };
			r.cycle();

			r.items.queried.clear();
			for (int i = 0; i < 5; ++i) {   // dragged around while its content keeps changing
				r.inj->changed(w, window_change::frame);
				r.inj->changed(w, window_change::content);
				Sleep(20);
				r.inj->query_round();
			}
			CHECK(t, r.items.queried.empty());

			Sleep(static_cast<DWORD>(window_item_injection::frame_quiet.count()) + 30);
			r.inj->query_round();
			CHECK(t, (r.items.queried == std::vector<HWND>{ w }));
		} },
		{ "covered_parts_are_clipped_away", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1), cover = fake_hwnd(2);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };
			r.cycle();

			r.stack.windows = { { cover, { 0, 0, 700, 1600 }, true }, { w, { 100, 100, 1300, 900 }, true } };
			r.stack_changed();
			r.inj->follow();
			CHECK(t, r.layer->last_groups.size() == 1);
			const badge_group& g = r.layer->last_groups[0];
			for (const RECT& v : g.visible) CHECK(t, g.origin.x + v.left >= 700);

			r.stack.windows[0].frame = { 0, 0, 2560, 1600 };   // fully covered
			r.stack_changed();
			r.inj->follow();
			CHECK(t, !r.layer->shown());
		} },
		{ "a_minimized_window_keeps_its_layout", [](test::context& t) {
			rig r;
			const HWND a = fake_hwnd(1), b = fake_hwnd(2);
			r.items.order = { a, b };
			r.items.snaps[a] = explorer_like(a);
			r.items.snaps[b] = explorer_like(b);
			r.stack.windows = { { a, { 100, 100, 1300, 900 }, false }, { b, { 100, 100, 1300, 900 }, true } };

			r.cycle();
			CHECK(t, r.layer->last_groups.size() == 2);

			r.stack.windows.pop_back();   // b minimized: off the screen, still a candidate
			r.stack_changed();
			r.inj->follow();
			CHECK(t, r.layer->last_groups.size() == 1);

			r.items.queried.clear();
			r.stack.windows.push_back({ b, { 100, 100, 1300, 900 }, true });   // restored
			r.stack_changed();
			r.inj->follow();
			CHECK(t, r.layer->last_groups.size() == 2);   // back at once, before any query
			CHECK(t, r.items.queried.empty());
		} },
		{ "a_closed_window_drops_out", [](test::context& t) {
			rig r;
			const HWND a = fake_hwnd(1), b = fake_hwnd(2);
			r.items.order = { a, b };
			r.items.snaps[a] = explorer_like(a);
			r.items.snaps[b] = explorer_like(b);
			r.stack.windows = { { a, { 100, 100, 1300, 900 }, false }, { b, { 100, 100, 1300, 900 }, true } };
			r.cycle();

			r.items.order = { a };   // b closed: no longer a candidate
			r.inj->store(r.inj->query_round());
			r.inj->follow();
			CHECK(t, r.layer->last_groups.size() == 1);
		} },
		{ "an_unchanged_layout_is_not_handed_over_again", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };
			r.cycle();
			const int reads = r.stack.reads;

			r.inj->changed(w, window_change::content);
			Sleep(30);   // past the pacing of the last query, however long it took
			const auto same = r.inj->query_round();
			CHECK(t, r.items.queried.size() == 2 && same.updated.empty());

			r.items.snaps[w].items.pop_back();   // a row went away
			r.inj->changed(w, window_change::content);
			Sleep(30);   // past the pacing of the last query, however long it took
			r.inj->store(r.inj->query_round());
			r.inj->follow();
			CHECK(t, r.layer->last_groups[0].badges.size() == 1);
			CHECK(t, r.stack.reads == reads);   // a new layout alone needs no new look at the stack
		} },
		{ "rounds_not_taken_yet_are_merged", [](test::context& t) {
			const HWND a = fake_hwnd(1), b = fake_hwnd(2), c = fake_hwnd(3);
			const auto layout = [](HWND w, int n) {
				return window_item_injection::window_badges{ w, { 100, 100 }, { { badge{ 10, 10, 14, n } } } };
			};
			using hwnds = std::vector<HWND>;
			window_item_injection::query_result unread{ hwnds{ a, b, c }, { layout(a, 1), layout(b, 1) } };
			window_item_injection::merge(unread, { hwnds{ a, c }, { layout(a, 2), layout(c, 3) } });

			// a newer, b closed since, c new: nothing handed over is lost, nothing stale is kept.
			CHECK(t, (unread.candidates == hwnds{ a, c }));
			CHECK(t, unread.updated.size() == 2);
			CHECK(t, unread.updated[0].window == a && unread.updated[0].badges[0].offset.number == 2);
			CHECK(t, unread.updated[1].window == c);

			// The focused window's worker doesn't know the others; what it hands over adds to the rest.
			window_item_injection::merge(unread, { std::nullopt, { layout(c, 4) } });
			CHECK(t, (unread.candidates == hwnds{ a, c }));
			CHECK(t, unread.updated.size() == 2 && unread.updated[1].badges[0].offset.number == 4);
		} },
		{ "badges_come_back_after_off_and_on", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };

			r.inj->on();
			CHECK(t, pump_until([&] { return r.layer->shown(); }));
			r.inj->off();
			CHECK(t, !r.layer->shown());
			r.inj->on();
			CHECK(t, pump_until([&] { return r.layer->shown(); }));
			r.inj->off();
		} },
		{ "a_query_taken_mid_move_is_dropped_and_retried", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.items.snaps[w].settled = false;
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };
			r.inj->follow();

			CHECK(t, r.inj->query_round().updated.empty());
			r.items.queried.clear();
			r.items.snaps[w].settled = true;
			Sleep(10);   // past the pacing of a query that cost next to nothing
			CHECK(t, r.inj->query_round().updated.size() == 1);   // retried right away, not after a second
		} },
		{ "a_slow_noisy_window_is_paced_by_its_cost", [](test::context& t) {
			rig r;
			const HWND slow = fake_hwnd(1);
			r.items.order = { slow };
			r.items.snaps[slow] = explorer_like(slow);
			r.items.delay = std::chrono::milliseconds(30);
			r.stack.windows = { { slow, { 100, 100, 1300, 900 }, true } };
			r.inj->follow();

			r.inj->query_round();
			r.inj->changed(slow, window_change::content);
			r.items.queried.clear();
			r.inj->query_round();   // changed again at once, but its last query took 30ms
			CHECK(t, r.items.queried.empty());

			Sleep(30 * window_item_injection::cost_factor + 20);
			r.inj->query_round();   // still marked as changed, and now its turn has come
			CHECK(t, r.items.queried.size() == 1);
		} },
		{ "backoff_doubles_while_nothing_changes", [](test::context& t) {
			CHECK(t, window_item_injection::next_backoff(1, false) == 2);
			CHECK(t, window_item_injection::next_backoff(4, false) == 8);
			CHECK(t, window_item_injection::next_backoff(8, false) == window_item_injection::max_backoff);
			CHECK(t, window_item_injection::next_backoff(8, true) == 1);
		} },
		{ "the_focused_window_is_asked_back_to_back_only_while_it_is_lively", [](test::context& t) {
			using namespace std::chrono_literals;
			using wi = window_item_injection;
			CHECK(t, wi::front_gap(40ms, true, 1) == 40ms);    // as soon as the last answer is in
			CHECK(t, wi::front_gap(2ms, true, 1) == 16ms);     // but not faster than the screen
			CHECK(t, wi::front_gap(40ms, true, 4) == 160ms);   // stretching while nothing new turns up
			CHECK(t, wi::front_gap(2ms, true, 4) == 64ms);
			CHECK(t, wi::front_gap(40ms, false, 8) == wi::front_refresh);   // quiet
			CHECK(t, wi::front_gap(900ms, false, 1) == 2700ms);              // quiet and heavy
		} },
		{ "a_focused_window_that_keeps_changing_is_asked_again_at_once", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.focus = w;
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };
			r.stack_changed();
			r.inj->follow();

			CHECK(t, r.inj->query_front().updated.size() == 1);
			for (int i = 0; i < 3; ++i) {   // scrolling, with no events at all
				r.items.scroll(w, -10);
				Sleep(20);                      // a frame's time, nothing more
				CHECK(t, r.inj->query_front().updated.size() == 1);
			}
			CHECK(t, r.items.asked(w) == 4);

			Sleep(static_cast<DWORD>(window_item_injection::front_settle.count()) + 100);
			r.inj->query_front();            // quiet for a while: one routine look
			Sleep(20);
			r.inj->query_front();
			CHECK(t, r.items.asked(w) == 5);   // and the next one much later

			r.inj->changed(w, window_change::content);
			Sleep(20);
			r.inj->query_front();
			CHECK(t, r.items.asked(w) == 6);   // unless the app says something changed
		} },
		{ "between_looks_one_element_tells_whether_to_look_early", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.focus = w;
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };
			r.stack_changed();
			r.inj->follow();

			r.inj->query_front();
			r.inj->query_front();   // right after: the element sits where it was
			CHECK(t, r.items.asked(w) == 1 && r.items.probes == 1);

			r.items.scroll(w, -10);
			r.inj->query_front();   // it moved: look now, not when the schedule says
			CHECK(t, r.items.asked(w) == 2);
			r.inj->query_front();
			CHECK(t, r.items.asked(w) == 2);
		} },
		{ "the_background_round_leaves_the_focused_window_alone", [](test::context& t) {
			rig r;
			const HWND front = fake_hwnd(1), back = fake_hwnd(2);
			r.items.order = { front, back };
			r.items.focus = front;
			r.stack.windows = { { front, { 0, 0, 500, 500 }, true }, { back, { 600, 0, 1100, 500 }, true } };
			r.stack_changed();
			r.inj->follow();

			r.inj->query_round();
			CHECK(t, (r.items.queried == std::vector<HWND>{ back }));
		} },
		{ "a_slow_window_behind_never_holds_up_the_focused_one", [](test::context& t) {
			rig r;
			const HWND front = fake_hwnd(1), back = fake_hwnd(2);
			r.items.order = { front, back };
			r.items.focus = front;
			r.items.snaps[front] = explorer_like(front);
			r.items.snaps[back] = explorer_like(back);
			r.items.slow[back] = std::chrono::milliseconds(1500);
			r.stack.windows = { { front, { 100, 100, 1300, 900 }, true }, { back, { 1300, 0, 2500, 800 }, true } };

			r.inj->on();
			CHECK(t, pump_until([&] { return r.items.asked(back) == 1; }));   // the slow one is being asked now
			const std::size_t before = r.items.asked(front);
			for (int i = 0; i < 10; ++i) {   // meanwhile the focused window scrolls
				r.items.scroll(front, -10);
				pump_until([] { return false; }, std::chrono::milliseconds(30));
			}
			CHECK(t, r.items.asked(front) >= before + 5);
			r.inj->off();
		} },
		{ "a_window_crying_wolf_is_asked_less_and_less", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.items.delay = std::chrono::milliseconds(20);   // each query "costs" 20ms, so the base gap is 60ms
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };
			r.inj->follow();

			r.inj->query_round();                            // first look
			r.inj->changed(w, window_change::content);
			Sleep(110);   // past the base gap: Sleep(20) really lasts ~31ms, so up to ~95ms
			r.inj->query_round();                            // same layout as before: wait twice as long next
			CHECK(t, r.items.queried.size() == 2);

			r.inj->changed(w, window_change::content);
			Sleep(70);
			r.inj->query_round();
			CHECK(t, r.items.queried.size() == 2);           // ~110ms since the last query, twice the base gap needed now

			Sleep(200);
			r.inj->query_round();
			CHECK(t, r.items.queried.size() == 3);
		} },
		{ "numbers_follow_the_element", [](test::context& t) {
			rig r;
			const HWND w = fake_hwnd(1);
			r.items.order = { w };
			r.items.snaps[w] = explorer_like(w);
			r.stack.windows = { { w, { 100, 100, 1300, 900 }, true } };

			r.cycle();
			const int row = r.layer->last_groups[0].badges[1].number;

			std::swap(r.items.snaps[w].items[0], r.items.snaps[w].items[1]);
			r.inj->changed(w, window_change::content);
			Sleep(20);   // past the pacing of the last query, however long it took
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
			CHECK(t, r.changes.users == 1);
			CHECK(t, r.changes.on_change().count() == 1);
			CHECK(t, pump_until([&] { return r.layer->shown(); }));

			// A move reported by the hook reaches the injection and moves the badges.
			r.stack.windows[0].frame = { 300, 100, 1500, 900 };
			r.changes.fire(w, window_change::frame);
			CHECK(t, pump_until([&] { return r.layer->last_groups[0].origin.x == 300; }));

			r.inj->off();
			CHECK(t, !r.layer->shown());
			CHECK(t, r.changes.users == 0);
			CHECK(t, r.changes.on_change().count() == 0);
			CHECK(t, r.items.disconnects == 2);   // each worker releases its own UI Automation objects
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

			fake_window_stack stack;
			stack.windows = { { fake_hwnd(0x10), { 0, 0, 2560, 1600 }, true } };   // the desktop, uncovered
			fake_window_changes changes;
			desktop_injection inj(desk, stack, changes, std::move(owned));
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

			fake_window_stack stack;
			stack.windows = { { fake_hwnd(0x10), { 0, 0, 2560, 1600 }, true } };   // the desktop, uncovered
			fake_window_changes changes;
			desktop_injection inj(desk, stack, changes, std::move(owned));
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

			fake_window_stack stack;
			stack.windows = { { fake_hwnd(0x10), { 0, 0, 2560, 1600 }, true } };   // the desktop, uncovered
			fake_window_changes changes;
			desktop_injection inj(desk, stack, changes, std::move(owned));
			inj.refresh();
			desk.snap.reset();
			inj.refresh();
			CHECK(t, !layer->shown());
		} },
		{ "a_covered_desktop_is_not_queried", [](test::context& t) {
			fake_desktop desk;
			auto [owned, layer] = fake_overlay::make();
			desk.snap = two_icons();
			fake_window_stack stack;
			stack.windows = {
				{ fake_hwnd(0x99), { 0, 0, 2560, 1600 }, true },   // a maximized window
				{ fake_hwnd(0x10), { 0, 0, 2560, 1600 }, true },   // the desktop behind it
			};
			fake_window_changes changes;
			desktop_injection inj(desk, stack, changes, std::move(owned));

			CHECK(t, !inj.compute().has_value());
			CHECK(t, desk.snapshots == 0);

			stack.windows.erase(stack.windows.begin());   // minimized
			inj.desktop_changed();
			CHECK(t, inj.compute().has_value());
			CHECK(t, desk.snapshots == 1);
		} },
		{ "off_hides", [](test::context& t) {
			fake_desktop desk;
			auto [owned, layer] = fake_overlay::make();
			desk.snap = two_icons();

			fake_window_stack stack;
			stack.windows = { { fake_hwnd(0x10), { 0, 0, 2560, 1600 }, true } };   // the desktop, uncovered
			fake_window_changes changes;
			desktop_injection inj(desk, stack, changes, std::move(owned));
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
