#pragma once

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>

#include "i_desktop.h"
#include "i_injection.h"
#include "i_overlay.h"
#include "poller.h"

// Draws a badge on every desktop icon through the overlay, which sits right above the desktop.
class desktop_injection final : public i_injection {
public:
	static constexpr std::chrono::milliseconds refresh_interval{ 500 };

	desktop_injection(i_desktop& desk, std::unique_ptr<i_overlay> layer);

	// Shell queries run on a worker thread so the UI thread never waits on explorer.
	void on() override;
	void off() override;

	// One synchronous round of compute + apply.
	void refresh() { apply(compute()); }

	// Follows icons being moved, added or removed.
	overlay_frame compute();
	void apply(const overlay_frame& frame);

	// Top-right corner of the icon image, overhanging it slightly.
	static badge place(const RECT& icon, int number);

private:
	int number_for(const std::wstring& icon_id);

	i_desktop& desk;
	std::unique_ptr<i_overlay> layer;
	lib::poller<overlay_frame> refresher;

	std::unordered_map<std::wstring, int> numbers;   // used by compute() only
	std::mt19937 rng{ std::random_device{}() };
};
