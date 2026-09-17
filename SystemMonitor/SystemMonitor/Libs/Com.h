#pragma once

#include "Libs/Platform.h"

#include <wrl/client.h>

#include <stdexcept>
#include <string>

namespace slick {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// Direct3D, DXGI, Direct2D and DirectWrite all report failure through HRESULT.
// None of those failures are locally recoverable -- the one that is, device
// loss, is detected at Present time and handled by rebuilding the renderer --
// so everything else unwinds to WinMain and is shown to the user once.
class ComError : public std::runtime_error {
public:
    ComError(HRESULT hr, const char* context)
        : std::runtime_error(context), hr_(hr) {}

    HRESULT Code() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

inline void Check(HRESULT hr, const char* context) {
    if (FAILED(hr)) throw ComError(hr, context);
}

} // namespace slick
