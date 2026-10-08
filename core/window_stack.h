#pragma once

#include "i_window_stack.h"

class window_stack final : public i_window_stack {
public:
	std::vector<stacked_window> front_to_back() override;
	RECT screen() override;
};
