#pragma once

// One way of putting badges on icons. notifyinjector runs all of them together.
class i_injection {
public:
	virtual ~i_injection() = default;

	virtual void on() = 0;
	virtual void off() = 0;
};
