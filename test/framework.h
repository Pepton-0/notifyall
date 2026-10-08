#pragma once
// CHECK records a failure and keeps going. A case fails if any of its checks fail.

#include <format>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace test {

	class context {
	public:
		void check(bool ok, std::string_view expr,
		           std::source_location loc = std::source_location::current())
		{
			if (ok) return;
			const std::string_view file = loc.file_name();
			const auto pos = file.find_last_of("\\/");
			failures.push_back(std::format("{}:{} CHECK({})",
				pos == std::string_view::npos ? file : file.substr(pos + 1), loc.line(), expr));
		}

		void fail(std::string message) { failures.push_back(std::move(message)); }

		std::vector<std::string> failures;
	};

	struct test_case {
		std::string name;
		std::function<void(context&)> body;
	};

	struct suite {
		std::string name;   // selected by --<name>
		std::vector<test_case> cases;
	};

}

#define CHECK(ctx, expr) (ctx).check(static_cast<bool>(expr), #expr)
