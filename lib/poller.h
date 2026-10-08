#pragma once
// Runs a slow query on a worker thread and hands each result to the thread that started it.
// Keeps cross-process calls (UI Automation, shell COM) off a UI thread that other processes may be waiting on.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include <windows.h>
#include <objbase.h>

namespace lib {

	namespace detail {

		// One window class serves every poller<T>, so the window procedure must not depend on T.
		class poller_mailbox {
		public:
			virtual ~poller_mailbox() { close(); }

		protected:
			void open()
			{
				const HINSTANCE instance = GetModuleHandleW(nullptr);
				WNDCLASSEXW wc{ sizeof(wc) };
				wc.lpfnWndProc = &poller_mailbox::window_proc;
				wc.hInstance = instance;
				wc.lpszClassName = L"notifyall.poller";
				RegisterClassExW(&wc);

				hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
			}

			void close()
			{
				if (hwnd) DestroyWindow(hwnd);
				hwnd = nullptr;
			}

			void notify() const { PostMessageW(hwnd, WM_APP, 0, 0); }

			virtual void take() = 0;

		private:
			static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
			{
				if (msg != WM_APP) return DefWindowProcW(hwnd, msg, wp, lp);
				if (auto* self = reinterpret_cast<poller_mailbox*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) self->take();
				return 0;
			}

			HWND hwnd = nullptr;
		};

	}

	template <class T>
	class poller final : private detail::poller_mailbox {
	public:
		poller() = default;
		~poller() override { stop(); }

		poller(const poller&) = delete;
		poller& operator=(const poller&) = delete;

		// fetch runs on the worker (COM initialized as MTA); apply runs on this thread, which must pump messages.
		// cleanup runs on the worker as it ends; name shows up in debuggers and profilers.
		void start(std::chrono::milliseconds interval, std::function<T()> fetch, std::function<void(T)> apply,
		           std::function<void()> cleanup = {}, const wchar_t* name = L"notifyall poller")
		{
			stop();
			deliver = std::move(apply);
			open();

			worker = std::jthread([this, interval, name, fetch = std::move(fetch), cleanup = std::move(cleanup)](std::stop_token stop) {
				SetThreadDescription(GetCurrentThread(), name);
				const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				while (!stop.stop_requested()) {
					T result = fetch();
					{
						std::lock_guard lock(m);
						latest = std::move(result);
					}
					notify();

					std::unique_lock lock(m);
					wake.wait_for(lock, stop, interval, [] { return false; });
				}
				if (cleanup) cleanup();
				if (SUCCEEDED(com)) CoUninitialize();
			});
		}

		void stop()
		{
			if (worker.joinable()) {
				worker.request_stop();
				worker.join();
			}
			close();
			latest.reset();
		}

		bool running() const { return worker.joinable(); }

	private:
		void take() override
		{
			std::optional<T> result;
			{
				std::lock_guard lock(m);
				result.swap(latest);
			}
			if (result && deliver) deliver(std::move(*result));
		}

		std::function<void(T)> deliver;
		std::mutex m;
		std::condition_variable_any wake;
		std::optional<T> latest;   // only the newest result matters; older ones are dropped
		std::jthread worker;
	};

}
