// Verifies badging other apps' taskbar buttons via ITaskbarList3::SetOverlayIcon.
// Run without arguments for usage.

#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <format>
#include <random>
#include <string>
#include <vector>

#include "badge.h"
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
		for (int n = 1; n <= 9; ++n) icons.push_back(badge::make(n, size));
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
		return badge::save_preview(value(L"--preview", L"badges.bmp"), size) ? 0 : 1;
	}
	if (has(L"--apply"))   return apply(std::stoi(value(L"--apply", L"5")), has(L"--keep"));
	if (has(L"--clear"))   return clear();

	out(L"usage:\n"
		L"  exp.exe --list                          list windows that own a taskbar button\n"
		L"  exp.exe --preview <file.bmp> [--size N] write badges 1..9 side by side\n"
		L"  exp.exe --apply <seconds> [--keep]      badge every button, then remove unless --keep\n"
		L"  exp.exe --clear                         remove badges from every button\n");
	return 2;
}
