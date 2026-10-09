#pragma once

#include <optional>
#include <string>
#include <vector>
#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <mutex>
#include <unordered_map>

// A clickable part of an app window: list or tree item, tab, button, menu item.
struct window_item {
	std::wstring id;   // UI Automation runtime id, stable while the element lives
	RECT rect{};       // screen coordinates at the time of the query
};

struct window_snapshot {
	HWND window = nullptr;
	RECT frame{};      // the window's frame at the time of the query, same coordinates as the items
	UINT dpi = 96;
	bool settled = true;   // false if the window moved or resized while being queried
	std::vector<window_item> items;
};

// Parts of ordinary app windows, as UI Automation exposes them. Slow: call it off the UI thread.
// Several worker threads may use it at once.
class i_window_items {
public:
	virtual ~i_window_items() = default;

	// Windows worth badging, the one in front first, minimized ones included. Skips the shell, notifyall and elevated ones.
	virtual std::vector<HWND> candidates() = 0;

	// The focused window, if it is worth badging by the same rules.
	virtual HWND front() = 0;

	virtual std::optional<window_snapshot> query(HWND window) = 0;

	// Picks one element near the middle of what a query found, for moved() to check on later.
	virtual void watch(const window_snapshot& snap) = 0;

	// A cheap look, on the thread that called watch(): has that element moved since? Nothing if there is
	// none to check on.
	virtual std::optional<bool> moved(HWND window) = 0;

	// Releases the COM objects query() created on the calling thread.
	virtual void disconnect() = 0;
};

// Needs COM on the calling thread. Threads may call it at once: each gets UI Automation objects of its own.
class window_items final : public i_window_items {
public:
	static constexpr std::size_t item_limit = 400;   // a huge list shouldn't turn into thousands of badges

	std::vector<HWND> candidates() override;
	HWND front() override;
	std::optional<window_snapshot> query(HWND window) override;
	void watch(const window_snapshot& snap) override;
	std::optional<bool> moved(HWND window) override;
	void disconnect() override;

private:
	struct sentinel {
		HWND window = nullptr;
		Microsoft::WRL::ComPtr<IUIAutomationElement> element;
		RECT rect{};
		POINT spot{};   // where it was found
	};

	struct session {
		Microsoft::WRL::ComPtr<IUIAutomation> automation;
		Microsoft::WRL::ComPtr<IUIAutomationCondition> isItem;
		Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache;
		sentinel watched;
	};

	session* this_threads_session();
	static bool open(session& s);
	bool eligible(HWND window);
	bool elevated(HWND window);

	std::mutex lock;
	std::unordered_map<DWORD, session> sessions;   // by thread id; each thread only touches its own
	std::unordered_map<HWND, bool> elevation;      // a window's process doesn't change, so look once
};
