#pragma once

#include <chrono>
#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>

namespace lib {

	// Writes "[threadid timestamp file:func:line] message" to the debugger.
	// to_file also appends it to <exe dir>/logs/yyyymmdd.log. Thread safe.
	void log(std::string_view message, bool to_file = false,
	         std::source_location loc = std::source_location::current());

	// Mirrors logs to stderr. Off by default: GUI apps have no console and tests print failures only.
	void log_to_console(bool enabled);

	namespace detail {

		// "C:\dir\sub/foo.cpp" -> "foo.cpp"
		std::string_view base_name(std::string_view path);

		// "void __cdecl ns::cls::fn(int)" -> "ns::cls::fn"
		std::string_view short_function_name(std::string_view signature);

		std::string format_line(std::string_view message,
		                        std::uint32_t thread_id,
		                        std::chrono::system_clock::time_point time,
		                        const std::source_location& loc);
	}
}
