#pragma once

#include <vector>

#include <windows.h>

enum class icon_kind { big, little };

// Other apps' window icons (title bar, Alt+Tab, Task View), through WM_GETICON / WM_SETICON.
class i_window_icons {
public:
	virtual ~i_window_icons() = default;

	virtual std::vector<HWND> windows() = 0;

	// Windows of elevated processes: our messages to them are blocked, and we leave them alone anyway.
	virtual bool elevated(HWND window) = 0;

	// What WM_GETICON returns; nullptr means the window falls back to its class icon.
	virtual HICON get(HWND window, icon_kind kind) = 0;

	// What the window actually shows: its own icon, else the class icon, else the default application icon.
	virtual HICON shown(HWND window, icon_kind kind) = 0;

	// nullptr reverts the window to its class icon. False when the window is gone or hung.
	virtual bool set(HWND window, icon_kind kind, HICON icon) = 0;

	// Edge length in pixels at the window's dpi.
	virtual int size(HWND window, icon_kind kind) = 0;
};
