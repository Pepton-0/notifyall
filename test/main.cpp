// Prints failed checks only, then "fail: [failed/ran]"; exits 1 on any failure.
// Tests touching the disk are left out on purpose.

#include <cstdio>
#include <exception>
#include <format>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

#include "framework.h"

test::suite logger_suite();
test::suite evt_suite();
test::suite i18n_suite();
test::suite timer_suite();
test::suite poller_suite();
test::suite change_pacer_suite();
test::suite notifyinjector_suite();
test::suite taskbar_injection_suite();
test::suite cursor_injection_suite();
test::suite tray_area_injection_suite();
test::suite window_icon_injection_suite();
test::suite window_item_injection_suite();
test::suite window_changes_suite();
test::suite occlusion_suite();
test::suite badge_scene_suite();
test::suite drift_suite();
test::suite desktop_injection_suite();
test::suite tray_icon_suite();
test::suite composition_suite();

namespace {

	std::vector<test::suite> all_suites()
	{
		return {
			logger_suite(),
			evt_suite(),
			i18n_suite(),
			timer_suite(),
			poller_suite(),
			change_pacer_suite(),
			notifyinjector_suite(),
			taskbar_injection_suite(),
			cursor_injection_suite(),
			tray_area_injection_suite(),
			window_icon_injection_suite(),
			window_item_injection_suite(),
			window_changes_suite(),
			occlusion_suite(),
			badge_scene_suite(),
			drift_suite(),
			desktop_injection_suite(),
			tray_icon_suite(),
			composition_suite(),
		};
	}

	void print(const std::string& s)
	{
		std::fputs(s.c_str(), stdout);
	}

	void usage(const std::vector<test::suite>& suites)
	{
		std::string flags = "--all";
		for (const auto& s : suites) flags += " --" + s.name;
		print(std::format("usage: test.exe [{}]\n", flags));
	}

}

int main(int argc, char** argv)
{
	SetConsoleOutputCP(CP_UTF8);

	const auto suites = all_suites();

	bool all = false;
	std::set<std::string> wanted;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--all") { all = true; continue; }

		bool known = false;
		for (const auto& s : suites) known |= arg == "--" + s.name;
		if (!known) {
			print(std::format("unknown option: {}\n", arg));
			usage(suites);
			return 2;
		}
		wanted.insert(arg.substr(2));
	}

	if (!all && wanted.empty()) {
		usage(suites);
		return 2;
	}

	std::size_t ran = 0, failed = 0;
	for (const auto& s : suites) {
		if (!all && !wanted.contains(s.name)) continue;

		for (const auto& c : s.cases) {
			test::context ctx;
			try {
				c.body(ctx);
			}
			catch (const std::exception& e) {
				ctx.fail(std::format("exception: {}", e.what()));
			}
			catch (...) {
				ctx.fail("unknown exception");
			}

			++ran;
			if (ctx.failures.empty()) continue;

			++failed;
			for (const auto& f : ctx.failures) print(std::format("[{}.{}] {}\n", s.name, c.name, f));
		}
	}

	print(std::format("fail: [{}/{}]\n", failed, ran));
	return failed == 0 ? 0 : 1;
}
