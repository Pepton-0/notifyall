#pragma once

#include <memory>
#include <vector>

#include "i_injection.h"
#include "i_notifyinjector.h"

// Switches every injection method on and off together.
class notifyinjector final : public i_notifyinjector {
public:
	explicit notifyinjector(std::vector<std::unique_ptr<i_injection>> methods);

	void on() override;
	void off() override;
	bool active() const override { return active_; }

private:
	std::vector<std::unique_ptr<i_injection>> methods;
	bool active_ = false;
};
