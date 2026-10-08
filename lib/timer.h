#pragma once

#include <chrono>
#include <functional>

#include <windows.h>

namespace lib {

	// Calls a function periodically on the thread that started it. That thread must pump messages.
	class timer {
	public:
		timer() = default;
		~timer();

		timer(const timer&) = delete;
		timer& operator=(const timer&) = delete;

		void start(std::chrono::milliseconds interval, std::function<void()> tick);
		void stop();
		bool running() const { return id != 0; }

	private:
		static void CALLBACK on_timer(HWND, UINT, UINT_PTR id, DWORD);

		UINT_PTR id = 0;
		std::function<void()> tick;
	};

}
