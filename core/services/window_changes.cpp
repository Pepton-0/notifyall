#include "window_changes.h"

#include <utility>

namespace {

	// Hook callbacks carry no context; one watcher per thread is all we need.
	thread_local window_changes* current = nullptr;

	// [first, last] ranges of events that can move badges or uncover windows.
	constexpr std::pair<DWORD, DWORD> watched[] = {
		{ EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
		{ EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND },
		{ EVENT_SYSTEM_SCROLLINGEND, EVENT_SYSTEM_SCROLLINGEND },
		{ EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND },
		{ EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER },              // create, destroy, show, hide, reorder
		{ EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_LOCATIONCHANGE },  // expand/collapse, move
		{ EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED },
	};

}

window_changes::~window_changes()
{
	unhook();
}

void window_changes::start()
{
	if (users++ > 0) return;
	current = this;
	for (const auto& [first, last] : watched) {
		if (const HWINEVENTHOOK h = SetWinEventHook(first, last, nullptr, &window_changes::on_event, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)) hooks.push_back(h);
	}
}

void window_changes::stop()
{
	if (users > 0 && --users == 0) unhook();
}

void window_changes::unhook()
{
	for (const HWINEVENTHOOK h : hooks) UnhookWinEvent(h);
	hooks.clear();
	if (current == this) current = nullptr;
}

window_change window_changes::classify(DWORD event, HWND hwnd, HWND root, LONG object)
{
	const bool whole_window = hwnd == root && object == OBJID_WINDOW;
	switch (event) {
	case EVENT_SYSTEM_FOREGROUND:
	case EVENT_SYSTEM_MINIMIZESTART:
	case EVENT_SYSTEM_MINIMIZEEND:
	case EVENT_OBJECT_CLOAKED:
	case EVENT_OBJECT_UNCLOAKED:
		return window_change::stack;
	case EVENT_SYSTEM_MOVESIZEEND:
		return window_change::frame;
	case EVENT_OBJECT_LOCATIONCHANGE:
		return whole_window ? window_change::frame : window_change::content;
	case EVENT_OBJECT_CREATE:
	case EVENT_OBJECT_DESTROY:
	case EVENT_OBJECT_SHOW:
	case EVENT_OBJECT_HIDE:
		return whole_window ? window_change::stack : window_change::content;
	default:
		return window_change::content;
	}
}

void CALLBACK window_changes::on_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD)
{
	// The mouse pointer and the text caret move constantly and change no layout.
	if (!hwnd || object == OBJID_CURSOR || object == OBJID_CARET) return;
	if (!current) return;

	const HWND root = GetAncestor(hwnd, GA_ROOT);
	if (root) current->changeEvent.publish(root, classify(event, hwnd, root, object));
}
