#pragma once

#include <optional>
#include <string>
#include <vector>

#include <windows.h>

struct desktop_icon {
	std::wstring id;   // parsing name, unique per item
	RECT rect{};       // the icon image in screen coordinates
};

struct desktop_snapshot {
	HWND host = nullptr;   // top-level window holding the icons (Progman or WorkerW)
	RECT bounds{};         // icon area in screen coordinates
	std::vector<desktop_icon> icons;
};

// Desktop icons as the shell reports them.
class i_desktop {
public:
	virtual ~i_desktop() = default;

	// Empty when the desktop can't be reached or its icons are hidden.
	virtual std::optional<desktop_snapshot> snapshot() = 0;

	// Releases the COM objects snapshot() created; call it on the thread that called snapshot().
	virtual void disconnect() = 0;
};
