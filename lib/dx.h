#pragma once
// Direct3D 11 with Direct2D/DirectWrite drawing, presented through DirectComposition.
// All objects are thread affine: create and use them on one thread.

#include <functional>
#include <memory>

#include <windows.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wrl/client.h>

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
		IDCompositionDevice& composition() const;

		// A width x height picture that any number of visuals can show. paint gets the top-left to draw at,
		// as composition may hand out a part of a bigger texture. Returns nullptr on failure.
		Microsoft::WRL::ComPtr<IDCompositionSurface> paint_surface(UINT width, UINT height,
			const std::function<void(ID2D1DeviceContext&, D2D1_POINT_2F)>& paint);

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

	// The tree of visuals a window shows. The window must have been created with WS_EX_NOREDIRECTIONBITMAP.
	class layer {
	public:
		static std::unique_ptr<layer> create(device& dev, HWND hwnd);
		~layer();

		layer(const layer&) = delete;
		layer& operator=(const layer&) = delete;

		IDCompositionVisual& root() const;

		// Shows every change made to the tree since the last commit, all at once.
		bool commit();

	private:
		struct impl;
		explicit layer(std::unique_ptr<impl> p);
		std::unique_ptr<impl> p;
	};

}
