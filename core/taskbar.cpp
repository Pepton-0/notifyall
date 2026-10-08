#include "taskbar.h"

#include <dwmapi.h>

#include "logger.h"

#pragma comment(lib, "dwmapi.lib")

namespace {

	// Same rules the taskbar uses, judged from window styles and DWM state only.
	bool has_taskbar_button(HWND hwnd)
	{
		if (!IsWindowVisible(hwnd)) return false;

		const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		if (ex & WS_EX_TOOLWINDOW) return false;

		// Owned windows get a button only with WS_EX_APPWINDOW.
		if (GetWindow(hwnd, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;

		// Cloaked: on another virtual desktop, or a suspended UWP app.
		DWORD cloaked = 0;
		if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;

		return GetWindowTextLengthW(hwnd) > 0;
	}

	BOOL CALLBACK collect(HWND hwnd, LPARAM lp)
	{
		if (has_taskbar_button(hwnd)) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(hwnd);
		return TRUE;
	}

}

std::vector<HWND> taskbar::windows()
{
	std::vector<HWND> out;
	EnumWindows(&collect, reinterpret_cast<LPARAM>(&out));
	return out;
}

int taskbar::overlay_size()
{
	const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
	return GetSystemMetricsForDpi(SM_CXSMICON, tray ? GetDpiForWindow(tray) : USER_DEFAULT_SCREEN_DPI);
}

bool taskbar::set_overlay(HWND window, HICON icon)
{
	ITaskbarList3* l = list();
	return l && SUCCEEDED(l->SetOverlayIcon(window, icon, icon ? L"notification" : nullptr));
}

ITaskbarList3* taskbar::list()
{
	if (taskbarList || unavailable) return taskbarList.Get();

	unavailable = true;
	if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(taskbarList.GetAddressOf())))
		|| FAILED(taskbarList->HrInit())) {
		taskbarList.Reset();
		lib::log("taskbar: ITaskbarList3 is not available", true);
		return nullptr;
	}
	unavailable = false;
	return taskbarList.Get();
}
