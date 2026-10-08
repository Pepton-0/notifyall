#pragma once

#include <vector>

#include <windows.h>

// Taskbar buttons of running windows, badged through ITaskbarList3::SetOverlayIcon.
// Overlays outlive our process, so whoever sets them must clear them.
class i_taskbar {
public:
	virtual ~i_taskbar() = default;

	// Top-level windows that own a taskbar button right now.
	virtual std::vector<HWND> windows() = 0;

	// Edge length of an overlay icon in pixels for the current dpi.
	virtual int overlay_size() = 0;

	// icon == nullptr removes the overlay. The taskbar keeps its own copy of the icon.
	virtual bool set_overlay(HWND window, HICON icon) = 0;
};
