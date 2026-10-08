#include "window_stack.h"

#include <dwmapi.h>

#include "app_windows.h"

namespace {

	bool see_through(HWND hwnd)
	{
		const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		if (ex & WS_EX_TRANSPARENT) return true;
		if (!(ex & WS_EX_LAYERED)) return false;

		BYTE alpha = 255;
		DWORD flags = 0;
		return GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0;
	}

	RECT frame_of(HWND hwnd)
	{
		RECT r{};
		if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
		return r;
	}

	BOOL CALLBACK collect(HWND hwnd, LPARAM lp)
	{
		if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || app_windows::cloaked(hwnd)) return TRUE;

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == GetCurrentProcessId()) return TRUE;

		stacked_window w{ hwnd, frame_of(hwnd), !see_through(hwnd) };
		if (w.frame.right > w.frame.left && w.frame.bottom > w.frame.top)
			reinterpret_cast<std::vector<stacked_window>*>(lp)->push_back(w);
		return TRUE;
	}

}

std::vector<stacked_window> window_stack::front_to_back()
{
	// EnumWindows walks top-level windows in z-order, topmost ones first.
	std::vector<stacked_window> out;
	EnumWindows(&collect, reinterpret_cast<LPARAM>(&out));
	return out;
}

RECT window_stack::screen()
{
	const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	return { x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}
