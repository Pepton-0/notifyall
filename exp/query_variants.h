#pragma once

#include <windows.h>

// Times ways of asking UI Automation for a window's items, in wall time and in this thread's CPU time.
namespace query_variants {

	int run(HWND window, int runs);

	// Asks with gap ms between queries and prints when the reported position of one item changes.
	// hit also asks what sits at the window's middle after each query, the way core picks what to watch.
	int watch(HWND window, int runs, int gap, bool hit);

	// Finds the first scrollable element and prints when its vertical scroll position changes.
	int watch_scroll(HWND window, int runs, int gap);

}
