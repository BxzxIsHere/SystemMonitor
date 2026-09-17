#include "Libs/TrayIcon.h"

#include <algorithm>

namespace slick {

TrayIcon::~TrayIcon() {
    Remove();
}

bool TrayIcon::Add(HWND owner, UINT callbackMessage, HICON icon, const std::wstring& tooltip) {
    if (present_) return true;

    data_ = {};
    data_.cbSize = sizeof(data_);
    data_.hWnd = owner;
    data_.uID = 1;
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data_.uCallbackMessage = callbackMessage;
    data_.hIcon = icon;

    // szTip is a fixed width field, so an over-long tooltip is simply cut
    // rather than allowed to run off the end of the structure.
    const std::size_t length = std::min<std::size_t>(tooltip.size(), ARRAYSIZE(data_.szTip) - 1);
    tooltip.copy(data_.szTip, length);
    data_.szTip[length] = L'\0';

    // The shell can refuse, most often because it is still starting up. The
    // caller has to cope rather than assume the icon is there.
    present_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    return present_;
}

void TrayIcon::Remove() {
    if (!present_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    present_ = false;
}

TrayIcon::Action TrayIcon::Interpret(LPARAM lParam) {
    // Without an explicit version the shell uses the original convention, where
    // the callback carries the mouse message itself.
    switch (static_cast<UINT>(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            return Action::Open;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            return Action::Menu;

        default:
            return Action::None;
    }
}

} // namespace slick
