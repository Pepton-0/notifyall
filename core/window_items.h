#pragma once

#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <unordered_map>

#include "i_window_items.h"

// Needs COM on the calling thread.
class window_items final : public i_window_items {
public:
	static constexpr std::size_t item_limit = 400;   // a huge list shouldn't turn into thousands of badges

	std::vector<HWND> candidates() override;
	std::optional<window_snapshot> query(HWND window) override;
	void disconnect() override;

private:
	bool ensure_automation();
	bool elevated(HWND window);

	Microsoft::WRL::ComPtr<IUIAutomation> automation;
	Microsoft::WRL::ComPtr<IUIAutomationCondition> isItem;
	Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache;
	std::unordered_map<HWND, bool> elevation;   // a window's process doesn't change, so look once
};
