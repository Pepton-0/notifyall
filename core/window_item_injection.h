#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "i_injection.h"
#include "i_overlay.h"
#include "i_window_changes.h"
#include "i_window_items.h"
#include "i_window_stack.h"
#include "poller.h"
#include "timer.h"

// Badges the buttons, tabs, list and tree items of every visible app window, kept relative to each frame and
// clipped to its visible parts; moving and covering windows is cheap, UI Automation only refreshes layouts.
class window_item_injection final : public i_injection {
public:
	static constexpr std::chrono::milliseconds query_interval{ 100 };     // worker wakes up this often
	static constexpr std::chrono::milliseconds follow_interval{ 16 };     // UI thread: positions and stacking
	static constexpr std::chrono::milliseconds front_refresh{ 1000 };     // re-query without a change event
	static constexpr std::chrono::milliseconds back_refresh{ 3000 };
	static constexpr std::size_t queries_per_round = 3;
	static constexpr int cost_factor = 3;   // a window waits this many times its last query time before the next

	// A badge measured from the nearer edges of its window, so a resize moves it along with its element.
	struct anchored_badge {
		badge offset;              // x from the left or right edge, y from the top or bottom edge
		bool from_right = false;
		bool from_bottom = false;

		bool operator==(const anchored_badge&) const = default;
	};

	struct window_badges {
		HWND window = nullptr;
		SIZE size{};                          // frame size the badges were measured at
		std::vector<anchored_badge> badges;
	};

	struct query_result {
		std::vector<HWND> candidates;        // windows still worth badging; layouts of the others are dropped
		std::vector<window_badges> updated;
	};

	window_item_injection(i_window_items& items, i_window_stack& stack, i_window_changes& changes,
	                      std::unique_ptr<i_overlay> layer);

	void on() override;
	void off() override;

	// Worker side: changed windows first, then whatever is overdue for a refresh.
	query_result query_round();

	// UI side.
	void store(const query_result& result);
	void follow();
	void changed(HWND window);

	// Top-right of compact items; near the left end of wide rows, where their icon is.
	static badge place(const RECT& item, UINT dpi, int number);

	// Measure from whichever edges the item sits closer to.
	static anchored_badge anchor(const badge& b, SIZE frame);
	static badge resolve(const anchored_badge& a, const RECT& frame);

private:
	using clock = std::chrono::steady_clock;

	int number_for(const std::wstring& id);
	std::vector<HWND> pick(const std::vector<HWND>& candidates);

	i_window_items& items;
	i_window_stack& stack;
	i_window_changes& changes;
	std::unique_ptr<i_overlay> layer;
	lib::poller<query_result> querier;
	lib::timer follower;

	struct layout {
		SIZE size{};
		std::vector<anchored_badge> badges;
	};
	std::unordered_map<HWND, layout> layouts;   // UI thread only

	std::mutex dirtyLock;
	std::unordered_set<HWND> dirty;   // changed or resized windows; written by the UI thread, drained by the worker

	std::unordered_map<HWND, clock::time_point> lastQuery;   // worker only
	std::unordered_map<HWND, clock::duration> lastCost;      // worker only
	std::unordered_map<std::wstring, int> numbers;          // worker only
	std::size_t turn = 0;                                   // worker only
	std::mt19937 rng{ std::random_device{}() };             // worker only
};
