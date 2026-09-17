#pragma once

#include "Gui/Easing.h"
#include "Gui/ProcessIcons.h"
#include "Gui/TextField.h"
#include "Libs/Window.h"
#include "Logic/Metrics.h"
#include "Logic/Sampler.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

namespace sysmon::ui {

// The application view: everything below the caption, plus the branding inside
// it. Holds no system state of its own beyond the newest published readings,
// the values it eases towards for the sake of the animation, and how the user
// has asked for the process table to be ordered.
class Dashboard : public slick::WindowDelegate {
public:
    explicit Dashboard(Sampler& sampler);

    void OnAttach(slick::Window& window) override;
    void OnRender(slick::Canvas& canvas, const slick::Rect& content) override;
    void OnRenderCaption(slick::Canvas& canvas, const slick::Rect& branding) override;
    void OnMouseMove(slick::Point position) override;
    void OnMouseLeave() override;
    void OnMouseDown(slick::Point position) override;
    void OnMouseUp(slick::Point position) override;
    void OnScroll(slick::Point position, float ticks) override;
    void OnRightClick(slick::Point position, POINT screenPosition) override;
    void OnChar(wchar_t character) override;
    void OnKeyDown(int virtualKey, bool control, bool shift) override;
    bool WantsHandCursor(slick::Point position) override;

private:
    // Columns the process table can be ordered by.
    enum class Column { Name, Cpu, Memory };
    static constexpr int kColumnCount = 3;

    // Which tile the big chart is following. Clicking a tile selects it, so the
    // four headline readouts double as the chart's own tab strip.
    enum class Metric { Cpu, Memory, Gpu, Storage };
    static constexpr int kTileCount = 4;

    struct Tile {
        std::wstring_view title;
        slick::Color accent;
        float load;
        std::wstring detail;
        std::wstring caption;
        // Sits in the card header, opposite the title. Empty means no badge.
        std::wstring badge;
    };

    struct ColumnHeader {
        slick::Rect box;
        Column column = Column::Cpu;
    };

    void Advance(float seconds);
    void ReorderProcesses();
    void ToggleSort(Column column);
    const ColumnHeader* HeaderAt(slick::Point position) const;

    // Which row sits under a content position, or -1.
    std::ptrdiff_t RowAt(slick::Point position) const;
    void ClampScroll();

    // Ends a process the way the task manager's End task does: immediately, with
    // no request to close first.
    void EndProcess(std::uint32_t pid, const std::wstring& name);
    void CopyToClipboard(const std::wstring& text);
    void Notify(std::wstring text, double seconds);

    // What the chart panel shows, which follows the selected tile.
    const Trace& SelectedTrace() const;
    slick::Color SelectedColour() const;
    std::wstring_view SelectedTitle() const;
    float SelectedValue() const;
    std::wstring SelectedHeadline() const;

    // Every hard-coded measurement in the layout goes through here, so the
    // whole dashboard grows with the window instead of leaving a maximised
    // screen full of small text and empty panels.
    float S(float value) const { return value * uiScale_; }

    slick::Rect DrawCard(slick::Canvas& canvas, const slick::Rect& box,
                         std::wstring_view title, const slick::Color& dot,
                         std::wstring_view trailing = {}, float press = 0.0f,
                         float glow = 0.0f) const;

    void DrawTile(slick::Canvas& canvas, const slick::Rect& box, const Tile& tile,
                  int index) const;
    void DrawCpuPanel(slick::Canvas& canvas, const slick::Rect& box) const;
    void DrawProcessorPanel(slick::Canvas& canvas, const slick::Rect& box) const;
    void DrawNetworkPanel(slick::Canvas& canvas, const slick::Rect& box) const;
    void DrawStatusBar(slick::Canvas& canvas, const slick::Rect& box) const;

    // Not const: it records where the sortable headers ended up, which is what
    // the next click is tested against.
    void DrawProcessPanel(slick::Canvas& canvas, const slick::Rect& box);
    void DrawColumnHeader(slick::Canvas& canvas, const slick::Rect& box,
                          std::wstring_view label, Column column, slick::HAlign align) const;

    Sampler* sampler_ = nullptr;
    slick::Window* window_ = nullptr;
    const slick::Theme* theme_ = nullptr;
    Readings readings_;

    using Clock = std::chrono::steady_clock;
    Clock::time_point lastFrame_ = Clock::now();
    Clock::time_point lastSample_ = Clock::now();

    // Fraction of the way through the current sample interval, which is what
    // lets the charts scroll smoothly between samples.
    float scrollPhase_ = 0.0f;
    float uiScale_ = 1.0f;

    Eased cpuLoad_;
    Eased cpuPeak_;
    Eased cpuAverage_;
    Eased memoryLoad_;
    Eased memoryUsed_;
    Eased memoryCommitted_;
    Eased gpuLoad_;
    Eased gpuMemoryUsed_;
    Eased gpuTemperature_;
    Eased diskLoad_;
    Eased diskRead_;
    Eased diskWrite_;
    Eased diskFree_;
    Eased networkDown_;
    Eased networkUp_;
    Eased clockMhz_;
    Eased processCount_;
    Eased threadCount_;
    Eased handleCount_;
    EasedSeries coreLoads_;

    // Process rows animate individually, keyed by process id so a row never
    // inherits the value of whatever used to sit in that slot.
    // The shell's icon for each row, resolved lazily on a per-frame budget.
    mutable ProcessIcons icons_;

    EasedTable processCpu_;
    std::vector<float> easedProcessCpu_;

    // Reused every frame so that reordering the visible rows by their eased
    // value costs no allocation.
    std::vector<std::size_t> rowOrder_;
    std::vector<std::size_t> scratchOrder_;
    std::vector<float> scratchEased_;
    Eased busiestProcessCpu_;

    // Filters the table by name. Typing anywhere goes here, because there is
    // nothing else on this window that wants the keyboard.
    TextField search_;
    slick::Rect searchBox_{};
    bool caretClickPending_ = false;
    slick::Point caretClick_{};

    // Held Ctrl freezes the ordering, the way the task manager does, so a row
    // stops moving out from under the pointer while it is being aimed at.
    bool frozen_ = false;
    Eased frozenFade_;

    Column sortColumn_ = Column::Cpu;
    bool sortDescending_ = true;
    std::vector<std::size_t> order_;
    bool orderDirty_ = true;

    std::array<ColumnHeader, kColumnCount> headers_{};
    bool headersValid_ = false;
    slick::Point mouse_{-1.0f, -1.0f};
    bool mouseInside_ = false;

    // Which tile the chart panel is following, and where the tiles ended up so
    // a click can find them.
    Metric metric_ = Metric::Cpu;
    std::array<slick::Rect, kTileCount> tileBoxes_{};

    // Selection lights a tile; hover only moves it. Kept apart so that running
    // the pointer across the row does not make every card look chosen.
    std::array<Eased, kTileCount> tileSelection_{};
    std::array<Eased, kTileCount> tileHover_{};

    // The four panels below the tiles answer to the pointer as well, just far
    // more quietly: they are worth looking at, not worth clicking.
    static constexpr int kPanelCount = 4;
    std::array<slick::Rect, kPanelCount> panelBoxes_{};
    std::array<Eased, kPanelCount> panelHover_{};

    // The process list scrolls. The offset is eased so the wheel glides rather
    // than jumping a row at a time.
    float scrollTarget_ = 0.0f;
    Eased scroll_;
    std::size_t visibleRows_ = 0;
    std::size_t firstVisibleRow_ = 0;
    slick::Rect listBox_{};
    slick::Rect scrollbarBox_{};
    bool draggingScrollbar_ = false;
    float dragOffset_ = 0.0f;

    // The row under the pointer, and the one the context menu was opened on.
    std::ptrdiff_t hoveredRow_ = -1;
    std::uint32_t menuPid_ = 0;
    std::wstring menuName_;

    // What the status bar says after an action, and for how long.
    std::wstring notice_;
    double noticeUntil_ = 0.0;
    double seconds_ = 0.0;
};

} // namespace sysmon::ui
