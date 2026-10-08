#include "window_icons.h"

#include "app_windows.h"

namespace {

	// A hung app must not stall us: give up quickly, and at once if Windows already knows it hangs.
	constexpr UINT send_timeout_ms = 200;
	constexpr UINT send_flags = SMTO_ABORTIFHUNG | SMTO_BLOCK;

	WPARAM wparam_of(icon_kind kind) { return kind == icon_kind::big ? ICON_BIG : ICON_SMALL; }

}

std::vector<HWND> window_icons::windows()
{
	return app_windows::list();
}

bool window_icons::elevated(HWND window)
{
	return app_windows::elevated(window);
}

HICON window_icons::get(HWND window, icon_kind kind)
{
	DWORD_PTR result = 0;
	if (!SendMessageTimeoutW(window, WM_GETICON, wparam_of(kind), 0, send_flags, send_timeout_ms, &result)) return nullptr;
	return reinterpret_cast<HICON>(result);
}

HICON window_icons::shown(HWND window, icon_kind kind)
{
	if (const HICON own = get(window, kind)) return own;
	const int index = kind == icon_kind::big ? GCLP_HICON : GCLP_HICONSM;
	if (const auto cls = reinterpret_cast<HICON>(GetClassLongPtrW(window, index))) return cls;
	if (kind == icon_kind::little) {
		if (const auto big = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON))) return big;
	}
	return LoadIconW(nullptr, IDI_APPLICATION);
}

bool window_icons::set(HWND window, icon_kind kind, HICON icon)
{
	DWORD_PTR ignored = 0;
	return SendMessageTimeoutW(window, WM_SETICON, wparam_of(kind), reinterpret_cast<LPARAM>(icon),
		send_flags, send_timeout_ms, &ignored) != 0;
}

int window_icons::size(HWND window, icon_kind kind)
{
	const UINT dpi = GetDpiForWindow(window);
	return GetSystemMetricsForDpi(kind == icon_kind::big ? SM_CXICON : SM_CXSMICON, dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
}
