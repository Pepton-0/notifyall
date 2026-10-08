#include "timer.h"

#include <unordered_map>

namespace {

	// Thread timers carry no context, so map their ids back to the owners.
	thread_local std::unordered_map<UINT_PTR, lib::timer*> owners;

}

namespace lib {

	timer::~timer()
	{
		stop();
	}

	void timer::start(std::chrono::milliseconds interval, std::function<void()> fn)
	{
		stop();
		tick = std::move(fn);
		id = SetTimer(nullptr, 0, static_cast<UINT>(interval.count()), &timer::on_timer);
		if (id != 0) owners[id] = this;
	}

	void timer::stop()
	{
		if (id == 0) return;
		KillTimer(nullptr, id);
		owners.erase(id);
		id = 0;
	}

	void CALLBACK timer::on_timer(HWND, UINT, UINT_PTR id, DWORD)
	{
		const auto itr = owners.find(id);
		if (itr != owners.end() && itr->second->tick) itr->second->tick();
	}

}
