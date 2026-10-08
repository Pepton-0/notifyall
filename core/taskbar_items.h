#pragma once

#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include "i_taskbar_items.h"

// Needs COM on the calling thread.
class taskbar_items final : public i_taskbar_items {
public:
	std::optional<tray_snapshot> snapshot() override;
	bool visible_at(POINT pt) override;
	void disconnect() override;

private:
	bool ensure_automation();

	Microsoft::WRL::ComPtr<IUIAutomation> automation;
	Microsoft::WRL::ComPtr<IUIAutomationCondition> isButton;
	Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache;
};
