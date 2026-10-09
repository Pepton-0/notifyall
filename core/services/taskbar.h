#pragma once

#include <vector>
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

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

// Needs COM on the calling thread.
class taskbar final : public i_taskbar {
public:
	std::vector<HWND> windows() override;
	int overlay_size() override;
	bool set_overlay(HWND window, HICON icon) override;

private:
	ITaskbarList3* list();

	Microsoft::WRL::ComPtr<ITaskbarList3> taskbarList;
	bool unavailable = false;
};
