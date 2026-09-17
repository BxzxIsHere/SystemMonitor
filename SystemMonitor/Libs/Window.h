#pragma once

#include "Libs/Canvas.h"
#include "Libs/Renderer.h"
#include "Libs/Theme.h"
#include "Libs/TrayIcon.h"
#include "Libs/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace slick {

class Window;

// One line of a context menu. An empty label is a separator.
struct MenuItem {
    unsigned id = 0;
    std::wstring text;
    bool enabled = true;
    bool isDefault = false;
};

// Shows a context menu at a screen position and returns the chosen id, or zero
// if the user dismissed it. Uses the shell's own menus, which are dark wherever
// the rest of the application's menus are, so it matches the tray menu without
// anything here having to draw one.
unsigned ShowContextMenu(HWND owner, POINT screen, const std::vector<MenuItem>& items);

// Implemented by the application. The window owns the frame, the caption
// buttons and every piece of native behaviour; the delegate only fills in the
// content and, optionally, the branding strip on the left of the caption.
class WindowDelegate {
public:
    virtual ~WindowDelegate() = default;

    // Called once, when the delegate is handed to a window, so it can keep a
    // reference for the things that need the window itself.
    virtual void OnAttach(Window& window) {}

    virtual void OnRender(Canvas& canvas, const Rect& content) = 0;
    virtual void OnRenderCaption(Canvas& canvas, const Rect& brandingArea) {}
    virtual void OnResize(Size logicalSize) {}

    virtual void OnMouseMove(Point position) {}
    virtual void OnMouseLeave() {}
    virtual void OnMouseDown(Point position) {}
    virtual void OnMouseUp(Point position) {}
    virtual void OnScroll(Point position, float ticks) {}

    // Both the content position and the screen position, because a context menu
    // has to be placed in screen coordinates while the thing it acts on is
    // found in content ones.
    virtual void OnRightClick(Point position, POINT screenPosition) {}

    // A typed character, already translated, so this is text rather than keys.
    virtual void OnChar(wchar_t character) {}

    // Everything text cannot express: arrows, Home, Delete, shortcuts.
    virtual void OnKeyDown(int virtualKey, bool control, bool shift) {}

    // True for spots inside the caption strip that belong to the delegate, so
    // the window reports HTCLIENT there instead of starting a window drag.
    virtual bool WantsMouseInCaption(Point position) { return false; }

    // True over anything clickable, so the window can show the hand cursor the
    // way every other clickable surface in the shell does.
    virtual bool WantsHandCursor(Point position) { return false; }
};

struct WindowConfig {
    std::wstring title = L"Slick";
    std::wstring className = L"SlickWindowClass";
    Size initialSize{1200.0f, 780.0f};
    Size minimumSize{940.0f, 620.0f};
    bool centreOnScreen = true;

    // Frames per second the render loop aims for. Without a cap the loop runs
    // at the display refresh rate, so a 165 Hz monitor would cost 165 full
    // redraws a second to animate a dashboard that changes four times a second.
    unsigned targetFrameRate = 60;

    // Keeps an icon in the notification area for as long as the app runs,
    // alongside the ordinary taskbar button rather than instead of it, which is
    // how the task manager and every other resident monitor behaves.
    bool showTrayIcon = true;

    // Icon resource used for the window, the taskbar and the tray. Zero leaves
    // the window with the default application icon.
    unsigned iconResourceId = 0;
};

// A real top-level Win32 window with its non-client area removed and redrawn
// with Direct2D.
//
// Everything the shell expects still works, because the window keeps a normal
// overlapped style and lets DefWindowProc run the interactions: Aero Snap and
// snap layouts, drag-to-maximise, double-click the caption, Alt+Space, window
// shake, taskbar previews, minimise and restore animations, per-monitor DPI,
// and activation. The only thing taken over is the painting.
class Window {
public:
    Window();
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void Create(const WindowConfig& config, WindowDelegate& delegate);
    void Show(int showCommand);
    int RunMessageLoop();
    void RequestClose();

    HWND Handle() const { return hwnd_; }
    bool Active() const { return active_; }
    bool Maximised() const;

    Renderer& GetRenderer() { return renderer_; }
    const Theme& GetTheme() const { return *theme_; }

    // Height of the caption strip in DIPs. The delegate lays out below it.
    float CaptionHeight() const { return theme_->titleBarHeight; }

private:
    enum class CaptionButton { None, Minimise, Maximise, Close };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT OnNcCalcSize(WPARAM wParam, LPARAM lParam);
    LRESULT OnNcHitTest(LPARAM lParam);

    void ApplyFrameExtensions();
    void LoadIcons(HINSTANCE instance);
    void InstallTrayIcon();
    void ActivateFromTray();
    void ShowTrayMenu();
    void RenderFrame();
    void DrawChrome(Canvas& canvas);
    void DrawCaptionButton(Canvas& canvas, CaptionButton button, const Rect& box);

    Rect CaptionButtonRect(CaptionButton button) const;
    CaptionButton ButtonAtScreenPoint(POINT screenPoint) const;
    void SetHoveredButton(CaptionButton button);
    void TrackMouseLeave(bool nonClient);
    Point ToLogical(POINT clientPoint) const;
    int FrameGrabThickness() const;
    Size ContentSize() const;

    // A high resolution waitable timer for frame pacing. Null on anything
    // older than Windows 10 1803, where the loop falls back to a millisecond
    // timeout and the granularity it brings with it.
    HANDLE frameTimer_ = nullptr;

    HWND hwnd_ = nullptr;
    WindowDelegate* delegate_ = nullptr;
    const Theme* theme_ = nullptr;
    WindowConfig config_;

    Renderer renderer_;
    std::unique_ptr<Canvas> canvas_;

    unsigned dpi_ = 96;
    bool active_ = true;
    bool trackingClient_ = false;
    bool trackingNonClient_ = false;
    bool inSizeMove_ = false;
    bool drawOwnBorder_ = false;
    bool quitting_ = false;

    CaptionButton hovered_ = CaptionButton::None;
    CaptionButton pressed_ = CaptionButton::None;

    TrayIcon tray_;
    HICON largeIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    UINT taskbarCreated_ = 0;
};

} // namespace slick
