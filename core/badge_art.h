#pragma once
// CPU-side pictures for the injection methods that hand Windows an HICON or HCURSOR.

#include <cstdint>
#include <vector>

#include <windows.h>

namespace badge_art {

	// Premultiplied BGRA, rows top to bottom.
	struct image {
		int width = 0;
		int height = 0;
		std::vector<std::uint32_t> pixels;

		image() = default;
		image(int w, int h) : width(w), height(h), pixels(static_cast<std::size_t>(w) * h) {}

		std::uint32_t& at(int x, int y) { return pixels[static_cast<std::size_t>(y) * width + x]; }
		std::uint32_t at(int x, int y) const { return pixels[static_cast<std::size_t>(y) * width + x]; }
	};

	// A white digit on a red circle, size x size.
	image badge(int number, int size);

	// Blends src over dst with its top-left at (x, y); parts outside dst are dropped.
	void draw_over(image& dst, const image& src, int x, int y);

	// The cursor as drawn by Windows. Inverting (XOR) pixels come out opaque.
	image capture(HCURSOR cursor, int width, int height);

	// The caller owns the result and destroys it with DestroyIcon / DestroyCursor.
	HICON to_icon(const image& img);
	HCURSOR to_cursor(const image& img, POINT hotspot);

}
