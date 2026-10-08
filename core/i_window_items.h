#pragma once

#include <optional>
#include <string>
#include <vector>

#include <windows.h>

// A clickable part of an app window: list or tree item, tab, button, menu item.
struct window_item {
	std::wstring id;   // UI Automation runtime id, stable while the element lives
	RECT rect{};       // screen coordinates at the time of the query
};

struct window_snapshot {
	HWND window = nullptr;
	RECT frame{};      // the window's frame at the time of the query, same coordinates as the items
	UINT dpi = 96;
	bool settled = true;   // false if the window moved or resized while being queried
	std::vector<window_item> items;
};

// Parts of ordinary app windows, as UI Automation exposes them. Slow: call it off the UI thread.
class i_window_items {
public:
	virtual ~i_window_items() = default;

	// Windows worth badging, the one in front first. Skips the shell, notifyall, elevated and hidden windows.
	virtual std::vector<HWND> candidates() = 0;

	virtual std::optional<window_snapshot> query(HWND window) = 0;

	// Releases the COM objects query() created; call it on the thread that called query().
	virtual void disconnect() = 0;
};
