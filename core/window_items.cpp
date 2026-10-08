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
	const auto eligible = [this](HWND w) {
		return IsWindowVisible(w) && !IsIconic(w) && !is_shell(w) && !ours(w)
			&& !app_windows::cloaked(w) && !elevated(w);
	};

	const auto windows = app_windows::list();
	std::erase_if(elevation, [&](const auto& e) { return std::ranges::find(windows, e.first) == windows.end(); });

	std::vector<HWND> out;
	const HWND front = GetAncestor(GetForegroundWindow(), GA_ROOT);
	if (front && eligible(front)) out.push_back(front);
	for (const HWND w : windows)
		if (w != front && eligible(w)) out.push_back(w);
	return out;
}

bool window_items::elevated(HWND window)
{
	const auto [itr, added] = elevation.try_emplace(window, false);
	if (added) itr->second = app_windows::elevated(window);
	return itr->second;
}

std::optional<window_snapshot> window_items::query(HWND window)
{
	if (!IsWindow(window) || !ensure_automation()) return std::nullopt;

	window_snapshot s;
	s.window = window;
	s.frame = frame_of(window);
	s.dpi = GetDpiForWindow(window);

	ComPtr<IUIAutomationElement> root;
	ComPtr<IUIAutomationElementArray> found;
	if (FAILED(automation->ElementFromHandle(window, root.GetAddressOf())) || !root) return s;
	if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, isItem.Get(), cache.Get(), found.GetAddressOf())) || !found) return s;

	const int min_size = MulDiv(8, s.dpi, USER_DEFAULT_SCREEN_DPI);
	int count = 0;
	found->get_Length(&count);
	for (int i = 0; i < count && s.items.size() < item_limit; ++i) {
		ComPtr<IUIAutomationElement> e;
		if (FAILED(found->GetElement(i, e.GetAddressOf())) || !e) continue;

		window_item item;
		if (FAILED(e->get_CachedBoundingRectangle(&item.rect))) continue;
		if (item.rect.right - item.rect.left < min_size || item.rect.bottom - item.rect.top < min_size) continue;
		if (!inside(item.rect, s.frame)) continue;

		item.id = runtime_id(e.Get());
		s.items.push_back(std::move(item));
	}

	const RECT after = frame_of(window);
	s.settled = EqualRect(&after, &s.frame) != FALSE;
	return s;
}

void window_items::disconnect()
{
	cache.Reset();
	isItem.Reset();
	automation.Reset();
}

bool window_items::ensure_automation()
{
	if (automation) return true;

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
	if (FAILED(a->CreateAndCondition(anyType.Get(), onScreen.Get(), isItem.GetAddressOf()))) return false;

	// Values only, in one round trip: no live element references to build and release afterwards.
	if (FAILED(a->CreateCacheRequest(cache.GetAddressOf()))) return false;
	cache->AddProperty(UIA_BoundingRectanglePropertyId);
	cache->AddProperty(UIA_RuntimeIdPropertyId);
	cache->put_AutomationElementMode(AutomationElementMode_None);

	// A hung app must not hold the worker for long.
	ComPtr<IUIAutomation2> a2;
	if (SUCCEEDED(a.As(&a2))) {
		a2->put_ConnectionTimeout(1000);
		a2->put_TransactionTimeout(1000);
	}

	automation = a;
	return true;
}
