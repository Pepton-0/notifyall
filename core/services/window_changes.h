#pragma once

#include <windows.h>
#include <vector>

#include "evt.h"

enum class window_change {
	content,   // something inside the window moved, appeared, vanished or scrolled
	frame,     // the window itself moved or resized
	stack,     // the window was activated, shown, hidden, minimized, restored, cloaked or closed
};

// Tells which top-level window just changed, and how. Changes arrive on the thread that called start().
class i_window_changes {
public:
	virtual ~i_window_changes() = default;

	evt::IEvent<HWND, window_change>& on_change() { return changeEvent; }

	// Watching runs while at least one start() has no matching stop(); that thread must pump messages.
	virtual void start() = 0;
	virtual void stop() = 0;

protected:
	evt::event<HWND, window_change> changeEvent;
};

// WinEvent hooks, out of context: other processes' events arrive as messages on our thread.
class window_changes final : public i_window_changes {
public:
	~window_changes() override;

	void start() override;
	void stop() override;

	// How an event on hwnd (with root as its top-level window) affects layouts and stacking.
	static window_change classify(DWORD event, HWND hwnd, HWND root, LONG object);

private:
	static void CALLBACK on_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD);
	void unhook();

	std::vector<HWINEVENTHOOK> hooks;
	int users = 0;
};
