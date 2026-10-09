#include "desktop_injection.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "logger.h"
#include "occlusion.h"

namespace {

	bool is_desktop(HWND window)
	{
		wchar_t cls[16]{};
		const std::wstring_view name(cls, static_cast<std::size_t>(GetClassNameW(window, cls, 16)));
		return name == L"Progman" || name == L"WorkerW";
	}

}

desktop_injection::desktop_injection(i_desktop& desk, i_window_stack& stack, i_window_changes& changes,
                                     std::unique_ptr<i_overlay> layer)
	: desk(desk), stack(stack), changes(changes), layer(std::move(layer))
{
}

void desktop_injection::on()
{
	changeSub = changes.on_change().connect([this](HWND w, window_change) { if (is_desktop(w)) desktop_changed(); });
	changes.start();
	refresher.start(tick,
		[this] { return compute(); },
		[this](const std::optional<overlay_frame>& frame) { apply(frame); },
		[this] { desk.disconnect(); }, L"desktop");
	lib::log("desktop injection: on");
}

void desktop_injection::off()
{
	changes.stop();
	changeSub.reset();
	refresher.stop();
	layer->hide();
	lib::log("desktop injection: off", true);
}

std::optional<overlay_frame> desktop_injection::compute(bool force)
{
	if (!force) {
		if (!pacer.due()) return std::nullopt;
		// Under other windows the badges can't be seen, and the last positions still hold when it shows again.
		if (const HWND host = desk.host(); host && !visible(host)) return std::nullopt;
	}

	const auto snap = desk.snapshot();
	if (!snap) return overlay_frame{};

	overlay_frame frame{ snap->host, snap->bounds };
	frame.badges.reserve(snap->icons.size());
	for (const auto& icon : snap->icons) frame.badges.push_back(place(icon.rect, number_for(icon.id)));
	return frame;
}

void desktop_injection::apply(const std::optional<overlay_frame>& frame)
{
	if (!frame) return;   // not due, or covered: the last picture still holds
	if (frame->badges.empty()) layer->hide();
	else                       layer->show(frame->owner, frame->area, frame->badges);
}

bool desktop_injection::visible(HWND host)
{
	return occlusion::visible_parts(stack.front_to_back()).contains(host);
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
