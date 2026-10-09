#pragma once

#include <string>

// Shows badge groups over a plain window of our own, then reads the screen back to check that badges show
// where they should, stay clipped where covered, and leave nothing behind when their group moves.
namespace scene_test {

	// Returns 0 when every probe matched; saves what the screen showed to shot (a .bmp) if given.
	int run(const std::wstring& shot);

}
