#include "dx.h"

#include <algorithm>
#include <format>

#include <d3d11.h>
#include <dxgi.h>

#include "logger.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

namespace {

	std::string hex(HRESULT hr)
	{
		return std::format("0x{:08X}", static_cast<unsigned>(hr));
	}

	bool failed(HRESULT hr, const char* what)
	{
		if (SUCCEEDED(hr)) return false;
		lib::log(std::format("dx: {} failed ({})", what, hex(hr)), true);
		return true;
	}

	ComPtr<ID3D11Device> create_d3d()
	{
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		for (const D3D_DRIVER_TYPE type : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP }) {
			ComPtr<ID3D11Device> d3d;
			// BGRA_SUPPORT is required for Direct2D. Everything runs on one thread and redraws rarely,
			// so the driver needs neither locking nor worker threads of its own.
			constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED
				| D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
			const HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, flags,
				levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d.GetAddressOf(), nullptr, nullptr);
			if (SUCCEEDED(hr)) {
				lib::log(std::format("dx: device created ({})", type == D3D_DRIVER_TYPE_HARDWARE ? "hardware" : "warp"), true);
				return d3d;
			}
			lib::log(std::format("dx: D3D11CreateDevice failed ({}), trying the next driver", hex(hr)));
		}
		return nullptr;
	}

}

namespace dx {

	struct device::impl {
		ComPtr<ID3D11Device> d3d;
		ComPtr<IDXGIDevice> dxgi;
		ComPtr<ID2D1DeviceContext> d2d;
		ComPtr<IDWriteFactory> dwrite;
		ComPtr<IDCompositionDevice> dcomp;
	};

	std::unique_ptr<device> device::create()
	{
		auto p = std::make_unique<impl>();

		p->d3d = create_d3d();
		if (!p->d3d) return nullptr;
		if (failed(p->d3d.As(&p->dxgi), "IDXGIDevice")) return nullptr;

		ComPtr<ID2D1Factory1> d2dFactory;
		ComPtr<ID2D1Device> d2dDevice;
		if (failed(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf()), "D2D1CreateFactory")) return nullptr;
		if (failed(d2dFactory->CreateDevice(p->dxgi.Get(), d2dDevice.GetAddressOf()), "ID2D1Device")) return nullptr;
		if (failed(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, p->d2d.GetAddressOf()), "ID2D1DeviceContext")) return nullptr;

		if (failed(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
			reinterpret_cast<IUnknown**>(p->dwrite.GetAddressOf())), "DWriteCreateFactory")) return nullptr;

		if (failed(DCompositionCreateDevice(p->dxgi.Get(), IID_PPV_ARGS(p->dcomp.GetAddressOf())), "DCompositionCreateDevice")) return nullptr;

		return std::unique_ptr<device>(new device(std::move(p)));
	}

	device::device(std::unique_ptr<impl> p) : p(std::move(p)) {}
	device::~device() = default;

	ID2D1DeviceContext& device::d2d() const { return *p->d2d.Get(); }
	IDWriteFactory& device::dwrite() const { return *p->dwrite.Get(); }
	IDCompositionDevice& device::composition() const { return *p->dcomp.Get(); }

	ComPtr<IDCompositionSurface> device::paint_surface(UINT width, UINT height,
		const std::function<void(ID2D1DeviceContext&, D2D1_POINT_2F)>& paint)
	{
		width = std::max(1u, width);
		height = std::max(1u, height);
		ComPtr<IDCompositionSurface> surface;
		if (failed(p->dcomp->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
			surface.GetAddressOf()), "CreateSurface")) return nullptr;

		POINT at{};
		ComPtr<IDXGISurface> texture;
		if (failed(surface->BeginDraw(nullptr, IID_PPV_ARGS(texture.GetAddressOf()), &at), "BeginDraw")) return nullptr;

		const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
			D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		ID2D1DeviceContext& ctx = *p->d2d.Get();
		ComPtr<ID2D1Bitmap1> target;
		bool drawn = !failed(ctx.CreateBitmapFromDxgiSurface(texture.Get(), &props, target.GetAddressOf()), "CreateBitmapFromDxgiSurface");
		if (drawn) {
			// The rest of a shared texture belongs to other surfaces: stay inside ours.
			const D2D1_POINT_2F origin = D2D1::Point2F(static_cast<float>(at.x), static_cast<float>(at.y));
			ctx.SetTarget(target.Get());
			ctx.BeginDraw();
			ctx.PushAxisAlignedClip(D2D1::RectF(origin.x, origin.y, origin.x + width, origin.y + height), D2D1_ANTIALIAS_MODE_ALIASED);
			ctx.Clear(D2D1::ColorF(0, 0, 0, 0));
			paint(ctx, origin);
			ctx.PopAxisAlignedClip();
			drawn = !failed(ctx.EndDraw(), "EndDraw");
			ctx.SetTarget(nullptr);
		}
		const bool ended = !failed(surface->EndDraw(), "EndDraw (surface)");
		return drawn && ended ? surface : nullptr;
	}

	std::shared_ptr<device> device_source::get()
	{
		if (!shared && !failed) {
			shared = device::create();
			failed = !shared;
		}
		return shared;
	}

	struct layer::impl {
		device::impl& dev;
		ComPtr<IDCompositionTarget> target;
		ComPtr<IDCompositionVisual> root;
	};

	std::unique_ptr<layer> layer::create(device& dev, HWND hwnd)
	{
		auto p = std::unique_ptr<impl>(new impl{ *dev.p });
		auto& d = *dev.p;

		if (failed(d.dcomp->CreateTargetForHwnd(hwnd, TRUE, p->target.GetAddressOf()), "CreateTargetForHwnd")) return nullptr;
		if (failed(d.dcomp->CreateVisual(p->root.GetAddressOf()), "CreateVisual")) return nullptr;
		if (failed(p->target->SetRoot(p->root.Get()), "SetRoot")) return nullptr;
		if (failed(d.dcomp->Commit(), "Commit")) return nullptr;

		return std::unique_ptr<layer>(new layer(std::move(p)));
	}

	layer::layer(std::unique_ptr<impl> p) : p(std::move(p)) {}
	layer::~layer() = default;

	IDCompositionVisual& layer::root() const { return *p->root.Get(); }

	bool layer::commit()
	{
		return !failed(p->dev.dcomp->Commit(), "Commit");
	}

}
