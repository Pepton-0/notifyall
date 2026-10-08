#include "taskbar_items.h"

#include <string_view>

#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#pragma comment(lib, "uiautomationcore.lib")

using Microsoft::WRL::ComPtr;

namespace taskbar_items {

	std::vector<item> read(HWND tray)
	{
		static ComPtr<IUIAutomation> automation;
		if (!automation && FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(automation.GetAddressOf())))) return {};

		ComPtr<IUIAutomationElement> root;
		if (FAILED(automation->ElementFromHandle(tray, root.GetAddressOf())) || !root) return {};

		VARIANT button{};
		button.vt = VT_I4;
		button.lVal = UIA_ButtonControlTypeId;
		ComPtr<IUIAutomationCondition> is_button;
		ComPtr<IUIAutomationCacheRequest> cache;
		automation->CreatePropertyCondition(UIA_ControlTypePropertyId, button, is_button.GetAddressOf());
		automation->CreateCacheRequest(cache.GetAddressOf());
		cache->AddProperty(UIA_AutomationIdPropertyId);
		cache->AddProperty(UIA_ClassNamePropertyId);
		cache->AddProperty(UIA_BoundingRectanglePropertyId);

		ComPtr<IUIAutomationElementArray> found;
		if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, is_button.Get(), cache.Get(), found.GetAddressOf())) || !found) return {};

		int count = 0;
		found->get_Length(&count);
		std::vector<item> out;
		for (int i = 0; i < count; ++i) {
			ComPtr<IUIAutomationElement> e;
			if (FAILED(found->GetElement(i, e.GetAddressOf()))) continue;

			BSTR id = nullptr, cls = nullptr;
			e->get_CachedAutomationId(&id);
			e->get_CachedClassName(&cls);
			const std::wstring_view id_view = id ? id : L"";
			const std::wstring_view cls_view = cls ? cls : L"";
			const bool app = id_view.starts_with(L"Appid:");
			const bool right = cls_view.starts_with(L"SystemTray.") && cls_view != L"SystemTray.ShowDesktopButton";
			SysFreeString(id);
			SysFreeString(cls);
			if (!app && !right) continue;

			item it{ {}, right };
			if (SUCCEEDED(e->get_CachedBoundingRectangle(&it.rect)) && it.rect.right > it.rect.left) out.push_back(it);
		}
		return out;
	}

}
