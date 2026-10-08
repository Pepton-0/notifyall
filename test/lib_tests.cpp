#include <chrono>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "evt.h"
#include "framework.h"
#include "i18n.h"
#include "logger.h"
#include "strconv.h"

test::suite logger_suite()
{
	using namespace lib::detail;

	return { "logger", {
		{ "base_name", [](test::context& t) {
			CHECK(t, base_name("C:\\dir\\sub/foo.cpp") == "foo.cpp");
			CHECK(t, base_name("C:\\dir\\foo.cpp") == "foo.cpp");
			CHECK(t, base_name("foo.cpp") == "foo.cpp");
			CHECK(t, base_name("") == "");
		} },
		{ "short_function_name", [](test::context& t) {
			CHECK(t, short_function_name("void __cdecl ns::cls::fn(int)") == "ns::cls::fn");
			CHECK(t, short_function_name("int __cdecl main(int,char **)") == "main");
			CHECK(t, short_function_name("void __cdecl f<class std::basic_string<char> >(void)")
				== "f<class std::basic_string<char> >");
			CHECK(t, short_function_name("fn") == "fn");
			CHECK(t, short_function_name("auto __cdecl suite::<lambda_1>::operator ()(class test::context &) const")
				== "suite::<lambda_1>::operator ()");
		} },
		{ "format_line", [](test::context& t) {
			const auto loc = std::source_location::current();
			const auto line = format_line("hello world", 1234, std::chrono::system_clock::now(), loc);

			const std::regex re(R"(^\[1234 \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} lib_tests\.cpp:logger_suite::.+:(\d+)\] hello world$)");
			std::smatch m;
			if (!std::regex_match(line, m, re)) {
				t.fail("unexpected log line: " + line);
				return;
			}
			CHECK(t, m[1] == std::to_string(loc.line()));
		} },
		{ "log_does_not_throw", [](test::context&) {
			lib::log("logger test (no file)");
		} },
		{ "strconv_roundtrip", [](test::context& t) {
			const std::string s = "表示/非表示 abc";
			CHECK(t, lib::to_utf8(lib::to_wide(s)) == s);
			CHECK(t, lib::to_wide("") == L"");
		} },
	} };
}

test::suite evt_suite()
{
	return { "evt", {
		{ "publish_reaches_handlers", [](test::context& t) {
			evt::event<int> e;
			int sum = 0;
			auto a = e.connect([&](int v) { sum += v; });
			auto b = e.connect([&](int v) { sum += v * 10; });
			e.publish(2);
			CHECK(t, sum == 22);
			CHECK(t, e.count() == 2);
		} },
		{ "subscription_unsubscribes_on_destroy", [](test::context& t) {
			evt::event<> e;
			int calls = 0;
			{
				auto s = e.connect([&] { ++calls; });
				e.publish();
			}
			e.publish();
			CHECK(t, calls == 1);
			CHECK(t, e.count() == 0);
		} },
		{ "subscription_outlives_event", [](test::context& t) {
			evt::subscription<> s;
			{
				evt::event<> e;
				s = e.connect([] {});
				CHECK(t, s.active());
			}
			CHECK(t, !s.active());
			s.reset();   // must not crash without the event
		} },
		{ "connect_while_publishing_is_deferred", [](test::context& t) {
			evt::event<> e;
			int inner = 0;
			std::vector<evt::subscription<>> subs;
			subs.push_back(e.connect([&] { subs.push_back(e.connect([&] { ++inner; })); }));
			e.publish();
			CHECK(t, inner == 0);
			e.publish();
			CHECK(t, inner == 1);
		} },
		{ "exception_restores_state", [](test::context& t) {
			evt::event<> e;
			auto s = e.connect([] { throw std::runtime_error("boom"); });
			bool thrown = false;
			try { e.publish(); }
			catch (const std::runtime_error&) { thrown = true; }
			CHECK(t, thrown);

			s.reset();
			CHECK(t, e.count() == 0);
		} },
	} };
}

test::suite i18n_suite()
{
	return { "i18n", {
		{ "parse", [](test::context& t) {
			const auto table = i18n::detail::parse(
				"\xEF\xBB\xBF# comment\r\n"
				"\r\n"
				"  a = 1  \r\n"
				"b==x=y\n"
				"no_equal_line\n"
				"=no_key\n"
				"multi=line1\\nline2");
			CHECK(t, table.size() == 3);
			CHECK(t, table.at("a") == "1");
			CHECK(t, table.at("b") == "=x=y");
			CHECK(t, table.at("multi") == "line1\nline2");
		} },
		{ "fallback_to_en", [](test::context& t) {
			i18n::detail::apply("ja", "a=A\nb=B\n", "a=あ\n");
			CHECK(t, i18n::current() == "ja");
			CHECK(t, i18n::get("a") == "あ");
			CHECK(t, i18n::get("b") == "B");
		} },
		{ "missing_key_returns_key", [](test::context& t) {
			i18n::detail::apply("en", "a=A\n", "");
			CHECK(t, i18n::get("nothing.here") == "nothing.here");
		} },
		{ "switch_language_drops_previous", [](test::context& t) {
			i18n::detail::apply("ja", "a=A\n", "a=あ\nonly_ja=x\n");
			i18n::detail::apply("en", "a=A\n", "");
			CHECK(t, i18n::current() == "en");
			CHECK(t, i18n::get("a") == "A");
			CHECK(t, i18n::get("only_ja") == "only_ja");
		} },
	} };
}
