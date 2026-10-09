#pragma once

#include <vector>
#include <windows.h>
#include <chrono>
#include <climits>
#include <map>
#include <unordered_map>
#include <utility>
#include <wrl/client.h>
#include <memory>

#include "dx.h"

// A notification badge: a digit on a red circle, centered at (x, y) on screen, or relative to its group.
struct badge {
	float x = 0;
	float y = 0;
	float diameter = 0;
	int number = 0;

	bool operator==(const badge&) const = default;
};

// One window's badges, clipped to its visible parts so windows above it hide them. Everything is relative
// to origin, so a moved window moves its group as a whole.
struct badge_group {
	HWND key = nullptr;          // the same group across calls
	POINT origin{};              // screen coordinates
	std::vector<RECT> visible;
	std::vector<badge> badges;
};

// What an overlay should show: badges over area, kept right above owner. No badges means hidden.
struct overlay_frame {
	HWND owner = nullptr;
	RECT area{};
	std::vector<badge> badges;
};

// See-through, click-through layer that draws badges.
class i_overlay {
public:
	virtual ~i_overlay() = default;

	// Covers area (screen coordinates) just above owner in the z-order, so whatever covers owner covers it too.
	// Redraws only when something changed.
	virtual void show(HWND owner, const RECT& area, const std::vector<badge>& badges) = 0;

	// Each group is drawn only inside its visible rectangles; only groups that changed cost anything.
	// A null owner makes a free-standing topmost layer.
	virtual void show_groups(HWND owner, const RECT& area, const std::vector<badge_group>& groups) = 0;
	virtual void hide() = 0;
	virtual bool shown() const = 0;
};

// Composition visuals for badge groups: one visual per badge under one per group, so a moved group costs a
// single offset and a new layout touches only the badges that changed. Each digit is drawn once and shared.
class badge_scene {
public:
	static constexpr std::chrono::seconds keep_hidden{ 30 };   // a group that went away may soon come back

	// Where a badge's picture goes and which parts of it show.
	struct cut {
		RECT sprite{};               // relative to the group origin
		std::vector<RECT> pieces;    // relative to the sprite; a single one covering it all when nothing hides it
	};

	static int sprite_size(float diameter);
	static cut cut_out(const badge& b, const std::vector<RECT>& visible);

	badge_scene(dx::device& dev, IDCompositionVisual& root);

	badge_scene(const badge_scene&) = delete;
	badge_scene& operator=(const badge_scene&) = delete;

	// Groups need distinct keys; origins are taken relative to root_origin.
	// Returns whether the tree changed, in which case the caller commits.
	bool update(const std::vector<badge_group>& groups, POINT root_origin);

private:
	using clock = std::chrono::steady_clock;
	using visual_ptr = Microsoft::WRL::ComPtr<IDCompositionVisual>;

	struct placed {
		visual_ptr visual;
		IDCompositionSurface* shows = nullptr;
		POINT at{ LONG_MIN, LONG_MIN };
		RECT clip{};
		bool clipped = false;
	};

	struct group {
		visual_ptr visual;
		POINT offset{ LONG_MIN, LONG_MIN };
		std::vector<RECT> visible;
		std::vector<badge> badges;
		std::vector<std::vector<placed>> slots;   // per badge: one visual, or one per visible piece of it
		bool attached = false;
		clock::time_point seen{};
	};

	bool lay_out(group& g, const badge_group& wanted);
	bool place(group& g, std::vector<placed>& slot, const badge& b, const std::vector<RECT>& visible);
	static bool clear(group& g, std::vector<placed>& slot);
	IDCompositionSurface* sprite(const badge& b);
	IDWriteTextFormat* text_format(int size);

	dx::device& dev;
	IDCompositionVisual& root;
	std::unordered_map<HWND, group> groups;
	std::map<std::pair<int, int>, Microsoft::WRL::ComPtr<IDCompositionSurface>> sprites;   // by number and size
	std::map<int, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats;                     // by font size
	bool broken = false;   // drawing failed once; the device is likely gone
};

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
	bool under_any(const std::vector<badge_group>& groups) const;

	HWND hwnd = nullptr;
	HWND owner_ = nullptr;   // the window is recreated when the owner changes (explorer restart)
	dx::device_source& gpu;
	std::shared_ptr<dx::device> device;   // shared by every overlay; declared first so it outlives the rest
	std::unique_ptr<dx::layer> layer;
	std::unique_ptr<badge_scene> scene;   // built on layer's root
	bool broken = false;   // stop retrying after the GPU setup failed

	RECT area_{};
	bool shown_ = false;
};
