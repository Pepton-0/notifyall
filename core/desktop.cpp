#include "desktop.h"

#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>

#include "logger.h"

#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

	// The icons live in Progman (or WorkerW) > SHELLDLL_DefView > SysListView32.
	HWND find_icon_list_view()
	{
		HWND defView = nullptr;
		EnumWindows([](HWND top, LPARAM found) -> BOOL {
			const HWND view = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
			if (!view) return TRUE;
			*reinterpret_cast<HWND*>(found) = view;
			return FALSE;
		}, reinterpret_cast<LPARAM>(&defView));
		return defView ? FindWindowExW(defView, nullptr, L"SysListView32", nullptr) : nullptr;
	}

	std::wstring parsing_name(IShellFolder* parent, PCUITEMID_CHILD item)
	{
		STRRET ret{};
		wchar_t name[MAX_PATH]{};
		if (FAILED(parent->GetDisplayNameOf(item, SHGDN_FORPARSING, &ret))) return {};
		StrRetToBufW(&ret, item, name, MAX_PATH);
		return name;
	}

}

std::optional<desktop_snapshot> desktop::snapshot()
{
	if (!connect()) return std::nullopt;
	if (!IsWindowVisible(listView)) return std::nullopt;   // "Show desktop icons" is off

	desktop_snapshot s;
	s.host = GetAncestor(listView, GA_ROOT);
	GetWindowRect(listView, &s.bounds);

	FOLDERVIEWMODE mode{};
	int logical_size = 0;
	int count = 0;
	if (FAILED(view->GetViewModeAndIconSize(&mode, &logical_size)) || FAILED(view->ItemCount(SVGIO_ALLVIEW, &count))) {
		view.Reset();   // explorer restarted; reconnect next time
		return std::nullopt;
	}

	const UINT dpi = GetDpiForWindow(listView);
	const int size = MulDiv(logical_size, dpi, USER_DEFAULT_SCREEN_DPI);

	for (int i = 0; i < count; ++i) {
		PITEMID_CHILD item = nullptr;
		if (FAILED(view->Item(i, &item))) continue;

		POINT pos{};
		if (SUCCEEDED(view->GetItemPosition(item, &pos))) {
			// pos is the item's top-left in list view client coordinates; the image sits just inside it.
			ClientToScreen(listView, &pos);
			const int left = pos.x + MulDiv(1, dpi, USER_DEFAULT_SCREEN_DPI);
			const int top = pos.y + MulDiv(4, dpi, USER_DEFAULT_SCREEN_DPI);
			s.icons.push_back({ parsing_name(folder.Get(), item), { left, top, left + size, top + size } });
		}
		CoTaskMemFree(item);
	}
	return s;
}

void desktop::disconnect()
{
	view.Reset();
	folder.Reset();
	listView = nullptr;
}

bool desktop::connect()
{
	if (view && IsWindow(listView)) return true;
	view.Reset();
	folder.Reset();
	listView = nullptr;

	ComPtr<IShellWindows> windows;
	if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.GetAddressOf())))) return false;

	VARIANT empty{};
	long found = 0;
	ComPtr<IDispatch> dispatch;
	if (FAILED(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &found, SWFO_NEEDDISPATCH, dispatch.GetAddressOf())) || !dispatch) return false;

	ComPtr<IShellBrowser> browser;
	ComPtr<IShellView> shellView;
	if (FAILED(IUnknown_QueryService(dispatch.Get(), SID_STopLevelBrowser, IID_PPV_ARGS(browser.GetAddressOf())))) return false;
	if (FAILED(browser->QueryActiveShellView(shellView.GetAddressOf()))) return false;
	if (FAILED(shellView.As(&view))) return false;
	view->GetFolder(IID_PPV_ARGS(folder.GetAddressOf()));

	// IShellView::GetWindow is input-synchronous and fails from a worker thread, so find the view by class.
	listView = find_icon_list_view();
	if (!listView || !folder) {
		view.Reset();
		lib::log("desktop: the icon view was not found", true);
		return false;
	}
	return true;
}
