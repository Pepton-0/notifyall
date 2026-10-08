#pragma once
// Windows that own a taskbar button, judged from window styles and DWM state only.

#include <string>
#include <vector>
#include <windows.h>

namespace taskbar {

	struct window {
		HWND hwnd;
		DWORD pid;
		std::wstring class_name;
		std::wstring exe;
	};

	std::vector<window> enumerate();

}
