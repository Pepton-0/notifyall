#include "overlay.h"

#include <algorithm>
#include <format>

#include "logger.h"

namespace {

	constexpr wchar_t class_name[] = L"notifyall.overlay";

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
	badge_group all{ nullptr, { area.left, area.top }, { { 0, 0, area.right - area.left, area.bottom - area.top } } };
	all.badges.reserve(badges.size());
	for (badge b : badges) {
		b.x -= area.left;
		b.y -= area.top;
		all.badges.push_back(b);
	}
	show_groups(owner, area, { all });
}

void overlay::show_groups(HWND owner, const RECT& area, const std::vector<badge_group>& groups)
{
	if (!ensure_ready(owner)) return;

	const bool moved = !same_rect(area, area_);
	area_ = area;

	// Touch the z-order as little as possible: explorer is rearranging the same windows.
	if (moved || !shown_ || !above_owner() || under_any(groups)) place();
	if (scene->update(groups, { area.left, area.top })) layer->commit();

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

bool overlay::under_any(const std::vector<badge_group>& groups) const
{
	// A free-standing layer sits on top of the topmost windows, until a topmost window that has badges
	// itself comes up later and covers it.
	if (owner_) return false;
	constexpr int search_limit = 256;
	int steps = 0;
	for (HWND w = GetWindow(hwnd, GW_HWNDPREV); w && steps < search_limit; w = GetWindow(w, GW_HWNDPREV), ++steps)
		if (std::ranges::any_of(groups, [w](const badge_group& g) { return g.key == w; })) return true;
	return false;
}

bool overlay::ensure_ready(HWND owner)
{
	if (scene && owner == owner_ && IsWindow(hwnd)) return true;
	if (broken) return false;

	destroy_window();
	if (!create_window(owner)) return false;

	if (!device) device = gpu.get();
	if (device) layer = dx::layer::create(*device, hwnd);
	if (!layer) {
		broken = true;
		destroy_window();
		return false;
	}
	scene = std::make_unique<badge_scene>(*device, layer->root());

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
	return true;
}

void overlay::destroy_window()
{
	scene.reset();
	layer.reset();
	if (hwnd) DestroyWindow(hwnd);
	hwnd = nullptr;
	owner_ = nullptr;
	shown_ = false;
}
