#include "desktop_injection.h"

#include <algorithm>
#include <cmath>

#include "logger.h"

desktop_injection::desktop_injection(i_desktop& desk, std::unique_ptr<i_overlay> layer)
	: desk(desk), layer(std::move(layer))
{
}

void desktop_injection::on()
{
	refresher.start(refresh_interval,
		[this] { return compute(); },
		[this](const overlay_frame& frame) { apply(frame); },
		[this] { desk.disconnect(); }, L"desktop");
	lib::log("desktop injection: on");
}

void desktop_injection::off()
{
	refresher.stop();
	layer->hide();
	lib::log("desktop injection: off", true);
}

overlay_frame desktop_injection::compute()
{
	const auto snap = desk.snapshot();
	if (!snap) return {};

	overlay_frame frame{ snap->host, snap->bounds };
	frame.badges.reserve(snap->icons.size());
	for (const auto& icon : snap->icons) frame.badges.push_back(place(icon.rect, number_for(icon.id)));
	return frame;
}

void desktop_injection::apply(const overlay_frame& frame)
{
	if (frame.badges.empty()) layer->hide();
	else                      layer->show(frame.owner, frame.area, frame.badges);
}

badge desktop_injection::place(const RECT& icon, int number)
{
	const float size = static_cast<float>(std::min(icon.right - icon.left, icon.bottom - icon.top));
	const float diameter = std::round(size * 0.42f);
	const float r = diameter / 2;
	return { icon.right - r * 0.7f, icon.top + r * 0.7f, diameter, number };
}

int desktop_injection::number_for(const std::wstring& icon_id)
{
	const auto [itr, added] = numbers.try_emplace(icon_id, 0);
	if (added) itr->second = std::uniform_int_distribution<int>(1, 9)(rng);
	return itr->second;
}
