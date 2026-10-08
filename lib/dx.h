#pragma once
// Direct3D 11 with Direct2D/DirectWrite drawing, presented through DirectComposition.
// All objects are thread affine: create and use them on one thread.

#include <functional>
#include <memory>

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

namespace dx {

	class device {
	public:
		// Hardware first, WARP after. Returns nullptr (and logs why) when neither works.
		static std::unique_ptr<device> create();
		~device();

		device(const device&) = delete;
		device& operator=(const device&) = delete;

		ID2D1DeviceContext& d2d() const;
		IDWriteFactory& dwrite() const;

	private:
		struct impl;
		explicit device(std::unique_ptr<impl> p);
		std::unique_ptr<impl> p;

		friend class layer;
	};

	// Creates the device on first use and gives every caller the same one; a failure is not retried.
	class device_source {
	public:
		std::shared_ptr<device> get();

	private:
		std::shared_ptr<device> shared;
		bool failed = false;
	};

	// A see-through surface covering a window's client area, blended with premultiplied alpha.
	// The window must have been created with WS_EX_NOREDIRECTIONBITMAP.
	class layer {
	public:
		static std::unique_ptr<layer> create(device& dev, HWND hwnd, UINT width, UINT height);
		~layer();

		layer(const layer&) = delete;
		layer& operator=(const layer&) = delete;

		bool resize(UINT width, UINT height);

		// Clears to fully transparent, runs paint, then presents.
		bool draw(const std::function<void(ID2D1DeviceContext&)>& paint);

	private:
		struct impl;
		explicit layer(std::unique_ptr<impl> p);
		std::unique_ptr<impl> p;
	};

}
