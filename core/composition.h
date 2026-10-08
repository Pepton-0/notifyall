#pragma once
// DI wiring shared by main and test.

#include <boost/di.hpp>

#include "cursor_injection.h"
#include "desktop.h"
#include "desktop_injection.h"
#include "notifyinjector.h"
#include "overlay.h"
#include "system_cursors.h"
#include "taskbar.h"
#include "taskbar_injection.h"
#include "taskbar_items.h"
#include "tray_area_injection.h"
#include "tray_icon.h"

namespace composition {

	inline auto make_injector()
	{
		namespace di = boost::di;
		return di::make_injector(
			di::bind<i_taskbar>().to<taskbar>().in(di::singleton),
			di::bind<i_cursors>().to<system_cursors>().in(di::singleton),
			di::bind<i_desktop>().to<desktop>().in(di::singleton),
			di::bind<i_taskbar_items>().to<taskbar_items>().in(di::singleton),
			di::bind<dx::device_source>().in(di::singleton),   // one GPU device for every overlay
			di::bind<i_overlay>().to<overlay>(),   // one per injection: each covers a different area
			di::bind<i_injection* []>().to<taskbar_injection, tray_area_injection, cursor_injection, desktop_injection>(),
			di::bind<i_notifyinjector>().to<notifyinjector>().in(di::singleton),
			di::bind<i_tray_icon>().to<tray_icon>().in(di::singleton));
	}

}
