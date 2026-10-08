#pragma once

#include <memory>
#include <unordered_map>

#include <wrl/client.h>

#include "dx.h"
#include "i_overlay.h"

class overlay final : public i_overlay {
public:
	explicit overlay(dx::device_source& gpu) : gpu(gpu) {}
	~overlay() override;

	overlay(const overlay&) = delete;
	overlay& operator=(const overlay&) = delete;

	void show(HWND owner, const RECT& area, const std::vector<badge>& badges) override;
	void show_groups(HWND owner, const RECT& area, const std::vector<badge_group>& groups) override;
	void hide() override;
	bool shown() const override { return shown_; }

private:
	// The window and the GPU objects are made on first show, so constructing stays cheap.
	bool ensure_ready(HWND owner);
	bool create_window(HWND owner);
	void destroy_window();
	void place();
	bool above_owner() const;
	bool paint();
	IDWriteTextFormat* text_format(float diameter);

	HWND hwnd = nullptr;
	HWND owner_ = nullptr;   // the window is recreated when the owner changes (explorer restart)
	dx::device_source& gpu;
	std::shared_ptr<dx::device> device;   // shared by every overlay; declared before layer so it outlives it
	std::unique_ptr<dx::layer> layer;
	std::unordered_map<int, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats;   // by font size
	bool broken = false;   // stop retrying after the GPU setup failed

	RECT area_{};
	std::vector<badge_group> groups_;
	bool painted = false;
	bool shown_ = false;
};
