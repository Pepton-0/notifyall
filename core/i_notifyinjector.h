#pragma once

// Puts notification badges on every icon.
class i_notifyinjector {
public:
	virtual ~i_notifyinjector() = default;

	virtual void on() = 0;
	virtual void off() = 0;
	virtual bool active() const = 0;
};
