#pragma once

#include "i_cursors.h"

class system_cursors final : public i_cursors {
public:
	HCURSOR copy(UINT id) override;
	bool replace(UINT id, HCURSOR cursor) override;
	void restore() override;
};
