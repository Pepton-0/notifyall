#include "taskbar_windows.h"

#include <dwmapi.h>

namespace {

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

	std::wstring exe_of(DWORD pid)
	{
		const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!proc) return L"?";

		wchar_t path[MAX_PATH]{};
		DWORD len = MAX_PATH;
		std::wstring name = L"?";
		if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
			name.assign(path, len);
			if (const auto pos = name.find_last_of(L"\\/"); pos != std::wstring::npos) name = name.substr(pos + 1);
		}
		CloseHandle(proc);
		return name;
	}

	BOOL CALLBACK collect(HWND hwnd, LPARAM lp)
	{
		if (!has_taskbar_button(hwnd)) return TRUE;

		taskbar::window w{ hwnd };
		GetWindowThreadProcessId(hwnd, &w.pid);

		wchar_t cls[256]{};
		w.class_name = std::wstring(cls, static_cast<std::size_t>(GetClassNameW(hwnd, cls, 256)));
		w.exe = exe_of(w.pid);

		reinterpret_cast<std::vector<taskbar::window>*>(lp)->push_back(std::move(w));
		return TRUE;
	}

}

namespace taskbar {

	std::vector<window> enumerate()
	{
		std::vector<window> out;
		EnumWindows(&collect, reinterpret_cast<LPARAM>(&out));
		return out;
	}

}
