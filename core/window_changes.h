#pragma once

#include <vector>

#include "i_window_changes.h"

// WinEvent hooks, out of context: other processes' events arrive as messages on our thread.
class window_changes final : public i_window_changes {
public:
	~window_changes() override;

	void start(std::function<void(HWND)> changed) override;
	void stop() override;

private:
	static void CALLBACK on_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD);

	std::vector<HWINEVENTHOOK> hooks;
	std::function<void(HWND)> changed;
};
