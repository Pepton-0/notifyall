#include "app_windows.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace {

	bool is_app_window(HWND hwnd, bool include_cloaked)
	{
		if (!IsWindowVisible(hwnd)) return false;

		const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		if (ex & WS_EX_TOOLWINDOW) return false;

		// Owned windows get a button only with WS_EX_APPWINDOW.
		if (GetWindow(hwnd, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;

		if (!include_cloaked && app_windows::cloaked(hwnd)) return false;
		return GetWindowTextLengthW(hwnd) > 0;
	}

	struct collecting {
		std::vector<HWND> windows;
		bool include_cloaked = false;
	};

	BOOL CALLBACK collect(HWND hwnd, LPARAM lp)
	{
		auto& c = *reinterpret_cast<collecting*>(lp);
		if (is_app_window(hwnd, c.include_cloaked)) c.windows.push_back(hwnd);
		return TRUE;
	}

}

namespace app_windows {

	std::vector<HWND> list(bool include_cloaked)
	{
		collecting c{ {}, include_cloaked };
		EnumWindows(&collect, reinterpret_cast<LPARAM>(&c));
		return std::move(c.windows);
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
