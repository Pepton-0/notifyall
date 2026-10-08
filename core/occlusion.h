#pragma once
// Which parts of each window are not covered by the windows in front of it. Pure rectangle math.

#include <unordered_map>
#include <vector>

#include "i_window_stack.h"

namespace occlusion {

	// from minus cut, as at most four rectangles.
	std::vector<RECT> subtract(const RECT& from, const RECT& cut);

	// Visible parts of every window in the stack (front to back). Fully covered windows map to nothing.
	std::unordered_map<HWND, std::vector<RECT>> visible_parts(const std::vector<stacked_window>& stack);

}
