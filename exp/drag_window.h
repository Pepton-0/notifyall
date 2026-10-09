#pragma once

#include <windows.h>

// A topmost window full of buttons that circles around on its own, to load notifyall the way a drag does.
namespace drag_window {

	enum class motion { still, move, scroll };   // scroll keeps the window still and slides its buttons

	// hold keeps it still for that many seconds first, so notifyall can badge it before it moves.
	// focus brings it to the front with a click of its own, the way a user would.
	int run(int seconds, int buttons, motion how, int hold, bool focus);

	// Brings a window of the test's own to the front with a click just inside its client area.
	// Clicks only if that spot shows the window itself; the pointer goes back where it was.
	bool take_focus(HWND hwnd);

}
