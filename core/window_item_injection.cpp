#include "window_item_injection.h"

#include <algorithm>
#include <cmath>

#include "logger.h"
#include "occlusion.h"

namespace {

	std::chrono::milliseconds rounded_up(window_item_injection::clock::duration d)
	{
		return std::max(std::chrono::milliseconds{ 0 }, std::chrono::ceil<std::chrono::milliseconds>(d));
	}

}

window_item_injection::window_item_injection(i_window_items& items, i_window_stack& stack, i_window_changes& changes,
                                             std::unique_ptr<i_overlay> layer)
	: items(items), stack(stack), changes(changes), layer(std::move(layer))
{
}

void window_item_injection::on()
{
	stackDirty = true;
	changeSub = changes.on_change().connect([this](HWND w, window_change kind) { changed(w, kind); });
	changes.start();
	frontQuerier.fold_with(&window_item_injection::merge);
	frontQuerier.start(front_refresh,
		[this] {
			auto result = query_front();
			frontQuerier.wait_next(rounded_up(frontWait));
			return result;
		},
		[this](const query_result& result) { store(result); },
		[this] { items.disconnect(); }, L"focused window items");
	querier.fold_with(&window_item_injection::merge);
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
	changeSub.reset();
	follower.stop();
	frontQuerier.stop();
	querier.stop();
	layouts.clear();
	lastStack.clear();
	lastVisible.clear();
	moved.clear();
	layer->hide();

	// The workers have stopped: forget what they handed over, so turning on again hands it all anew.
	histories.clear();
	lastFront = nullptr;
	lib::log("window item injection: off", true);
}

void window_item_injection::changed(HWND window, window_change kind)
{
	// Moving and stacking only need a new look at the frames, not a new query.
	if (kind == window_change::stack) {
		stackDirty = true;
		frontQuerier.poke();   // another window may have come forward
		return;
	}
	if (kind == window_change::frame) {
		const bool known = std::ranges::find(lastStack, window, &stacked_window::hwnd) != lastStack.end();
		if (known) moved.insert(window);
		else       stackDirty = true;   // perhaps it just came on screen

		std::lock_guard lock(dirtyLock);
		lastMoved[window] = clock::now();
		return;
	}

	bool fresh = false;
	{
		std::lock_guard lock(dirtyLock);
		fresh = dirty.insert(window).second;
	}
	// The focused window's worker sleeps until told; once per batch of changes is enough.
	if (fresh && window == GetAncestor(GetForegroundWindow(), GA_ROOT)) frontQuerier.poke();
}

float window_item_injection::visible_fraction(HWND window)
{
	std::lock_guard lock(visibilityLock);
	const auto it = visibleFraction.find(window);
	return it == visibleFraction.end() ? 0.0f : it->second;
}

window_item_injection::query_result window_item_injection::query_front()
{
	frontWait = front_refresh;   // unless something comes up sooner, which pokes this worker
	const HWND front = items.front();
	if (!front || visible_fraction(front) <= 0) return {};

	const auto now = clock::now();
	bool lively = false;
	std::optional<clock::time_point> due;   // the next look by the schedule alone; probes may bring it forward
	{
		std::scoped_lock lock(dirtyLock, stateLock);
		std::erase_if(lastMoved, [&](const auto& e) { return now - e.second >= frame_quiet; });
		if (const auto moving = lastMoved.find(front); moving != lastMoved.end()) {
			frontWait = moving->second + frame_quiet - now;
			return {};
		}

		history& h = histories[front];
		if (front != lastFront) {
			frontBackoff = 1;   // brought forward: the clearest sign someone cares about it now
			lastFront = front;
		}
		if (h.busy) {
			frontWait = follow_interval;   // the background worker is asking it just now
			return {};
		}
		lively = dirty.contains(front) || now - h.changedAt < front_settle;
		if (!lively) frontBackoff = 1;   // a quiet spell starts afresh, so the next report is answered at once
		if (h.asked != clock::time_point{}) {
			const auto next = h.asked + front_gap(h.cost, lively, frontBackoff);
			if (now < next) due = next;
		}
		if (!due) {
			dirty.erase(front);
			h.busy = true;
		}
	}

	// Until then, where one element sits tells cheaply whether the content moved and is worth a look now.
	if (due) {
		const auto moved = items.moved(front);
		frontWait = *due - now;
		if (!moved) return {};   // nothing to check on: the schedule alone decides
		if (!*moved) {
			frontWait = std::min<clock::duration>(frontWait, lively ? follow_interval : probe_quiet);
			return {};
		}
		std::scoped_lock lock(dirtyLock, stateLock);
		history& h = histories[front];
		if (h.busy) return {};
		dirty.erase(front);
		h.busy = true;
	}

	query_result result;
	auto news = ask(front, true);
	const bool found = news.has_value();
	if (news) result.updated.push_back(std::move(*news));

	// Only reports and recent motion can cry wolf; a routine look that finds nothing proves no one wrong.
	std::scoped_lock lock(dirtyLock, stateLock);
	frontBackoff = lively ? next_backoff(frontBackoff, found) : 1;
	const history& h = histories[front];
	const auto after = clock::now();
	const bool stillLively = dirty.contains(front) || after - h.changedAt < front_settle;
	const clock::duration probe = stillLively ? follow_interval : probe_quiet;
	frontWait = std::min(h.asked + front_gap(h.cost, stillLively, frontBackoff) - after, probe);
	return result;
}

std::vector<HWND> window_item_injection::pick(const std::vector<HWND>& others)
{
	std::unordered_map<HWND, float> shown;
	{
		std::lock_guard lock(visibilityLock);
		shown = visibleFraction;
	}
	const auto fraction = [&](HWND w) {
		const auto it = shown.find(w);
		return it == shown.end() ? 0.0f : it->second;
	};

	std::vector<HWND> picks;
	const auto take = [&](HWND w) {
		if (picks.size() < queries_per_round && std::ranges::find(picks, w) == picks.end()) picks.push_back(w);
	};

	const auto now = clock::now();
	std::scoped_lock lock(dirtyLock, stateLock);

	// Slow windows are paced by their own cost, so a window that reports changes nonstop
	// (some fire hundreds of events a second) can't keep the worker busy.
	const auto overdue = [&](HWND w, clock::duration age) {
		const auto h = histories.find(w);
		if (h == histories.end() || h->second.asked == clock::time_point{}) return true;
		const auto gap = std::max(age, h->second.cost * cost_factor);
		return now - h->second.asked >= gap * h->second.backoff;
	};

	// A window on the move would come back half old, half new; its badges ride along with the frame meanwhile.
	std::erase_if(lastMoved, [&](const auto& e) { return now - e.second >= frame_quiet; });
	const auto ready = [&](HWND w) {
		const auto h = histories.find(w);
		const bool busy = h != histories.end() && h->second.busy;
		return fraction(w) > 0 && !lastMoved.contains(w) && !busy;
	};

	// Changed windows first. The rest keep their mark until they are ready.
	for (const HWND w : others)
		if (dirty.contains(w) && ready(w) && overdue(w, clock::duration{})) take(w);

	// Without events, still look again now and then: not every app reports its changes.
	for (std::size_t i = 0; i < others.size() && picks.size() < queries_per_round; ++i) {
		const HWND w = others[(turn + i) % others.size()];
		if (ready(w) && overdue(w, fraction(w) < dim_fraction ? dim_refresh : back_refresh)) take(w);
	}
	++turn;

	for (const HWND w : picks) {
		dirty.erase(w);
		histories[w].busy = true;
	}
	return picks;
}

window_item_injection::query_result window_item_injection::query_round()
{
	query_result result{ items.candidates() };
	const auto& all = *result.candidates;

	// The focused window is the other worker's.
	const HWND front = items.front();
	std::vector<HWND> others;
	for (const HWND w : all) if (w != front) others.push_back(w);

	{
		std::lock_guard lock(stateLock);
		std::erase_if(histories, [&](const auto& e) {
			return !e.second.busy && std::ranges::find(all, e.first) == all.end();
		});
		if (numbers.size() > 8192) numbers.clear();   // numbers of elements that went away would only pile up
	}
	if (others.empty()) return result;

	for (const HWND window : pick(others))
		if (auto wb = ask(window)) result.updated.push_back(std::move(*wb));
	return result;
}

std::optional<window_item_injection::window_badges> window_item_injection::ask(HWND window, bool watch)
{
	const auto began = clock::now();
	clock::time_point previous{};
	{
		std::lock_guard lock(stateLock);
		history& h = histories[window];
		previous = h.asked;
		h.asked = began;
	}
	const auto snap = items.query(window);
	const auto cost = clock::now() - began;
	if (watch && snap && snap->settled) items.watch(*snap);

	if (snap && !snap->settled) {
		{
			std::lock_guard lock(stateLock);
			history& h = histories[window];
			h.busy = false;
			h.cost = cost;
		}
		changed(window, window_change::content);   // half old, half new positions: look again soon
		return std::nullopt;
	}

	std::lock_guard lock(stateLock);
	window_badges wb{ window };
	// Whatever changed did so after the last look: halfway is the best guess.
	wb.at = previous == clock::time_point{} ? began : previous + (began - previous) / 2;
	if (snap) {
		wb.size = { snap->frame.right - snap->frame.left, snap->frame.bottom - snap->frame.top };
		for (const auto& item : snap->items) {
			RECT relative = item.rect;
			OffsetRect(&relative, -snap->frame.left, -snap->frame.top);
			anchored_badge a = anchor(place(relative, snap->dpi, number_for(item.id)), wb.size);
			a.key = std::hash<std::wstring>{}(item.id);
			wb.badges.push_back(a);
		}
	}

	// Only news crosses over: the same layout again would cost the UI thread a rebuild for nothing.
	history& h = histories[window];
	h.busy = false;
	h.cost = cost;
	const bool different = h.seen.badges != wb.badges || h.seen.size.cx != wb.size.cx || h.seen.size.cy != wb.size.cy;
	h.backoff = next_backoff(h.backoff, different);
	if (!different) return std::nullopt;
	h.changedAt = began;
	h.seen = { wb.size, wb.badges };
	return wb;
}

void window_item_injection::merge(query_result& unread, query_result fresh)
{
	for (window_badges& wb : fresh.updated) {
		const auto older = std::ranges::find(unread.updated, wb.window, &window_badges::window);
		if (older != unread.updated.end()) *older = std::move(wb);
		else                               unread.updated.push_back(std::move(wb));
	}
	if (fresh.candidates) unread.candidates = std::move(fresh.candidates);
	if (!unread.candidates) return;

	const auto& all = *unread.candidates;
	std::erase_if(unread.updated, [&](const window_badges& wb) { return std::ranges::find(all, wb.window) == all.end(); });
}

int window_item_injection::next_backoff(int current, bool changed)
{
	return changed ? 1 : std::min(current * 2, max_backoff);
}

window_item_injection::clock::duration window_item_injection::front_gap(clock::duration cost, bool lively, int backoff)
{
	const clock::duration frame = follow_interval;   // asking faster than the screen shows it buys nothing
	if (lively) return std::max(cost, frame) * backoff;
	return std::max<clock::duration>(front_refresh, cost * cost_factor);
}

void window_item_injection::store(const query_result& result)
{
	// Minimized and cloaked windows stay candidates, so their layouts are ready when they come back.
	std::size_t dropped = 0;
	if (result.candidates) {
		const auto& all = *result.candidates;
		dropped = std::erase_if(layouts, [&](const auto& e) { return std::ranges::find(all, e.first) == all.end(); });
	}
	for (const auto& wb : result.updated) {
		if (wb.badges.empty()) {
			layouts.erase(wb.window);
			continue;
		}
		const auto [itr, added] = layouts.try_emplace(wb.window);
		shown& s = itr->second;
		const bool same_size = !added && s.current.size.cx == wb.size.cx && s.current.size.cy == wb.size.cy;
		float largest = 0;
		for (const anchored_badge& a : wb.badges) largest = std::max(largest, a.offset.diameter);
		s.motion = same_size
			? drift::track(s.motion, spots(s.current.badges, s.current.size), s.at, spots(wb.badges, wb.size), wb.at,
				static_cast<int>(std::lround(largest)))
			: drift{};
		s.current = { wb.size, wb.badges };
		s.at = wb.at;
	}
	if (dropped == 0 && result.updated.empty()) return;
	layoutsChanged = true;
	follow();   // on screen now rather than at the next tick
}

void window_item_injection::follow()
{
	// Re-reading the stack walks every window and asks DWM about each, so do it on stacking events only,
	// plus a slow fallback for z-order changes that raise none. A move re-reads the moved frames alone.
	const auto now = clock::now();
	const bool full = stackDirty || now - lastFollow >= stack_fallback;
	// Content that keeps scrolling is redrawn every tick; once it stops, one last time where the last layout says.
	const bool drifting = std::ranges::any_of(layouts, [&](const auto& e) {
		return e.second.motion.offset(now + display_lead - e.second.at).has_value();
	});
	if (!full && moved.empty() && !layoutsChanged && !drifting && !drifted) return;

	if (full || !moved.empty()) read_screen(full, now);   // new layouts alone leave the screen as it was
	stackDirty = false;
	moved.clear();
	layoutsChanged = false;
	drifted = drifting;

	std::vector<badge_group> groups;
	for (const stacked_window& w : lastStack) {
		const auto layout = layouts.find(w.hwnd);
		if (layout == layouts.end()) continue;
		const auto parts = lastVisible.find(w.hwnd);
		if (parts == lastVisible.end()) continue;

		// After a resize the anchors keep badges near their elements until a fresh query lands.
		const shown& s = layout->second;
		const SIZE size{ w.frame.right - w.frame.left, w.frame.bottom - w.frame.top };
		const bool resized = size.cx != s.current.size.cx || size.cy != s.current.size.cy;
		if (resized) changed(w.hwnd, window_change::content);
		const auto shift = resized ? std::nullopt : s.motion.offset(now + display_lead - s.at);

		// Relative to the frame: a window that only moved hands the overlay the same group again.
		badge_group group{ w.hwnd, { w.frame.left, w.frame.top } };
		group.visible.reserve(parts->second.size());
		for (RECT v : parts->second) {
			OffsetRect(&v, -w.frame.left, -w.frame.top);
			group.visible.push_back(v);
		}
		const RECT local{ 0, 0, size.cx, size.cy };
		group.badges.reserve(s.current.badges.size());
		for (const anchored_badge& a : s.current.badges) {
			badge b = resolve(a, local);
			if (shift && s.motion.keys.contains(a.key)) {
				b.x += static_cast<float>(shift->x);
				b.y += static_cast<float>(shift->y);
				const POINT centre{ std::lround(b.x), std::lround(b.y) };
				if (!PtInRect(&s.motion.region, centre)) continue;   // scrolled out of view
			}
			group.badges.push_back(b);
		}
		groups.push_back(std::move(group));
	}

	if (groups.empty()) layer->hide();
	else                layer->show_groups(nullptr, stack.screen(), groups);
}

void window_item_injection::read_screen(bool whole, clock::time_point now)
{
	if (whole || !refresh_moved()) {
		lastStack = stack.front_to_back();
		lastFollow = now;
	}
	lastVisible = occlusion::visible_parts(lastStack);
	publish_visibility(lastStack, lastVisible);
	frontQuerier.poke();   // the focused window may have come into view or stopped moving
}

bool window_item_injection::refresh_moved()
{
	for (const HWND w : moved) {
		const auto known = std::ranges::find(lastStack, w, &stacked_window::hwnd);
		if (known == lastStack.end()) continue;
		const auto frame = stack.frame_of(w);
		if (!frame) return false;   // it left the screen: only a full read tells what shows instead
		known->frame = *frame;
	}
	return true;
}

void window_item_injection::publish_visibility(const std::vector<stacked_window>& windows,
                                               const std::unordered_map<HWND, std::vector<RECT>>& visible)
{
	std::unordered_map<HWND, float> fractions;
	for (const stacked_window& w : windows) {
		const auto parts = visible.find(w.hwnd);
		if (parts == visible.end()) continue;

		long long shown = 0;
		for (const RECT& r : parts->second) shown += static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
		const long long whole = static_cast<long long>(w.frame.right - w.frame.left) * (w.frame.bottom - w.frame.top);
		if (whole > 0) fractions.emplace(w.hwnd, static_cast<float>(shown) / static_cast<float>(whole));
	}

	std::lock_guard lock(visibilityLock);
	visibleFraction.swap(fractions);
}

std::vector<drift::spot> window_item_injection::spots(const std::vector<anchored_badge>& badges, SIZE size)
{
	const RECT frame{ 0, 0, size.cx, size.cy };
	std::vector<drift::spot> out;
	out.reserve(badges.size());
	for (const anchored_badge& a : badges) {
		const badge b = resolve(a, frame);
		out.push_back({ a.key, { std::lround(b.x), std::lround(b.y) } });
	}
	return out;
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
