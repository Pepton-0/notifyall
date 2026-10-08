#include "window_icon_injection.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "badge_art.h"
#include "logger.h"

window_icon_injection::window_icon_injection(i_window_icons& icons) : icons(icons)
{
}

window_icon_injection::~window_icon_injection()
{
	// off() already put the originals back; whatever is left here no window shows any more.
	for (auto& [window, e] : windows) {
		if (e.big.badged) DestroyIcon(e.big.badged);
		if (e.little.badged) DestroyIcon(e.little.badged);
	}
}

void window_icon_injection::on()
{
	if (refresher.running()) return;
	refresh();
	refresher.start(refresh_interval, [this] { refresh(); });
	lib::log(std::format("window icon injection: on ({} windows, {} elevated skipped)", windows.size(), skipped.size()));
}

void window_icon_injection::off()
{
	refresher.stop();
	for (auto& [window, e] : windows) {
		restore(window, icon_kind::big, e.big);
		restore(window, icon_kind::little, e.little);
	}
	windows.clear();
	skipped.clear();
	lib::log("window icon injection: off", true);
}

void window_icon_injection::refresh()
{
	const auto current = icons.windows();
	const std::unordered_set<HWND> alive(current.begin(), current.end());

	// A closed window no longer shows our icons, so they can go.
	std::erase_if(windows, [&](auto& item) {
		if (alive.contains(item.first)) return false;
		if (item.second.big.badged) DestroyIcon(item.second.big.badged);
		if (item.second.little.badged) DestroyIcon(item.second.little.badged);
		return true;
	});
	std::erase_if(skipped, [&](HWND w) { return !alive.contains(w); });

	for (const HWND window : current) {
		if (skipped.contains(window)) continue;
		if (!windows.contains(window) && icons.elevated(window)) {
			skipped.insert(window);
			continue;
		}

		auto [itr, added] = windows.try_emplace(window);
		entry& e = itr->second;
		if (added) e.number = std::uniform_int_distribution<int>(1, 9)(rng);

		badge(window, icon_kind::big, e.number, e.big);
		badge(window, icon_kind::little, e.number, e.little);
	}
}

void window_icon_injection::badge(HWND window, icon_kind kind, int number, slot& s)
{
	const HICON now = icons.get(window, kind);
	if (s.badged && now == s.badged) return;   // still ours

	// New window, or its app replaced our icon (browsers do per tab): badge whatever it shows now.
	const HICON badged = with_badge(icons.shown(window, kind), icons.size(window, kind), number);
	if (!badged) return;
	if (!icons.set(window, kind, badged)) {
		DestroyIcon(badged);
		return;
	}

	if (s.badged) DestroyIcon(s.badged);
	s = { now, badged };
}

void window_icon_injection::restore(HWND window, icon_kind kind, slot& s)
{
	if (!s.badged) return;
	// Leave it if the app has since put up an icon of its own.
	if (icons.get(window, kind) == s.badged) icons.set(window, kind, s.original);
	DestroyIcon(s.badged);
	s = {};
}

HICON window_icon_injection::with_badge(HICON source, int size, int number)
{
	if (!source || size <= 0) return nullptr;

	badge_art::image img = badge_art::capture(source, size, size);
	const int d = std::max(1, static_cast<int>(std::lround(size * 0.55)));
	badge_art::draw_over(img, badge_art::badge(number, d), size - d, 0);
	return badge_art::to_icon(img);
}
