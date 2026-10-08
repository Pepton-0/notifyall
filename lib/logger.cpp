#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>

#include <windows.h>

#include "strconv.h"

namespace {

	std::atomic<bool> console_enabled{ false };

	// Empty (= current directory) when the exe path is unavailable.
	std::filesystem::path module_dir()
	{
		wchar_t path[MAX_PATH]{};
		const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
		if (len == 0 || len == MAX_PATH) return {};
		return std::filesystem::path(std::wstring_view(path, len)).parent_path();
	}

	// Falls back to UTC when the time zone database is unavailable.
	template <class Duration>
	auto local(std::chrono::sys_time<Duration> t)
	{
		try {
			return std::chrono::current_zone()->to_local(t);
		}
		catch (...) {
			return std::chrono::local_time<Duration>(t.time_since_epoch());
		}
	}

	// A lambda's "operator ()" contains a "(" that belongs to the name.
	std::size_t parameter_list_begin(std::string_view signature)
	{
		constexpr std::string_view call_operator = "operator ()";
		const auto op = signature.find(call_operator);
		const auto from = op == std::string_view::npos ? 0 : op + call_operator.size();
		return std::min(signature.find('(', from), signature.size());
	}

	// The name starts after the last space outside template brackets, e.g. after "__cdecl".
	std::size_t name_begin(std::string_view signature, std::size_t end)
	{
		int template_depth = 0;
		for (std::size_t i = end; i-- > 0;) {
			const char c = signature[i];
			if (c == '>') ++template_depth;
			else if (c == '<') --template_depth;
			else if (c == ' ' && template_depth == 0 && !signature.substr(0, i).ends_with("operator")) return i + 1;
		}
		return 0;
	}

	void write_file(std::chrono::system_clock::time_point now, const std::string& line)
	{
		using namespace std::chrono;
		static std::mutex m;
		const std::lock_guard lock(m);

		const auto day = floor<days>(local(floor<milliseconds>(now)));
		const auto dir = module_dir() / L"logs";
		const auto file = dir / std::format("{:%Y%m%d}.log", day);

		std::error_code ec;
		std::filesystem::create_directories(dir, ec);

		std::ofstream out(file, std::ios::app | std::ios::binary);
		if (out) out << line;
	}

}

namespace lib {

	void log_to_console(bool enabled)
	{
		console_enabled = enabled;
	}

	void log(std::string_view message, bool to_file, std::source_location loc)
	{
		const auto now = std::chrono::system_clock::now();
		const std::string line = detail::format_line(message, GetCurrentThreadId(), now, loc) + "\n";

		OutputDebugStringW(to_wide(line).c_str());

		if (console_enabled) {
			std::fputs(line.c_str(), stderr);
		}

		if (to_file) {
			write_file(now, line);
		}
	}

	namespace detail {

		std::string_view base_name(std::string_view path)
		{
			const auto pos = path.find_last_of("\\/");
			return pos == std::string_view::npos ? path : path.substr(pos + 1);
		}

		std::string_view short_function_name(std::string_view signature)
		{
			const std::size_t end = parameter_list_begin(signature);
			const std::size_t begin = name_begin(signature, end);
			return signature.substr(begin, end - begin);
		}

		std::string format_line(std::string_view message,
		                        std::uint32_t thread_id,
		                        std::chrono::system_clock::time_point time,
		                        const std::source_location& loc)
		{
			using namespace std::chrono;
			const auto t = local(floor<milliseconds>(time));

			return std::format("[{} {:%Y-%m-%d %H:%M:%S} {}:{}:{}] {}",
				thread_id, t,
				base_name(loc.file_name()), short_function_name(loc.function_name()), loc.line(),
				message);
		}
	}
}
