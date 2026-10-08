#pragma once

#include <functional>

#include <windows.h>

// Tells which top-level window just changed inside: something moved, appeared, vanished, scrolled or was resized.
class i_window_changes {
public:
	virtual ~i_window_changes() = default;

	// changed runs on this thread, which must pump messages.
	virtual void start(std::function<void(HWND)> changed) = 0;
	virtual void stop() = 0;
};
