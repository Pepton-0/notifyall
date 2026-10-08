#include "badge_art.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

	struct rgb { double r, g, b; };

	constexpr rgb badge_red{ 220, 30, 40 };
	constexpr rgb white{ 255, 255, 255 };

	rgb mix(rgb from, rgb to, double t)
	{
		return { from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t };
	}

	std::uint32_t channel(double v)
	{
		return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 255.0)));
	}

	std::uint32_t premultiplied(rgb c, double alpha)
	{
		return (channel(alpha * 255) << 24) | (channel(c.r * alpha) << 16) | (channel(c.g * alpha) << 8) | channel(c.b * alpha);
	}

	// A 32-bit top-down DIB selected into a memory DC; everything is released on destruction.
	class canvas {
	public:
		canvas(int width, int height, std::uint32_t fill) : width(width), height(height)
		{
			BITMAPINFO bi{};
			bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
			bi.bmiHeader.biWidth = width;
			bi.bmiHeader.biHeight = -height;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;

			dc = CreateCompatibleDC(nullptr);
			bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
			old = SelectObject(dc, bmp);
			std::fill_n(bits, static_cast<std::size_t>(width) * height, fill);
		}
		~canvas()
		{
			SelectObject(dc, old);
			DeleteObject(bmp);
			DeleteDC(dc);
		}

		canvas(const canvas&) = delete;
		canvas& operator=(const canvas&) = delete;

		std::uint32_t pixel(int x, int y) const { GdiFlush(); return bits[static_cast<std::size_t>(y) * width + x]; }

		HDC dc = nullptr;
		int width, height;

	private:
		HBITMAP bmp = nullptr;
		HGDIOBJ old = nullptr;
		std::uint32_t* bits = nullptr;
	};

	int green(std::uint32_t p) { return static_cast<int>((p >> 8) & 0xFF); }
	int brightest(std::uint32_t p) { return std::max({ p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF }); }

	// GDI text has no alpha, so draw white on black and read the brightness back as coverage.
	std::vector<double> digit_coverage(int number, int size)
	{
		canvas c(size, size, 0xFF000000);

		const HFONT font = CreateFontW(-static_cast<int>(size * 0.78), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
			DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
		const HGDIOBJ old_font = SelectObject(c.dc, font);
		SetBkMode(c.dc, TRANSPARENT);
		SetTextColor(c.dc, RGB(255, 255, 255));

		const int segoe_baseline_lift = std::max(1, size / 16);
		RECT rc{ 0, -segoe_baseline_lift, size, size - segoe_baseline_lift };
		DrawTextW(c.dc, std::to_wstring(number).c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

		std::vector<double> coverage(static_cast<std::size_t>(size) * size);
		for (int y = 0; y < size; ++y)
			for (int x = 0; x < size; ++x)
				coverage[static_cast<std::size_t>(y) * size + x] = brightest(c.pixel(x, y)) / 255.0;

		SelectObject(c.dc, old_font);
		DeleteObject(font);
		return coverage;
	}

	// 1 inside the circle, 0 outside, with a one-pixel soft edge.
	double circle_coverage(int x, int y, int size)
	{
		const double center = (size - 1) / 2.0;
		const double radius = size / 2.0 - 0.5;
		return std::clamp(radius - std::hypot(x - center, y - center) + 0.5, 0.0, 1.0);
	}

	HBITMAP alpha_bitmap(const badge_art::image& img)
	{
		BITMAPV5HEADER bh{};
		bh.bV5Size = sizeof(bh);
		bh.bV5Width = img.width;
		bh.bV5Height = -img.height;
		bh.bV5Planes = 1;
		bh.bV5BitCount = 32;
		bh.bV5Compression = BI_BITFIELDS;
		bh.bV5RedMask = 0x00FF0000;
		bh.bV5GreenMask = 0x0000FF00;
		bh.bV5BlueMask = 0x000000FF;
		bh.bV5AlphaMask = 0xFF000000;

		void* bits = nullptr;
		const HDC screen = GetDC(nullptr);
		const HBITMAP bmp = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
		ReleaseDC(nullptr, screen);
		if (bmp) std::ranges::copy(img.pixels, static_cast<std::uint32_t*>(bits));
		return bmp;
	}

	HICON make_icon(const badge_art::image& img, BOOL is_icon, POINT hotspot)
	{
		ICONINFO ii{};
		ii.fIcon = is_icon;
		ii.xHotspot = static_cast<DWORD>(hotspot.x);
		ii.yHotspot = static_cast<DWORD>(hotspot.y);
		ii.hbmColor = alpha_bitmap(img);
		ii.hbmMask = CreateBitmap(img.width, img.height, 1, 1, nullptr);   // ignored when the color bitmap has alpha
		const HICON icon = CreateIconIndirect(&ii);
		DeleteObject(ii.hbmColor);
		DeleteObject(ii.hbmMask);
		return icon;
	}

}

namespace badge_art {

	image badge(int number, int size)
	{
		image img(size, size);
		const auto text = digit_coverage(number, size);

		for (int y = 0; y < size; ++y)
			for (int x = 0; x < size; ++x)
				img.at(x, y) = premultiplied(mix(badge_red, white, text[static_cast<std::size_t>(y) * size + x]),
					circle_coverage(x, y, size));
		return img;
	}

	void draw_over(image& dst, const image& src, int ox, int oy)
	{
		for (int y = 0; y < src.height; ++y) {
			for (int x = 0; x < src.width; ++x) {
				const int dx = ox + x, dy = oy + y;
				if (dx < 0 || dy < 0 || dx >= dst.width || dy >= dst.height) continue;

				const std::uint32_t s = src.at(x, y);
				const std::uint32_t d = dst.at(dx, dy);
				const std::uint32_t keep = 255 - (s >> 24);
				std::uint32_t out = 0;
				for (int shift = 0; shift < 32; shift += 8)
					out |= std::min(255u, ((s >> shift) & 0xFF) + ((d >> shift) & 0xFF) * keep / 255) << shift;
				dst.at(dx, dy) = out;
			}
		}
	}

	image capture(HCURSOR cursor, int width, int height)
	{
		// Drawn over black the pixel is color * alpha; over white it gains (1 - alpha) of white.
		canvas on_black(width, height, 0xFF000000);
		canvas on_white(width, height, 0xFFFFFFFF);
		DrawIconEx(on_black.dc, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
		DrawIconEx(on_white.dc, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);

		image img(width, height);
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const std::uint32_t b = on_black.pixel(x, y);
				const int alpha = std::clamp(255 - (green(on_white.pixel(x, y)) - green(b)), 0, 255);
				const std::uint32_t a = static_cast<std::uint32_t>(alpha);
				const auto ch = [&](int shift) { return std::min(a, (b >> shift) & 0xFF) << shift; };
				img.at(x, y) = (a << 24) | ch(16) | ch(8) | ch(0);
			}
		}
		return img;
	}

	HICON to_icon(const image& img)
	{
		return make_icon(img, TRUE, {});
	}

	HCURSOR to_cursor(const image& img, POINT hotspot)
	{
		return static_cast<HCURSOR>(make_icon(img, FALSE, hotspot));
	}

}
