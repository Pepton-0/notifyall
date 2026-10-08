#pragma once

#include <shobjidl.h>
#include <wrl/client.h>

#include "i_taskbar.h"

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
