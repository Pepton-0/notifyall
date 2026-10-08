#include "occlusion.h"

#include <algorithm>

namespace occlusion {

	std::vector<RECT> subtract(const RECT& from, const RECT& cut)
	{
		RECT overlap{};
		if (!IntersectRect(&overlap, &from, &cut)) return { from };

		// Slice off bands above and below the overlap, then the pieces left and right of it.
		std::vector<RECT> out;
		if (from.top < overlap.top) out.push_back({ from.left, from.top, from.right, overlap.top });
		if (overlap.bottom < from.bottom) out.push_back({ from.left, overlap.bottom, from.right, from.bottom });
		if (from.left < overlap.left) out.push_back({ from.left, overlap.top, overlap.left, overlap.bottom });
		if (overlap.right < from.right) out.push_back({ overlap.right, overlap.top, from.right, overlap.bottom });
		return out;
	}

	std::unordered_map<HWND, std::vector<RECT>> visible_parts(const std::vector<stacked_window>& stack)
	{
		std::unordered_map<HWND, std::vector<RECT>> result;
		std::vector<RECT> covers;   // frames of the occluding windows seen so far, i.e. in front

		for (const stacked_window& w : stack) {
			std::vector<RECT> parts{ w.frame };
			for (const RECT& cover : covers) {
				std::vector<RECT> next;
				for (const RECT& p : parts) {
					auto pieces = subtract(p, cover);
					next.insert(next.end(), pieces.begin(), pieces.end());
				}
				parts.swap(next);
				if (parts.empty()) break;
			}
			if (!parts.empty()) result.emplace(w.hwnd, std::move(parts));
			if (w.occludes) covers.push_back(w.frame);
		}
		return result;
	}

}
