#include "Libs/Clipboard.h"

#include <cstring>

namespace slick {
namespace {

// Closes the clipboard however the surrounding code leaves, including on an
// early return part way through building the copy.
class ClipboardSession {
public:
    explicit ClipboardSession(HWND owner) : open_(OpenClipboard(owner) != FALSE) {}
    ~ClipboardSession() {
        if (open_) CloseClipboard();
    }

    ClipboardSession(const ClipboardSession&) = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;

    bool Open() const { return open_; }

private:
    bool open_ = false;
};

} // namespace

std::wstring ReadClipboardText(HWND owner) {
    ClipboardSession session(owner);
    if (!session.Open()) return {};

    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return {};

    const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!text) return {};

    std::wstring copy(text);
    GlobalUnlock(handle);
    return copy;
}

void WriteClipboardText(HWND owner, std::wstring_view text) {
    ClipboardSession session(owner);
    if (!session.Open()) return;

    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    const HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!block) return;

    auto* destination = static_cast<wchar_t*>(GlobalLock(block));
    if (!destination) {
        GlobalFree(block);
        return;
    }
    std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    destination[text.size()] = L'\0';
    GlobalUnlock(block);

    EmptyClipboard();
    // Ownership passes to the clipboard on success; on failure it is ours again.
    if (!SetClipboardData(CF_UNICODETEXT, block)) GlobalFree(block);
}

} // namespace slick
