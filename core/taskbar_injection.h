#pragma once

#include <array>
#include <chrono>
#include <random>
#include <unordered_map>

#include "i_injection.h"
#include "taskbar.h"
#include "timer.h"

// Badges the taskbar button of every running window. Pinned apps that aren't running have no window to badge.
class taskbar_injection final : public i_injection {
public:
	static constexpr std::chrono::milliseconds refresh_interval{ 1000 };

	explicit taskbar_injection(i_taskbar& bar);
	~taskbar_injection() override;

	void on() override;
	void off() override;

	// Badges windows that appeared since the last refresh and forgets closed ones.
	void refresh();

	std::size_t badged() const { return numbers.size(); }

private:
	void make_icons();
	void destroy_icons();

	i_taskbar& bar;
	lib::timer refresher;

	std::array<HICON, 9> icons{};                // badges 1..9
	std::unordered_map<HWND, int> numbers;     // windows we badged and their number
	std::mt19937 rng{ std::random_device{}() };
};
