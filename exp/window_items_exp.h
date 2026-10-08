#pragma once
// Clickable parts of an ordinary app window (list and tree items, tabs, buttons, menu items) through UI Automation.

#include <vector>

#include <windows.h>

namespace window_items_exp {

	// Needs COM on the calling thread. At most limit rectangles, all inside the window and on screen.
	std::vector<RECT> read(HWND window, std::size_t limit);

}
