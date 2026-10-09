#include "scene_test.h"

#include <windows.h>
#include <dwmapi.h>

#include <cstdio>
#include <format>
#include <fstream>
#include <vector>

#include "overlay.h"

#pragma comment(lib, "dwmapi.lib")

namespace scene_test {

	namespace {

		constexpr wchar_t class_name[] = L"notifyall.exp.scene_test";
		constexpr int width = 600, height = 400;

		enum class seen { badge, background, other };

		const char* name(seen s)
		{
			return s == seen::badge ? "badge" : s == seen::background ? "background" : "other";
		}

		struct probe {
			const char* what;
			POINT at;   // screen coordinates
			seen expected;
		};

		struct shot {
			RECT area{};
			std::vector<DWORD> pixels;   // top-down rows of 0x00RRGGBB

			DWORD at(POINT p) const
			{
				return pixels[static_cast<std::size_t>(p.y - area.top) * (area.right - area.left) + (p.x - area.left)];
			}
		};

		void pump(DWORD ms)
		{
			const auto until = GetTickCount64() + ms;
			MSG msg{};
			while (GetTickCount64() < until) {
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
				MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
			}
		}

		// What the screen shows once composition has caught up.
		shot capture(const RECT& r)
		{
			pump(300);
			DwmFlush();

			shot s{ r };
			const int w = r.right - r.left, h = r.bottom - r.top;
			const HDC screen = GetDC(nullptr);
			const HDC mem = CreateCompatibleDC(screen);
			BITMAPINFO bi{};
			bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
			void* bits = nullptr;
			const HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
			const HGDIOBJ old = SelectObject(mem, bmp);
			BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
			const auto* first = static_cast<const DWORD*>(bits);
			s.pixels.assign(first, first + static_cast<std::size_t>(w) * h);
			SelectObject(mem, old);
			DeleteObject(bmp);
			DeleteDC(mem);
			ReleaseDC(nullptr, screen);
			return s;
		}

		seen classify(DWORD p)
		{
			const int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
			if (r > 150 && g < 110 && b < 110) return seen::badge;        // the red circle
			if (r > 200 && g > 200 && b > 200) return seen::badge;        // its white digit
			if (g > 100 && r < 60 && b < 60) return seen::background;
			return seen::other;
		}

		void save(const shot& s, const std::wstring& path)
		{
			const int w = s.area.right - s.area.left, h = s.area.bottom - s.area.top;
			BITMAPINFOHEADER info{ sizeof(info), w, -h, 1, 32, BI_RGB };
			BITMAPFILEHEADER file{};
			file.bfType = 0x4D42;
			file.bfOffBits = sizeof(file) + sizeof(info);
			file.bfSize = file.bfOffBits + static_cast<DWORD>(s.pixels.size() * 4);
			std::ofstream f(path, std::ios::binary);
			f.write(reinterpret_cast<const char*>(&file), sizeof(file));
			f.write(reinterpret_cast<const char*>(&info), sizeof(info));
			f.write(reinterpret_cast<const char*>(s.pixels.data()), static_cast<std::streamsize>(s.pixels.size() * 4));
		}

		int check(const char* phase, const shot& s, const std::vector<probe>& probes)
		{
			int failures = 0;
			for (const probe& p : probes) {
				const DWORD pixel = s.at(p.at);
				const seen got = classify(pixel);
				failures += got != p.expected;
				std::printf("%s", std::format("{} {:<10} {:<24} expected {:<10} saw {} (#{:06X})\n",
					got == p.expected ? "ok  " : "FAIL", phase, p.what, name(p.expected), name(got), pixel).c_str());
			}
			return failures;
		}

		// Probes around the four badges of the test group, whose origin is at o.
		std::vector<probe> around(POINT o)
		{
			const auto at = [&](int x, int y) { return POINT{ o.x + x, o.y + y }; };
			return {
				{ "open", at(60, 52), seen::badge },
				{ "same picture again", at(60, 152), seen::badge },
				{ "half, shown side", at(293, 56), seen::badge },
				{ "half, covered side", at(307, 56), seen::background },
				{ "corner, top right", at(307, 193), seen::badge },
				{ "corner, bottom left", at(293, 207), seen::badge },
				{ "corner, bottom right", at(307, 207), seen::background },
				{ "between badges", at(180, 110), seen::background },
			};
		}

	}

	int run(const std::wstring& shot_path)
	{
		const HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc{ sizeof(wc) };
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = instance;
		wc.hbrBackground = CreateSolidBrush(RGB(0, 128, 0));
		wc.lpszClassName = class_name;
		RegisterClassExW(&wc);

		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const RECT area{ work.left + 200, work.top + 200, work.left + 200 + width, work.top + 200 + height };
		const HWND back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, class_name,
			L"notifyall scene test", WS_POPUP, area.left, area.top, width, height, nullptr, nullptr, instance, nullptr);
		if (!back) return 2;
		ShowWindow(back, SW_SHOWNOACTIVATE);
		ShowWindow(back, SW_SHOWNOACTIVATE);   // the launcher's show state may override the first call
		UpdateWindow(back);

		int failures = 0;
		{
			dx::device_source gpu;
			overlay layer(gpu);

			// Everything shows but two cut-outs: { 300, 40, 600, 80 } and { 300, 200, 600, 400 }.
			badge_group group{ back, { area.left, area.top },
				{ { 0, 0, 600, 40 }, { 0, 40, 300, 80 }, { 0, 80, 600, 200 }, { 0, 200, 300, 400 } },
				{ { 60, 60, 24, 3 }, { 60, 160, 24, 3 }, { 300, 60, 24, 5 }, { 300, 200, 24, 8 } } };

			layer.show_groups(nullptr, area, { group });
			shot s = capture(area);
			failures += check("shown", s, around(group.origin));
			if (!shot_path.empty()) save(s, shot_path + L"-1.bmp");

			const POINT before = group.origin;
			group.origin.x += 100;   // the window moved
			layer.show_groups(nullptr, area, { group });
			s = capture(area);
			auto probes = around(group.origin);
			probes.push_back({ "left behind", { before.x + 60, before.y + 52 }, seen::background });
			failures += check("moved", s, probes);
			if (!shot_path.empty()) save(s, shot_path + L"-2.bmp");

			// Another group alone: the first comes down, then returns as it was.
			const badge_group other{ reinterpret_cast<HWND>(1), { area.left, area.top }, { { 0, 0, width, height } },
				{ { 540, 340, 24, 9 } } };
			layer.show_groups(nullptr, area, { other });
			s = capture(area);
			failures += check("swapped", s, {
				{ "first group gone", { group.origin.x + 60, group.origin.y + 52 }, seen::background },
				{ "other group", { area.left + 540, area.top + 332 }, seen::badge } });

			layer.show_groups(nullptr, area, { group, other });
			s = capture(area);
			probes = around(group.origin);
			probes.push_back({ "other group", { area.left + 540, area.top + 332 }, seen::badge });
			failures += check("both", s, probes);
			if (!shot_path.empty()) save(s, shot_path + L"-3.bmp");
		}

		DestroyWindow(back);
		std::printf("%s", std::format("{} failure(s)\n", failures).c_str());
		return failures == 0 ? 0 : 1;
	}

}
