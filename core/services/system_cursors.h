#pragma once

#include <windows.h>

// The system-wide cursor set (arrow, I-beam, hand, ...), identified by OCR_* ids.
class i_cursors {
public:
	virtual ~i_cursors() = default;

	// The cursor currently shown for id, at its on-screen size. The caller destroys it.
	virtual HCURSOR copy(UINT id) = 0;

	// Takes ownership of cursor, as SetSystemCursor does.
	virtual bool replace(UINT id, HCURSOR cursor) = 0;

	// Reloads the user's cursor scheme, undoing every replace.
	virtual void restore() = 0;
};

class system_cursors final : public i_cursors {
public:
	HCURSOR copy(UINT id) override;
	bool replace(UINT id, HCURSOR cursor) override;
	void restore() override;
};
