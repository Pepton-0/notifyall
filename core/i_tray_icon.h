#pragma once

#include "evt.h"

// Notification area icon with a "Show/Hide" toggle and "Exit".
class i_tray_icon {
public:
	virtual ~i_tray_icon() = default;

	// Carries the new visibility.
	evt::IEvent<bool>& on_visibility_changed() { return visibilityChangedEvent; }

	// Blocks until exit is chosen and returns the exit code.
	virtual int run() = 0;

	virtual void toggle() = 0;
	virtual bool visible() const = 0;

	// Makes run() return. Callable from any thread.
	virtual void quit() = 0;

protected:
	evt::event<bool> visibilityChangedEvent;
};
