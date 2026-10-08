#include <windows.h>

#include <exception>
#include <format>

#include "composition.h"
#include "i18n.h"
#include "logger.h"

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
	const HANDLE single_instance = CreateMutexW(nullptr, TRUE, L"Local\\notifyall.single_instance");
	if (single_instance && GetLastError() == ERROR_ALREADY_EXISTS) {
		lib::log("notifyall is already running.");
		CloseHandle(single_instance);
		return 0;
	}

	lib::log("notifyall: start", true);
	i18n::set(i18n::system_language());

	int code = 0;
	try {
		auto injector = composition::make_injector();
		auto& tray = injector.create<i_tray_icon&>();
		code = tray.run();
	}
	catch (const std::exception& e) {
		lib::log(std::format("unhandled exception: {}", e.what()), true);
		code = 1;
	}

	if (single_instance) CloseHandle(single_instance);
	return code;
}
