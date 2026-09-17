#include "Libs/Window.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace slick {
namespace {

// Fires while something else owns the message pump, so animations keep running
// instead of freezing. Two things take the pump away from the render loop: the
// modal move/size loop when the window is grabbed, and TrackPopupMenu while a
// context menu is open. Both post WM_TIMER to the owner, so one timer serves
// both and the window carries on drawing underneath.
constexpr UINT_PTR kModalRenderTimer = 1;

// Eight milliseconds, which keeps a sixty frame animation running with a frame
// to spare and costs nothing while nothing is animating.
constexpr UINT kModalRenderIntervalMs = 8;

constexpr float kButtonWidth = 46.0f;
constexpr float kSystemMenuWidth = 30.0f;

// Private message the shell uses to report interaction with the tray icon.
constexpr UINT kTrayCallbackMessage = WM_APP + 1;

constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuExit = 2;

// Windows 10 1809 is where the shell first understood a dark menu.
constexpr unsigned kFirstDarkModeBuild = 17763;

// Asks the shell to draw this process's menus dark.
//
// There is no documented way to do this: uxtheme exports the entry point by
// ordinal only, which is why every dark themed Win32 app calls it exactly like
// this. It is strictly cosmetic, so if anything about the lookup fails the
// context menu just falls back to the ordinary light styling.
void EnableDarkMenus() {
    if (WindowsBuildNumber() < kFirstDarkModeBuild) return;

    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    constexpr int kForceDark = 2;

    const HMODULE uxtheme =
        LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;

    const auto setMode = reinterpret_cast<SetPreferredAppModeFn>(
        GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    const auto flush =
        reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));

    if (setMode) setMode(kForceDark);
    if (flush) flush();
}

COLORREF ToColorRef(const Color& c) {
    auto channel = [](float v) {
        return static_cast<DWORD>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(channel(c.r), channel(c.g), channel(c.b));
}

// A maximised window must leave a sliver of non-client space on whichever edge
// hosts an auto-hiding taskbar, otherwise the taskbar can never be summoned.
bool HasAutoHideTaskbar(HMONITOR monitor, UINT edge) {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.uEdge = edge;

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return false;
    data.rc = info.rcMonitor;

    return SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data) != 0;
}

// The usable area of whichever monitor the window is on, in physical pixels.
// Everything a window is sized to has to fit inside this, and at 200% scaling a
// perfectly reasonable design size does not.
SIZE WorkAreaFor(HWND window, POINT fallbackPoint) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);

    const HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                    : MonitorFromPoint(fallbackPoint, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(monitor, &info)) return SIZE{1280, 720};

    return SIZE{info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top};
}

} // namespace

Window::Window() : theme_(&Theme::Dark()) {
    // Exact frame pacing without raising the system-wide timer resolution,
    // which is what timeBeginPeriod would do and what it would cost every other
    // process on the machine.
    frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_MANUAL_RESET |
                                             CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
}

Window::~Window() {
    // Normally the window is already gone by the time this runs, having been
    // closed by the user. If it is not, it is torn down here, with the back
    // pointer cleared first so the remaining messages go straight to
    // DefWindowProc rather than into a half destroyed object.
    if (hwnd_ && IsWindow(hwnd_)) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;

    if (frameTimer_) {
        CloseHandle(frameTimer_);
        frameTimer_ = nullptr;
    }
}

LRESULT CALLBACK Window::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Window* self = nullptr;

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Window*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) return self->HandleMessage(message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void Window::Create(const WindowConfig& config, WindowDelegate& delegate) {
    config_ = config;
    delegate_ = &delegate;
    delegate_->OnAttach(*this);

    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Matching the base colour means even the split second before the first
    // present looks like the finished window rather than a white flash.
    wc.hbrBackground = CreateSolidBrush(ToColorRef(theme_->windowBase));
    wc.lpszClassName = config_.className.c_str();

    LoadIcons(instance);
    wc.hIcon = largeIcon_;
    wc.hIconSm = smallIcon_;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, config_.className.c_str(), config_.title.c_str(),
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) Check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowEx");

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) dpi_ = 96;

    // Explorer broadcasts this when it restarts, which is the only warning we
    // get that the notification area has thrown our icon away.
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    ApplyFrameExtensions();

    // Size and position are applied after the frame change so the window lands
    // at exactly the requested logical size on whichever monitor it opens on.
    const float scale = dpi_ / 96.0f;

    POINT cursor{};
    GetCursorPos(&cursor);
    const SIZE work = WorkAreaFor(hwnd_, cursor);

    // The design size is in DIPs, so at 200% scaling it asks for twice as many
    // pixels as it does at 100% and a window that fits comfortably on one
    // machine hangs off the bottom of the screen on another. Shrunk to fit, on
    // both axes by the same factor so the proportions the layout was designed
    // around survive.
    float width = config_.initialSize.width * scale;
    float height = config_.initialSize.height * scale;

    const float room = std::min(static_cast<float>(work.cx) * 0.94f / width,
                                static_cast<float>(work.cy) * 0.94f / height);
    if (room < 1.0f) {
        width *= room;
        height *= room;
    }

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (config_.centreOnScreen) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &info)) {
            x = info.rcWork.left +
                static_cast<int>(((info.rcWork.right - info.rcWork.left) - width) / 2);
            y = info.rcWork.top +
                static_cast<int>(((info.rcWork.bottom - info.rcWork.top) - height) / 2);
        }
    }

    SetWindowPos(hwnd_, nullptr, x, y, static_cast<int>(width), static_cast<int>(height),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    InstallTrayIcon();

    renderer_.Attach(hwnd_, dpi_);
    canvas_ = std::make_unique<Canvas>(renderer_);
    delegate_->OnResize(ContentSize());

    // One frame before the window is ever shown: the reveal animation then
    // plays over finished content instead of an empty surface.
    RenderFrame();
}

void Window::ApplyFrameExtensions() {
    // Extending the frame by a single pixel is what buys back the standard
    // drop shadow once the non-client area has been taken over.
    const MARGINS shadow{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd_, &shadow);

    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    EnableDarkMenus();

    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    const HRESULT rounded =
        DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    const COLORREF border = ToColorRef(theme_->borderStrong);
    const HRESULT outlined =
        DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));

    // Windows 10 has neither attribute, so the outline is drawn by hand there.
    drawOwnBorder_ = FAILED(rounded) || FAILED(outlined);
}

void Window::Show(int showCommand) {
    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
}

void Window::RequestClose() {
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void Window::LoadIcons(HINSTANCE instance) {
    if (config_.iconResourceId == 0) return;

    const auto id = MAKEINTRESOURCEW(config_.iconResourceId);
    // Asking for each size by name lets the shell pick the matching frame out
    // of the icon rather than rescaling one of the others.
    auto load = [&](int metric) {
        return static_cast<HICON>(LoadImageW(instance, id, IMAGE_ICON,
                                             GetSystemMetrics(metric),
                                             GetSystemMetrics(metric + 1),
                                             LR_DEFAULTCOLOR | LR_SHARED));
    };

    largeIcon_ = load(SM_CXICON);
    smallIcon_ = load(SM_CXSMICON);
}

void Window::InstallTrayIcon() {
    if (!config_.showTrayIcon || !smallIcon_ || tray_.Present()) return;
    // A refusal is not fatal: the window keeps its ordinary taskbar button, so
    // there is always a way to reach it.
    tray_.Add(hwnd_, kTrayCallbackMessage, smallIcon_, config_.title);
}

void Window::ActivateFromTray() {
    // The icon stays put. It sits alongside the taskbar button rather than
    // replacing it, so clicking it only has to surface the window.
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    // Allowed here because the click on our own icon made this process
    // foreground-eligible in the first place.
    SetForegroundWindow(hwnd_);
}

unsigned ShowContextMenu(HWND owner, POINT screen, const std::vector<MenuItem>& items) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;

    for (const MenuItem& item : items) {
        if (item.text.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        AppendMenuW(menu, MF_STRING | (item.enabled ? MF_ENABLED : MF_GRAYED), item.id,
                    item.text.c_str());
        if (item.isDefault) SetMenuDefaultItem(menu, item.id, FALSE);
    }

    // Taking the foreground first, and posting afterwards, is the documented
    // dance that lets the menu dismiss when the user clicks elsewhere.
    SetForegroundWindow(owner);

    // TrackPopupMenu does not return until the menu closes, and it runs its own
    // message loop while it is up. Without this the window behind it stops
    // being drawn and the whole dashboard appears to freeze for as long as the
    // menu is open.
    SetTimer(owner, kModalRenderTimer, kModalRenderIntervalMs, nullptr);
    const int choice =
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y,
                       0, owner, nullptr);
    KillTimer(owner, kModalRenderTimer);

    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);

    return choice > 0 ? static_cast<unsigned>(choice) : 0u;
}

void Window::ShowTrayMenu() {
    POINT cursor{};
    GetCursorPos(&cursor);

    // The same helper the content uses, so the tray menu is themed identically
    // and the window keeps drawing while it is open.
    const std::vector<MenuItem> items{
        {kMenuOpen, L"Open " + config_.title, true, true},
        {0, L"", true, false},
        {kMenuExit, L"Exit", true, false},
    };

    const unsigned choice = ShowContextMenu(hwnd_, cursor, items);
    if (choice == kMenuOpen) ActivateFromTray();
    else if (choice == kMenuExit) RequestClose();
}

bool Window::Maximised() const {
    return hwnd_ && IsZoomed(hwnd_);
}

Size Window::ContentSize() const {
    const Size logical = renderer_.LogicalSize();
    return Size{logical.width, std::max(0.0f, logical.height - CaptionHeight())};
}

Point Window::ToLogical(POINT clientPoint) const {
    const float scale = dpi_ / 96.0f;
    return Point{clientPoint.x / scale, clientPoint.y / scale};
}

int Window::FrameGrabThickness() const {
    // The padded border is what makes the grab band comfortably wide on modern
    // themes; without it the window has the razor-thin edge of classic frames.
    return GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi_) +
           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
}

int Window::RunMessageLoop() {
    using Clock = std::chrono::steady_clock;

    const auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / std::max(1u, config_.targetFrameRate)));
    auto nextFrame = Clock::now();

    MSG message{};
    for (;;) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return static_cast<int>(message.wParam);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (quitting_) return 0;

        if (IsIconic(hwnd_) || !IsWindowVisible(hwnd_)) {
            // Minimised, or parked in the tray. Nothing to draw either way, so
            // block until the shell has something for us rather than spinning.
            WaitMessage();
            nextFrame = Clock::now();
            continue;
        }

        const auto now = Clock::now();
        if (now < nextFrame) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::nanoseconds>(nextFrame - now).count();

            // Idles until the next frame is due, but returns the instant the
            // shell has a message, so input never waits on the clock.
            //
            // The wait has to be a high resolution timer rather than a timeout
            // in milliseconds. A timeout is rounded up to the system timer
            // granularity, which is 15.6 ms by default: ask to be woken in
            // 16 ms for a sixty frame loop and Windows wakes you in 31, so the
            // loop runs at half the rate it was asked for and every animation
            // on screen judders. This timer is exact and costs nothing.
            if (remaining > 0) {
                if (frameTimer_) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -(remaining / 100); // relative, 100 ns units
                    SetWaitableTimer(frameTimer_, &due, 0, nullptr, nullptr, FALSE);
                    MsgWaitForMultipleObjectsEx(1, &frameTimer_, INFINITE, QS_ALLINPUT,
                                                MWMO_INPUTAVAILABLE);
                } else {
                    const auto milliseconds = remaining / 1'000'000;
                    if (milliseconds > 0) {
                        MsgWaitForMultipleObjectsEx(0, nullptr,
                                                    static_cast<DWORD>(milliseconds),
                                                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    }
                }
            }
            continue;
        }

        // Set from now rather than advanced by one interval, so a stall does
        // not leave a queue of catch-up frames to burn through.
        nextFrame = now + interval;


        if (!renderer_.BeginFrame()) {
            // Fully occluded. Back off instead of hammering the test present.
            Sleep(32);
            continue;
        }

        DrawChrome(*canvas_);
        renderer_.EndFrame();
    }
}

void Window::RenderFrame() {
    if (!canvas_ || !renderer_.BeginFrame()) return;
    DrawChrome(*canvas_);
    renderer_.EndFrame();
}

Rect Window::CaptionButtonRect(CaptionButton button) const {
    const Size logical = renderer_.LogicalSize();
    const float height = CaptionHeight();
    const float right = logical.width;

    switch (button) {
        case CaptionButton::Close:
            return Rect{right - kButtonWidth, 0.0f, right, height};
        case CaptionButton::Maximise:
            return Rect{right - kButtonWidth * 2.0f, 0.0f, right - kButtonWidth, height};
        case CaptionButton::Minimise:
            return Rect{right - kButtonWidth * 3.0f, 0.0f, right - kButtonWidth * 2.0f, height};
        default:
            return Rect{};
    }
}

Window::CaptionButton Window::ButtonAtScreenPoint(POINT screenPoint) const {
    POINT client = screenPoint;
    ScreenToClient(hwnd_, &client);
    const Point logical = ToLogical(client);

    for (auto button : {CaptionButton::Minimise, CaptionButton::Maximise, CaptionButton::Close}) {
        if (CaptionButtonRect(button).Contains(logical)) return button;
    }
    return CaptionButton::None;
}

void Window::SetHoveredButton(CaptionButton button) {
    if (hovered_ == button) return;
    hovered_ = button;
    // The next frame picks the change up; the render loop is always running.
}

void Window::TrackMouseLeave(bool nonClient) {
    bool& flag = nonClient ? trackingNonClient_ : trackingClient_;
    if (flag) return;

    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE | (nonClient ? TME_NONCLIENT : 0u);
    track.hwndTrack = hwnd_;
    if (TrackMouseEvent(&track)) flag = true;
}

LRESULT Window::OnNcCalcSize(WPARAM wParam, LPARAM lParam) {
    if (wParam == FALSE) return DefWindowProcW(hwnd_, WM_NCCALCSIZE, wParam, lParam);

    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
    RECT& client = params->rgrc[0];

    // Returning the proposed window rect untouched hands the entire frame,
    // caption included, to the client area. Resize borders are reinstated in
    // OnNcHitTest, so the window still behaves like any other.
    if (IsZoomed(hwnd_)) {
        // A maximised window is deliberately inflated by the frame thickness so
        // the borders sit off-screen; without this inset the content would be
        // clipped by the monitor edges.
        const int frameX = GetSystemMetricsForDpi(SM_CXFRAME, dpi_);
        const int frameY = GetSystemMetricsForDpi(SM_CYFRAME, dpi_);
        const int padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);

        client.left += frameX + padding;
        client.right -= frameX + padding;
        client.top += frameY + padding;
        client.bottom -= frameY + padding;

        const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        if (HasAutoHideTaskbar(monitor, ABE_BOTTOM)) client.bottom -= 1;
        else if (HasAutoHideTaskbar(monitor, ABE_TOP)) client.top += 1;
        else if (HasAutoHideTaskbar(monitor, ABE_LEFT)) client.left += 1;
        else if (HasAutoHideTaskbar(monitor, ABE_RIGHT)) client.right -= 1;
    }

    return 0;
}

LRESULT Window::OnNcHitTest(LPARAM lParam) {
    const POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

    RECT window{};
    GetWindowRect(hwnd_, &window);

    if (!IsZoomed(hwnd_)) {
        const int grab = FrameGrabThickness();
        const bool left = screen.x < window.left + grab;
        const bool right = screen.x >= window.right - grab;
        const bool top = screen.y < window.top + grab;
        const bool bottom = screen.y >= window.bottom - grab;

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }

    POINT client = screen;
    ScreenToClient(hwnd_, &client);
    const Point logical = ToLogical(client);

    if (logical.y >= CaptionHeight()) return HTCLIENT;

    // Reporting the real button codes is what lets Windows 11 attach its snap
    // layout flyout to the maximise button, exactly as it does for any app.
    if (CaptionButtonRect(CaptionButton::Close).Contains(logical)) return HTCLOSE;
    if (CaptionButtonRect(CaptionButton::Maximise).Contains(logical)) return HTMAXBUTTON;
    if (CaptionButtonRect(CaptionButton::Minimise).Contains(logical)) return HTMINBUTTON;

    if (delegate_ && delegate_->WantsMouseInCaption(logical)) return HTCLIENT;

    // The logo square doubles as the system menu, the way a titled window does.
    if (logical.x < kSystemMenuWidth) return HTSYSMENU;

    return HTCAPTION;
}

LRESULT Window::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCCALCSIZE:
            return OnNcCalcSize(wParam, lParam);

        case WM_NCHITTEST:
            return OnNcHitTest(lParam);

        case WM_NCACTIVATE:
            // There is no system caption left to repaint, and letting
            // DefWindowProc try would flash the old frame over ours.
            active_ = wParam != FALSE;
            return TRUE;

        case WM_ACTIVATE:
            active_ = LOWORD(wParam) != WA_INACTIVE;
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            RenderFrame();
            ValidateRect(hwnd_, nullptr);
            return 0;

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                renderer_.Resize(LOWORD(lParam), HIWORD(lParam));
                if (delegate_) delegate_->OnResize(ContentSize());
                // Painting inline keeps a live resize glued to the cursor.
                RenderFrame();
            }
            break;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            renderer_.SetDpi(dpi_);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            // The system pre-fills sensible defaults, so only the floor needs
            // replacing; maximise geometry is left to DefWindowProc.
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const float scale = dpi_ / 96.0f;

            // Scaled the same way as everything else, and then held below the
            // work area. A minimum of 980 by 660 DIPs is 1960 by 1320 pixels at
            // 200%, which is larger than a 1080p screen: without this the window
            // could not be made small enough to fit the monitor it is on.
            POINT origin{0, 0};
            const SIZE work = WorkAreaFor(hwnd_, origin);
            info->ptMinTrackSize.x =
                std::min(static_cast<LONG>(config_.minimumSize.width * scale),
                         static_cast<LONG>(work.cx * 0.9f));
            info->ptMinTrackSize.y =
                std::min(static_cast<LONG>(config_.minimumSize.height * scale),
                         static_cast<LONG>(work.cy * 0.9f));
            return 0;
        }

        case WM_ENTERSIZEMOVE:
            inSizeMove_ = true;
            SetTimer(hwnd_, kModalRenderTimer, kModalRenderIntervalMs, nullptr);
            break;

        case WM_EXITSIZEMOVE:
            inSizeMove_ = false;
            KillTimer(hwnd_, kModalRenderTimer);
            break;

        case WM_TIMER:
            if (wParam == kModalRenderTimer) RenderFrame();
            return 0;

        case WM_MOUSEMOVE: {
            TrackMouseLeave(false);
            SetHoveredButton(CaptionButton::None);
            const POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (delegate_) delegate_->OnMouseMove(ToLogical(client));
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingClient_ = false;
            if (delegate_) delegate_->OnMouseLeave();
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (delegate_) delegate_->OnMouseDown(ToLogical(client));
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (delegate_) delegate_->OnMouseUp(ToLogical(client));
            return 0;
        }

        case WM_CHAR:
            // Control characters arrive here too; the delegate decides what is
            // worth keeping, since backspace and escape are useful and the rest
            // is not.
            if (delegate_) delegate_->OnChar(static_cast<wchar_t>(wParam));
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (delegate_) {
                delegate_->OnKeyDown(static_cast<int>(wParam),
                                     (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                     (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            }
            // Alt combinations still belong to the system menu.
            if (message == WM_SYSKEYDOWN) break;
            return 0;
        }

        case WM_RBUTTONUP: {
            // On the release rather than the press, which is where every other
            // Windows application puts its context menu.
            const POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT screen = client;
            ClientToScreen(hwnd_, &screen);
            if (delegate_) delegate_->OnRightClick(ToLogical(client), screen);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &client);
            const float ticks = GET_WHEEL_DELTA_WPARAM(wParam) / static_cast<float>(WHEEL_DELTA);
            if (delegate_) delegate_->OnScroll(ToLogical(client), ticks);
            return 0;
        }

        case WM_NCMOUSEMOVE: {
            const POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            SetHoveredButton(ButtonAtScreenPoint(screen));
            TrackMouseLeave(true);
            if (hovered_ != CaptionButton::None) return 0;
            break;
        }

        case WM_NCMOUSELEAVE:
            trackingNonClient_ = false;
            SetHoveredButton(CaptionButton::None);
            break;

        case WM_NCLBUTTONDOWN: {
            const POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const CaptionButton hit = ButtonAtScreenPoint(screen);
            if (hit != CaptionButton::None) {
                pressed_ = hit;
                hovered_ = hit;
                // Swallowed so DefWindowProc does not start its own caption
                // button loop over buttons that do not exist any more.
                return 0;
            }
            pressed_ = CaptionButton::None;
            break;
        }

        case WM_NCLBUTTONUP: {
            if (pressed_ == CaptionButton::None) break;

            const POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const CaptionButton released = ButtonAtScreenPoint(screen);
            const CaptionButton action = released == pressed_ ? released : CaptionButton::None;
            pressed_ = CaptionButton::None;

            switch (action) {
                case CaptionButton::Minimise:
                    ShowWindow(hwnd_, SW_MINIMIZE);
                    break;
                case CaptionButton::Maximise:
                    ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
                    break;
                case CaptionButton::Close:
                    RequestClose();
                    break;
                default:
                    break;
            }
            return 0;
        }

        case WM_SETCURSOR:
            // Only inside the client area; the frame keeps the resize cursors
            // DefWindowProc picks for it.
            if (LOWORD(lParam) == HTCLIENT && delegate_) {
                POINT cursor{};
                GetCursorPos(&cursor);
                ScreenToClient(hwnd_, &cursor);
                if (delegate_->WantsHandCursor(ToLogical(cursor))) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;

        case kTrayCallbackMessage:
            switch (TrayIcon::Interpret(lParam)) {
                case TrayIcon::Action::Open:
                    ActivateFromTray();
                    break;
                case TrayIcon::Action::Menu:
                    ShowTrayMenu();
                    break;
                default:
                    break;
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            // Removed explicitly: left to the destructor the icon would linger
            // in the notification area until something made the shell notice.
            tray_.Remove();
            quitting_ = true;
            PostQuitMessage(0);
            return 0;

        default:
            // Explorer restarted and rebuilt the notification area, taking our
            // icon with it. Without this the window stays hidden with no way
            // back to it.
            if (message == taskbarCreated_ && taskbarCreated_ != 0) {
                tray_.Remove();
                InstallTrayIcon();
                return 0;
            }
            break;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void Window::DrawChrome(Canvas& canvas) {
    const Theme& theme = *theme_;
    const Size logical = renderer_.LogicalSize();
    const float caption = CaptionHeight();
    const float hairline = canvas.Hairline();

    canvas.Clear(theme.windowBase);

    const Rect captionRect{0.0f, 0.0f, logical.width, caption};
    canvas.FillRect(captionRect, theme.chromeBar);

    // A single lit pixel along the top edge is the whole trick behind the
    // glassy top rail that premium dark UIs have.
    if (active_ && !Maximised()) {
        canvas.FillRect(Rect{0.0f, 0.0f, logical.width, hairline},
                        Color::Hex(0xFFFFFF, 0.05f));
    }

    canvas.FillRect(Rect{0.0f, caption - hairline, logical.width, caption}, theme.borderSoft);

    const Rect branding{0.0f, 0.0f, CaptionButtonRect(CaptionButton::Minimise).left, caption};
    if (delegate_) {
        canvas.PushClip(branding);
        canvas.PushOpacity(active_ ? 1.0f : theme.inactiveFade);
        delegate_->OnRenderCaption(canvas, branding);
        canvas.PopOpacity();
        canvas.PopClip();
    }

    DrawCaptionButton(canvas, CaptionButton::Minimise, CaptionButtonRect(CaptionButton::Minimise));
    DrawCaptionButton(canvas, CaptionButton::Maximise, CaptionButtonRect(CaptionButton::Maximise));
    DrawCaptionButton(canvas, CaptionButton::Close, CaptionButtonRect(CaptionButton::Close));

    const Rect content{0.0f, caption, logical.width, logical.height};
    if (delegate_) {
        canvas.PushClip(content);
        delegate_->OnRender(canvas, content);
        canvas.PopClip();
    }

    if (drawOwnBorder_ && !Maximised()) {
        // Windows 10 fallback for the outline that DWM draws for us on 11.
        canvas.StrokeRoundedRect(Rect{0.0f, 0.0f, logical.width, logical.height}, 0.0f,
                                 theme.borderStrong, hairline);
    }
}

void Window::DrawCaptionButton(Canvas& canvas, CaptionButton button, const Rect& box) {
    const Theme& theme = *theme_;
    const bool hovered = hovered_ == button;
    const bool pressed = pressed_ == button && hovered;
    const bool isClose = button == CaptionButton::Close;

    if (hovered || pressed) {
        Color fill = theme.chromeHover;
        if (isClose) fill = pressed ? theme.closeHover.Fade(0.8f) : theme.closeHover;
        else if (pressed) fill = theme.chromePressed;
        canvas.FillRect(box, fill);
    }

    Color glyph = active_ ? theme.textSecondary : theme.textFaint;
    if (hovered) glyph = isClose ? Color::Hex(0xFFFFFF) : theme.textPrimary;

    // Glyphs are snapped to the physical pixel grid; a half-pixel line here is
    // instantly recognisable as a cheap custom title bar.
    const float thickness = canvas.Hairline();
    const float cx = canvas.Snap(box.CenterX());
    const float cy = canvas.Snap(box.CenterY());
    const float half = 5.0f;

    switch (button) {
        case CaptionButton::Minimise:
            canvas.StrokeLine(Point{cx - half, cy}, Point{cx + half, cy}, glyph, thickness);
            break;

        case CaptionButton::Maximise:
            if (Maximised()) {
                // The restore glyph: the front pane plus the two visible edges
                // of the one behind it.
                const Rect front{cx - half, cy - half + 2.0f, cx + half - 2.0f, cy + half};
                canvas.StrokeRoundedRect(front, 1.0f, glyph, thickness);
                canvas.StrokeLine(Point{cx - half + 2.0f, cy - half + 2.0f},
                                  Point{cx - half + 2.0f, cy - half}, glyph, thickness);
                canvas.StrokeLine(Point{cx - half + 2.0f, cy - half},
                                  Point{cx + half, cy - half}, glyph, thickness);
                canvas.StrokeLine(Point{cx + half, cy - half}, Point{cx + half, cy + half - 2.0f},
                                  glyph, thickness);
                canvas.StrokeLine(Point{cx + half, cy + half - 2.0f},
                                  Point{cx + half - 2.0f, cy + half - 2.0f}, glyph, thickness);
            } else {
                canvas.StrokeRoundedRect(Rect{cx - half, cy - half, cx + half, cy + half}, 1.0f,
                                         glyph, thickness);
            }
            break;

        case CaptionButton::Close:
            canvas.StrokeLine(Point{cx - half, cy - half}, Point{cx + half, cy + half}, glyph,
                              thickness);
            canvas.StrokeLine(Point{cx + half, cy - half}, Point{cx - half, cy + half}, glyph,
                              thickness);
            break;

        default:
            break;
    }
}

} // namespace slick
