#include "taskbar.h"

#include "app_windows.h"
#include "logger.h"

std::vector<HWND> taskbar::windows()
{
	return app_windows::list();
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

void taskbar::disconnect()
{
	taskbarList.Reset();
	unavailable = false;
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
