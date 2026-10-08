#include "window_items_exp.h"

#include <array>

#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#pragma comment(lib, "uiautomationcore.lib")

using Microsoft::WRL::ComPtr;

namespace window_items_exp {

	std::vector<RECT> read(HWND window, std::size_t limit)
	{
		static ComPtr<IUIAutomation> automation;
		if (!automation && FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(automation.GetAddressOf())))) return {};

		ComPtr<IUIAutomationElement> root;
		if (FAILED(automation->ElementFromHandle(window, root.GetAddressOf())) || !root) return {};

		constexpr std::array<CONTROLTYPEID, 5> types{
			UIA_ListItemControlTypeId, UIA_TreeItemControlTypeId, UIA_TabItemControlTypeId,
			UIA_ButtonControlTypeId, UIA_MenuItemControlTypeId,
		};
		std::array<IUIAutomationCondition*, types.size()> each{};
		for (std::size_t i = 0; i < types.size(); ++i) {
			VARIANT v{};
			v.vt = VT_I4;
			v.lVal = types[i];
			automation->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &each[i]);
		}
		ComPtr<IUIAutomationCondition> any;
		automation->CreateOrConditionFromNativeArray(each.data(), static_cast<int>(each.size()), any.GetAddressOf());
		for (auto* c : each) if (c) c->Release();

		ComPtr<IUIAutomationCacheRequest> cache;
		automation->CreateCacheRequest(cache.GetAddressOf());
		cache->AddProperty(UIA_BoundingRectanglePropertyId);
		cache->AddProperty(UIA_IsOffscreenPropertyId);

		ComPtr<IUIAutomationElementArray> found;
		if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, any.Get(), cache.Get(), found.GetAddressOf())) || !found) return {};

		RECT bounds{};
		GetWindowRect(window, &bounds);

		int count = 0;
		found->get_Length(&count);
		std::vector<RECT> out;
		for (int i = 0; i < count && out.size() < limit; ++i) {
			ComPtr<IUIAutomationElement> e;
			if (FAILED(found->GetElement(i, e.GetAddressOf())) || !e) continue;

			BOOL offscreen = FALSE;
			RECT r{};
			e->get_CachedIsOffscreen(&offscreen);
			if (offscreen || FAILED(e->get_CachedBoundingRectangle(&r))) continue;
			if (r.right - r.left < 8 || r.bottom - r.top < 8) continue;
			if (r.left < bounds.left || r.top < bounds.top || r.right > bounds.right || r.bottom > bounds.bottom) continue;
			out.push_back(r);
		}
		return out;
	}

}
