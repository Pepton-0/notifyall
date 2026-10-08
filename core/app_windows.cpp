#include "app_windows.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace {

	bool is_app_window(HWND hwnd)
	{
		if (!IsWindowVisible(hwnd)) return false;

		const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		if (ex & WS_EX_TOOLWINDOW) return false;

		// Owned windows get a button only with WS_EX_APPWINDOW.
		if (GetWindow(hwnd, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;

		if (app_windows::cloaked(hwnd)) return false;
		return GetWindowTextLengthW(hwnd) > 0;
	}

	BOOL CALLBACK collect(HWND hwnd, LPARAM lp)
	{
		if (is_app_window(hwnd)) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(hwnd);
		return TRUE;
	}

}

namespace app_windows {

	std::vector<HWND> list()
	{
		std::vector<HWND> out;
		EnumWindows(&collect, reinterpret_cast<LPARAM>(&out));
		return out;
	}

	bool elevated(HWND window)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(window, &pid);

		const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process) return true;

		HANDLE token = nullptr;
		TOKEN_ELEVATION elevation{};
		DWORD size = 0;
		const bool known = OpenProcessToken(process, TOKEN_QUERY, &token)
			&& GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
		if (token) CloseHandle(token);
		CloseHandle(process);
		return !known || elevation.TokenIsElevated;
	}

	bool cloaked(HWND window)
	{
		DWORD value = 0;
		return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &value, sizeof(value))) && value;
	}

}
