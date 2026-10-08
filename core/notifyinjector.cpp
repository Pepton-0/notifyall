#include "notifyinjector.h"

#include "logger.h"

notifyinjector::~notifyinjector()
{
	if (active_) off();
}

void notifyinjector::on()
{
	active_ = true;
	lib::log("notify icons: on");
}

void notifyinjector::off()
{
	active_ = false;
	lib::log("notify icons: off");
}
