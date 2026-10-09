#pragma once

#include <windows.h>

// Counts the WinEvents notifyall listens to, by kind and by process, to see what the hooks cost.
namespace event_census {

	// only, if not 0, keeps the events of that one process.
	int run(int seconds, DWORD only = 0);

}
