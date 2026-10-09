#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "drift.h"
#include "i_injection.h"
#include "overlay.h"
#include "window_changes.h"
#include "window_items.h"
#include "window_stack.h"
#include "poller.h"
#include "timer.h"

// Badges the buttons, tabs, list and tree items of every visible app window, kept relative to each frame and
// clipped to its visible parts; moving and covering windows is cheap, UI Automation only refreshes layouts.
class window_item_injection final : public i_injection {
public:
	using clock = std::chrono::steady_clock;

	static constexpr std::chrono::milliseconds query_interval{ 100 };     // the background worker wakes up this often
	static constexpr std::chrono::milliseconds follow_interval{ 16 };     // UI thread checks for stacking changes
	static constexpr std::chrono::milliseconds stack_fallback{ 500 };     // re-read stacking without any event
	static constexpr std::chrono::milliseconds frame_quiet{ 200 };        // a moving window is queried once it stops
	static constexpr std::chrono::milliseconds front_refresh{ 1000 };     // the focused window, when all is quiet
	static constexpr std::chrono::milliseconds front_settle{ 1000 };      // and how long "lately changed" lasts
	static constexpr std::chrono::milliseconds probe_quiet{ 100 };        // checks on one element while all is quiet
	static constexpr std::chrono::milliseconds display_lead{ 16 };        // moving badges show where they will be once on screen
	static constexpr std::chrono::milliseconds back_refresh{ 3000 };
	static constexpr std::chrono::milliseconds dim_refresh{ 10000 };      // for windows mostly covered
	static constexpr float dim_fraction = 0.1f;
	static constexpr std::size_t queries_per_round = 3;
	static constexpr int cost_factor = 3;   // a background window waits this many times its last query time
	static constexpr int max_backoff = 8;   // waits stretch up to this many times while nothing changes

	// A badge measured from the nearer edges of its window, so a resize moves it along with its element.
	struct anchored_badge {
		badge offset;              // x from the left or right edge, y from the top or bottom edge
		bool from_right = false;
		bool from_bottom = false;
		std::size_t key = 0;       // the element it marks, the same across layouts

		bool operator==(const anchored_badge&) const = default;
	};

	struct window_badges {
		HWND window = nullptr;
		SIZE size{};                          // frame size the badges were measured at
		std::vector<anchored_badge> badges;
		clock::time_point at{};               // when this layout most likely came about
	};

	struct query_result {
		std::optional<std::vector<HWND>> candidates;   // every window worth badging, if known; others are dropped
		std::vector<window_badges> updated;            // only windows whose badges changed
	};

	window_item_injection(i_window_items& items, i_window_stack& stack, i_window_changes& changes,
	                      std::unique_ptr<i_overlay> layer);

	void on() override;
	void off() override;

	// Worker side. The focused window has a worker of its own, so a slow window elsewhere never holds it up.
	query_result query_front();   // asks again at once while its layout keeps changing
	query_result query_round();   // the others: changed ones first, then whatever is overdue; hidden ones wait

	// A round the UI thread hasn't taken yet still counts: the fresh one is folded into it.
	static void merge(query_result& unread, query_result fresh);

	// UI side.
	void store(const query_result& result);
	void follow();
	void changed(HWND window, window_change kind);

	// Top-right of compact items; near the left end of wide rows, where their icon is.
	static badge place(const RECT& item, UINT dpi, int number);

	// Doubles while queries find nothing new, so windows that report changes nonstop
	// without changing (some fire hundreds a second) are asked less and less.
	static int next_backoff(int current, bool changed);

	// How long after its last query began the focused window is asked again. Lively (changed lately, or said
	// to have changed): back to back, stretching while nothing new turns up. Quiet: now and then.
	static clock::duration front_gap(clock::duration cost, bool lively, int backoff);

	// Measure from whichever edges the item sits closer to.
	static anchored_badge anchor(const badge& b, SIZE frame);
	static badge resolve(const anchored_badge& a, const RECT& frame);

	// Where each badge of a layout sits, for telling how its content moves from one layout to the next.
	static std::vector<drift::spot> spots(const std::vector<anchored_badge>& badges, SIZE size);

private:
	struct layout {
		SIZE size{};
		std::vector<anchored_badge> badges;
	};

	// A window's badges as the UI thread shows them, and how its content keeps moving meanwhile.
	struct shown {
		layout current;
		clock::time_point at{};
		drift motion;
	};

	// What the workers know about one window.
	struct history {
		clock::time_point asked{};   // when its last query began
		clock::duration cost{};
		int backoff = 1;
		clock::time_point changedAt{};   // when a query last found a new layout
		bool busy = false;           // a worker is asking it right now
		layout seen;                 // what the UI thread was handed
	};

	std::vector<HWND> pick(const std::vector<HWND>& others);
	std::optional<window_badges> ask(HWND window, bool watch = false);
	int number_for(const std::wstring& id);
	void read_screen(bool whole, clock::time_point now);
	bool refresh_moved();
	void publish_visibility(const std::vector<stacked_window>& windows,
	                        const std::unordered_map<HWND, std::vector<RECT>>& visible);
	float visible_fraction(HWND window);

	i_window_items& items;
	i_window_stack& stack;
	i_window_changes& changes;
	std::unique_ptr<i_overlay> layer;
	evt::subscription<HWND, window_change> changeSub;
	lib::poller<query_result> frontQuerier;
	lib::poller<query_result> querier;
	lib::timer follower;

	std::unordered_map<HWND, shown> layouts;    // UI thread only
	std::vector<stacked_window> lastStack;      // UI thread only
	std::unordered_map<HWND, std::vector<RECT>> lastVisible;   // UI thread only
	std::unordered_set<HWND> moved;             // UI thread only: frames to read again
	bool stackDirty = true;                     // UI thread only
	bool layoutsChanged = false;                // UI thread only
	bool drifted = false;                       // UI thread only: some badges were shown moving on their own
	clock::time_point lastFollow{};             // UI thread only

	// Lock dirtyLock before stateLock when both are needed.
	std::mutex dirtyLock;
	std::unordered_set<HWND> dirty;   // windows whose content changed; marked by the UI thread, drained by the workers
	std::unordered_map<HWND, clock::time_point> lastMoved;   // frame changes, under the same lock

	std::mutex visibilityLock;
	std::unordered_map<HWND, float> visibleFraction;   // share of each frame on screen; written by follow()

	std::mutex stateLock;
	std::unordered_map<HWND, history> histories;
	std::unordered_map<std::wstring, int> numbers;   // an element keeps its number while it lives
	std::mt19937 rng{ std::random_device{}() };

	HWND lastFront = nullptr;                    // focused window's worker only
	int frontBackoff = 1;                        // focused window's worker only
	clock::duration frontWait{};                 // focused window's worker only: how long it sleeps next
	std::size_t turn = 0;                        // background worker only
};
