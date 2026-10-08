#include "system_cursors.h"

#include <format>

#include "logger.h"

HCURSOR system_cursors::copy(UINT id)
{
	// LR_SHARED images belong to the system, so hand out a private copy.
	const HANDLE shared = LoadImageW(nullptr, MAKEINTRESOURCEW(id), IMAGE_CURSOR, 0, 0, LR_SHARED | LR_DEFAULTSIZE);
	return shared ? CopyCursor(static_cast<HCURSOR>(shared)) : nullptr;
}

bool system_cursors::replace(UINT id, HCURSOR cursor)
{
	if (SetSystemCursor(cursor, id)) return true;
	lib::log(std::format("SetSystemCursor({}) failed: {}", id, GetLastError()));
	DestroyCursor(cursor);
	return false;
}

void system_cursors::restore()
{
	SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
}
