#pragma once

#include <memory>
#include <vector>

#include "i_injection.h"

// Puts notification badges on every icon.
class i_notifyinjector {
public:
	virtual ~i_notifyinjector() = default;

	virtual void on() = 0;
	virtual void off() = 0;
	virtual bool active() const = 0;
};

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
