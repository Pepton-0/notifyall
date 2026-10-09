#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

#include "change_pacer.h"
#include "desktop.h"
#include "i_injection.h"
#include "overlay.h"
#include "window_changes.h"
#include "window_stack.h"
#include "poller.h"

// Draws a badge on every desktop icon through the overlay, which sits right above the desktop.
class desktop_injection final : public i_injection {
public:
	static constexpr std::chrono::milliseconds tick{ 300 };           // worker wakes up this often
	static constexpr std::chrono::milliseconds busy_period{ 2000 };   // query every tick this long after a change
	static constexpr std::chrono::milliseconds fallback{ 5000 };      // otherwise this rarely

	desktop_injection(i_desktop& desk, i_window_stack& stack, i_window_changes& changes, std::unique_ptr<i_overlay> layer);

	// Shell queries run on a worker thread so the UI thread never waits on explorer.
	void on() override;
	void off() override;

	// One synchronous round of compute + apply, due or not.
	void refresh() { apply(compute(true)); }

	// Follows icons being moved, added or removed; nothing if not due yet or the desktop is fully covered.
	std::optional<overlay_frame> compute(bool force = false);
	void apply(const std::optional<overlay_frame>& frame);

	// What the event hook calls when the desktop reports a change.
	void desktop_changed() { pacer.poke(); }

	// Top-right corner of the icon image, overhanging it slightly.
	static badge place(const RECT& icon, int number);

private:
	int number_for(const std::wstring& icon_id);
	bool visible(HWND host);

	i_desktop& desk;
	i_window_stack& stack;
	i_window_changes& changes;
	std::unique_ptr<i_overlay> layer;
	evt::subscription<HWND, window_change> changeSub;
	lib::change_pacer pacer{ busy_period, fallback };
	lib::poller<std::optional<overlay_frame>> refresher;

	std::unordered_map<std::wstring, int> numbers;   // stays fixed per icon
	std::mt19937 rng{ std::random_device{}() };
};
