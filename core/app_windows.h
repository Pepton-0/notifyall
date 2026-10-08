#pragma once

#include <vector>

#include <windows.h>

namespace app_windows {

	// Top-level windows that get a taskbar button and appear in Alt+Tab,
	// judged from window styles and DWM state only.
	std::vector<HWND> list();

	// The window belongs to an elevated process, or one we can't even inspect.
	bool elevated(HWND window);

	// Hidden by DWM: on another virtual desktop, a suspended UWP app, or a closed shell surface like Start.
	bool cloaked(HWND window);

}
