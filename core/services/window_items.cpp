#include "window_items.h"

#include <algorithm>
#include <array>
#include <string_view>

#include <dwmapi.h>

#include "app_windows.h"
#include "logger.h"

#pragma comment(lib, "uiautomationcore.lib")

using Microsoft::WRL::ComPtr;

namespace {

	// Other injections cover the shell, and Start can't be drawn over anyway.
	constexpr std::array<std::wstring_view, 5> shell_classes{
		L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW", L"Windows.UI.Core.CoreWindow",
	};

	bool is_shell(HWND window)
	{
		wchar_t cls[64]{};
		const std::wstring_view name(cls, static_cast<std::size_t>(GetClassNameW(window, cls, 64)));
		for (const auto s : shell_classes) if (name == s) return true;
		return false;
	}

	bool ours(HWND window)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(window, &pid);
		return pid == GetCurrentProcessId();
	}

	std::wstring runtime_id(IUIAutomationElement* e)
	{
		VARIANT v{};
		if (FAILED(e->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &v))) return {};

		std::wstring key;
		if (v.vt == (VT_ARRAY | VT_I4) && v.parray) {
			LONG lower = 0, upper = -1;
			SafeArrayGetLBound(v.parray, 1, &lower);
			SafeArrayGetUBound(v.parray, 1, &upper);
			for (LONG i = lower; i <= upper; ++i) {
				int part = 0;
				SafeArrayGetElement(v.parray, &i, &part);
				key += std::to_wstring(part) + L'.';
			}
		}
		VariantClear(&v);
		return key;
	}

	RECT frame_of(HWND window)
	{
		RECT r{};
		if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(window, &r);
		return r;
	}

	bool inside(const RECT& r, const RECT& bounds)
	{
		return r.left >= bounds.left && r.top >= bounds.top && r.right <= bounds.right && r.bottom <= bounds.bottom;
	}

}

std::vector<HWND> window_items::candidates()
{
	// Minimized and cloaked windows stay in, so their badges are ready when they come back.
	const auto windows = app_windows::list(true);
	const HWND focused = front();
	{
		std::lock_guard guard(lock);
		std::erase_if(elevation, [&](const auto& e) {
			return e.first != focused && std::ranges::find(windows, e.first) == windows.end();
		});
	}

	std::vector<HWND> out;
	if (focused) out.push_back(focused);
	for (const HWND w : windows)
		if (w != focused && eligible(w)) out.push_back(w);
	return out;
}

HWND window_items::front()
{
	const HWND w = GetAncestor(GetForegroundWindow(), GA_ROOT);
	return w && eligible(w) ? w : nullptr;
}

bool window_items::eligible(HWND window)
{
	return IsWindowVisible(window) && !is_shell(window) && !ours(window) && !elevated(window);
}

bool window_items::elevated(HWND window)
{
	{
		std::lock_guard guard(lock);
		if (const auto known = elevation.find(window); known != elevation.end()) return known->second;
	}
	const bool admin = app_windows::elevated(window);
	std::lock_guard guard(lock);
	elevation[window] = admin;
	return admin;
}

std::optional<window_snapshot> window_items::query(HWND window)
{
	session* s = this_threads_session();
	if (!IsWindow(window) || !s) return std::nullopt;

	window_snapshot snap;
	snap.window = window;
	snap.frame = frame_of(window);
	snap.dpi = GetDpiForWindow(window);

	ComPtr<IUIAutomationElement> root;
	ComPtr<IUIAutomationElementArray> found;
	if (FAILED(s->automation->ElementFromHandle(window, root.GetAddressOf())) || !root) return snap;
	if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, s->isItem.Get(), s->cache.Get(), found.GetAddressOf())) || !found) return snap;

	const int min_size = MulDiv(8, snap.dpi, USER_DEFAULT_SCREEN_DPI);
	int count = 0;
	found->get_Length(&count);
	for (int i = 0; i < count && snap.items.size() < item_limit; ++i) {
		ComPtr<IUIAutomationElement> e;
		if (FAILED(found->GetElement(i, e.GetAddressOf())) || !e) continue;

		window_item item;
		if (FAILED(e->get_CachedBoundingRectangle(&item.rect))) continue;
		if (item.rect.right - item.rect.left < min_size || item.rect.bottom - item.rect.top < min_size) continue;
		if (!inside(item.rect, snap.frame)) continue;

		item.id = runtime_id(e.Get());
		snap.items.push_back(std::move(item));
	}

	const RECT after = frame_of(window);
	snap.settled = EqualRect(&after, &snap.frame) != FALSE;
	return snap;
}

void window_items::watch(const window_snapshot& snap)
{
	session* s = this_threads_session();
	if (!s) return;
	s->watched = {};
	if (snap.items.empty()) return;

	// The element nearest the middle most likely sits in the content that scrolls.
	const POINT middle{ (snap.frame.left + snap.frame.right) / 2, (snap.frame.top + snap.frame.bottom) / 2 };
	const auto centre = [](const RECT& r) { return POINT{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 }; };
	const auto distance = [&](const window_item& i) {
		const POINT c = centre(i.rect);
		return static_cast<long long>(c.x - middle.x) * (c.x - middle.x) + static_cast<long long>(c.y - middle.y) * (c.y - middle.y);
	};
	const POINT at = centre(std::ranges::min_element(snap.items, {}, distance)->rect);

	// Whatever shows at that spot, as long as it is the window's own and not something above it.
	ComPtr<IUIAutomationElement> e;
	int pid = 0;
	DWORD owner = 0;
	GetWindowThreadProcessId(snap.window, &owner);
	if (FAILED(s->automation->ElementFromPoint(at, e.GetAddressOf())) || !e) return;
	if (FAILED(e->get_CurrentProcessId(&pid)) || static_cast<DWORD>(pid) != owner) return;
	RECT r{};
	if (FAILED(e->get_CurrentBoundingRectangle(&r))) return;
	s->watched = { snap.window, e, r, at };
}

std::optional<bool> window_items::moved(HWND window)
{
	session* s = this_threads_session();
	if (!s || s->watched.window != window || !s->watched.element) return std::nullopt;

	// Asking what sits at that spot makes some apps bring positions up to date; Chromium otherwise
	// refreshes them only twice a second while scrolling.
	ComPtr<IUIAutomationElement> there;
	s->automation->ElementFromPoint(s->watched.spot, there.GetAddressOf());
	RECT now{};
	if (FAILED(s->watched.element->get_CurrentBoundingRectangle(&now))) {
		s->watched = {};
		return true;   // it went away: something surely changed
	}
	return EqualRect(&now, &s->watched.rect) == FALSE;
}

void window_items::disconnect()
{
	session gone;
	{
		std::lock_guard guard(lock);
		const auto mine = sessions.find(GetCurrentThreadId());
		if (mine == sessions.end()) return;
		gone = std::move(mine->second);
		sessions.erase(mine);
	}
	// gone releases its objects here, on the thread that made them
}

window_items::session* window_items::this_threads_session()
{
	const DWORD thread = GetCurrentThreadId();
	{
		std::lock_guard guard(lock);
		if (const auto mine = sessions.find(thread); mine != sessions.end()) return &mine->second;
	}
	session made;
	if (!open(made)) return nullptr;

	// Elements stay put when the map grows, so the pointer holds until disconnect().
	std::lock_guard guard(lock);
	return &(sessions[thread] = std::move(made));
}

bool window_items::open(session& s)
{
	ComPtr<IUIAutomation> a;
	if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(a.GetAddressOf())))) {
		lib::log("window items: UI Automation is not available", true);
		return false;
	}

	constexpr std::array<CONTROLTYPEID, 5> types{
		UIA_ListItemControlTypeId, UIA_TreeItemControlTypeId, UIA_TabItemControlTypeId,
		UIA_ButtonControlTypeId, UIA_MenuItemControlTypeId,
	};
	std::array<IUIAutomationCondition*, types.size()> each{};
	for (std::size_t i = 0; i < types.size(); ++i) {
		VARIANT v{};
		v.vt = VT_I4;
		v.lVal = types[i];
		a->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &each[i]);
	}
	ComPtr<IUIAutomationCondition> anyType;
	const HRESULT made = a->CreateOrConditionFromNativeArray(each.data(), static_cast<int>(each.size()), anyType.GetAddressOf());
	for (auto* c : each) if (c) c->Release();
	if (FAILED(made)) return false;

	// Let the app drop off-screen elements itself, so they never cross the process boundary.
	VARIANT no{};
	no.vt = VT_BOOL;
	no.boolVal = VARIANT_FALSE;
	ComPtr<IUIAutomationCondition> onScreen;
	if (FAILED(a->CreatePropertyCondition(UIA_IsOffscreenPropertyId, no, onScreen.GetAddressOf()))) return false;
	if (FAILED(a->CreateAndCondition(anyType.Get(), onScreen.Get(), s.isItem.GetAddressOf()))) return false;

	// Values only, in one round trip: no live element references to build and release afterwards.
	if (FAILED(a->CreateCacheRequest(s.cache.GetAddressOf()))) return false;
	s.cache->AddProperty(UIA_BoundingRectanglePropertyId);
	s.cache->AddProperty(UIA_RuntimeIdPropertyId);
	s.cache->put_AutomationElementMode(AutomationElementMode_None);

	// A hung app must not hold the worker for long.
	ComPtr<IUIAutomation2> a2;
	if (SUCCEEDED(a.As(&a2))) {
		a2->put_ConnectionTimeout(1000);
		a2->put_TransactionTimeout(1000);
	}

	s.automation = a;
	return true;
}
