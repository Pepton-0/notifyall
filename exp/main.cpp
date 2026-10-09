// Verifies badging other apps' taskbar buttons via ITaskbarList3::SetOverlayIcon.
// Run without arguments for usage.

#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <string>
#include <vector>

#include "badge.h"
#include "desktop_icons.h"
#include "drag_window.h"
#include "event_census.h"
#include "overlay.h"
#include "query_variants.h"
#include "scene_test.h"
#include "start_items.h"
#include "taskbar_items.h"
#include "window_items.h"
#include "window_stack.h"
#include "window_items_exp.h"
#include "logger.h"
#include "strconv.h"
#include "taskbar_windows.h"

namespace {

	void out(const std::wstring& s)
	{
		std::fputs(lib::to_utf8(s).c_str(), stdout);
	}

	struct taskbar_list {
		ITaskbarList3* p = nullptr;

		taskbar_list()
		{
			CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p)))) return;
			if (FAILED(p->HrInit())) { p->Release(); p = nullptr; }
		}
		~taskbar_list()
		{
			if (p) p->Release();
			CoUninitialize();
		}
	};

	int list()
	{
		for (const auto& w : taskbar::enumerate()) {
			out(std::format(L"{:>10} pid={:<6} {:<32} {}\n",
				reinterpret_cast<std::uintptr_t>(w.hwnd), w.pid, w.exe, w.class_name));
		}
		return 0;
	}

	int apply(int seconds, bool keep)
	{
		taskbar_list tb;
		if (!tb.p) { out(L"ITaskbarList3 is not available\n"); return 1; }

		// Overlays are drawn at small icon size: 16px at 100% scaling.
		const int size = GetSystemMetrics(SM_CXSMICON);
		std::vector<HICON> icons;
		for (int n = 1; n <= 9; ++n) icons.push_back(badge_icon::make(n, size));
		out(std::format(L"badge size: {}px\n", size));

		std::mt19937 rng{ std::random_device{}() };
		std::uniform_int_distribution<int> dist(1, 9);

		const auto windows = taskbar::enumerate();
		int ok = 0;
		for (const auto& w : windows) {
			const int n = dist(rng);
			const HRESULT hr = tb.p->SetOverlayIcon(w.hwnd, icons[n - 1], std::to_wstring(n).c_str());
			ok += SUCCEEDED(hr);
			out(std::format(L"{} hr=0x{:08X} badge={} {}\n",
				SUCCEEDED(hr) ? L"OK  " : L"FAIL", static_cast<unsigned>(hr), n, w.exe));
		}
		out(std::format(L"applied: {}/{}\n", ok, windows.size()));

		Sleep(static_cast<DWORD>(seconds) * 1000);

		if (!keep) {
			for (const auto& w : windows) tb.p->SetOverlayIcon(w.hwnd, nullptr, nullptr);
			out(L"cleared\n");
		}

		// The taskbar keeps its own copies.
		for (const HICON i : icons) DestroyIcon(i);
		return 0;
	}

	int desktop_icons()
	{
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		const auto d = desktop::read();
		out(std::format(L"list view={} icon size={} spacing={}x{} icons={}\n",
			reinterpret_cast<std::uintptr_t>(d.list_view), d.icon_size, d.spacing.x, d.spacing.y, d.icons.size()));
		for (const auto& i : d.icons)
			out(std::format(L"  [{},{} - {},{}]\n", i.rect.left, i.rect.top, i.rect.right, i.rect.bottom));
		CoUninitialize();
		return 0;
	}

	// Badges every taskbar item through an overlay owned by the taskbar, pumping messages for a while.
	int taskbar_overlay(int seconds)
	{
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
		if (!tray) return 1;

		RECT bounds{};
		GetWindowRect(tray, &bounds);
		const float scale = GetDpiForWindow(tray) / 96.0f;

		std::mt19937 rng{ std::random_device{}() };
		std::vector<int> numbers;
		dx::device_source gpu;
		overlay layer(gpu);

		const auto until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
		std::size_t shown = 0;
		while (GetTickCount64() < until) {
			const auto items = taskbar_items::read(tray);
			while (numbers.size() < items.size()) numbers.push_back(std::uniform_int_distribution<int>(1, 9)(rng));

			std::vector<badge> badges;
			for (std::size_t i = 0; i < items.size(); ++i) {
				const RECT& r = items[i].rect;
				const float icon = (items[i].right_side ? 16.0f : 24.0f) * scale;
				const float d = std::round(icon * 0.6f);
				const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
				badges.push_back({ cx + icon / 2 - d / 4, cy - icon / 2 + d / 4, d, numbers[i] });
			}
			layer.show(tray, bounds, badges);
			shown = badges.size();

			MSG msg{};
			const auto next = GetTickCount64() + 300;
			while (GetTickCount64() < next) {
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
				Sleep(10);
			}
		}
		out(std::format(L"badged {} taskbar items\n", shown));
		return 0;
	}

	// Badges every Start menu item through an overlay owned by the Start window.
	int start_overlay(int seconds)
	{
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		const HWND start = start_items::window();
		if (!start) { out(L"Start window not found\n"); return 1; }

		dx::device_source gpu;
		overlay layer(gpu);
		const float scale = GetDpiForWindow(start) / 96.0f;

		const auto until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
		std::size_t most = 0;
		while (GetTickCount64() < until) {
			if (!start_items::open(start)) {
				layer.hide();
			}
			else {
				RECT bounds{};
				GetWindowRect(start, &bounds);
				std::vector<badge> badges;
				int n = 0;
				for (const RECT& r : start_items::read(start)) {
					const float d = std::round(18.0f * scale);
					badges.push_back({ r.right - d * 0.8f, r.top + d * 0.8f, d, n++ % 9 + 1 });
				}
				most = std::max(most, badges.size());
				layer.show(start, bounds, badges);
			}

			MSG msg{};
			const auto next = GetTickCount64() + 300;
			while (GetTickCount64() < next) {
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
				Sleep(10);
			}
		}
		out(std::format(L"most badges at once: {}\n", most));
		return 0;
	}

	// Badges every item of whatever window is in front, through an overlay owned by that window.
	int window_overlay(int seconds)
	{
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		dx::device_source gpu;
		overlay layer(gpu);

		const auto until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
		std::size_t most = 0;
		ULONGLONG slowest = 0;
		while (GetTickCount64() < until) {
			const HWND front = GetForegroundWindow();
			if (!front || IsIconic(front)) {
				layer.hide();
			}
			else {
				const auto began = GetTickCount64();
				const auto items = window_items_exp::read(front, 400);
				slowest = std::max(slowest, GetTickCount64() - began);

				RECT bounds{};
				GetWindowRect(front, &bounds);
				const float scale = GetDpiForWindow(front) / 96.0f;
				std::vector<badge> badges;
				int n = 0;
				for (const RECT& r : items) {
					const float d = std::round(std::min(14.0f * scale, (r.bottom - r.top) * 0.7f));
					badges.push_back({ r.right - d * 0.6f, r.top + d * 0.6f, d, n++ % 9 + 1 });
				}
				most = std::max(most, badges.size());
				if (badges.empty()) layer.hide();
				else                layer.show(front, bounds, badges);
			}

			MSG msg{};
			const auto next = GetTickCount64() + 400;
			while (GetTickCount64() < next) {
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
				Sleep(10);
			}
		}
		out(std::format(L"most badges at once: {}, slowest query: {} ms\n", most, slowest));
		return 0;
	}

	// Times core's window_items::query on one window.
	int bench_query(HWND target, int runs)
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		window_items items;
		ULONGLONG total = 0, worst = 0;
		std::size_t count = 0;
		for (int i = 0; i < runs; ++i) {
			LARGE_INTEGER f{}, a{}, b{};
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&a);
			const auto snap = items.query(target);
			QueryPerformanceCounter(&b);
			const ULONGLONG ms = static_cast<ULONGLONG>((b.QuadPart - a.QuadPart) * 1000 / f.QuadPart);
			total += ms;
			worst = std::max(worst, ms);
			count = snap ? snap->items.size() : 0;
		}
		out(std::format(L"query: avg {} ms, worst {} ms, {} items\n", total / runs, worst, count));
		items.disconnect();
		return 0;
	}

	// Creates the GPU objects an overlay uses and then only pumps messages, to see what idling costs.
	int idle_device(int seconds, bool with_layer, int size)
	{
		dx::device_source gpu;
		overlay layer(gpu);
		if (with_layer) {
			const RECT area{ 0, 0, size, size * 10 / 16 };
			layer.show(nullptr, area, { badge{ 32, 32, 24, 1 } });
		}
		else {
			gpu.get();
		}

		const auto until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
		MSG msg{};
		while (GetTickCount64() < until) {
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
		}
		return 0;
	}

	// Runs only the window stack query at the follow rate, to see what it costs on its own.
	int stack_loop(int seconds, bool with_layer)
	{
		dx::device_source gpu;
		overlay layer(gpu);
		if (with_layer) {
			const RECT area{ 0, 0, 64, 64 };
			layer.show(nullptr, area, { badge{ 32, 32, 24, 1 } });
		}

		window_stack stack;
		std::size_t windows = 0;
		const auto until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
		MSG msg{};
		while (GetTickCount64() < until) {
			windows = stack.front_to_back().size();
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLINPUT);
		}
		out(std::format(L"windows on screen: {}\n", windows));
		return 0;
	}

	int clear()
	{
		taskbar_list tb;
		if (!tb.p) return 1;
		for (const auto& w : taskbar::enumerate()) tb.p->SetOverlayIcon(w.hwnd, nullptr, nullptr);
		out(L"cleared\n");
		return 0;
	}

}

int wmain(int argc, wchar_t** argv)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	lib::log_to_console(true);

	const std::vector<std::wstring> args(argv + 1, argv + argc);
	const auto has = [&](const wchar_t* f) { return std::find(args.begin(), args.end(), f) != args.end(); };
	const auto value = [&](const wchar_t* f, const wchar_t* def) -> std::wstring {
		for (std::size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == f) return args[i + 1];
		return def;
	};

	if (has(L"--list"))    return list();
	if (has(L"--preview")) {
		const int size = std::stoi(value(L"--size", L"64"));
		return badge_icon::save_preview(value(L"--preview", L"badges.bmp"), size) ? 0 : 1;
	}
	if (has(L"--apply"))   return apply(std::stoi(value(L"--apply", L"5")), has(L"--keep"));
	if (has(L"--clear"))   return clear();
	if (has(L"--desktop")) return desktop_icons();
	if (has(L"--watch-scroll")) return query_variants::watch_scroll(reinterpret_cast<HWND>(std::stoull(value(L"--watch-scroll", L"0"))), std::stoi(value(L"--runs", L"100")), std::stoi(value(L"--gap", L"0")));
	if (has(L"--watch-query")) return query_variants::watch(reinterpret_cast<HWND>(std::stoull(value(L"--watch-query", L"0"))), std::stoi(value(L"--runs", L"100")), std::stoi(value(L"--gap", L"0")), has(L"--hit"));
	if (has(L"--query-variants")) return query_variants::run(reinterpret_cast<HWND>(std::stoull(value(L"--query-variants", L"0"))), std::stoi(value(L"--runs", L"10")));
	if (has(L"--scene-test")) return scene_test::run(value(L"--scene-test", L""));
	if (has(L"--focus-window")) return drag_window::take_focus(reinterpret_cast<HWND>(std::stoull(value(L"--focus-window", L"0")))) ? 0 : 1;
	if (has(L"--drag-window")) return drag_window::run(std::stoi(value(L"--drag-window", L"20")), std::stoi(value(L"--buttons", L"100")),
		has(L"--still") ? drag_window::motion::still : has(L"--scroll") ? drag_window::motion::scroll : drag_window::motion::move,
		std::stoi(value(L"--hold", L"0")), has(L"--focus"));
	if (has(L"--event-census")) return event_census::run(std::stoi(value(L"--event-census", L"20")), std::stoul(value(L"--pid", L"0")));
	if (has(L"--stack-loop")) return stack_loop(std::stoi(value(L"--stack-loop", L"20")), has(L"--with-layer"));
	if (has(L"--idle-device")) return idle_device(std::stoi(value(L"--idle-device", L"20")), has(L"--with-layer"), std::stoi(value(L"--size", L"64")));
	if (has(L"--bench-query")) return bench_query(reinterpret_cast<HWND>(std::stoull(value(L"--bench-query", L"0"))), std::stoi(value(L"--runs", L"10")));
	if (has(L"--window-overlay")) return window_overlay(std::stoi(value(L"--window-overlay", L"10")));
	if (has(L"--start-overlay")) return start_overlay(std::stoi(value(L"--start-overlay", L"10")));
	if (has(L"--taskbar-overlay")) return taskbar_overlay(std::stoi(value(L"--taskbar-overlay", L"10")));

	out(L"usage:\n"
		L"  exp.exe --list                          list windows that own a taskbar button\n"
		L"  exp.exe --preview <file.bmp> [--size N] write badges 1..9 side by side\n"
		L"  exp.exe --apply <seconds> [--keep]      badge every button, then remove unless --keep\n"
		L"  exp.exe --clear                         remove badges from every button\n"
		L"  exp.exe --desktop                       list desktop icon rectangles\n"
		L"  exp.exe --taskbar-overlay <seconds>    badge every taskbar item with an overlay owned by the taskbar\n"
		L"  exp.exe --event-census <seconds>       count the WinEvents notifyall listens to, by kind and process\n"
		L"  exp.exe --drag-window <seconds> [--buttons N] [--hold S] [--still | --scroll] [--focus]\n"
		L"                                          a topmost window of buttons that circles, sits or scrolls\n"
		L"  exp.exe --query-variants <hwnd> [--runs N] time ways of asking UI Automation for a window's items\n"
		L"  exp.exe --scene-test [<shot>]           check badge drawing against the screen; saves <shot>-N.bmp\n");
	return 2;
}
