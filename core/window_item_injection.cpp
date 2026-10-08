#include "window_item_injection.h"

#include <algorithm>
#include <cmath>

#include "logger.h"
#include "occlusion.h"


window_item_injection::window_item_injection(i_window_items& items, i_window_stack& stack, i_window_changes& changes,
                                             std::unique_ptr<i_overlay> layer)
	: items(items), stack(stack), changes(changes), layer(std::move(layer))
{
}

void window_item_injection::on()
{
	changes.start([this](HWND w) { changed(w); });
	querier.start(query_interval,
		[this] { return query_round(); },
		[this](const query_result& result) { store(result); },
		[this] { items.disconnect(); }, L"window items");
	follower.start(follow_interval, [this] { follow(); });
	lib::log("window item injection: on");
}

void window_item_injection::off()
{
	changes.stop();
	follower.stop();
	querier.stop();
	layouts.clear();
	layer->hide();
	lib::log("window item injection: off", true);
}

void window_item_injection::changed(HWND window)
{
	std::lock_guard lock(dirtyLock);
	dirty.insert(window);
}

std::vector<HWND> window_item_injection::pick(const std::vector<HWND>& candidates)
{
	// Slow windows are paced by their own cost, so a window that reports changes nonstop
	// (some fire hundreds of events a second) can't keep the worker busy.
	const auto now = clock::now();
	const auto overdue = [&](HWND w, clock::duration age) {
		const auto last = lastQuery.find(w);
		if (last == lastQuery.end()) return true;
		const auto cost = lastCost.find(w);
		return now - last->second >= std::max(age, cost == lastCost.end() ? clock::duration{} : cost->second * cost_factor);
	};

	std::vector<HWND> picks;
	const auto take = [&](HWND w) {
		if (picks.size() < queries_per_round && std::ranges::find(picks, w) == picks.end()) picks.push_back(w);
	};

	// Windows that reported a change, the one in front before the others.
	{
		std::lock_guard lock(dirtyLock);
		for (const HWND w : candidates)
			if (dirty.contains(w) && overdue(w, clock::duration{})) take(w);
		for (const HWND w : picks) dirty.erase(w);
	}

	// Without events, still look again now and then: not every app reports its changes.
	if (overdue(candidates.front(), front_refresh)) take(candidates.front());
	for (std::size_t i = 1; i < candidates.size() && picks.size() < queries_per_round; ++i) {
		const HWND w = candidates[1 + (turn + i - 1) % (candidates.size() - 1)];
		if (overdue(w, back_refresh)) take(w);
	}
	++turn;
	return picks;
}

window_item_injection::query_result window_item_injection::query_round()
{
	query_result result{ items.candidates() };
	if (result.candidates.empty()) return result;

	const auto gone = [&](const auto& e) { return std::ranges::find(result.candidates, e.first) == result.candidates.end(); };
	std::erase_if(lastQuery, gone);
	std::erase_if(lastCost, gone);
	if (numbers.size() > 8192) numbers.clear();   // numbers of elements that went away would only pile up

	for (const HWND window : pick(result.candidates)) {
		const auto began = clock::now();
		lastQuery[window] = began;
		const auto snap = items.query(window);
		lastCost[window] = clock::now() - began;
		if (snap && !snap->settled) {
			changed(window);   // half old, half new positions: look again next round
			continue;
		}

		window_badges wb{ window };
		if (snap) {
			wb.size = { snap->frame.right - snap->frame.left, snap->frame.bottom - snap->frame.top };
			for (const auto& item : snap->items) {
				RECT relative = item.rect;
				OffsetRect(&relative, -snap->frame.left, -snap->frame.top);
				wb.badges.push_back(anchor(place(relative, snap->dpi, number_for(item.id)), wb.size));
			}
		}
		result.updated.push_back(std::move(wb));
	}
	return result;
}

void window_item_injection::store(const query_result& result)
{
	std::erase_if(layouts, [&](const auto& e) { return std::ranges::find(result.candidates, e.first) == result.candidates.end(); });

	for (const auto& wb : result.updated) {
		if (wb.badges.empty()) layouts.erase(wb.window);
		else                   layouts[wb.window] = { wb.size, wb.badges };
	}
}

void window_item_injection::follow()
{
	if (layouts.empty()) {
		layer->hide();
		return;
	}

	const auto windows = stack.front_to_back();
	const auto visible = occlusion::visible_parts(windows);

	std::vector<badge_group> groups;
	for (const stacked_window& w : windows) {
		const auto layout = layouts.find(w.hwnd);
		if (layout == layouts.end()) continue;
		const auto parts = visible.find(w.hwnd);
		if (parts == visible.end()) continue;

		// After a resize the anchors keep badges near their elements until a fresh query lands.
		const SIZE now{ w.frame.right - w.frame.left, w.frame.bottom - w.frame.top };
		if (now.cx != layout->second.size.cx || now.cy != layout->second.size.cy) changed(w.hwnd);

		badge_group group{ parts->second };
		group.badges.reserve(layout->second.badges.size());
		for (const anchored_badge& a : layout->second.badges) group.badges.push_back(resolve(a, w.frame));
		groups.push_back(std::move(group));
	}

	if (groups.empty()) layer->hide();
	else                layer->show_groups(nullptr, stack.screen(), groups);
}

badge window_item_injection::place(const RECT& item, UINT dpi, int number)
{
	const float scale = static_cast<float>(dpi) / 96.0f;
	const float width = static_cast<float>(item.right - item.left);
	const float height = static_cast<float>(item.bottom - item.top);
	const float diameter = std::round(std::min(14.0f * scale, height * 0.7f));
	const float r = diameter / 2;

	const bool row = width > height * 4;
	const float x = row ? item.left + std::min(height, 32.0f * scale) : item.right - r * 1.2f;
	return { std::max(x, item.left + r), item.top + r * 1.2f, diameter, number };
}

window_item_injection::anchored_badge window_item_injection::anchor(const badge& b, SIZE frame)
{
	anchored_badge a{ b };
	a.from_right = b.x > frame.cx / 2.0f;
	a.from_bottom = b.y > frame.cy / 2.0f;
	if (a.from_right) a.offset.x = frame.cx - b.x;
	if (a.from_bottom) a.offset.y = frame.cy - b.y;
	return a;
}

badge window_item_injection::resolve(const anchored_badge& a, const RECT& frame)
{
	badge b = a.offset;
	b.x = a.from_right ? frame.right - b.x : frame.left + b.x;
	b.y = a.from_bottom ? frame.bottom - b.y : frame.top + b.y;
	return b;
}

int window_item_injection::number_for(const std::wstring& id)
{
	const auto [itr, added] = numbers.try_emplace(id, 0);
	if (added) itr->second = std::uniform_int_distribution<int>(1, 9)(rng);
	return itr->second;
}
