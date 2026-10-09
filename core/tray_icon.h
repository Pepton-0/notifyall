#pragma once

#include <windows.h>
#include <shellapi.h>

#include "evt.h"
#include "notifyinjector.h"

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

class tray_icon final : public i_tray_icon {
public:
	explicit tray_icon(i_notifyinjector& injector);
	~tray_icon() override;

	tray_icon(const tray_icon&) = delete;
	tray_icon& operator=(const tray_icon&) = delete;

	int run() override;
	void toggle() override;
	bool visible() const override { return visible_; }
	void quit() override;

private:
	static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

	bool create_window();
	void add_icon();
	void remove_icon();
	void show_menu();

	i_notifyinjector& injector;
	bool visible_ = true;

	HWND hwnd = nullptr;
	NOTIFYICONDATAW nid{};
	UINT taskbarCreatedMsg = 0;   // sent when explorer restarts
};
