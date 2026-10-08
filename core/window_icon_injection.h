#pragma once

#include <chrono>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "i_injection.h"
#include "i_window_icons.h"
#include "timer.h"

// Swaps every app window's icon (title bar, Alt+Tab, Task View) for a badged copy, and puts the original back on off().
// Elevated apps are left alone.
class window_icon_injection final : public i_injection {
public:
	static constexpr std::chrono::milliseconds refresh_interval{ 1000 };

	explicit window_icon_injection(i_window_icons& icons);
	~window_icon_injection() override;

	void on() override;
	void off() override;

	// Badges new windows, re-badges icons their app replaced, and forgets closed windows.
	void refresh();

	// The icon with a badge in its top-right corner, size x size. The caller destroys it.
	static HICON with_badge(HICON source, int size, int number);

	std::size_t badged() const { return windows.size(); }

private:
	struct slot {
		HICON original = nullptr;   // as WM_GETICON returned it; nullptr means "use the class icon"
		HICON badged = nullptr;     // ours, alive while the window may show it
	};

	struct entry {
		int number = 0;
		slot big;
		slot little;
	};

	void badge(HWND window, icon_kind kind, int number, slot& s);
	void restore(HWND window, icon_kind kind, slot& s);

	i_window_icons& icons;
	lib::timer refresher;

	std::unordered_map<HWND, entry> windows;
	std::unordered_set<HWND> skipped;   // elevated
	std::mt19937 rng{ std::random_device{}() };
};
