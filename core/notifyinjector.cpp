#include "notifyinjector.h"

#include <format>

#include "logger.h"

notifyinjector::notifyinjector(std::vector<std::unique_ptr<i_injection>> methods)
	: methods(std::move(methods))
{
}

void notifyinjector::on()
{
	active_ = true;
	lib::log(std::format("notify icons: on ({} methods)", methods.size()));
	for (auto& m : methods) m->on();
}

void notifyinjector::off()
{
	active_ = false;
	lib::log("notify icons: off");
	for (auto& m : methods) m->off();
}
