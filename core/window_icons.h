#pragma once

#include "i_window_icons.h"

class window_icons final : public i_window_icons {
public:
	std::vector<HWND> windows() override;
	bool elevated(HWND window) override;
	HICON get(HWND window, icon_kind kind) override;
	HICON shown(HWND window, icon_kind kind) override;
	bool set(HWND window, icon_kind kind, HICON icon) override;
	int size(HWND window, icon_kind kind) override;
};
