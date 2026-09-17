#pragma once

#include "Libs/Com.h"
#include "Libs/Types.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_6.h>

namespace slick {

// Owns the Direct3D 11 device, the flip-model swap chain bound to the window,
// and the Direct2D device context that draws into its back buffer.
//
// Direct2D is given the real window DPI, so every coordinate handed to the
// context is a DIP and the whole UI scales without any per-call arithmetic.
class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Attach(HWND hwnd, unsigned dpi);

    // Both are no-ops when nothing actually changed, so the window can call
    // them freely from WM_SIZE and WM_DPICHANGED.
    void Resize(unsigned pixelWidth, unsigned pixelHeight);
    void SetDpi(unsigned dpi);

    // False means the frame should be skipped: the window is fully occluded,
    // or the device was lost and has just been rebuilt.
    bool BeginFrame();
    void EndFrame();

    ID2D1DeviceContext* Dc() const { return dc_.Get(); }
    IDWriteFactory* Dwrite() const { return dwrite_.Get(); }

    Size LogicalSize() const;
    float Scale() const { return dpi_ / 96.0f; }
    unsigned Dpi() const { return dpi_; }
    bool Ready() const { return dc_ && target_; }

private:
    void CreateDeviceResources();
    void CreateSwapChain();
    void CreateTargetBitmap();
    void ReleaseTargetBitmap();
    void RebuildAfterDeviceLoss();

    HWND hwnd_ = nullptr;
    unsigned dpi_ = 96;
    unsigned pixelWidth_ = 0;
    unsigned pixelHeight_ = 0;
    bool occluded_ = false;

    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIFactory2> dxgiFactory_;
    ComPtr<IDXGISwapChain1> swapChain_;

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;

    ComPtr<IDWriteFactory> dwrite_;
};

} // namespace slick
