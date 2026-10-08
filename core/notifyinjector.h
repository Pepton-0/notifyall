#pragma once

#include "i_notifyinjector.h"

// Placeholder: only logs until the approach verified in exp moves here.
class notifyinjector final : public i_notifyinjector {
public:
	~notifyinjector() override;

	void on() override;
	void off() override;
	bool active() const override { return active_; }

private:
	bool active_ = false;
};
