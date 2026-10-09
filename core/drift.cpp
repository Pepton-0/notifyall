#include "drift.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <utility>

namespace {

	constexpr long close_enough = 2;   // pixels: steps this close to the common one count as the same step

	bool agree(float a, float b)
	{
		const float scale = std::max({ std::abs(a), std::abs(b), drift::slowest });
		return std::abs(a - b) <= drift::pace_tolerance * scale;
	}

	void stretch(RECT& r, bool& empty, POINT p)
	{
		if (empty) {
			r = { p.x, p.y, p.x, p.y };
			empty = false;
			return;
		}
		r.left = std::min(r.left, p.x);
		r.top = std::min(r.top, p.y);
		r.right = std::max(r.right, p.x);
		r.bottom = std::max(r.bottom, p.y);
	}

}

drift drift::track(const drift& before, const std::vector<spot>& older, clock::time_point olderAt,
                   const std::vector<spot>& newer, clock::time_point newerAt, int margin)
{
	drift d;
	const auto dt = newerAt - olderAt;
	if (dt < std::chrono::milliseconds(5) || dt > std::chrono::seconds(3)) return d;

	std::unordered_map<std::size_t, POINT> was;
	for (const spot& s : older) was.emplace(s.key, s.at);

	// The step most badges took, standing still aside.
	std::map<std::pair<long, long>, int> votes;
	int matched = 0;
	for (const spot& s : newer) {
		const auto w = was.find(s.key);
		if (w == was.end()) continue;
		++matched;
		const std::pair<long, long> step{ s.at.x - w->second.x, s.at.y - w->second.y };
		if (step != std::pair<long, long>{ 0, 0 }) ++votes[step];
	}
	if (votes.empty()) return d;
	const auto common = std::ranges::max_element(votes, {}, [](const auto& v) { return v.second; })->first;

	// Everything that moved about as far, and the region where it was seen.
	std::vector<std::size_t> movers;
	float sx = 0, sy = 0;
	RECT region{};
	bool empty = true;
	for (const spot& s : newer) {
		const auto w = was.find(s.key);
		if (w == was.end()) continue;
		const long dx = s.at.x - w->second.x, dy = s.at.y - w->second.y;
		if (std::abs(dx - common.first) > close_enough || std::abs(dy - common.second) > close_enough) continue;
		movers.push_back(s.key);
		sx += static_cast<float>(dx);
		sy += static_cast<float>(dy);
		stretch(region, empty, w->second);
		stretch(region, empty, s.at);
	}
	// A few strays moving are not the content scrolling.
	if (movers.size() < 3 || movers.size() * 10 < static_cast<std::size_t>(matched) * 3) return d;

	const float seconds = std::chrono::duration<float>(dt).count();
	const float vx = sx / static_cast<float>(movers.size()) / seconds;
	const float vy = sy / static_cast<float>(movers.size()) / seconds;
	if (std::abs(vx) < slowest && std::abs(vy) < slowest) return d;

	if (before.streak > 0 && agree(vx, before.vx) && agree(vy, before.vy)) {
		d.streak = before.streak + 1;
		d.vx = (vx + before.vx) / 2;
		d.vy = (vy + before.vy) / 2;
		d.interval = (dt + before.interval) / 2;
	}
	else {
		d.streak = 1;
		d.vx = vx;
		d.vy = vy;
		d.interval = dt;
	}

	InflateRect(&region, margin, margin);
	d.region = region;
	d.keys.insert(movers.begin(), movers.end());
	// Items that just scrolled into view move along with the rest.
	for (const spot& s : newer)
		if (!was.contains(s.key) && PtInRect(&region, s.at)) d.keys.insert(s.key);
	return d;
}

std::optional<POINT> drift::offset(clock::duration since) const
{
	if (!moving() || since < clock::duration::zero() || since > interval * 2) return std::nullopt;
	const float t = std::chrono::duration<float>(std::min(since, interval)).count();
	return POINT{ std::lround(vx * t), std::lround(vy * t) };
}
