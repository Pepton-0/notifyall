#pragma once

#include <array>
#include <random>

#include "i_cursors.h"
#include "i_injection.h"

// Replaces the static system cursors with copies carrying a badge at their bottom-right.
// Animated ones (busy, working in background) are left alone.
class cursor_injection final : public i_injection {
public:
	// OCR_NORMAL, IBEAM, HAND, CROSS, UP, SIZENWSE, SIZENESW, SIZEWE, SIZENS, SIZEALL, NO
	static constexpr std::array<UINT, 11> targets{
		32512, 32513, 32649, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648,
	};

	explicit cursor_injection(i_cursors& cursors);

	void on() override;
	void off() override;

	// Where the badge goes on a cursor of the given size.
	static RECT badge_rect(int cursor_width, int cursor_height);

	static HCURSOR with_badge(HCURSOR original, int number);

private:
	i_cursors& cursors;
	bool replaced = false;
	std::mt19937 rng{ std::random_device{}() };
};
