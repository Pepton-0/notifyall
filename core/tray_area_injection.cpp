#include "tray_area_injection.h"

#include <algorithm>
#include <cmath>

#include "logger.h"

tray_area_injection::tray_area_injection(i_taskbar_items& items, std::unique_ptr<i_overlay> layer)
	: items(items), layer(std::move(layer))
{
}

void tray_area_injection::on()
{
	refresher.start(refresh_interval,
		[this] { return compute(); },
		[this](const overlay_frame& frame) { apply(frame); },
		[this] { items.disconnect(); }, L"tray area");
	lib::log("tray area injection: on");
}

void tray_area_injection::off()
{
	refresher.stop();
	layer->hide();
	lib::log("tray area injection: off", true);
}

overlay_frame tray_area_injection::compute()
{
	const auto snap = items.snapshot();
	if (!snap) return {};

	overlay_frame frame{ snap->taskbar, snap->bounds };
	for (const auto& item : snap->items) {
		const badge b = place(settled_rect(item), snap->dpi, number_for(item.id), item.clock);
		if (items.visible_at({ static_cast<LONG>(b.x), static_cast<LONG>(b.y) })) frame.badges.push_back(b);
	}
	return frame;
}

void tray_area_injection::apply(const overlay_frame& frame)
{
	if (frame.badges.empty()) layer->hide();
	else                      layer->show(frame.owner, frame.area, frame.badges);
}

badge tray_area_injection::place(const RECT& item, UINT dpi, int number, bool clock)
{
	const float scale = static_cast<float>(dpi) / 96.0f;
	const float icon = 16.0f * scale;
	const float diameter = std::round(icon * 0.6f);
	const float r = diameter / 2;

	if (clock) return { item.right - r - 2 * scale, item.top + r + 6 * scale, diameter, number };

	const float cx = (item.left + item.right) / 2.0f;
	const float cy = (item.top + item.bottom) / 2.0f;
	return { std::min(cx + icon / 2 - r / 2, item.right - r), std::max(cy - icon / 2 + r / 2, item.top + r), diameter, number };
}

RECT tray_area_injection::settled_rect(const tray_item& item)
{
	const auto same = [](const RECT& a, const RECT& b) {
		return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
	};

	const auto [itr, added] = rects.try_emplace(item.id, seen_rect{ item.rect, item.rect });
	if (added) return item.rect;

	seen_rect& r = itr->second;
	if (same(item.rect, r.last)) r.settled = item.rect;
	r.last = item.rect;
	return r.settled;
}

int tray_area_injection::number_for(const std::wstring& id)
{
	const auto [itr, added] = numbers.try_emplace(id, 0);
	if (added) itr->second = std::uniform_int_distribution<int>(1, 9)(rng);
	return itr->second;
}
