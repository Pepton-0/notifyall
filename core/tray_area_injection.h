#pragma once

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>

#include "i_injection.h"
#include "i_overlay.h"
#include "i_taskbar_items.h"
#include "poller.h"

// Badges the taskbar's right-hand area (notification icons, IME, clock, ...) through an overlay owned
// by the taskbar, so it rises with the taskbar when Start or a flyout opens.
class tray_area_injection final : public i_injection {
public:
	static constexpr std::chrono::milliseconds refresh_interval{ 1000 };   // the tray rarely rearranges

	tray_area_injection(i_taskbar_items& items, std::unique_ptr<i_overlay> layer);

	// Queries run on a worker thread: explorer may be waiting on our UI thread, which owns the overlay.
	void on() override;
	void off() override;

	// One synchronous round of compute + apply.
	void refresh() { apply(compute()); }

	// Badges for the items where the taskbar is actually visible (not covered, not auto-hidden).
	overlay_frame compute();
	void apply(const overlay_frame& frame);

	// Top-right of the 16px (dpi-scaled) icon; the clock gets it at its own top-right corner.
	static badge place(const RECT& item, UINT dpi, int number, bool clock);

private:
	int number_for(const std::wstring& id);

	// Buttons briefly report odd bounds while animating (the chevron grows from 48 to 78px),
	// so a new position counts only once two polls in a row agree on it.
	RECT settled_rect(const tray_item& item);

	i_taskbar_items& items;
	std::unique_ptr<i_overlay> layer;
	lib::poller<overlay_frame> refresher;

	struct seen_rect { RECT settled; RECT last; };

	std::unordered_map<std::wstring, int> numbers;   // used by compute() only
	std::unordered_map<std::wstring, seen_rect> rects;   // used by compute() only
	std::mt19937 rng{ std::random_device{}() };
};
