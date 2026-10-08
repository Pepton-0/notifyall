#include "badge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {

	struct rgb { double r, g, b; };

	constexpr rgb badge_red{ 220, 30, 40 };
	constexpr rgb white{ 255, 255, 255 };

	rgb mix(rgb from, rgb to, double t)
	{
		return { from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t };
	}

	std::uint32_t to_premultiplied_bgra(rgb c, double alpha)
	{
		const auto ch = [](double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 255.0))); };
		return (ch(alpha * 255) << 24) | (ch(c.r * alpha) << 16) | (ch(c.g * alpha) << 8) | ch(c.b * alpha);
	}

	// Square image, rows top to bottom.
	struct image {
		int size = 0;
		std::vector<std::uint32_t> bgra;

		std::uint32_t& at(int x, int y) { return bgra[static_cast<std::size_t>(y) * size + x]; }
	};

	BITMAPINFO top_down_32bit(int width, int height)
	{
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = width;
		bi.bmiHeader.biHeight = -height;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		return bi;
	}

	// GDI text has no alpha, so draw white on black and use the brightness as text coverage.
	std::vector<double> digit_coverage(int number, int size)
	{
		const BITMAPINFO bi = top_down_32bit(size, size);
		void* bits = nullptr;
		const HDC dc = CreateCompatibleDC(nullptr);
		const HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
		const HGDIOBJ old_bmp = SelectObject(dc, bmp);

		const HFONT font = CreateFontW(-static_cast<int>(size * 0.78), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
			DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
		const HGDIOBJ old_font = SelectObject(dc, font);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(255, 255, 255));

		const int segoe_baseline_lift = std::max(1, size / 16);
		RECT rc{ 0, -segoe_baseline_lift, size, size - segoe_baseline_lift };
		DrawTextW(dc, std::to_wstring(number).c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
		GdiFlush();

		std::vector<double> coverage(static_cast<std::size_t>(size) * size);
		const auto* px = static_cast<const std::uint32_t*>(bits);
		for (std::size_t i = 0; i < coverage.size(); ++i)
			coverage[i] = std::max({ px[i] & 0xFF, (px[i] >> 8) & 0xFF, (px[i] >> 16) & 0xFF }) / 255.0;

		SelectObject(dc, old_font);
		DeleteObject(font);
		SelectObject(dc, old_bmp);
		DeleteObject(bmp);
		DeleteDC(dc);
		return coverage;
	}

	// 1 inside the circle, 0 outside, with a one-pixel soft edge.
	double circle_coverage(int x, int y, int size)
	{
		const double center = (size - 1) / 2.0;
		const double radius = size / 2.0 - 0.5;
		return std::clamp(radius - std::hypot(x - center, y - center) + 0.5, 0.0, 1.0);
	}

	image render(int number, int size)
	{
		image img{ size, std::vector<std::uint32_t>(static_cast<std::size_t>(size) * size) };
		const auto text = digit_coverage(number, size);

		for (int y = 0; y < size; ++y) {
			for (int x = 0; x < size; ++x) {
				const rgb color = mix(badge_red, white, text[static_cast<std::size_t>(y) * size + x]);
				img.at(x, y) = to_premultiplied_bgra(color, circle_coverage(x, y, size));
			}
		}
		return img;
	}

	HBITMAP to_alpha_bitmap(const image& img)
	{
		BITMAPV5HEADER bh{};
		bh.bV5Size = sizeof(bh);
		bh.bV5Width = img.size;
		bh.bV5Height = -img.size;
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
		if (bmp) std::ranges::copy(img.bgra, static_cast<std::uint32_t*>(bits));
		return bmp;
	}

	std::uint32_t over_white(std::uint32_t premultiplied)
	{
		const std::uint32_t transparency = 255 - (premultiplied >> 24);
		const auto channel = [&](int shift) { return (((premultiplied >> shift) & 0xFF) + transparency) << shift; };
		return 0xFF000000 | channel(16) | channel(8) | channel(0);
	}

}

namespace badge_icon {

	HICON make(int number, int size)
	{
		ICONINFO ii{};
		ii.fIcon = TRUE;
		ii.hbmColor = to_alpha_bitmap(render(number, size));
		ii.hbmMask = CreateBitmap(size, size, 1, 1, nullptr);   // ignored when the color bitmap has alpha
		const HICON icon = CreateIconIndirect(&ii);

		DeleteObject(ii.hbmColor);
		DeleteObject(ii.hbmMask);
		return icon;
	}

	bool save_preview(const std::filesystem::path& bmp_file, int size)
	{
		const int width = size * 9;
		std::vector<std::uint32_t> strip(static_cast<std::size_t>(width) * size);

		for (int n = 1; n <= 9; ++n) {
			image img = render(n, size);
			for (int y = 0; y < size; ++y)
				for (int x = 0; x < size; ++x)
					strip[static_cast<std::size_t>(y) * width + (n - 1) * size + x] = over_white(img.at(x, y));
		}

		const BITMAPINFO bi = top_down_32bit(width, size);
		BITMAPFILEHEADER fh{};
		fh.bfType = 0x4D42;   // "BM"
		fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
		fh.bfSize = fh.bfOffBits + static_cast<DWORD>(strip.size() * sizeof(std::uint32_t));

		std::ofstream out(bmp_file, std::ios::binary);
		out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
		out.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(bi.bmiHeader));
		out.write(reinterpret_cast<const char*>(strip.data()), static_cast<std::streamsize>(strip.size() * sizeof(std::uint32_t)));
		return static_cast<bool>(out);
	}

}
