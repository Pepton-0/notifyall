#include <chrono>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "change_pacer.h"
#include "evt.h"
#include "framework.h"
#include "i18n.h"
#include "logger.h"
#include "poller.h"
#include "strconv.h"
#include "timer.h"

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

test::suite timer_suite()
{
	return { "timer", {
		{ "ticks_while_pumping", [](test::context& t) {
			lib::timer timer;
			int ticks = 0;
			timer.start(std::chrono::milliseconds(10), [&] { ++ticks; });
			CHECK(t, timer.running());

			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
			MSG msg{};
			while (ticks < 2 && std::chrono::steady_clock::now() < until) {
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
				Sleep(1);
			}
			CHECK(t, ticks >= 2);

			timer.stop();
			CHECK(t, !timer.running());
		} },
	} };
}

test::suite poller_suite()
{
	return { "poller", {
		{ "fetches_on_worker_and_applies_here", [](test::context& t) {
			const auto here = std::this_thread::get_id();
			std::thread::id fetched_on, applied_on, cleaned_on;
			int applied = 0;

			lib::poller<int> p;
			p.start(std::chrono::milliseconds(5),
				[&] { fetched_on = std::this_thread::get_id(); return 42; },
				[&](int v) { applied_on = std::this_thread::get_id(); if (v == 42) ++applied; },
				[&] { cleaned_on = std::this_thread::get_id(); });

			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
			MSG msg{};
			while (applied < 2 && std::chrono::steady_clock::now() < until) {
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
				Sleep(1);
			}
			p.stop();

			CHECK(t, applied >= 2);
			CHECK(t, applied_on == here);
			CHECK(t, fetched_on != here);
			CHECK(t, cleaned_on == fetched_on);
			CHECK(t, !p.running());
		} },
		{ "a_poke_starts_the_next_round_early", [](test::context& t) {
			std::atomic<int> fetched = 0;
			lib::poller<int> p;
			p.start(std::chrono::milliseconds(10000), [&] { return ++fetched; }, [](int) {});

			const auto waited = [&](int count) {
				const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
				while (fetched < count && std::chrono::steady_clock::now() < until) Sleep(1);
				return fetched >= count;
			};
			CHECK(t, waited(1));   // the first round runs at once
			p.poke();
			CHECK(t, waited(2));   // and the next long before the ten seconds are up
			p.stop();
		} },
		{ "fetch_can_shorten_the_next_wait", [](test::context& t) {
			std::atomic<int> fetched = 0;
			lib::poller<int> p;
			p.start(std::chrono::milliseconds(10000), [&] {
				p.wait_next(std::chrono::milliseconds(1));
				return ++fetched;
			}, [](int) {});

			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
			while (fetched < 3 && std::chrono::steady_clock::now() < until) Sleep(1);
			p.stop();
			CHECK(t, fetched >= 3);
		} },
		{ "unread_results_are_folded_not_dropped", [](test::context& t) {
			std::atomic<int> fetched = 0;
			int applied = 0;

			lib::poller<int> p;
			p.fold_with([](int& unread, int fresh) { unread += fresh; });
			p.start(std::chrono::milliseconds(5), [&] { return fetched++ < 10 ? 1 : 0; }, [&](int v) { applied += v; });

			Sleep(400);   // nothing taken meanwhile: ten ones pile up unread
			CHECK(t, fetched > 10);
			MSG msg{};
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
			while (applied < 10 && std::chrono::steady_clock::now() < until) {
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
				Sleep(1);
			}
			p.stop();
			CHECK(t, applied == 10);
		} },
	} };
}

test::suite change_pacer_suite()
{
	using namespace std::chrono_literals;
	return { "change_pacer", {
		{ "busy_after_a_poke_quiet_otherwise", [](test::context& t) {
			lib::change_pacer pacer(60ms, 150ms);
			CHECK(t, pacer.due());     // the first run
			CHECK(t, !pacer.due());    // nothing since

			pacer.poke();
			CHECK(t, pacer.due());
			CHECK(t, pacer.due());     // still busy
			Sleep(80);
			CHECK(t, !pacer.due());    // busy period over, fallback not reached

			Sleep(160);
			CHECK(t, pacer.due());     // fallback
			CHECK(t, !pacer.due());
		} },
	} };
}
