#pragma once
// The Start menu window and its items (pinned tiles, recommendations, app list, user and power buttons).

#include <vector>

#include <windows.h>

namespace start_items {

	// The Start menu's CoreWindow; it stays alive and is cloaked while Start is closed.
	HWND window();

	bool open(HWND start);

	// Needs COM on the calling thread.
	std::vector<RECT> read(HWND start);

}
