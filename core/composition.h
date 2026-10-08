#pragma once
// DI wiring shared by main and test.

#include <boost/di.hpp>

#include "i_notifyinjector.h"
#include "i_tray_icon.h"
#include "notifyinjector.h"
#include "tray_icon.h"

namespace composition {

	inline auto make_injector()
	{
		namespace di = boost::di;
		return di::make_injector(
			di::bind<i_notifyinjector>().to<notifyinjector>().in(di::singleton),
			di::bind<i_tray_icon>().to<tray_icon>().in(di::singleton));
	}

}
