#pragma once

#include <optional>
#include <string>
#include <vector>

#include <windows.h>

// One button in the taskbar's right-hand area: a notification icon, IME, clock, quick settings, ...
struct tray_item {
	std::wstring id;      // stable while the item stays in place
	RECT rect{};          // screen coordinates
	bool clock = false;   // wide; its badge goes in the corner, clear of the time
};

struct tray_snapshot {
	HWND taskbar = nullptr;
	RECT bounds{};
	UINT dpi = 96;
	std::vector<tray_item> items;
};

// The primary taskbar's right-hand area as UI Automation exposes it.
class i_taskbar_items {
public:
	virtual ~i_taskbar_items() = default;

	// Empty when the taskbar can't be found, e.g. while explorer restarts.
	virtual std::optional<tray_snapshot> snapshot() = 0;

	// Whether the taskbar is the frontmost window at this screen point (not covered, not auto-hidden).
	virtual bool visible_at(POINT pt) = 0;

	// Releases the COM objects snapshot() created; call it on the thread that called snapshot().
	virtual void disconnect() = 0;
};
