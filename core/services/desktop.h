#pragma once

#include <optional>
#include <string>
#include <vector>
#include <windows.h>
#include <ole2.h>
#include <shobjidl.h>
#include <wrl/client.h>

struct desktop_icon {
	std::wstring id;   // parsing name, unique per item
	RECT rect{};       // the icon image in screen coordinates
};

struct desktop_snapshot {
	HWND host = nullptr;   // top-level window holding the icons (Progman or WorkerW)
	RECT bounds{};         // icon area in screen coordinates
	std::vector<desktop_icon> icons;
};

// Desktop icons as the shell reports them.
class i_desktop {
public:
	virtual ~i_desktop() = default;

	// Empty when the desktop can't be reached or its icons are hidden.
	virtual std::optional<desktop_snapshot> snapshot() = 0;

	// The top-level window holding the icons, found without asking the shell; nullptr if there is none.
	virtual HWND host() = 0;

	// Releases the COM objects snapshot() created; call it on the thread that called snapshot().
	virtual void disconnect() = 0;
};

// Reads icon positions through the shell's IFolderView. Needs COM on the calling thread.
class desktop final : public i_desktop {
public:
	std::optional<desktop_snapshot> snapshot() override;
	HWND host() override;
	void disconnect() override;

private:
	bool connect();

	Microsoft::WRL::ComPtr<IFolderView2> view;
	Microsoft::WRL::ComPtr<IShellFolder> folder;
	HWND listView = nullptr;
};
