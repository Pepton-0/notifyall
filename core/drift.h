#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <vector>

#include <windows.h>

// Content that keeps scrolling between layouts: when consecutive layouts agree that most badges moved by the same
// step at a steady pace, those badges carry on at that pace until the next layout is due.
struct drift {
	using clock = std::chrono::steady_clock;

	// Where one element's badge sits in a layout.
	struct spot {
		std::size_t key = 0;   // the element, across layouts
		POINT at{};            // relative to the window
	};

	static constexpr int agreeing_steps = 2;        // steps in a row at one pace before badges move on their own
	static constexpr float pace_tolerance = 0.35f;  // how far two paces may differ and still agree
	static constexpr float slowest = 20.0f;         // pixels a second; anything slower is standing still

	float vx = 0, vy = 0;                    // pixels per second
	clock::duration interval{};              // between layouts, lately
	int streak = 0;                          // steps in a row at about this pace
	std::unordered_set<std::size_t> keys;    // badges that move along
	RECT region{};                           // where they may show

	// The drift after a new layout, from the one before. Both layouts are of the same window size;
	// margin widens the region where moving badges were seen by about a badge.
	static drift track(const drift& before, const std::vector<spot>& older, clock::time_point olderAt,
	                   const std::vector<spot>& newer, clock::time_point newerAt, int margin);

	// How far moving badges have gone since the newer layout was taken. Nothing until the pace is settled,
	// and nothing once the next layout is overdue: then it has stopped, and the last layout stands.
	std::optional<POINT> offset(clock::duration since) const;

	bool moving() const { return streak >= agreeing_steps; }
};
