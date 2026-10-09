#pragma once

#include <optional>
#include <vector>
#include <windows.h>

// An on-screen top-level window, as far as stacking is concerned.
struct stacked_window {
	HWND hwnd = nullptr;
	RECT frame{};            // what is actually drawn: DWM's bounds, without invisible resize borders
	bool occludes = true;    // false for click-through or fully transparent windows
};

// The windows on screen, front to back. Cheap enough to call many times a second.
class i_window_stack {
public:
	virtual ~i_window_stack() = default;

	// Visible, not minimized, not cloaked, and not notifyall's own; frontmost first.
	virtual std::vector<stacked_window> front_to_back() = 0;

	// Just one window's frame, while it is still shown and not minimized.
	virtual std::optional<RECT> frame_of(HWND window) = 0;

	// The whole virtual screen, all monitors included.
	virtual RECT screen() = 0;
};

class window_stack final : public i_window_stack {
public:
	std::vector<stacked_window> front_to_back() override;
	std::optional<RECT> frame_of(HWND window) override;
	RECT screen() override;
};
