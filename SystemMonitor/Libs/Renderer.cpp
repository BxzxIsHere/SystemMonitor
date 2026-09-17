#include "Libs/Renderer.h"

#include <d2d1_1helper.h>

#include <algorithm>

namespace slick {
namespace {

// 9_1 is kept in the list so the UI still comes up on a WARP fallback or a
// remote-desktop adapter; Direct2D is happy with any of them.
constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
    D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_1,
};

bool IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == D2DERR_RECREATE_TARGET;
}

} // namespace

Renderer::~Renderer() {
    // Direct2D must let go of the back buffer before the swap chain dies, and
    // D3D wants a flushed context before the device is released.
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    if (d3dContext_) {
        d3dContext_->ClearState();
        d3dContext_->Flush();
    }
}

void Renderer::Attach(HWND hwnd, unsigned dpi) {
    hwnd_ = hwnd;
    dpi_ = dpi ? dpi : 96;

    RECT client{};
    GetClientRect(hwnd_, &client);
    pixelWidth_ = std::max<unsigned>(1, client.right - client.left);
    pixelHeight_ = std::max<unsigned>(1, client.bottom - client.top);

    Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())),
          "DWriteCreateFactory");

    CreateDeviceResources();
    CreateSwapChain();
    CreateTargetBitmap();
}

void Renderer::CreateDeviceResources() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    // Only available when the optional graphics tools feature is installed, so
    // the call is retried without it below.
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    auto create = [&](D3D_DRIVER_TYPE type, UINT createFlags) {
        return D3D11CreateDevice(nullptr, type, nullptr, createFlags, kFeatureLevels,
                                 ARRAYSIZE(kFeatureLevels), D3D11_SDK_VERSION,
                                 d3dDevice_.ReleaseAndGetAddressOf(), nullptr,
                                 d3dContext_.ReleaseAndGetAddressOf());
    };

    HRESULT hr = create(D3D_DRIVER_TYPE_HARDWARE, flags);
#if defined(_DEBUG)
    if (FAILED(hr)) {
        hr = create(D3D_DRIVER_TYPE_HARDWARE, flags & ~D3D11_CREATE_DEVICE_DEBUG);
    }
#endif
    if (FAILED(hr)) hr = create(D3D_DRIVER_TYPE_WARP, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
    Check(hr, "D3D11CreateDevice");

    ComPtr<IDXGIDevice1> dxgiDevice;
    Check(d3dDevice_.As(&dxgiDevice), "ID3D11Device as IDXGIDevice1");
    // One frame of latency keeps the UI feeling immediate rather than buffered.
    dxgiDevice->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> baseAdapter;
    Check(dxgiDevice->GetAdapter(&baseAdapter), "IDXGIDevice::GetAdapter");
    Check(baseAdapter.As(&adapter_), "IDXGIAdapter as IDXGIAdapter1");
    Check(adapter_->GetParent(IID_PPV_ARGS(dxgiFactory_.ReleaseAndGetAddressOf())),
          "IDXGIAdapter::GetParent(IDXGIFactory2)");

    D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                            &options,
                            reinterpret_cast<void**>(d2dFactory_.ReleaseAndGetAddressOf())),
          "D2D1CreateFactory");
    Check(d2dFactory_->CreateDevice(dxgiDevice.Get(), d2dDevice_.ReleaseAndGetAddressOf()),
          "ID2D1Factory1::CreateDevice");
    Check(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                          dc_.ReleaseAndGetAddressOf()),
          "ID2D1Device::CreateDeviceContext");

    // Greyscale AA: subpixel ClearType fringes are very visible against a dark
    // surface, and greyscale is what the flip-model compositor expects anyway.
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

void Renderer::CreateSwapChain() {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = pixelWidth_;
    desc.Height = pixelHeight_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    // NONE pins content to the top-left while a live resize catches up, which
    // reads as a crisp edge instead of a rubber-banding stretch.
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    Check(dxgiFactory_->CreateSwapChainForHwnd(d3dDevice_.Get(), hwnd_, &desc, nullptr,
                                               nullptr,
                                               swapChain_.ReleaseAndGetAddressOf()),
          "IDXGIFactory2::CreateSwapChainForHwnd");

    // The window owns its own behaviour; the DXGI Alt+Enter handler would
    // fight it by flipping into exclusive fullscreen.
    dxgiFactory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
}

void Renderer::CreateTargetBitmap() {
    ComPtr<IDXGISurface> surface;
    Check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)), "IDXGISwapChain::GetBuffer");

    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(dpi_), static_cast<float>(dpi_));

    Check(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props,
                                           target_.ReleaseAndGetAddressOf()),
          "CreateBitmapFromDxgiSurface");
    dc_->SetTarget(target_.Get());
}

void Renderer::ReleaseTargetBitmap() {
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    if (d3dContext_) d3dContext_->Flush();
}

void Renderer::Resize(unsigned pixelWidth, unsigned pixelHeight) {
    pixelWidth = std::max(1u, pixelWidth);
    pixelHeight = std::max(1u, pixelHeight);
    if (pixelWidth == pixelWidth_ && pixelHeight == pixelHeight_) return;

    pixelWidth_ = pixelWidth;
    pixelHeight_ = pixelHeight;
    if (!swapChain_) return;

    // Flip-model swap chains refuse to resize while anything still references a
    // back buffer, so the Direct2D target has to go first.
    ReleaseTargetBitmap();
    const HRESULT hr = swapChain_->ResizeBuffers(0, pixelWidth_, pixelHeight_,
                                                 DXGI_FORMAT_UNKNOWN, 0);
    if (IsDeviceLost(hr)) {
        RebuildAfterDeviceLoss();
        return;
    }
    Check(hr, "IDXGISwapChain::ResizeBuffers");
    CreateTargetBitmap();
}

void Renderer::SetDpi(unsigned dpi) {
    dpi = dpi ? dpi : 96;
    if (dpi == dpi_) return;
    dpi_ = dpi;
    if (!swapChain_) return;

    // The DPI lives on the target bitmap, so it just gets rebuilt around the
    // same back buffer.
    ReleaseTargetBitmap();
    CreateTargetBitmap();
}

Size Renderer::LogicalSize() const {
    const float scale = Scale();
    return Size{pixelWidth_ / scale, pixelHeight_ / scale};
}

bool Renderer::BeginFrame() {
    if (!Ready()) return false;

    if (occluded_) {
        // Cheapest way to ask DXGI whether the window is back on screen.
        if (swapChain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) return false;
        occluded_ = false;
    }

    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Renderer::EndFrame() {
    if (!Ready()) return;

    HRESULT hr = dc_->EndDraw();
    if (IsDeviceLost(hr)) {
        RebuildAfterDeviceLoss();
        return;
    }
    Check(hr, "ID2D1DeviceContext::EndDraw");

    // Vsync present, which is also what paces the whole render loop.
    hr = swapChain_->Present(1, 0);
    if (hr == DXGI_STATUS_OCCLUDED) {
        occluded_ = true;
        return;
    }
    if (IsDeviceLost(hr)) {
        RebuildAfterDeviceLoss();
        return;
    }
    Check(hr, "IDXGISwapChain::Present");
}

void Renderer::RebuildAfterDeviceLoss() {
    // A dropped GPU, a driver update, or a TDR. Everything below the window is
    // recreated from scratch and callers only notice one skipped frame.
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    swapChain_.Reset();
    dc_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();
    dxgiFactory_.Reset();
    adapter_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();

    CreateDeviceResources();
    CreateSwapChain();
    CreateTargetBitmap();
}

} // namespace slick
