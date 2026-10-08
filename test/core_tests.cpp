#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <map>
#include <vector>

#include "composition.h"
#include "framework.h"

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
		void hide() override { shown_ = false; }
		bool shown() const override { return shown_; }

		HWND last_owner = nullptr;
		RECT last_area{};
		std::vector<badge> last;

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
