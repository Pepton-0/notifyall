#include "overlay.h"

#include <cmath>
#include <format>
#include <string>

#include "logger.h"

using Microsoft::WRL::ComPtr;

namespace {

	constexpr wchar_t class_name[] = L"notifyall.overlay";

	const D2D1_COLOR_F badge_red = D2D1::ColorF(220 / 255.f, 30 / 255.f, 40 / 255.f);
	const D2D1_COLOR_F digit_white = D2D1::ColorF(D2D1::ColorF::White);

	bool same_rect(const RECT& a, const RECT& b)
	{
		return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
	}

	UINT width_of(const RECT& r) { return static_cast<UINT>(std::max(1L, r.right - r.left)); }
	UINT height_of(const RECT& r) { return static_cast<UINT>(std::max(1L, r.bottom - r.top)); }

}

overlay::~overlay()
{
	destroy_window();
}

void overlay::show(HWND owner, const RECT& area, const std::vector<badge>& badges)
{
	show_groups(owner, area, { badge_group{ { area }, badges } });
}

void overlay::show_groups(HWND owner, const RECT& area, const std::vector<badge_group>& groups)
{
	if (!ensure_ready(owner)) return;

	const bool moved = !same_rect(area, area_);
	if (moved) {
		area_ = area;
		layer->resize(width_of(area), height_of(area));
		painted = false;
	}

	// Touch the z-order as little as possible: explorer is rearranging the same windows.
	if (moved || !shown_ || !above_owner()) place();

	if (!painted || groups != groups_) {
		groups_ = groups;
		painted = paint();
	}

	if (!shown_) {
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		shown_ = true;
	}
}

void overlay::hide()
{
	if (!shown_) return;
	ShowWindow(hwnd, SW_HIDE);
	shown_ = false;
	painted = false;   // composition content isn't guaranteed to survive hiding
}

void overlay::place()
{
	// Slot in right above the owner, i.e. below whatever sits directly above it; a topmost owner (the taskbar)
	// rises with its owned windows anyway. NOOWNERZORDER leaves the owner alone.
	const bool topmost_owner = !owner_ || (GetWindowLongPtrW(owner_, GWL_EXSTYLE) & WS_EX_TOPMOST);
	const HWND above = GetWindow(owner_, GW_HWNDPREV);
	UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
	HWND after = topmost_owner ? HWND_TOPMOST : (above ? above : HWND_TOP);
	if (!topmost_owner && above == hwnd) flags |= SWP_NOZORDER;   // already in place
	SetWindowPos(hwnd, after, area_.left, area_.top,
		static_cast<int>(width_of(area_)), static_cast<int>(height_of(area_)), flags);
}

bool overlay::above_owner() const
{
	if (!owner_) return true;   // free-standing: windows above it are clipped out instead
	constexpr int search_limit = 256;
	int steps = 0;
	for (HWND w = GetWindow(owner_, GW_HWNDPREV); w && steps < search_limit; w = GetWindow(w, GW_HWNDPREV), ++steps)
		if (w == hwnd) return true;
	return false;
}

bool overlay::ensure_ready(HWND owner)
{
	if (layer && owner == owner_ && IsWindow(hwnd)) return true;
	if (broken) return false;

	destroy_window();
	if (!create_window(owner)) return false;

	if (!device) device = gpu.get();
	if (device) layer = dx::layer::create(*device, hwnd, 1, 1);
	if (!layer) {
		broken = true;
		destroy_window();
		return false;
	}

	lib::log("overlay: ready", true);
	return true;
}

bool overlay::create_window(HWND owner)
{
	const HINSTANCE instance = GetModuleHandleW(nullptr);

	WNDCLASSEXW wc{ sizeof(wc) };
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = instance;
	wc.lpszClassName = class_name;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		lib::log(std::format("overlay: RegisterClassExW failed: {}", GetLastError()), true);
		return false;
	}

	// LAYERED + TRANSPARENT makes clicks fall through; NOREDIRECTIONBITMAP leaves drawing to composition.
	constexpr DWORD ex_style = WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT
		| WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
	hwnd = CreateWindowExW(ex_style, class_name, L"notifyall overlay", WS_POPUP,
		0, 0, 1, 1, owner, nullptr, instance, nullptr);
	if (!hwnd) {
		lib::log(std::format("overlay: CreateWindowExW failed: {}", GetLastError()), true);
		return false;
	}

	SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
	owner_ = owner;
	area_ = {};
	painted = false;
	return true;
}

void overlay::destroy_window()
{
	layer.reset();
	if (hwnd) DestroyWindow(hwnd);
	hwnd = nullptr;
	owner_ = nullptr;
	shown_ = false;
}

bool overlay::paint()
{
	return layer->draw([&](ID2D1DeviceContext& ctx) {
		ComPtr<ID2D1SolidColorBrush> red, white;
		ctx.CreateSolidColorBrush(badge_red, red.GetAddressOf());
		ctx.CreateSolidColorBrush(digit_white, white.GetAddressOf());

		const float ox = static_cast<float>(area_.left);
		const float oy = static_cast<float>(area_.top);
		for (const badge_group& group : groups_) {
			for (const RECT& v : group.visible) {
				const D2D1_RECT_F clip = D2D1::RectF(v.left - ox, v.top - oy, v.right - ox, v.bottom - oy);
				ctx.PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
				for (const badge& b : group.badges) {
					const float x = b.x - ox;
					const float y = b.y - oy;
					const float r = b.diameter / 2;
					if (x + r < clip.left || x - r > clip.right || y + r < clip.top || y - r > clip.bottom) continue;

					ctx.FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), red.Get());
					const std::wstring digit = std::to_wstring(b.number);
					ctx.DrawText(digit.c_str(), static_cast<UINT32>(digit.size()), text_format(b.diameter),
						D2D1::RectF(x - r, y - r, x + r, y + r), white.Get());
				}
				ctx.PopAxisAlignedClip();
			}
		}
	});
}

IDWriteTextFormat* overlay::text_format(float diameter)
{
	const int size = std::max(1, static_cast<int>(std::lround(diameter * 0.72f)));
	auto& format = formats[size];
	if (format) return format.Get();

	device->dwrite().CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(size), L"en-us", format.GetAddressOf());
	if (format) {
		format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}
	return format.Get();
}
