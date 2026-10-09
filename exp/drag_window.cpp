#include "drag_window.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>

namespace drag_window {

	namespace {

		constexpr wchar_t class_name[] = L"notifyall.exp.drag_window";
		constexpr UINT_PTR move_timer = 1;
		constexpr int columns = 10;

		struct state {
			POINT home{};
			int radius = 0;
			int step = 0;
			motion how = motion::move;
			int row = 0;   // one row of buttons, for sliding them
			ULONGLONG start = 0;
			ULONGLONG until = 0;
		};

		LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			auto* s = reinterpret_cast<state*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			switch (msg) {
			case WM_TIMER:
				if (!s) break;
				if (GetTickCount64() >= s->until) {
					DestroyWindow(hwnd);
					return 0;
				}
				if (GetTickCount64() < s->start) return 0;
				if (s->how == motion::move) {
					// One lap every two seconds at the usual 60 moves a second.
					const double a = ++s->step * 3.14159265358979 / 60.0;
					SetWindowPos(hwnd, nullptr, s->home.x + static_cast<int>(s->radius * std::cos(a)),
						s->home.y + static_cast<int>(s->radius * std::sin(a)), 0, 0,
						SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				}
				else if (s->how == motion::scroll) {
					// Like a list scrolling by: everything slides up a pixel at a time, then jumps back.
					const int unit = std::max(1, s->row / 30);
					const int dy = (++s->step % 30 == 0) ? -29 * unit : unit;
					ScrollWindowEx(hwnd, 0, -dy, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
				}
				return 0;
			case WM_DESTROY:
				PostQuitMessage(0);
				return 0;
			}
			return DefWindowProcW(hwnd, msg, wp, lp);
		}

	}

	// Only a process that received the last input may take the foreground, hence the click.
	bool take_focus(HWND hwnd)
	{
		POINT before{};
		GetCursorPos(&before);
		POINT at{ 1, 1 };   // just inside the corner, clear of any control
		ClientToScreen(hwnd, &at);
		if (GetAncestor(WindowFromPoint(at), GA_ROOT) != hwnd) return false;   // something else is in the way

		SetCursorPos(at.x, at.y);
		INPUT click[2]{};
		click[0].type = click[1].type = INPUT_MOUSE;
		click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
		click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
		SendInput(2, click, sizeof(INPUT));

		MSG msg{};
		for (int i = 0; i < 20 && GetForegroundWindow() != hwnd; ++i) {
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
			Sleep(10);
		}
		SetCursorPos(before.x, before.y);
		return GetForegroundWindow() == hwnd;
	}

	int run(int seconds, int buttons, motion how, int hold, bool focus)
	{
		const HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc{ sizeof(wc) };
		wc.lpfnWndProc = proc;
		wc.hInstance = instance;
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		wc.lpszClassName = class_name;
		RegisterClassExW(&wc);

		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const UINT dpi = GetDpiForSystem();
		const int bw = MulDiv(64, dpi, 96), bh = MulDiv(28, dpi, 96), gap = MulDiv(4, dpi, 96);
		const int rows = (buttons + columns - 1) / columns;

		RECT frame{ 0, 0, columns * (bw + gap) + gap, rows * (bh + gap) + gap };
		AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOPMOST, dpi);
		const int w = frame.right - frame.left, h = frame.bottom - frame.top;

		state s;
		s.radius = MulDiv(120, dpi, 96);
		s.home = { work.left + s.radius + MulDiv(40, dpi, 96), work.top + s.radius + MulDiv(40, dpi, 96) };
		s.how = how;
		s.row = bh + gap;
		s.start = GetTickCount64() + static_cast<ULONGLONG>(hold) * 1000;
		s.until = s.start + static_cast<ULONGLONG>(seconds) * 1000;

		const HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | (focus ? 0 : WS_EX_NOACTIVATE), class_name, L"notifyall drag test",
			WS_OVERLAPPEDWINDOW, s.home.x, s.home.y, w, h, nullptr, nullptr, instance, nullptr);
		if (!hwnd) return 1;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));

		for (int i = 0; i < buttons; ++i) {
			CreateWindowExW(0, L"BUTTON", std::to_wstring(i + 1).c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				gap + (i % columns) * (bw + gap), gap + (i / columns) * (bh + gap), bw, bh,
				hwnd, nullptr, instance, nullptr);
		}

		// The launcher's show state may override the first call.
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		const bool front = focus && take_focus(hwnd);
		SetTimer(hwnd, move_timer, 16, nullptr);
		std::printf("%s", std::format("drag window {} ({} buttons, {}{}) for {} s\n",
			reinterpret_cast<std::uintptr_t>(hwnd), buttons,
			how == motion::move ? "moving" : how == motion::scroll ? "scrolling" : "still",
			front ? ", in front" : "", seconds).c_str());
		std::fflush(stdout);

		MSG msg{};
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		return 0;
	}

}
