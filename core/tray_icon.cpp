#include "tray_icon.h"

#include <format>

#include "i18n.h"
#include "logger.h"
#include "strconv.h"

namespace {

	constexpr wchar_t class_name[] = L"notifyall.tray_icon";
	constexpr UINT tray_callback = WM_APP + 1;
	constexpr UINT tray_id = 1;

	enum menu_id : UINT {
		menu_toggle = 1,
		menu_exit,
	};

}

tray_icon::tray_icon(i_notifyinjector& injector) : injector(injector)
{
}

tray_icon::~tray_icon()
{
	if (hwnd) DestroyWindow(hwnd);
}

int tray_icon::run()
{
	if (!create_window()) return 1;

	add_icon();
	if (visible_) injector.on();
	lib::log("tray icon: running", true);

	MSG msg{};
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	// Badges outlive our process, so never leave them behind.
	if (injector.active()) injector.off();

	lib::log("tray icon: exit", true);
	return static_cast<int>(msg.wParam);
}

void tray_icon::toggle()
{
	visible_ = !visible_;
	lib::log(std::format("tray icon: visible={}", visible_));

	if (visible_) injector.on();
	else          injector.off();

	visibilityChangedEvent.publish(visible_);
}

void tray_icon::quit()
{
	// Posting lets other threads close the window on its own thread.
	if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
	else      PostQuitMessage(0);
}

bool tray_icon::create_window()
{
	const HINSTANCE instance = GetModuleHandleW(nullptr);

	WNDCLASSEXW wc{ sizeof(wc) };
	wc.lpfnWndProc = &tray_icon::window_proc;
	wc.hInstance = instance;
	wc.lpszClassName = class_name;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		lib::log(std::format("RegisterClassExW failed: {}", GetLastError()), true);
		return false;
	}

	// A hidden top-level window, because message-only windows miss TaskbarCreated.
	hwnd = CreateWindowExW(0, class_name, L"notifyall", WS_OVERLAPPED,
		0, 0, 0, 0, nullptr, nullptr, instance, this);
	if (!hwnd) {
		lib::log(std::format("CreateWindowExW failed: {}", GetLastError()), true);
		return false;
	}

	taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
	return true;
}

void tray_icon::add_icon()
{
	nid = NOTIFYICONDATAW{ sizeof(nid) };
	nid.hWnd = hwnd;
	nid.uID = tray_id;
	nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	nid.uCallbackMessage = tray_callback;
	nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	wcsncpy_s(nid.szTip, lib::to_wide(i18n::get("tray.title")).c_str(), _TRUNCATE);

	if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
		lib::log("Shell_NotifyIconW(NIM_ADD) failed", true);
	}
}

void tray_icon::remove_icon()
{
	if (nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &nid);
	nid.hWnd = nullptr;
}

void tray_icon::show_menu()
{
	const HMENU menu = CreatePopupMenu();
	if (!menu) return;

	AppendMenuW(menu, MF_STRING | (visible_ ? MF_CHECKED : MF_UNCHECKED), menu_toggle,
		lib::to_wide(i18n::get("menu.toggle")).c_str());
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, menu_exit, lib::to_wide(i18n::get("menu.exit")).c_str());

	POINT pt{};
	GetCursorPos(&pt);

	// Without this the menu won't close when clicking elsewhere.
	SetForegroundWindow(hwnd);
	const UINT chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
		pt.x, pt.y, 0, hwnd, nullptr);
	PostMessageW(hwnd, WM_NULL, 0, 0);
	DestroyMenu(menu);

	switch (chosen) {
	case menu_toggle: toggle(); break;
	case menu_exit:   quit();   break;
	default: break;
	}
}

LRESULT CALLBACK tray_icon::window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		auto* self = static_cast<tray_icon*>(cs->lpCreateParams);
		self->hwnd = hwnd;   // needed before CreateWindowExW returns
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}

	if (auto* self = reinterpret_cast<tray_icon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
		return self->handle(msg, wp, lp);
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT tray_icon::handle(UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == taskbarCreatedMsg && taskbarCreatedMsg != 0) {
		lib::log("taskbar re-created. re-adding tray icon.");
		add_icon();
		return 0;
	}

	switch (msg) {
	case tray_callback:
		switch (LOWORD(lp)) {
		case WM_RBUTTONUP:
		case WM_LBUTTONUP:
		case WM_CONTEXTMENU:
			show_menu();
			break;
		default:
			break;
		}
		return 0;

	case WM_DESTROY:
		remove_icon();
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		PostQuitMessage(0);
		hwnd = nullptr;
		return 0;

	default:
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
}
