#pragma once
// Decides when a slow query is worth running: often for a while after a change, rarely otherwise.
// poke() may come from any thread; due() belongs to the one thread that runs the query.

#include <atomic>
#include <chrono>

namespace lib {

	class change_pacer {
	public:
		using clock = std::chrono::steady_clock;

		change_pacer(clock::duration busy_period, clock::duration fallback)
			: busyPeriod(busy_period), fallback(fallback) {}

		void poke() { lastPoke.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed); }

		// True right after a poke, and otherwise once per fallback. A true answer counts as a run.
		bool due()
		{
			const auto now = clock::now();
			const clock::time_point poked{ clock::duration{ lastPoke.load(std::memory_order_relaxed) } };
			const bool busy = poked.time_since_epoch().count() != 0 && now - poked < busyPeriod;
			if (!busy && ran && now - lastRun < fallback) return false;
			ran = true;
			lastRun = now;
			return true;
		}

	private:
		const clock::duration busyPeriod;
		const clock::duration fallback;
		std::atomic<clock::rep> lastPoke{ 0 };
		clock::time_point lastRun{};
		bool ran = false;
	};

}
