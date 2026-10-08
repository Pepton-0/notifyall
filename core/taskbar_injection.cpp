#include "taskbar_injection.h"

#include <format>
#include <unordered_set>

#include "badge_art.h"
#include "logger.h"

taskbar_injection::taskbar_injection(i_taskbar& bar) : bar(bar)
{
}

taskbar_injection::~taskbar_injection()
{
	destroy_icons();
}

void taskbar_injection::on()
{
	if (refresher.running()) return;
	make_icons();
	refresh();
	refresher.start(refresh_interval, [this] { refresh(); });
	lib::log(std::format("taskbar injection: on ({} windows)", numbers.size()));
}

void taskbar_injection::off()
{
	refresher.stop();
	for (const auto& [window, number] : numbers) bar.set_overlay(window, nullptr);
	numbers.clear();
	destroy_icons();
	lib::log("taskbar injection: off", true);
}

void taskbar_injection::refresh()
{
	const auto current = bar.windows();
	const std::unordered_set<HWND> alive(current.begin(), current.end());
	std::erase_if(numbers, [&](const auto& entry) { return !alive.contains(entry.first); });

	for (const HWND window : current) {
		if (numbers.contains(window)) continue;
		const int n = std::uniform_int_distribution<int>(1, 9)(rng);
		if (bar.set_overlay(window, icons[n - 1])) numbers.emplace(window, n);
	}
}

void taskbar_injection::make_icons()
{
	destroy_icons();
	const int size = bar.overlay_size();
	for (int n = 1; n <= 9; ++n) icons[n - 1] = badge_art::to_icon(badge_art::badge(n, size));
}

void taskbar_injection::destroy_icons()
{
	for (HICON& icon : icons) {
		if (icon) DestroyIcon(icon);
		icon = nullptr;
	}
}
