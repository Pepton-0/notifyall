#pragma once

#include <ole2.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "i_desktop.h"

// Reads icon positions through the shell's IFolderView. Needs COM on the calling thread.
class desktop final : public i_desktop {
public:
	std::optional<desktop_snapshot> snapshot() override;
	void disconnect() override;

private:
	bool connect();

	Microsoft::WRL::ComPtr<IFolderView2> view;
	Microsoft::WRL::ComPtr<IShellFolder> folder;
	HWND listView = nullptr;
};
