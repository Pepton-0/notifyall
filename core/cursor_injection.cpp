#include "cursor_injection.h"

#include <cmath>
#include <format>

#include "badge_art.h"
#include "logger.h"

cursor_injection::cursor_injection(i_cursors& cursors) : cursors(cursors)
{
}

void cursor_injection::on()
{
	// Start from the user's scheme, also clearing badges left by a crashed run.
	cursors.restore();

	const int number = std::uniform_int_distribution<int>(1, 9)(rng);
	int count = 0;
	for (const UINT id : targets) {
		const HCURSOR original = cursors.copy(id);
		if (!original) continue;

		const HCURSOR badged = with_badge(original, number);
		DestroyCursor(original);
		if (badged && cursors.replace(id, badged)) ++count;
	}
	replaced = count > 0;
	lib::log(std::format("cursor injection: on ({} cursors)", count));
}

void cursor_injection::off()
{
	if (!replaced) return;
	cursors.restore();
	replaced = false;
	lib::log("cursor injection: off", true);
}

RECT cursor_injection::badge_rect(int cursor_width, int cursor_height)
{
	const int d = std::max(1, static_cast<int>(std::lround(std::min(cursor_width, cursor_height) * 0.45)));
	return { cursor_width - d, cursor_height - d, cursor_width, cursor_height };
}

HCURSOR cursor_injection::with_badge(HCURSOR original, int number)
{
	ICONINFO info{};
	if (!GetIconInfo(original, &info)) return nullptr;

	BITMAP bm{};
	GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bm), &bm);
	const int width = bm.bmWidth;
	const int height = info.hbmColor ? bm.bmHeight : bm.bmHeight / 2;   // monochrome masks stack AND over XOR
	const POINT hotspot{ static_cast<LONG>(info.xHotspot), static_cast<LONG>(info.yHotspot) };
	if (info.hbmColor) DeleteObject(info.hbmColor);
	if (info.hbmMask) DeleteObject(info.hbmMask);
	if (width <= 0 || height <= 0) return nullptr;

	badge_art::image img = badge_art::capture(original, width, height);
	const RECT spot = badge_rect(width, height);
	badge_art::draw_over(img, badge_art::badge(number, spot.right - spot.left), spot.left, spot.top);
	return badge_art::to_cursor(img, hotspot);
}
