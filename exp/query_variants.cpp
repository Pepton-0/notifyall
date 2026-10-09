#include "query_variants.h"

#include <array>
#include <climits>
#include <cstdlib>
#include <optional>
#include <cstdio>
#include <format>
#include <functional>
#include <string>

#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#pragma comment(lib, "uiautomationcore.lib")

using Microsoft::WRL::ComPtr;

namespace query_variants {

	namespace {

		double now_ms()
		{
			LARGE_INTEGER f{}, c{};
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&c);
			return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
		}

		double cpu_ms()
		{
			FILETIME created{}, exited{}, kernel{}, user{};
			GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
			const auto ticks = [](const FILETIME& t) { return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
			return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
		}

		ComPtr<IUIAutomationCondition> any_type(IUIAutomation& a)
		{
			constexpr std::array<CONTROLTYPEID, 5> types{
				UIA_ListItemControlTypeId, UIA_TreeItemControlTypeId, UIA_TabItemControlTypeId,
				UIA_ButtonControlTypeId, UIA_MenuItemControlTypeId,
			};
			std::array<IUIAutomationCondition*, types.size()> each{};
			for (std::size_t i = 0; i < types.size(); ++i) {
				VARIANT v{};
				v.vt = VT_I4;
				v.lVal = types[i];
				a.CreatePropertyCondition(UIA_ControlTypePropertyId, v, &each[i]);
			}
			ComPtr<IUIAutomationCondition> any;
			a.CreateOrConditionFromNativeArray(each.data(), static_cast<int>(each.size()), any.GetAddressOf());
			for (auto* c : each) if (c) c->Release();
			return any;
		}

		ComPtr<IUIAutomationCondition> on_screen(IUIAutomation& a, IUIAutomationCondition* of)
		{
			VARIANT no{};
			no.vt = VT_BOOL;
			no.boolVal = VARIANT_FALSE;
			ComPtr<IUIAutomationCondition> visible, both;
			a.CreatePropertyCondition(UIA_IsOffscreenPropertyId, no, visible.GetAddressOf());
			a.CreateAndCondition(of, visible.Get(), both.GetAddressOf());
			return both;
		}

		ComPtr<IUIAutomationCacheRequest> values(IUIAutomation& a, bool ids, IUIAutomationCondition* filter)
		{
			ComPtr<IUIAutomationCacheRequest> cache;
			a.CreateCacheRequest(cache.GetAddressOf());
			cache->AddProperty(UIA_BoundingRectanglePropertyId);
			if (ids) cache->AddProperty(UIA_RuntimeIdPropertyId);
			cache->put_AutomationElementMode(AutomationElementMode_None);
			if (filter) cache->put_TreeFilter(filter);
			return cache;
		}

	}

	int watch(HWND window, int runs, int gap, bool hit)
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		{
			ComPtr<IUIAutomation> a;
			if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(a.GetAddressOf())))) return 1;
			ComPtr<IUIAutomationElement> root;
			if (FAILED(a->ElementFromHandle(window, root.GetAddressOf())) || !root) return 1;
			const auto types = any_type(*a.Get());
			const auto condition = on_screen(*a.Get(), types.Get());
			const auto cache = values(*a.Get(), true, nullptr);

			RECT frame{};
			GetWindowRect(window, &frame);
			const LONG middle = (frame.top + frame.bottom) / 2;

			// Follows one element by its runtime id: the one nearest the middle when first seen.
			const auto id_of = [](IUIAutomationElement* e) {
				std::string key;
				VARIANT v{};
				if (FAILED(e->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &v))) return key;
				if (v.vt == (VT_ARRAY | VT_I4) && v.parray) {
					LONG lower = 0, upper = -1;
					SafeArrayGetLBound(v.parray, 1, &lower);
					SafeArrayGetUBound(v.parray, 1, &upper);
					for (LONG i = lower; i <= upper; ++i) {
						int part = 0;
						SafeArrayGetElement(v.parray, &i, &part);
						key += std::to_string(part) + '.';
					}
				}
				VariantClear(&v);
				return key;
			};

			std::string target;
			const double start = now_ms();
			LONG last = LONG_MIN;
			int changes = 0, seen = 0;
			double queried = 0;
			for (int i = 0; i < runs; ++i) {
				const double began = now_ms();
				ComPtr<IUIAutomationElementArray> found;
				root->FindAllBuildCache(TreeScope_Descendants, condition.Get(), cache.Get(), found.GetAddressOf());
				if (hit) {
					ComPtr<IUIAutomationElement> at;
					a->ElementFromPoint(POINT{ (frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2 }, at.GetAddressOf());
				}
				queried += now_ms() - began;
				int count = 0;
				if (found) found->get_Length(&count);

				LONG best = LONG_MAX;
				std::string nearest;
				std::optional<LONG> top;
				for (int k = 0; k < count; ++k) {
					ComPtr<IUIAutomationElement> e;
					if (FAILED(found->GetElement(k, e.GetAddressOf())) || !e) continue;
					RECT b{};
					e->get_CachedBoundingRectangle(&b);
					const std::string id = id_of(e.Get());
					if (!target.empty() && id == target) top = b.top;
					if (target.empty() && b.bottom - b.top >= 20 && std::abs(b.top - middle) < best) {
						best = std::abs(b.top - middle);
						nearest = id;
					}
				}
				if (target.empty() && count > 50) target = nearest;
				if (gap > 0) Sleep(static_cast<DWORD>(gap));
				if (!top) continue;
				++seen;
				if (*top != last) {
					++changes;
					std::printf("%s", std::format("{:>8.0f} ms  top {}\n", began - start, *top).c_str());
					last = *top;
				}
			}
			const double total = now_ms() - start;
			std::printf("%s", std::format("{} position changes in {} queries that saw it, over {:.0f} ms; {:.1f} ms a query\n",
				changes, seen, total, queried / runs).c_str());
		}
		CoUninitialize();
		return 0;
	}

	int watch_scroll(HWND window, int runs, int gap)
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		{
			ComPtr<IUIAutomation> a;
			if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(a.GetAddressOf())))) return 1;
			ComPtr<IUIAutomationElement> root;
			if (FAILED(a->ElementFromHandle(window, root.GetAddressOf())) || !root) return 1;

			VARIANT yes{};
			yes.vt = VT_BOOL;
			yes.boolVal = VARIANT_TRUE;
			ComPtr<IUIAutomationCondition> scrollable;
			a->CreatePropertyCondition(UIA_IsScrollPatternAvailablePropertyId, yes, scrollable.GetAddressOf());

			// The tree may still be building right after accessibility wakes up.
			ComPtr<IUIAutomationElement> pane;
			for (int i = 0; i < 50 && !pane; ++i) {
				root->FindFirst(TreeScope_Descendants, scrollable.Get(), pane.GetAddressOf());
				if (!pane) Sleep(100);
			}
			if (!pane) { std::printf("nothing scrollable\n"); return 1; }

			CONTROLTYPEID type = 0;
			pane->get_CurrentControlType(&type);
			RECT bounds{};
			pane->get_CurrentBoundingRectangle(&bounds);
			std::printf("%s", std::format("scrollable element: control type {}, bounds {},{} - {},{}\n",
				type, bounds.left, bounds.top, bounds.right, bounds.bottom).c_str());

			const double start = now_ms();
			double last = -1, took = 0;
			int changes = 0;
			for (int i = 0; i < runs; ++i) {
				const double began = now_ms();
				VARIANT percent{}, view{};
				pane->GetCurrentPropertyValue(UIA_ScrollVerticalScrollPercentPropertyId, &percent);
				pane->GetCurrentPropertyValue(UIA_ScrollVerticalViewSizePropertyId, &view);
				took += now_ms() - began;
				if (percent.vt == VT_R8 && percent.dblVal != last) {
					++changes;
					std::printf("%s", std::format("{:>8.0f} ms  {:.3f}%  (view {:.2f}%)\n",
						began - start, percent.dblVal, view.vt == VT_R8 ? view.dblVal : -1.0).c_str());
					last = percent.dblVal;
				}
				if (gap > 0) Sleep(static_cast<DWORD>(gap));
			}
			std::printf("%s", std::format("{} changes in {} reads over {:.0f} ms; {:.2f} ms a read\n",
				changes, runs, now_ms() - start, took / runs).c_str());
		}
		CoUninitialize();
		return 0;
	}

	int run(HWND window, int runs)
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		{
			ComPtr<IUIAutomation> a;
			if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(a.GetAddressOf())))) return 1;
			ComPtr<IUIAutomationElement> root;
			if (FAILED(a->ElementFromHandle(window, root.GetAddressOf())) || !root) return 1;

			const auto types = any_type(*a.Get());
			const auto types_on_screen = on_screen(*a.Get(), types.Get());
			ComPtr<IUIAutomationCondition> raw;
			a->get_RawViewCondition(raw.GetAddressOf());

			struct variant {
				const char* name;
				IUIAutomationCondition* condition;
				ComPtr<IUIAutomationCacheRequest> cache;
			};
			const variant variants[] = {
				{ "as core does (types, on screen, ids)", types_on_screen.Get(), values(*a.Get(), true, nullptr) },
				{ "without the on-screen condition", types.Get(), values(*a.Get(), true, nullptr) },
				{ "without runtime ids", types_on_screen.Get(), values(*a.Get(), false, nullptr) },
				{ "raw view tree filter", types_on_screen.Get(), values(*a.Get(), true, raw.Get()) },
			};

			for (const variant& v : variants) {
				double wall = 0, cpu = 0;
				int count = 0;
				for (int i = 0; i < runs; ++i) {
					const double w0 = now_ms(), c0 = cpu_ms();
					ComPtr<IUIAutomationElementArray> found;
					root->FindAllBuildCache(TreeScope_Descendants, v.condition, v.cache.Get(), found.GetAddressOf());
					wall += now_ms() - w0;
					cpu += cpu_ms() - c0;
					count = 0;
					if (found) found->get_Length(&count);
				}
				std::printf("%s", std::format("{:<40} {:>7.1f} ms wall {:>7.1f} ms cpu  {} items\n",
					v.name, wall / runs, cpu / runs, count).c_str());
			}
		}
		CoUninitialize();
		return 0;
	}

}
