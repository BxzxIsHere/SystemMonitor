// System Monitor
//
// A live view of the machine, drawn with Direct2D into a window that keeps all
// of its native behaviour and none of its native chrome.
//
//   Libs/   the reusable window and rendering library
//   Logic/  the probes that read the machine, on their own thread
//   Gui/    the dashboard those two come together in
//
// Crafted by bxzx.

#include "Gui/Dashboard.h"
#include "Libs/Window.h"
#include "Logic/Sampler.h"
#include "Resource.h"

#include <cstdio>

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int showCommand) {
    // Declared before anything creates a window, so the first frame is already
    // laid out for the monitor it opens on rather than being bitmap-stretched.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    try {
        sysmon::Sampler sampler;
        sampler.Start();

        sysmon::ui::Dashboard dashboard(sampler);

        slick::WindowConfig config;
        config.title = L"System Monitor";
        config.className = L"SystemMonitorWindow";
        config.initialSize = slick::Size{1240.0f, 800.0f};
        config.minimumSize = slick::Size{980.0f, 660.0f};
        config.iconResourceId = IDI_APP;

        // Sixty. Recording a frame costs 0.57 ms, so the second thirty frames
        // come to about a fifth of a percent of an eight core machine, and the
        // scrolling charts and the row animations are visibly smoother for it.
        config.targetFrameRate = 60;
        // A monitor is something you leave running, so it keeps a notification
        // area icon for the whole session, next to the ordinary taskbar button
        // rather than instead of it.
        config.showTrayIcon = true;

        slick::Window window;
        window.Create(config, dashboard);
        window.Show(showCommand);

        const int result = window.RunMessageLoop();

        // Stopped explicitly so the probe thread is joined while the renderer
        // it never touches is still alive, keeping shutdown order obvious.
        sampler.Stop();
        return result;
    } catch (const slick::ComError& error) {
        wchar_t message[512]{};
        swprintf_s(message, L"%hs failed with HRESULT 0x%08X.\n\nThe graphics device could not "
                            L"be initialised.",
                   error.what(), static_cast<unsigned>(error.Code()));
        MessageBoxW(nullptr, message, L"System Monitor", MB_ICONERROR | MB_OK);
        return 1;
    }
}
