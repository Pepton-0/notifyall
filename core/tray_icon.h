#pragma once

#include <windows.h>
#include <shellapi.h>

#include "i_notifyinjector.h"
#include "i_tray_icon.h"

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
