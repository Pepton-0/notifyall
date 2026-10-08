#pragma once
// Every button on the primary taskbar (apps, tray icons, IME, clock, ...) through UI Automation.

#include <vector>

#include <windows.h>

namespace taskbar_items {

	struct item {
		RECT rect{};
		bool right_side = false;   // tray area: notification icons, IME, clock, quick settings
	};

	// Needs COM on the calling thread.
	std::vector<item> read(HWND tray);

}
