#pragma once
// Strings stay UTF-8 inside the app and become UTF-16 right before WinAPI calls.

#include <string>
#include <string_view>

namespace lib {

	std::wstring to_wide(std::string_view utf8);
	std::string to_utf8(std::wstring_view wide);

}
