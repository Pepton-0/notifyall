#include "window_changes.h"

#include <utility>

namespace {

	// Hook callbacks carry no context; one watcher per thread is all we need.
	thread_local window_changes* current = nullptr;

	// [first, last] ranges of events worth a new look at the window's layout.
	constexpr std::pair<DWORD, DWORD> watched[] = {
		{ EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER },              // create, destroy, show, hide, reorder
		{ EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_LOCATIONCHANGE },  // expand/collapse, move
		{ EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND },
		{ EVENT_SYSTEM_SCROLLINGEND, EVENT_SYSTEM_SCROLLINGEND },
	};

}

window_changes::~window_changes()
{
	stop();
}

void window_changes::start(std::function<void(HWND)> fn)
{
	stop();
	changed = std::move(fn);
	current = this;
	for (const auto& [first, last] : watched) {
		if (const HWINEVENTHOOK h = SetWinEventHook(first, last, nullptr, &window_changes::on_event, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)) hooks.push_back(h);
	}
}

void window_changes::stop()
{
	for (const HWINEVENTHOOK h : hooks) UnhookWinEvent(h);
	hooks.clear();
	if (current == this) current = nullptr;
}

void CALLBACK window_changes::on_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD)
{
	// The mouse pointer and the text caret move constantly and change no layout.
	if (!hwnd || object == OBJID_CURSOR || object == OBJID_CARET) return;
	if (!current || !current->changed) return;

	const HWND root = GetAncestor(hwnd, GA_ROOT);
	if (!root) return;

	// A whole window being dragged keeps its layout; resizes are caught by MOVESIZEEND and size checks.
	if (event == EVENT_OBJECT_LOCATIONCHANGE && hwnd == root && object == OBJID_WINDOW) return;
	current->changed(root);
}
