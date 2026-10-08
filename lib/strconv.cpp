#include "strconv.h"

#include <windows.h>

namespace lib {

	std::wstring to_wide(std::string_view utf8)
	{
		if (utf8.empty()) return {};

		const int src = static_cast<int>(utf8.size());
		const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), src, nullptr, 0);
		if (len <= 0) return {};

		std::wstring wide(static_cast<std::size_t>(len), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.data(), src, wide.data(), len);
		return wide;
	}

	std::string to_utf8(std::wstring_view wide)
	{
		if (wide.empty()) return {};

		const int src = static_cast<int>(wide.size());
		const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), src, nullptr, 0, nullptr, nullptr);
		if (len <= 0) return {};

		std::string utf8(static_cast<std::size_t>(len), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.data(), src, utf8.data(), len, nullptr, nullptr);
		return utf8;
	}

}
