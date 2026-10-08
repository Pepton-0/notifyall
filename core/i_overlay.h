#pragma once

#include <algorithm>
#include <vector>

#include <windows.h>

// A notification badge: a digit on a red circle, centered at screen coordinates.
struct badge {
	float x = 0;
	float y = 0;
	float diameter = 0;
	int number = 0;

	bool operator==(const badge&) const = default;
};

// Badges clipped to the visible parts of one window, so windows above it hide them.
struct badge_group {
	std::vector<RECT> visible;   // screen coordinates
	std::vector<badge> badges;

	bool operator==(const badge_group& o) const
	{
		return badges == o.badges && std::equal(visible.begin(), visible.end(), o.visible.begin(), o.visible.end(),
			[](const RECT& a, const RECT& b) { return EqualRect(&a, &b); });
	}
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

	// Each group is drawn only inside its visible rectangles. A null owner makes a free-standing topmost layer.
	virtual void show_groups(HWND owner, const RECT& area, const std::vector<badge_group>& groups) = 0;
	virtual void hide() = 0;
	virtual bool shown() const = 0;
};
