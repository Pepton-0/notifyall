#include "taskbar_items.h"

#include <format>
#include <map>
#include <string_view>

#include "logger.h"

#pragma comment(lib, "uiautomationcore.lib")

using Microsoft::WRL::ComPtr;

namespace {

	constexpr wchar_t taskbar_class[] = L"Shell_TrayWnd";

	// Windows 11 class names of the right-hand buttons; show-desktop is a sliver with nothing to badge.
	constexpr std::wstring_view tray_prefix = L"SystemTray.";
	constexpr std::wstring_view show_desktop = L"SystemTray.ShowDesktopButton";
	constexpr std::wstring_view clock_class = L"SystemTray.OmniButton";

	std::wstring take(BSTR s)
	{
		std::wstring out = s ? std::wstring(s, SysStringLen(s)) : std::wstring();
		SysFreeString(s);
		return out;
	}

	bool class_is(HWND hwnd, const wchar_t* name)
	{
		wchar_t cls[64]{};
		return GetClassNameW(hwnd, cls, 64) > 0 && std::wstring_view(cls) == name;
	}

}

std::optional<tray_snapshot> taskbar_items::snapshot()
{
	const HWND tray = FindWindowW(taskbar_class, nullptr);
	if (!tray || !ensure_automation()) return std::nullopt;

	tray_snapshot s;
	s.taskbar = tray;
	GetWindowRect(tray, &s.bounds);
	s.dpi = GetDpiForWindow(tray);

	ComPtr<IUIAutomationElement> root;
	ComPtr<IUIAutomationElementArray> found;
	if (FAILED(automation->ElementFromHandle(tray, root.GetAddressOf())) || !root) return s;
	if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, isButton.Get(), cache.Get(), found.GetAddressOf())) || !found) return s;

	int count = 0;
	found->get_Length(&count);
	std::map<std::wstring, int> seen;   // the automation ids repeat, so number them per class
	for (int i = 0; i < count; ++i) {
		ComPtr<IUIAutomationElement> e;
		if (FAILED(found->GetElement(i, e.GetAddressOf())) || !e) continue;

		BSTR raw = nullptr;
		e->get_CachedClassName(&raw);
		const std::wstring cls = take(raw);
		if (!cls.starts_with(tray_prefix) || cls == show_desktop) continue;

		tray_item item{ std::format(L"{}#{}", cls, seen[cls]++), {}, cls == clock_class };
		if (SUCCEEDED(e->get_CachedBoundingRectangle(&item.rect)) && item.rect.right > item.rect.left)
			s.items.push_back(std::move(item));
	}
	return s;
}

bool taskbar_items::visible_at(POINT pt)
{
	// Our overlays are click-through, so WindowFromPoint looks past them.
	const HWND top = GetAncestor(WindowFromPoint(pt), GA_ROOT);
	return top && class_is(top, taskbar_class);
}

void taskbar_items::disconnect()
{
	cache.Reset();
	isButton.Reset();
	automation.Reset();
}

bool taskbar_items::ensure_automation()
{
	if (automation) return true;

	ComPtr<IUIAutomation> a;
	if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(a.GetAddressOf())))) {
		lib::log("taskbar items: UI Automation is not available", true);
		return false;
	}

	VARIANT button{};
	button.vt = VT_I4;
	button.lVal = UIA_ButtonControlTypeId;
	if (FAILED(a->CreatePropertyCondition(UIA_ControlTypePropertyId, button, isButton.GetAddressOf()))) return false;

	// One cross-process round trip for all properties instead of one per property.
	if (FAILED(a->CreateCacheRequest(cache.GetAddressOf()))) return false;
	cache->AddProperty(UIA_ClassNamePropertyId);
	cache->AddProperty(UIA_BoundingRectanglePropertyId);

	automation = a;
	return true;
}
