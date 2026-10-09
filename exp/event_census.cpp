#include "event_census.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "strconv.h"

namespace event_census {

	namespace {

		// The same ranges core's window_changes hooks.
		constexpr std::pair<DWORD, DWORD> watched[] = {
			{ EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
			{ EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND },
			{ EVENT_SYSTEM_SCROLLINGEND, EVENT_SYSTEM_SCROLLINGEND },
			{ EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND },
			{ EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER },
			{ EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_LOCATIONCHANGE },
			{ EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED },
		};

		const char* event_name(DWORD e)
		{
			switch (e) {
			case EVENT_SYSTEM_FOREGROUND: return "FOREGROUND";
			case EVENT_SYSTEM_MOVESIZEEND: return "MOVESIZEEND";
			case EVENT_SYSTEM_SCROLLINGEND: return "SCROLLINGEND";
			case EVENT_SYSTEM_MINIMIZESTART: return "MINIMIZESTART";
			case EVENT_SYSTEM_MINIMIZEEND: return "MINIMIZEEND";
			case EVENT_OBJECT_CREATE: return "CREATE";
			case EVENT_OBJECT_DESTROY: return "DESTROY";
			case EVENT_OBJECT_SHOW: return "SHOW";
			case EVENT_OBJECT_HIDE: return "HIDE";
			case EVENT_OBJECT_REORDER: return "REORDER";
			case EVENT_OBJECT_STATECHANGE: return "STATECHANGE";
			case EVENT_OBJECT_LOCATIONCHANGE: return "LOCATIONCHANGE";
			case EVENT_OBJECT_CLOAKED: return "CLOAKED";
			case EVENT_OBJECT_UNCLOAKED: return "UNCLOAKED";
			default: return "?";
			}
		}

		const char* object_kind(HWND hwnd, LONG object)
		{
			if (!hwnd) return object == OBJID_CURSOR ? "cursor" : "no window";
			if (object == OBJID_CURSOR) return "cursor";
			if (object == OBJID_CARET) return "caret";
			const bool root = GetAncestor(hwnd, GA_ROOT) == hwnd;
			if (object == OBJID_WINDOW) return root ? "top window" : "child window";
			if (object == OBJID_CLIENT) return root ? "top client" : "child client";
			return object > 0 ? "element" : "system part";
		}

		struct tally {
			std::map<std::pair<DWORD, const char*>, long long> kinds;
			std::map<DWORD, long long> pids;
			std::map<DWORD, DWORD> threadPid;
			long long total = 0;
			DWORD only = 0;
		};
		tally* current = nullptr;

		DWORD pid_of(HWND hwnd, DWORD thread)
		{
			DWORD pid = 0;
			if (hwnd && GetWindowThreadProcessId(hwnd, &pid)) return pid;
			const auto [itr, added] = current->threadPid.try_emplace(thread, 0);
			if (added) {
				if (const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread)) {
					itr->second = GetProcessIdOfThread(h);
					CloseHandle(h);
				}
			}
			return itr->second;
		}

		void CALLBACK on_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD thread, DWORD)
		{
			const DWORD pid = pid_of(hwnd, thread);
			if (current->only && pid != current->only) return;
			++current->total;
			++current->kinds[{ event, object_kind(hwnd, object) }];
			++current->pids[pid];
		}

		std::string exe_of(DWORD pid)
		{
			wchar_t path[MAX_PATH]{};
			DWORD size = MAX_PATH;
			const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (!h) return std::format("pid {}", pid);
			const bool ok = QueryFullProcessImageNameW(h, 0, path, &size);
			CloseHandle(h);
			if (!ok) return std::format("pid {}", pid);
			const std::wstring full(path, size);
			return lib::to_utf8(full.substr(full.find_last_of(L'\\') + 1));
		}

		double thread_ms()
		{
			FILETIME created{}, exited{}, kernel{}, user{};
			GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
			const auto ticks = [](const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
			return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
		}

	}

	int run(int seconds, DWORD only)
	{
		tally t;
		t.only = only;
		current = &t;
		std::vector<HWINEVENTHOOK> hooks;
		for (const auto& [first, last] : watched)
			if (const HWINEVENTHOOK h = SetWinEventHook(first, last, nullptr, &on_event, 0, 0,
				WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)) hooks.push_back(h);

		const double cpu0 = thread_ms();
		const auto until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
		MSG msg{};
		while (GetTickCount64() < until) {
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
		}
		const double cpu = thread_ms() - cpu0;
		for (const HWINEVENTHOOK h : hooks) UnhookWinEvent(h);
		current = nullptr;

		std::printf("%s", std::format("events: {} in {} s ({}/s), receiving thread: {:.2f}% of one core\n",
			t.total, seconds, t.total / seconds, cpu / seconds / 10.0).c_str());

		std::vector<std::pair<long long, std::string>> kinds;
		for (const auto& [k, n] : t.kinds) kinds.push_back({ n, std::format("{:<15} {}", event_name(k.first), k.second) });
		std::ranges::sort(kinds, std::greater{});
		std::printf("by kind:\n");
		for (const auto& [n, name] : kinds) std::printf("%s", std::format("  {:>8}  {}\n", n, name).c_str());

		std::map<std::string, long long> exes;
		for (const auto& [pid, n] : t.pids) exes[exe_of(pid)] += n;
		std::vector<std::pair<long long, std::string>> byExe;
		for (const auto& [name, n] : exes) byExe.push_back({ n, name });
		std::ranges::sort(byExe, std::greater{});
		std::printf("by process:\n");
		for (std::size_t i = 0; i < byExe.size() && i < 15; ++i)
			std::printf("%s", std::format("  {:>8}  {}\n", byExe[i].first, byExe[i].second).c_str());
		return 0;
	}

}
