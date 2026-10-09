#pragma once

#include <chrono>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "change_pacer.h"
#include "i_injection.h"
#include "window_changes.h"
#include "window_icons.h"
#include "timer.h"

// Swaps every app window's icon (title bar, Alt+Tab, Task View) for a badged copy, and puts the original back on off().
// Elevated apps are left alone.
class window_icon_injection final : public i_injection {
public:
	static constexpr std::chrono::milliseconds tick{ 250 };          // how often to check whether a look is due
	static constexpr std::chrono::milliseconds busy_period{ 1000 };  // look every tick this long after windows change
	static constexpr std::chrono::milliseconds fallback{ 3000 };     // apps switch their own icons without telling

	window_icon_injection(i_window_icons& icons, i_window_changes& changes);
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
	i_window_changes& changes;
	evt::subscription<HWND, window_change> changeSub;
	lib::change_pacer pacer{ busy_period, fallback };
	lib::timer refresher;

	std::unordered_map<HWND, entry> windows;
	std::unordered_set<HWND> skipped;   // elevated
	std::mt19937 rng{ std::random_device{}() };
};
