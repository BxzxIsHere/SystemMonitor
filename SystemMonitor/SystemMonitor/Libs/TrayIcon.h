#pragma once

#include "Libs/Platform.h"

#include <shellapi.h>

#include <string>

namespace slick {

// A notification area icon bound to a window.
//
// The shell reports every interaction with the icon back to the owner as one
// private message, so the owner hands that message straight here and gets back
// what the user actually meant by it.
class TrayIcon {
public:
    enum class Action {
        None,
        Open, // left click or double click: bring the window back
        Menu, // right click: the owner should show its context menu
    };

    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Add(HWND owner, UINT callbackMessage, HICON icon, const std::wstring& tooltip);
    void Remove();
    bool Present() const { return present_; }

    static Action Interpret(LPARAM lParam);

private:
    NOTIFYICONDATAW data_{};
    bool present_ = false;
};

} // namespace slick
