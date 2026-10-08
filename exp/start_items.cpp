#include "start_items.h"

#include <string_view>

#include <ole2.h>
#include <UIAutomation.h>
#include <dwmapi.h>
#include <wrl/client.h>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

	std::wstring_view exe_name(DWORD pid, wchar_t (&buffer)[MAX_PATH])
	{
		const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process) return {};
		DWORD len = MAX_PATH;
		const bool ok = QueryFullProcessImageNameW(process, 0, buffer, &len);
		CloseHandle(process);
		if (!ok) return {};
		const std::wstring_view path(buffer, len);
		return path.substr(path.find_last_of(L'\\') + 1);
	}

}

namespace start_items {

	HWND window()
	{
		// The title is localized, so pick the CoreWindow by its process instead.
		HWND w = nullptr;
		while ((w = FindWindowExW(nullptr, w, L"Windows.UI.Core.CoreWindow", nullptr))) {
			DWORD pid = 0;
			GetWindowThreadProcessId(w, &pid);
			wchar_t buffer[MAX_PATH]{};
			if (_wcsicmp(exe_name(pid, buffer).data(), L"StartMenuExperienceHost.exe") == 0) return w;
		}
		return nullptr;
	}

	bool open(HWND start)
	{
		DWORD cloaked = 0;
		return start && SUCCEEDED(DwmGetWindowAttribute(start, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && !cloaked;
	}

	std::vector<RECT> read(HWND start)
	{
		static ComPtr<IUIAutomation> automation;
		if (!automation && FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(automation.GetAddressOf())))) return {};

		ComPtr<IUIAutomationElement> root;
		if (FAILED(automation->ElementFromHandle(start, root.GetAddressOf())) || !root) return {};

		VARIANT item{};
		item.vt = VT_I4;
		item.lVal = UIA_ListItemControlTypeId;
		ComPtr<IUIAutomationCondition> is_item;
		ComPtr<IUIAutomationCacheRequest> cache;
		automation->CreatePropertyCondition(UIA_ControlTypePropertyId, item, is_item.GetAddressOf());
		automation->CreateCacheRequest(cache.GetAddressOf());
		cache->AddProperty(UIA_BoundingRectanglePropertyId);

		ComPtr<IUIAutomationElementArray> found;
		if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, is_item.Get(), cache.Get(), found.GetAddressOf())) || !found) return {};

		int count = 0;
		found->get_Length(&count);
		std::vector<RECT> out;
		for (int i = 0; i < count; ++i) {
			ComPtr<IUIAutomationElement> e;
			RECT r{};
			if (SUCCEEDED(found->GetElement(i, e.GetAddressOf())) && SUCCEEDED(e->get_CachedBoundingRectangle(&r)) && r.right > r.left)
				out.push_back(r);
		}
		return out;
	}

}
