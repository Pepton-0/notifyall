#pragma once
// Desktop icon positions through the shell's own IFolderView (no list view memory reads).

#include <string>
#include <vector>

#include <windows.h>

namespace desktop {

	struct icon {
		std::wstring name;
		RECT rect{};   // the icon image, screen coordinates
	};

	struct layout {
		HWND list_view = nullptr;
		int icon_size = 0;
		POINT spacing{};
		std::vector<icon> icons;
	};

	// Needs COM on the calling thread. Empty icons when the desktop isn't reachable.
	layout read();

}
