#include "dx.h"

#include <algorithm>
#include <format>

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "logger.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
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
			// BGRA_SUPPORT is required for Direct2D interop.
			const HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
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
		ComPtr<IDXGIFactory2> factory;
		ComPtr<ID2D1DeviceContext> d2d;
		ComPtr<IDWriteFactory> dwrite;
		ComPtr<IDCompositionDevice> dcomp;
	};

	std::unique_ptr<device> device::create()
	{
		auto p = std::make_unique<impl>();

		p->d3d = create_d3d();
		if (!p->d3d) return nullptr;

		ComPtr<IDXGIAdapter> adapter;
		if (failed(p->d3d.As(&p->dxgi), "IDXGIDevice")) return nullptr;
		if (failed(p->dxgi->GetAdapter(adapter.GetAddressOf()), "GetAdapter")) return nullptr;
		if (failed(adapter->GetParent(IID_PPV_ARGS(p->factory.GetAddressOf())), "IDXGIFactory2")) return nullptr;

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
		ComPtr<IDXGISwapChain1> chain;
		ComPtr<IDCompositionTarget> target;
		ComPtr<IDCompositionVisual> visual;
	};

	std::unique_ptr<layer> layer::create(device& dev, HWND hwnd, UINT width, UINT height)
	{
		auto p = std::unique_ptr<impl>(new impl{ *dev.p });
		auto& d = *dev.p;

		DXGI_SWAP_CHAIN_DESC1 sd{};
		sd.Width = std::max(1u, width);
		sd.Height = std::max(1u, height);
		sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.BufferCount = 2;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.SampleDesc.Count = 1;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

		if (failed(d.factory->CreateSwapChainForComposition(d.d3d.Get(), &sd, nullptr, p->chain.GetAddressOf()), "CreateSwapChainForComposition")) return nullptr;
		if (failed(d.dcomp->CreateTargetForHwnd(hwnd, TRUE, p->target.GetAddressOf()), "CreateTargetForHwnd")) return nullptr;
		if (failed(d.dcomp->CreateVisual(p->visual.GetAddressOf()), "CreateVisual")) return nullptr;
		if (failed(p->visual->SetContent(p->chain.Get()), "SetContent")) return nullptr;
		if (failed(p->target->SetRoot(p->visual.Get()), "SetRoot")) return nullptr;
		if (failed(d.dcomp->Commit(), "Commit")) return nullptr;

		return std::unique_ptr<layer>(new layer(std::move(p)));
	}

	layer::layer(std::unique_ptr<impl> p) : p(std::move(p)) {}
	layer::~layer() = default;

	bool layer::resize(UINT width, UINT height)
	{
		return !failed(p->chain->ResizeBuffers(0, std::max(1u, width), std::max(1u, height), DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
	}

	bool layer::draw(const std::function<void(ID2D1DeviceContext&)>& paint)
	{
		ID2D1DeviceContext& ctx = *p->dev.d2d.Get();

		// Flip model rotates buffers, so bind the current back buffer on every draw.
		ComPtr<IDXGISurface> surface;
		if (failed(p->chain->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf())), "GetBuffer")) return false;

		const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
			D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		ComPtr<ID2D1Bitmap1> target;
		if (failed(ctx.CreateBitmapFromDxgiSurface(surface.Get(), &props, target.GetAddressOf()), "CreateBitmapFromDxgiSurface")) return false;

		ctx.SetTarget(target.Get());
		ctx.BeginDraw();
		ctx.Clear(D2D1::ColorF(0, 0, 0, 0));
		paint(ctx);
		const HRESULT drawn = ctx.EndDraw();
		ctx.SetTarget(nullptr);
		if (failed(drawn, "EndDraw")) return false;

		return !failed(p->chain->Present(1, 0), "Present");
	}

}
