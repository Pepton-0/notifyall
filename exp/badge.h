#pragma once
// Notification badge: a white digit on a red circle, rendered on the CPU into a HICON.

#include <filesystem>
#include <windows.h>

namespace badge {

	// The caller owns the icon and must DestroyIcon it.
	HICON make(int number, int size);

	// Writes badges 1..9 side by side on white, for eyeballing the design.
	bool save_preview(const std::filesystem::path& bmp_file, int size);

}
