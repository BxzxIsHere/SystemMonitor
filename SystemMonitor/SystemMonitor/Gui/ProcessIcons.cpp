#include "Gui/ProcessIcons.h"

#include <shellapi.h>

#include <vector>

namespace sysmon::ui {
namespace {

// Turns an icon handle into straight premultiplied BGRA.
//
// Icons are a colour bitmap plus a mask, and older ones leave the alpha channel
// entirely zero, which would come out invisible. Where that happens the mask is
// what says which pixels are really there.
bool ReadIconPixels(HICON icon, std::vector<std::uint32_t>& pixels, unsigned& width,
                    unsigned& height) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return false;

    BITMAP colour{};
    bool ok = GetObjectW(info.hbmColor, sizeof(colour), &colour) != 0;
    width = ok ? static_cast<unsigned>(colour.bmWidth) : 0;
    height = ok ? static_cast<unsigned>(colour.bmHeight) : 0;

    if (ok && width > 0 && height > 0) {
        BITMAPINFO request{};
        request.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        request.bmiHeader.biWidth = static_cast<LONG>(width);
        request.bmiHeader.biHeight = -static_cast<LONG>(height); // top down
        request.bmiHeader.biPlanes = 1;
        request.bmiHeader.biBitCount = 32;
        request.bmiHeader.biCompression = BI_RGB;

        pixels.assign(static_cast<std::size_t>(width) * height, 0);
        const HDC screen = GetDC(nullptr);
        ok = GetDIBits(screen, info.hbmColor, 0, height, pixels.data(), &request,
                       DIB_RGB_COLORS) != 0;

        if (ok) {
            bool anyAlpha = false;
            for (std::uint32_t value : pixels) {
                if ((value >> 24) != 0) {
                    anyAlpha = true;
                    break;
                }
            }

            if (!anyAlpha && info.hbmMask) {
                // Opaque wherever the mask is black, which is the old convention.
                std::vector<std::uint32_t> mask(pixels.size(), 0);
                if (GetDIBits(screen, info.hbmMask, 0, height, mask.data(), &request,
                              DIB_RGB_COLORS)) {
                    for (std::size_t at = 0; at < pixels.size(); ++at) {
                        const bool opaque = (mask[at] & 0x00FFFFFFu) == 0;
                        pixels[at] = opaque ? (pixels[at] | 0xFF000000u) : 0;
                    }
                }
            }

            // Direct2D wants the colour already multiplied by the alpha.
            for (std::uint32_t& value : pixels) {
                const std::uint32_t a = value >> 24;
                if (a == 255) continue;
                const std::uint32_t b = ((value & 0xFF) * a) / 255;
                const std::uint32_t g = (((value >> 8) & 0xFF) * a) / 255;
                const std::uint32_t r = (((value >> 16) & 0xFF) * a) / 255;
                value = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        ReleaseDC(nullptr, screen);
    }

    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return ok && width > 0 && height > 0;
}

slick::ComPtr<ID2D1Bitmap> BuildIcon(slick::Canvas& canvas, const std::wstring& target,
                                     bool byAttributes) {
    SHFILEINFOW info{};
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    DWORD attributes = 0;

    if (byAttributes) {
        // Asking about the attributes rather than the file means the shell
        // answers from the registry alone and never touches the disk.
        flags |= SHGFI_USEFILEATTRIBUTES;
        attributes = FILE_ATTRIBUTE_NORMAL;
    }

    if (!SHGetFileInfoW(target.c_str(), attributes, &info, sizeof(info), flags) || !info.hIcon) {
        return nullptr;
    }

    std::vector<std::uint32_t> pixels;
    unsigned width = 0;
    unsigned height = 0;
    const bool ok = ReadIconPixels(info.hIcon, pixels, width, height);
    DestroyIcon(info.hIcon);

    if (!ok) return nullptr;
    return canvas.CreateBitmap(pixels.data(), width, height);
}

} // namespace

void ProcessIcons::Clear() {
    icons_.clear();
    fallback_.Reset();
    fallbackTried_ = false;
}

const std::wstring* ProcessIcons::PathFor(std::uint32_t pid, int& budget) {
    const auto found = paths_.find(pid);
    if (found != paths_.end()) return &found->second;

    // Resolving costs a handle opened and a path queried, which is a syscall
    // pair per process. Unbudgeted, a fast scroll pays it for every new row on
    // every frame, and the list visibly stutters.
    if (budget <= 0) return nullptr;
    --budget;

    std::wstring path;

    // Limited information is the least that answers this, and it is the most a
    // normal account is granted for another user's process.
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle) {
        wchar_t buffer[MAX_PATH]{};
        DWORD length = ARRAYSIZE(buffer);
        if (QueryFullProcessImageNameW(handle, 0, buffer, &length) && length > 0) {
            path.assign(buffer, length);
        }
        CloseHandle(handle);
    }

    // Process ids are recycled, so this cannot grow for the life of the app.
    if (paths_.size() > 4000) paths_.clear();
    return &paths_.emplace(pid, std::move(path)).first->second;
}

ID2D1Bitmap* ProcessIcons::Fallback(slick::Canvas& canvas) {
    if (!fallbackTried_) {
        fallbackTried_ = true;
        // A name the shell can answer about from the registry alone: the
        // generic executable icon, for everything that cannot be opened.
        fallback_ = BuildIcon(canvas, L"process.exe", true);
    }
    return fallback_.Get();
}

ID2D1Bitmap* ProcessIcons::Acquire(slick::Canvas& canvas, std::uint32_t pid, int& budget) {
    const std::wstring* path = PathFor(pid, budget);
    if (!path || path->empty()) return Fallback(canvas);

    const auto found = icons_.find(*path);
    if (found != icons_.end()) {
        return found->second ? found->second.Get() : Fallback(canvas);
    }

    // Out of budget for this frame: the generic icon holds the place, and the
    // real one arrives a frame or two later. Scrolling never waits on the shell,
    // which matters because asking the shell about a path it has not seen can
    // reach the disk and take tens of milliseconds.
    if (budget <= 0) return Fallback(canvas);
    --budget;

    if (icons_.size() > 2000) icons_.clear();

    auto bitmap = BuildIcon(canvas, *path, false);
    ID2D1Bitmap* raw = bitmap.Get();
    icons_.emplace(*path, std::move(bitmap));
    return raw ? raw : Fallback(canvas);
}

} // namespace sysmon::ui
