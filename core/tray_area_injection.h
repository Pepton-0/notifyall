#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

#include "change_pacer.h"
#include "i_injection.h"
#include "overlay.h"
#include "taskbar_items.h"
#include "window_changes.h"
#include "poller.h"

// Badges the taskbar's right-hand area (notification icons, IME, clock, ...) through an overlay owned
// by the taskbar, so it rises with the taskbar when Start or a flyout opens.
class tray_area_injection final : public i_injection {
public:
	static constexpr std::chrono::milliseconds tick{ 300 };           // worker wakes up this often
	static constexpr std::chrono::milliseconds busy_period{ 2000 };   // query every tick this long after a change
	static constexpr std::chrono::milliseconds fallback{ 5000 };      // otherwise this rarely

	tray_area_injection(i_taskbar_items& items, i_window_changes& changes, std::unique_ptr<i_overlay> layer);

	// Queries run on a worker thread: explorer may be waiting on our UI thread, which owns the overlay.
	void on() override;
	void off() override;

	// One synchronous round of compute + apply, due or not.
	void refresh() { apply(compute(true)); }

	// Badges where the taskbar is actually visible (not covered, not auto-hidden); nothing if not due yet.
	std::optional<overlay_frame> compute(bool force = false);
	void apply(const std::optional<overlay_frame>& frame);

	// What the event hook calls when the taskbar reports a change.
	void taskbar_changed() { pacer.poke(); }

	// Top-right of the 16px (dpi-scaled) icon; the clock gets it at its own top-right corner.
	static badge place(const RECT& item, UINT dpi, int number, bool clock);

private:
	int number_for(const std::wstring& id);

	// Buttons briefly report odd bounds while animating (the chevron grows from 48 to 78px),
	// so a new position counts only once two polls in a row agree on it.
	RECT settled_rect(const tray_item& item);

	i_taskbar_items& items;
	i_window_changes& changes;
	std::unique_ptr<i_overlay> layer;
	evt::subscription<HWND, window_change> changeSub;
	lib::change_pacer pacer{ busy_period, fallback };
	lib::poller<std::optional<overlay_frame>> refresher;

	struct seen_rect { RECT settled; RECT last; };

	std::unordered_map<std::wstring, int> numbers;   // used by compute() only
	std::unordered_map<std::wstring, seen_rect> rects;   // used by compute() only
	std::mt19937 rng{ std::random_device{}() };
};
