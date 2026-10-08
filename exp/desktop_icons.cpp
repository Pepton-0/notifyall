#include "desktop_icons.h"

#include <ole2.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

	ComPtr<IFolderView2> desktop_view(HWND& view_window)
	{
		ComPtr<IShellWindows> windows;
		if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.GetAddressOf())))) return nullptr;

		VARIANT empty{};
		long hwnd = 0;
		ComPtr<IDispatch> dispatch;
		if (FAILED(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, dispatch.GetAddressOf())) || !dispatch) return nullptr;

		ComPtr<IShellBrowser> browser;
		ComPtr<IShellView> view;
		ComPtr<IFolderView2> folder;
		if (FAILED(IUnknown_QueryService(dispatch.Get(), SID_STopLevelBrowser, IID_PPV_ARGS(browser.GetAddressOf())))) return nullptr;
		if (FAILED(browser->QueryActiveShellView(view.GetAddressOf()))) return nullptr;
		if (FAILED(view.As(&folder))) return nullptr;

		view->GetWindow(&view_window);
		return folder;
	}

	std::wstring display_name(PCUITEMID_CHILD item, IShellFolder* parent)
	{
		STRRET ret{};
		wchar_t name[MAX_PATH]{};
		if (FAILED(parent->GetDisplayNameOf(item, SHGDN_NORMAL, &ret))) return {};
		StrRetToBufW(&ret, item, name, MAX_PATH);
		return name;
	}

}

namespace desktop {

	layout read()
	{
		layout out;
		HWND view_window = nullptr;
		const auto folder = desktop_view(view_window);
		if (!folder) return out;

		out.list_view = FindWindowExW(view_window, nullptr, L"SysListView32", nullptr);

		FOLDERVIEWMODE mode{};
		folder->GetViewModeAndIconSize(&mode, &out.icon_size);
		folder->GetSpacing(&out.spacing);

		// The view reports the icon size in logical pixels.
		const UINT dpi = GetDpiForWindow(out.list_view);
		const int icon_px = MulDiv(out.icon_size, dpi, 96);

		ComPtr<IShellFolder> parent;
		folder->GetFolder(IID_PPV_ARGS(parent.GetAddressOf()));

		int count = 0;
		folder->ItemCount(SVGIO_ALLVIEW, &count);
		for (int i = 0; i < count; ++i) {
			PITEMID_CHILD item = nullptr;
			if (FAILED(folder->Item(i, &item))) continue;

			POINT pos{};
			if (SUCCEEDED(folder->GetItemPosition(item, &pos))) {
				// The position is the item's top-left in list view client coordinates; the image sits just inside it.
				ClientToScreen(out.list_view, &pos);
				const int left = pos.x + MulDiv(1, dpi, 96);
				const int top = pos.y + MulDiv(4, dpi, 96);
				out.icons.push_back({ parent ? display_name(item, parent.Get()) : L"",
					{ left, top, left + icon_px, top + icon_px } });
			}
			CoTaskMemFree(item);
		}
		return out;
	}

}
