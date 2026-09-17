#include "Gui/Dashboard.h"

#include "Gui/Charts.h"
#include "Libs/Clipboard.h"
#include "Gui/Format.h"
#include "Gui/ProcessIcons.h"
#include "Gui/Watermark.h"
#include "Logic/SystemInfo.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sysmon::ui {
namespace {

using slick::Canvas;
using slick::Color;
using slick::HAlign;
using slick::Path;
using slick::Point;
using slick::Rect;
using slick::TextStyle;
using slick::Theme;
using slick::VAlign;

// The size every measurement in this file was chosen at. Anything larger scales
// up from here; anything smaller keeps these numbers, because shrinking the
// text below its design size costs more legibility than it wins back space.
constexpr float kDesignWidth = 1240.0f;
constexpr float kDesignHeight = 756.0f;
constexpr float kMaximumScale = 1.5f;

constexpr float kTileRowShare = 0.20f;
constexpr float kBottomRowShare = 0.30f;
constexpr float kLeftColumnRatio = 0.62f;

// Past this the middle row is taller than its contents can use. Everything
// above it goes to the process list, which can always use more.
constexpr float kMiddleRowCeiling = 430.0f;

// Rows animate individually, but only the ones that could plausibly be on
// screen; there is no point easing the four hundredth process.
constexpr std::size_t kEasedProcessRows = 48;

TextStyle LabelStyle(const Theme& theme, float size) {
    // Tracking proportional to the size, so the letter-spaced caps look the
    // same at every scale rather than loosening as they grow.
    return TextStyle{theme.fontText, size, 600, size * 0.105f};
}

// Tabular figures for anything that sits in a column and has to line up.
TextStyle ValueStyle(const Theme& theme, float size, int weight = 500) {
    return TextStyle{theme.fontMono, size, weight};
}

// Proportional figures for headline numbers, where a monospaced decimal point
// opens up a gap wide enough to read as a space.
TextStyle HeadlineStyle(const Theme& theme, float size, int weight = 400) {
    return TextStyle{theme.fontDisplay, size, weight};
}

TextStyle BodyStyle(const Theme& theme, float size) {
    return TextStyle{theme.fontText, size, 400, 0.0f, false, false, true};
}

// The card header badge for the graphics tile. Temperature comes from the
// display driver, so it is there on most cards and absent on some; the fan
// figure is only shown while the fan is actually turning, because a card idling
// in its zero-rpm mode reports zero and means it.
std::wstring GpuThermalBadge(const GpuSnapshot& gpu, float easedCelsius) {
    if (!gpu.hasTemperature) return {};

    wchar_t text[48]{};
    if (gpu.fanRpm > 0) {
        swprintf_s(text, L"%.0f°C  ·  %u RPM", easedCelsius, gpu.fanRpm);
    } else {
        swprintf_s(text, L"%.0f°C", easedCelsius);
    }
    return text;
}

// Cream up to two thirds, then amber, then red. Interpolated rather than
// stepped, so the number warms up gradually instead of snapping at a threshold.
Color SeverityColor(const Theme& theme, float load) {
    if (load <= 0.65f) return theme.textPrimary;
    if (load <= 0.88f) {
        return Color::Lerp(theme.textPrimary, theme.warn, (load - 0.65f) / 0.23f);
    }
    return Color::Lerp(theme.warn, theme.critical, std::min(1.0f, (load - 0.88f) / 0.10f));
}

// A label stacked over a value: the unit of information this dashboard repeats
// everywhere, so it is worth having exactly one implementation.
void DrawMiniStat(Canvas& canvas, const Theme& theme, const Rect& box, std::wstring_view label,
                  std::wstring_view value, HAlign align, float scale) {
    const float labelHeight = 13.0f * scale;
    canvas.DrawString(label, LabelStyle(theme, 9.5f * scale), box.TopSlice(labelHeight),
                      theme.textFaint, align, VAlign::Top);
    canvas.DrawString(value, ValueStyle(theme, 13.0f * scale), box.RestBelow(labelHeight),
                      theme.textSecondary, align, VAlign::Top);
}

void DrawLegendEntry(Canvas& canvas, const Theme& theme, const Rect& box, const Color& colour,
                     std::wstring_view label, std::wstring_view value, float scale) {
    const float dot = 5.0f * scale;
    canvas.FillRoundedRect(
        Rect::FromXYWH(box.left, canvas.Snap(box.CenterY() - dot * 0.5f), dot, dot), dot * 0.5f,
        colour);

    const TextStyle labelStyle = LabelStyle(theme, 9.5f * scale);
    const Rect text = box.RestRight(dot + 7.0f * scale);
    const float labelWidth = canvas.MeasureString(label, labelStyle).width;

    canvas.DrawString(label, labelStyle, text, theme.textMuted, HAlign::Left, VAlign::Middle);
    canvas.DrawString(value, HeadlineStyle(theme, 15.0f * scale, 500),
                      text.RestRight(labelWidth + 8.0f * scale), theme.textPrimary, HAlign::Left,
                      VAlign::Middle);
}

// A chevron, pointing the way the column is ordered.
void DrawSortArrow(Canvas& canvas, Point centre, float size, bool ascending, const Color& color,
                   float stroke) {
    const float half = size * 0.5f;
    const float rise = size * 0.34f;
    const float base = ascending ? centre.y + rise : centre.y - rise;
    const float tip = ascending ? centre.y - rise : centre.y + rise;

    Path arrow = canvas.NewPath();
    arrow.Begin(Point{centre.x - half, base}, false);
    arrow.LineTo(Point{centre.x, tip});
    arrow.LineTo(Point{centre.x + half, base});
    arrow.Close(false);
    canvas.StrokePath(arrow, color, stroke);
}

std::wstring ShortRate(double bytesPerSecond) {
    if (bytesPerSecond < 1.0) return L"0 B/s";
    return FormatRate(bytesPerSecond);
}

// Case-insensitive substring, which is what anyone typing into a filter box
// means by it. Ordinal folding rather than locale aware, for the same reason
// the ordering is: these are file names.
bool ContainsFolded(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;

    const auto fold = [](wchar_t character) {
        return character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character + 32)
                                                      : character;
    };

    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t at = 0; at <= last; ++at) {
        std::size_t matched = 0;
        while (matched < needle.size() &&
               fold(haystack[at + matched]) == fold(needle[matched])) {
            ++matched;
        }
        if (matched == needle.size()) return true;
    }
    return false;
}

int CompareNames(const std::wstring& left, const std::wstring& right) {
    // Ordinal rather than locale aware: process names are file names, and this
    // is the same ordering the shell uses for them.
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) - CSTR_EQUAL;
}

} // namespace

Dashboard::Dashboard(Sampler& sampler) : sampler_(&sampler), theme_(&Theme::Dark()) {
    search_.SetPlaceholder(L"Filter by name");
}

void Dashboard::OnAttach(slick::Window& window) {
    window_ = &window;
}

void Dashboard::OnMouseMove(Point position) {
    mouse_ = position;
    mouseInside_ = true;

    if (draggingScrollbar_ && !listBox_.Empty()) {
        const float rowHeight = S(22.0f);
        const float total = order_.size() * rowHeight;
        const float maximum = std::max(0.0f, total - listBox_.Height());
        const float thumbHeight =
            std::max(S(26.0f), listBox_.Height() * (listBox_.Height() / std::max(total, 1.0f)));
        const float travel = std::max(1.0f, listBox_.Height() - thumbHeight);

        scrollTarget_ =
            std::clamp((position.y - dragOffset_ - listBox_.top) / travel * maximum, 0.0f,
                       maximum);
        scroll_.Set(scrollTarget_); // dragging tracks the cursor exactly
    }
}

void Dashboard::OnMouseLeave() {
    mouseInside_ = false;
    hoveredRow_ = -1;
}

void Dashboard::OnMouseDown(Point position) {
    if (!scrollbarBox_.Empty() && scrollbarBox_.Contains(position)) {
        draggingScrollbar_ = true;
        dragOffset_ = position.y - scrollbarBox_.top;
        return;
    }

    if (const ColumnHeader* header = HeaderAt(position)) {
        ToggleSort(header->column);
        return;
    }

    if (!searchBox_.Empty() && searchBox_.Contains(position)) {
        caretClickPending_ = true;
        caretClick_ = position;
        return;
    }

    for (int index = 0; index < kTileCount; ++index) {
        if (!tileBoxes_[index].Empty() && tileBoxes_[index].Contains(position)) {
            metric_ = static_cast<Metric>(index);
            return;
        }
    }
}

void Dashboard::OnMouseUp(Point) {
    draggingScrollbar_ = false;
}

void Dashboard::OnChar(wchar_t character) {
    if (search_.Insert(character)) {
        orderDirty_ = true;
        scrollTarget_ = 0.0f;
        scroll_.Set(0.0f);
    }
}

void Dashboard::OnKeyDown(int virtualKey, bool control, bool shift) {
    if (virtualKey == VK_ESCAPE && !search_.Empty()) {
        search_.SetText({});
        orderDirty_ = true;
        scrollTarget_ = 0.0f;
        return;
    }

    if (search_.HandleKey(virtualKey, control, shift, window_ ? window_->Handle() : nullptr)) {
        orderDirty_ = true;
        scrollTarget_ = 0.0f;
    }
}

void Dashboard::OnScroll(Point position, float ticks) {
    if (listBox_.Empty() || !listBox_.Contains(position)) return;
    scrollTarget_ -= ticks * S(22.0f) * 3.0f;
    ClampScroll();
}

void Dashboard::ClampScroll() {
    const float rowHeight = S(22.0f);
    const float total = order_.size() * rowHeight;
    const float maximum = std::max(0.0f, total - visibleRows_ * rowHeight);
    scrollTarget_ = std::clamp(scrollTarget_, 0.0f, maximum);
}

std::ptrdiff_t Dashboard::RowAt(Point position) const {
    if (listBox_.Empty() || !listBox_.Contains(position)) return -1;

    const float rowHeight = S(22.0f);
    const auto row = static_cast<std::ptrdiff_t>(
        (position.y - listBox_.top + scroll_.Value()) / rowHeight);
    if (row < 0 || row >= static_cast<std::ptrdiff_t>(order_.size())) return -1;
    return row;
}

bool Dashboard::WantsHandCursor(Point position) {
    if (HeaderAt(position) != nullptr) return true;
    for (const Rect& tile : tileBoxes_) {
        if (!tile.Empty() && tile.Contains(position)) return true;
    }
    return false;
}

void Dashboard::Notify(std::wstring text, double seconds) {
    notice_ = std::move(text);
    noticeUntil_ = seconds_ + seconds;
}

void Dashboard::CopyToClipboard(const std::wstring& text) {
    slick::WriteClipboardText(window_ ? window_->Handle() : nullptr, text);
    Notify(L"Copied  " + text, 2.5);
}

void Dashboard::EndProcess(std::uint32_t pid, const std::wstring& name) {
    // Both rights, because IsProcessCritical needs the query one and asking for
    // terminate alone leaves it failing every time, which quietly turns the
    // guard below into dead code.
    HANDLE handle =
        OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    bool canAsk = handle != nullptr;
    if (!handle) handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid);

    if (!handle) {
        Notify(L"Cannot end " + name + L" — access denied", 4.0);
        return;
    }

    // Killing a process the kernel considers critical bugchecks the machine on
    // the spot. The task manager refuses for the same reason, and taking down
    // everything else unsaved is not a thing to do without being asked.
    BOOL critical = FALSE;
    if (canAsk && IsProcessCritical(handle, &critical) && critical) {
        CloseHandle(handle);
        Notify(name + L" is a critical system process — Windows would stop", 4.0);
        return;
    }

    // Straight to TerminateProcess, with no request to close first. That is
    // what End task does, and what was asked for.
    const BOOL ok = ::TerminateProcess(handle, 1);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(handle);

    if (ok) {
        Notify(L"Ended " + name, 3.0);
        orderDirty_ = true;
    } else {
        wchar_t text[160]{};
        swprintf_s(text, L"Could not end %.60s (error %lu)", name.c_str(), error);
        Notify(text, 4.0);
    }
}

void Dashboard::OnRightClick(Point position, POINT screenPosition) {
    const std::ptrdiff_t row = RowAt(position);
    if (row < 0 || !window_) return;

    const ProcessEntry& entry = readings_.snapshot.processes[order_[row]];
    menuPid_ = entry.pid;
    menuName_ = entry.name;

    enum : unsigned { kEnd = 1, kCopyName, kCopyPid };

    const std::vector<slick::MenuItem> items{
        {kEnd, L"End task", true, true},
        {0, L"", true, false},
        {kCopyName, L"Copy name", true, false},
        {kCopyPid, L"Copy process id", true, false},
    };

    switch (slick::ShowContextMenu(window_->Handle(), screenPosition, items)) {
        case kEnd:
            EndProcess(menuPid_, menuName_);
            break;
        case kCopyName:
            CopyToClipboard(menuName_);
            break;
        case kCopyPid:
            CopyToClipboard(std::to_wstring(menuPid_));
            break;
        default:
            break;
    }
}

const Dashboard::ColumnHeader* Dashboard::HeaderAt(Point position) const {
    if (!headersValid_) return nullptr;
    for (const ColumnHeader& header : headers_) {
        if (header.box.Contains(position)) return &header;
    }
    return nullptr;
}

void Dashboard::ToggleSort(Column column) {
    if (sortColumn_ == column) {
        sortDescending_ = !sortDescending_;
    } else {
        sortColumn_ = column;
        // Names read best from A downwards; the numbers are interesting from
        // the top, which is what anyone clicking them is looking for.
        sortDescending_ = column != Column::Name;
    }
    orderDirty_ = true;
}

void Dashboard::ReorderProcesses() {
    const auto& processes = readings_.snapshot.processes;

    // The filter comes first, so everything downstream of here — the sort, the
    // scroll bounds, the row count in the header — is about the rows that are
    // actually being shown.
    const std::wstring& query = search_.Text();
    order_.clear();
    order_.reserve(processes.size());

    if (query.empty()) {
        order_.resize(processes.size());
        std::iota(order_.begin(), order_.end(), std::size_t{0});
    } else {
        for (std::size_t index = 0; index < processes.size(); ++index) {
            if (ContainsFolded(processes[index].name, query)) order_.push_back(index);
        }
    }

    const Column column = sortColumn_;
    const bool descending = sortDescending_;

    std::sort(order_.begin(), order_.end(), [&](std::size_t a, std::size_t b) {
        const ProcessEntry& left = processes[a];
        const ProcessEntry& right = processes[b];

        int comparison = 0;
        switch (column) {
            case Column::Name:
                comparison = CompareNames(left.name, right.name);
                break;
            case Column::Cpu:
                comparison = left.cpu < right.cpu ? -1 : (left.cpu > right.cpu ? 1 : 0);
                break;
            case Column::Memory:
                comparison = left.workingSetBytes < right.workingSetBytes
                                 ? -1
                                 : (left.workingSetBytes > right.workingSetBytes ? 1 : 0);
                break;
        }

        // Falling through to the process id keeps the order total, so rows do
        // not swap places between frames just because two values tie.
        if (comparison == 0 && left.pid != right.pid) {
            comparison = left.pid < right.pid ? -1 : 1;
        }
        return descending ? comparison > 0 : comparison < 0;
    });
}

void Dashboard::Advance(float seconds) {
    if (sampler_->Poll(readings_)) {
        lastSample_ = Clock::now();
        orderDirty_ = true;
    }

    const float interval = kSampleIntervalMs / 1000.0f;
    scrollPhase_ = std::clamp(
        std::chrono::duration<float>(Clock::now() - lastSample_).count() / interval, 0.0f, 1.0f);

    // Held Ctrl freezes the ordering, exactly as the task manager does it, so
    // a row stops sliding out from under a pointer that is aiming at it. New
    // readings still arrive and every number still moves; only the order that
    // decides which row is where is held still.
    frozen_ = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    frozenFade_.Approach(frozen_ ? 1.0f : 0.0f, seconds, 14.0f);

    if (orderDirty_ && !frozen_) {
        ReorderProcesses();
        orderDirty_ = false;
    }

    const Snapshot& snapshot = readings_.snapshot;
    const Traces& traces = readings_.traces;

    cpuLoad_.Approach(snapshot.cpu.total, seconds);
    cpuPeak_.Approach(traces.cpu.Peak(), seconds);
    cpuAverage_.Approach(traces.cpu.Average(), seconds);
    coreLoads_.Approach(snapshot.cpu.cores, seconds);

    memoryLoad_.Approach(snapshot.memory.load, seconds);
    memoryUsed_.Approach(static_cast<float>(snapshot.memory.usedBytes), seconds);
    memoryCommitted_.Approach(static_cast<float>(snapshot.memory.committedBytes), seconds);

    gpuLoad_.Approach(snapshot.gpu.utilisation, seconds);
    gpuMemoryUsed_.Approach(static_cast<float>(snapshot.gpu.dedicatedUsedBytes), seconds);

    // Slower than the rest: a temperature that chases every reading looks
    // twitchy, and the thing it is measuring genuinely does move slowly.
    if (snapshot.gpu.hasTemperature) {
        if (gpuTemperature_.Value() <= 0.0f) {
            gpuTemperature_.Set(snapshot.gpu.temperatureCelsius);
        } else {
            gpuTemperature_.Approach(snapshot.gpu.temperatureCelsius, seconds, 3.0f);
        }
    }

    // Lit only when it is the tile the chart is following. Moved whenever the
    // pointer is over it, and further than a panel moves, because this is a
    // card you can actually click.
    for (int index = 0; index < kTileCount; ++index) {
        const bool selected = static_cast<int>(metric_) == index;
        const bool hovered = mouseInside_ && !tileBoxes_[index].Empty() &&
                             tileBoxes_[index].Contains(mouse_);
        tileSelection_[index].Approach(selected ? 1.0f : 0.0f, seconds, 12.0f);
        tileHover_[index].Approach(hovered ? 1.0f : 0.0f, seconds, 14.0f);
    }

    // A panel lifts a fraction of what a tile does. Enough that the surface
    // answers the pointer, not so much that it looks clickable when it is not.
    for (int index = 0; index < kPanelCount; ++index) {
        const bool hovered = mouseInside_ && !panelBoxes_[index].Empty() &&
                             panelBoxes_[index].Contains(mouse_);
        panelHover_[index].Approach(hovered ? 0.34f : 0.0f, seconds, 12.0f);
    }

    scroll_.Approach(scrollTarget_, seconds, 22.0f);

    diskLoad_.Approach(snapshot.storage.activity, seconds);
    diskRead_.Approach(static_cast<float>(snapshot.storage.readBytesPerSecond), seconds);
    diskWrite_.Approach(static_cast<float>(snapshot.storage.writeBytesPerSecond), seconds);
    diskFree_.Approach(static_cast<float>(snapshot.storage.freeBytes), seconds);

    networkDown_.Approach(static_cast<float>(snapshot.network.downBytesPerSecond), seconds);
    networkUp_.Approach(static_cast<float>(snapshot.network.upBytesPerSecond), seconds);

    clockMhz_.Approach(static_cast<float>(snapshot.cpu.currentMhz), seconds);
    processCount_.Approach(static_cast<float>(snapshot.cpu.processes), seconds);
    threadCount_.Approach(static_cast<float>(snapshot.cpu.threads), seconds);
    handleCount_.Approach(static_cast<float>(snapshot.cpu.handles), seconds);

    // Process rows ease individually. The order they are drawn in comes from
    // the raw values, so a row never drifts up or down the table mid-animation.
    // Only the rows that could be on screen are eased, and which those are
    // depends on the scroll position the last frame settled on. One frame of
    // lag on a scroll is not something an eye can catch, and it keeps this from
    // animating four hundred rows nobody is looking at.
    const std::size_t firstRow = std::min(firstVisibleRow_, order_.size());
    const std::size_t easedRows =
        std::min(order_.size() - firstRow, std::min(visibleRows_ + 2, kEasedProcessRows));
    easedProcessCpu_.resize(easedRows);

    float busiest = 0.0f;
    for (const ProcessEntry& entry : snapshot.processes) busiest = std::max(busiest, entry.cpu);
    busiestProcessCpu_.Approach(std::max(busiest, 0.02f), seconds);

    for (std::size_t row = 0; row < easedRows; ++row) {
        const ProcessEntry& entry = snapshot.processes[order_[firstRow + row]];
        easedProcessCpu_[row] = processCpu_.Approach(entry.pid, entry.cpu, seconds, 7.0f);
    }
    processCpu_.Sweep();

    // The figure on a row is the eased one, so the rows are ordered by the eased
    // one as well. Ordering by the raw value instead lets a row reading 9.3% sit
    // below a row reading 8.7% for as long as the two are still catching up,
    // which reads as a sorting bug rather than as animation.
    //
    // Only when the table is actually sorted by CPU: ordered by name or memory
    // there is nothing for the CPU column to contradict.
    // Only the rows actually on screen are reordered, which is a strict subset
    // of the rows being eased: that way the sort can never move a row past the
    // end of the eased window, where it would stop animating and be swept.
    const std::size_t sortableRows = std::min(easedRows, visibleRows_ + 1);
    if (sortColumn_ == Column::Cpu && sortableRows > 1 && !frozen_) {
        rowOrder_.resize(sortableRows);
        std::iota(rowOrder_.begin(), rowOrder_.end(), std::size_t{0});

        const bool descending = sortDescending_;
        std::stable_sort(rowOrder_.begin(), rowOrder_.end(),
                         [&](std::size_t a, std::size_t b) {
                             return descending ? easedProcessCpu_[a] > easedProcessCpu_[b]
                                               : easedProcessCpu_[a] < easedProcessCpu_[b];
                         });

        scratchOrder_.assign(order_.begin() + firstRow,
                             order_.begin() + firstRow + sortableRows);
        scratchEased_.assign(easedProcessCpu_.begin(),
                             easedProcessCpu_.begin() + sortableRows);
        for (std::size_t row = 0; row < sortableRows; ++row) {
            order_[firstRow + row] = scratchOrder_[rowOrder_[row]];
            easedProcessCpu_[row] = scratchEased_[rowOrder_[row]];
        }
    }
}

void Dashboard::OnRenderCaption(Canvas& canvas, const Rect& branding) {
    const Theme& theme = *theme_;

    // The mark sits inside the system menu strip, which is where a window icon
    // would be, so right-clicking it does the expected thing. It is drawn as
    // vectors rather than blitted from the icon so it stays crisp at any DPI,
    // and traces the same geometry the taskbar icon and the badge show.
    DrawMark(canvas, Point{16.0f, canvas.Snap(branding.CenterY())}, 8.0f, 1.3f, theme.accent);

    const TextStyle title{theme.fontText, 11.5f, 600, 1.6f};
    const std::wstring_view name = L"SYSTEM MONITOR";
    const float width = canvas.MeasureString(name, title).width;

    const Rect titleBox{33.0f, branding.top, branding.right, branding.bottom};
    canvas.DrawString(name, title, titleBox, theme.textPrimary, HAlign::Left, VAlign::Middle);

    const float separator = canvas.Snap(33.0f + width + 14.0f);
    if (separator + 90.0f < branding.right) {
        canvas.FillRect(Rect{separator, branding.CenterY() - 7.0f, separator + canvas.Hairline(),
                             branding.CenterY() + 7.0f},
                        theme.border);
        canvas.DrawString(SystemInfo::Current().machineName,
                          TextStyle{theme.fontText, 11.0f, 400, 0.3f, false, false, true},
                          Rect{separator + 12.0f, branding.top, branding.right, branding.bottom},
                          theme.textMuted, HAlign::Left, VAlign::Middle);
    }
}

void Dashboard::OnRender(Canvas& canvas, const Rect& content) {
    const Clock::time_point now = Clock::now();
    const float seconds =
        std::min(0.1f, std::chrono::duration<float>(now - lastFrame_).count());
    lastFrame_ = now;
    seconds_ += seconds;

    // Measured against the content area rather than the window, so the fixed
    // caption height does not skew the ratio on a short window.
    uiScale_ = std::clamp(std::min(content.Width() / kDesignWidth,
                                   content.Height() / kDesignHeight),
                          1.0f, kMaximumScale);

    Advance(seconds);

    const Theme& theme = *theme_;
    const Snapshot& snapshot = readings_.snapshot;
    const SystemInfo& system = SystemInfo::Current();

    const float gap = S(12.0f);
    const Rect status = content.BottomSlice(S(theme.statusBarHeight));
    const Rect body = content.RestAbove(status.Height())
                          .Deflate(S(theme.pagePadding), S(14.0f));

    const float tileHeight =
        std::clamp(body.Height() * kTileRowShare, S(112.0f), S(150.0f));

    const Rect tiles = body.TopSlice(tileHeight);
    const Rect belowTiles = body.RestBelow(tileHeight + gap);

    // The middle row holds a chart and a group of bar meters, and neither gets
    // any better past a certain height: a 75 second trace stretched over half a
    // metre of screen says nothing more than it did, and a meter taller than
    // nine times its own width stops reading as a meter. So the middle row
    // takes what it can use and no more.
    //
    // The process list underneath has no such ceiling — more height is simply
    // more rows — so on a tall window everything left over goes to it. Without
    // this the core panel fills with dead space while the list shows six rows.
    // The floor accounts for what the process card spends before its first
    // row: a title, a filter box and a set of column headers. Without allowing
    // for those, a short window leaves the list showing three rows.
    float bottomHeight = std::clamp(body.Height() * kBottomRowShare, S(218.0f), S(330.0f));
    const float middleCeiling = S(kMiddleRowCeiling);
    if (belowTiles.Height() - bottomHeight - gap > middleCeiling) {
        bottomHeight = belowTiles.Height() - middleCeiling - gap;
    }
    bottomHeight = std::min(bottomHeight, std::max(0.0f, belowTiles.Height() - gap));

    const Rect bottom = belowTiles.BottomSlice(bottomHeight);
    const Rect middle = belowTiles.RestAbove(bottomHeight + gap);

    const float leftWidth = std::floor((body.Width() - gap) * kLeftColumnRatio);

    const Tile tileSet[] = {
        Tile{L"PROCESSOR", theme.seriesCpu, cpuLoad_.Value(),
             FormatFrequency(static_cast<unsigned>(clockMhz_.Value() + 0.5f)),
             std::to_wstring(system.physicalCores) + L" cores · " +
                 std::to_wstring(system.logicalCores) + L" threads"},
        Tile{L"MEMORY", theme.seriesMemory, memoryLoad_.Value(),
             FormatBytes(static_cast<std::uint64_t>(memoryUsed_.Value())) + L" / " +
                 FormatBytes(snapshot.memory.totalBytes),
             L"commit " + FormatBytes(static_cast<std::uint64_t>(memoryCommitted_.Value()))},
        Tile{L"GRAPHICS", theme.seriesGpu, gpuLoad_.Value(),
             snapshot.gpu.dedicatedTotalBytes > 0
                 ? FormatBytes(static_cast<std::uint64_t>(gpuMemoryUsed_.Value())) + L" / " +
                       FormatBytes(snapshot.gpu.dedicatedTotalBytes)
                 : std::wstring(L"--"),
             snapshot.gpu.available ? snapshot.gpu.name : std::wstring(L"no adapter detected"),
             GpuThermalBadge(snapshot.gpu, gpuTemperature_.Value())},
        Tile{L"STORAGE", theme.seriesDisk, diskLoad_.Value(),
             FormatBytesShort(static_cast<std::uint64_t>(diskFree_.Value())) + L" free",
             snapshot.storage.available
                 ? L"↓ " + ShortRate(diskRead_.Value()) + L"   ↑ " + ShortRate(diskWrite_.Value())
                 : std::wstring(L"disk counters unavailable")},
    };

    static_assert(std::size(tileSet) == kTileCount, "tile order must match Metric");
    const float tileWidth = (tiles.Width() - gap * (kTileCount - 1)) / kTileCount;
    for (int index = 0; index < kTileCount; ++index) {
        const float left = tiles.left + index * (tileWidth + gap);
        tileBoxes_[index] = Rect{left, tiles.top, left + tileWidth, tiles.bottom};
        DrawTile(canvas, tileBoxes_[index], tileSet[index], index);
    }

    panelBoxes_[0] = Rect{middle.left, middle.top, middle.left + leftWidth, middle.bottom};
    panelBoxes_[1] = Rect{middle.left + leftWidth + gap, middle.top, middle.right, middle.bottom};
    panelBoxes_[2] = Rect{bottom.left, bottom.top, bottom.left + leftWidth, bottom.bottom};
    panelBoxes_[3] = Rect{bottom.left + leftWidth + gap, bottom.top, bottom.right, bottom.bottom};

    DrawCpuPanel(canvas, panelBoxes_[0]);
    DrawProcessorPanel(canvas, panelBoxes_[1]);
    DrawNetworkPanel(canvas, panelBoxes_[2]);
    DrawProcessPanel(canvas, panelBoxes_[3]);

    DrawStatusBar(canvas, status);
}

Rect Dashboard::DrawCard(Canvas& canvas, const Rect& box, std::wstring_view title,
                         const Color& dot, std::wstring_view trailing, float press,
                         float glow) const {
    const Theme& theme = *theme_;
    const float hairline = canvas.Hairline();
    const float radius = S(theme.cornerRadius);

    // Two separate things, deliberately. Press is how far the card has moved
    // under the pointer, and every card does it. Glow is the accent light, and
    // only the card the chart is following gets any. Lighting everything the
    // pointer touches makes the whole page look selected.
    press = std::clamp(press, 0.0f, 1.0f);
    glow = std::clamp(glow, 0.0f, 1.0f);

    // Shrinking is what sells the movement: the eye reads a shape that has got
    // smaller against a fixed neighbour as one that has moved away from it.
    const Rect face = box.Deflate(press * S(2.0f));

    // The recess the card presses into. Neutral, not accented, so a hovered
    // card reads as moved rather than as chosen.
    if (press > 0.01f) {
        canvas.FillRoundedRect(box, radius, Color::Hex(0x000000, 0.30f * press));
    }

    canvas.FillRoundedRect(face, radius, theme.surface);

    // Light from above, in the card's own colour, and only when it is the
    // active one. A gradient across the whole face rather than a rule along its
    // top edge: a rule reads as a drawn line, this reads as illumination.
    if (glow > 0.01f) {
        canvas.FillVerticalGradient(face, dot.WithAlpha(0.17f * glow), dot.WithAlpha(0.0f),
                                    radius);
    }

    canvas.StrokeRoundedRect(face, radius,
                             Color::Lerp(theme.border, dot.WithAlpha(0.55f), glow), hairline);

    // A specular edge along the top and a shadow along the bottom. One pixel
    // each, and the card stops being a rectangle and starts being a surface.
    // Both firm up a little as it presses in, which is what light does.
    const float inset = radius * 0.7f;
    canvas.StrokeLine(Point{face.left + inset, face.top + hairline},
                      Point{face.right - inset, face.top + hairline},
                      Color::Hex(0xFFFFFF, 0.04f + 0.05f * press), hairline);
    canvas.StrokeLine(Point{face.left + inset, face.bottom - hairline},
                      Point{face.right - inset, face.bottom - hairline},
                      Color::Hex(0x000000, 0.18f + 0.14f * press), hairline);

    const Rect inner = face.Deflate(S(theme.cardPadding), S(12.0f));
    const float titleHeight = S(16.0f);
    const Rect header = inner.TopSlice(titleHeight);

    const float marker = S(5.0f);
    canvas.FillRoundedRect(
        Rect::FromXYWH(header.left, canvas.Snap(header.CenterY() - marker * 0.5f), marker,
                       marker),
        marker * 0.3f, dot);

    const Rect labels = header.RestRight(marker + S(8.0f));
    canvas.DrawString(title, LabelStyle(theme, S(10.0f)), labels, theme.textMuted, HAlign::Left,
                      VAlign::Middle);

    if (!trailing.empty()) {
        // Anything ending in a degree sign is a temperature, and a temperature
        // is a reading rather than a caption: it gets the brightness and the
        // tabular figures every other number on the dashboard gets, and it
        // warms towards amber and red as it climbs.
        const bool temperature = trailing.find(L'°') != std::wstring_view::npos;
        if (temperature) {
            const float celsius = static_cast<float>(_wtof(std::wstring(trailing).c_str()));
            const Color warmth = SeverityColor(theme, std::clamp(celsius / 95.0f, 0.0f, 1.0f));
            canvas.DrawString(trailing, ValueStyle(theme, S(11.0f), 600), labels, warmth,
                              HAlign::Right, VAlign::Middle);
        } else {
            canvas.DrawString(trailing, BodyStyle(theme, S(10.0f)), labels, theme.textFaint,
                              HAlign::Right, VAlign::Middle);
        }
    }

    return inner.RestBelow(titleHeight + S(10.0f));
}

void Dashboard::DrawTile(Canvas& canvas, const Rect& box, const Tile& tile, int index) const {
    const Theme& theme = *theme_;

    // Selection and hover are eased, so a tile lights up rather than blinking.
    // The light itself is the card's job; all this decides is how much.
    const Rect content = DrawCard(canvas, box, tile.title, tile.accent, tile.badge,
                                  tileHover_[index].Value(), tileSelection_[index].Value());
    if (content.Empty()) return;

    const float groupWidth = std::min(content.Width(), S(250.0f));
    const Rect group{content.left + (content.Width() - groupWidth) * 0.5f, content.top,
                     content.left + (content.Width() + groupWidth) * 0.5f, content.bottom};

    const float diameter = std::min(group.Height(), S(60.0f));
    const Rect gauge{group.left, canvas.Snap(group.CenterY() - diameter * 0.5f),
                     group.left + diameter, canvas.Snap(group.CenterY() + diameter * 0.5f)};

    DrawRingGauge(canvas, gauge, tile.load, tile.accent, theme.gaugeTrack, S(4.0f));
    canvas.DrawString(FormatPercent(tile.load), ValueStyle(theme, S(14.0f), 600), gauge,
                      theme.textPrimary, HAlign::Center, VAlign::Middle);

    const Rect text = group.RestRight(diameter + S(14.0f));
    const float middle = canvas.Snap(text.CenterY());

    canvas.DrawString(tile.detail, HeadlineStyle(theme, S(16.5f), 500),
                      Rect{text.left, middle - S(21.0f), text.right, middle + S(1.0f)},
                      theme.textPrimary, HAlign::Left, VAlign::Bottom);
    canvas.DrawString(tile.caption, BodyStyle(theme, S(10.5f)),
                      Rect{text.left, middle + S(3.0f), text.right, middle + S(19.0f)},
                      theme.textMuted, HAlign::Left, VAlign::Top);
}

const Trace& Dashboard::SelectedTrace() const {
    const Traces& traces = readings_.traces;
    switch (metric_) {
        case Metric::Memory: return traces.memory;
        case Metric::Gpu: return traces.gpu;
        case Metric::Storage: return traces.diskActivity;
        default: return traces.cpu;
    }
}

Color Dashboard::SelectedColour() const {
    const Theme& theme = *theme_;
    switch (metric_) {
        case Metric::Memory: return theme.seriesMemory;
        case Metric::Gpu: return theme.seriesGpu;
        case Metric::Storage: return theme.seriesDisk;
        default: return theme.seriesCpu;
    }
}

std::wstring_view Dashboard::SelectedTitle() const {
    switch (metric_) {
        case Metric::Memory: return L"MEMORY LOAD";
        case Metric::Gpu: return L"GRAPHICS LOAD";
        case Metric::Storage: return L"DISK ACTIVITY";
        default: return L"CPU LOAD";
    }
}

float Dashboard::SelectedValue() const {
    switch (metric_) {
        case Metric::Memory: return memoryLoad_.Value();
        case Metric::Gpu: return gpuLoad_.Value();
        case Metric::Storage: return diskLoad_.Value();
        default: return cpuLoad_.Value();
    }
}

// The line under the headline percentage, saying what that percentage is of.
std::wstring Dashboard::SelectedHeadline() const {
    const Snapshot& snapshot = readings_.snapshot;
    switch (metric_) {
        case Metric::Memory:
            return FormatBytes(static_cast<std::uint64_t>(memoryUsed_.Value())) + L" of " +
                   FormatBytes(snapshot.memory.totalBytes) + L" in use";
        case Metric::Gpu: {
            std::wstring text = snapshot.gpu.available ? snapshot.gpu.name : L"no adapter";
            if (snapshot.gpu.dedicatedTotalBytes > 0) {
                text += L"   ·   " +
                        FormatBytes(static_cast<std::uint64_t>(gpuMemoryUsed_.Value())) + L" / " +
                        FormatBytes(snapshot.gpu.dedicatedTotalBytes);
            }
            if (snapshot.gpu.hasTemperature) {
                wchar_t degrees[24]{};
                swprintf_s(degrees, L"   ·   %.0f°C", gpuTemperature_.Value());
                text += degrees;
            }
            return text;
        }
        case Metric::Storage:
            return L"↓ " + ShortRate(diskRead_.Value()) + L"   ↑ " +
                   ShortRate(diskWrite_.Value()) + L"   ·   " +
                   FormatBytesShort(static_cast<std::uint64_t>(diskFree_.Value())) + L" free";
        default:
            return std::to_wstring(SystemInfo::Current().physicalCores) + L" cores  ·  " +
                   std::to_wstring(SystemInfo::Current().logicalCores) + L" threads  ·  " +
                   FormatFrequency(static_cast<unsigned>(clockMhz_.Value() + 0.5f));
    }
}

void Dashboard::DrawCpuPanel(Canvas& canvas, const Rect& box) const {
    const Theme& theme = *theme_;
    const Color accent = SelectedColour();

    const Rect content =
        DrawCard(canvas, box, SelectedTitle(), accent, L"LAST 75 s", panelHover_[0].Value());
    if (content.Empty()) return;

    const float value = SelectedValue();
    const Rect header = content.TopSlice(S(36.0f));
    canvas.DrawString(FormatPercent(value, 1), HeadlineStyle(theme, S(32.0f), 300), header,
                      SeverityColor(theme, value), HAlign::Left, VAlign::Middle);

    // Peak and average follow the selected series rather than staying on the
    // processor, so the two numbers always describe the line underneath them.
    const Trace& trace = SelectedTrace();
    float peak = 0.0f;
    float sum = 0.0f;
    for (std::size_t at = 0; at < trace.Size(); ++at) {
        peak = std::max(peak, trace.At(at));
        sum += trace.At(at);
    }
    const float average = trace.Size() > 0 ? sum / static_cast<float>(trace.Size()) : 0.0f;

    const float statWidth = S(74.0f);
    DrawMiniStat(canvas, theme, header.RightSlice(statWidth), L"PEAK", FormatPercent(peak),
                 HAlign::Right, uiScale_);
    DrawMiniStat(canvas, theme, header.RestLeft(statWidth).RightSlice(statWidth), L"AVERAGE",
                 FormatPercent(average), HAlign::Right, uiScale_);

    const Rect caption = content.RestBelow(header.Height()).TopSlice(S(15.0f));
    canvas.DrawString(SelectedHeadline(), BodyStyle(theme, S(10.5f)), caption, theme.textMuted,
                      HAlign::Left, VAlign::Top);

    const Rect plot = content.RestBelow(header.Height() + caption.Height() + S(6.0f));
    if (plot.Empty()) return;

    canvas.FillRoundedRect(plot, S(6.0f), theme.surfaceSunken);
    DrawPlotGrid(canvas, plot);
    DrawSeries(canvas, plot, trace, SeriesStyle{accent, 1.0f, S(1.6f)}, scrollPhase_);
    DrawGhostSignature(canvas, plot, theme);
}

void Dashboard::DrawProcessorPanel(Canvas& canvas, const Rect& box) const {
    const Theme& theme = *theme_;
    const CpuSnapshot& cpu = readings_.snapshot.cpu;

    const std::wstring trailing = std::to_wstring(cpu.cores.size()) + L" LOGICAL";
    const Rect content =
        DrawCard(canvas, box, L"CORE ACTIVITY", theme.accent, trailing, panelHover_[1].Value());
    if (content.Empty()) return;

    const float rowHeight = S(32.0f);
    const Rect stats = content.BottomSlice(rowHeight * 2.0f);
    const Rect bars = content.RestAbove(stats.Height() + S(12.0f));

    CoreBarStyle style;
    style.low = theme.accentDeep;
    style.high = theme.accentSoft;
    style.track = Color::Hex(0xFFFFFF, 0.028f);
    style.label = ValueStyle(theme, S(9.5f));
    style.labelColor = theme.textFaint;
    // Wider when the panel is wide, so eight cores on a very wide window fill
    // the card instead of huddling in the middle of it. The cap keeps them from
    // turning into slabs on an ultrawide.
    style.maximumBarWidth =
        std::clamp(bars.Width() / 14.0f, S(46.0f), S(74.0f));
    style.gap = S(8.0f);
    DrawCoreBars(canvas, bars, coreLoads_.Values(), style);

    // Two by two, so the numbers stay legible however narrow the column gets.
    const float columnWidth = stats.Width() * 0.5f;
    const Rect top = stats.TopSlice(rowHeight);
    const Rect lower = stats.BottomSlice(rowHeight);

    auto rounded = [](const Eased& value) {
        return static_cast<std::uint64_t>(value.Value() + 0.5f);
    };

    DrawMiniStat(canvas, theme, top.LeftSlice(columnWidth), L"CLOCK",
                 FormatFrequency(static_cast<unsigned>(rounded(clockMhz_))), HAlign::Left,
                 uiScale_);
    DrawMiniStat(canvas, theme, top.RestRight(columnWidth), L"PROCESSES",
                 FormatCount(rounded(processCount_)), HAlign::Left, uiScale_);
    DrawMiniStat(canvas, theme, lower.LeftSlice(columnWidth), L"THREADS",
                 FormatCount(rounded(threadCount_)), HAlign::Left, uiScale_);
    DrawMiniStat(canvas, theme, lower.RestRight(columnWidth), L"HANDLES",
                 FormatCount(rounded(handleCount_)), HAlign::Left, uiScale_);
}

void Dashboard::DrawNetworkPanel(Canvas& canvas, const Rect& box) const {
    const Theme& theme = *theme_;
    const NetworkSnapshot& network = readings_.snapshot.network;

    std::wstring trailing = network.adapter;
    if (network.linkSpeedBitsPerSecond > 0) {
        trailing += L"  ·  " + FormatLinkSpeed(network.linkSpeedBitsPerSecond);
    }

    const Rect content =
        DrawCard(canvas, box, L"NETWORK", theme.seriesNetDown, trailing, panelHover_[2].Value());
    if (content.Empty()) return;

    const Rect header = content.TopSlice(S(24.0f));
    const float legendWidth = std::min(S(170.0f), header.Width() * 0.5f);
    DrawLegendEntry(canvas, theme, header.LeftSlice(legendWidth), theme.seriesNetDown, L"DOWN",
                    FormatRate(networkDown_.Value()), uiScale_);
    DrawLegendEntry(canvas, theme, header.RestRight(legendWidth).LeftSlice(legendWidth),
                    theme.seriesNetUp, L"UP", FormatRate(networkUp_.Value()), uiScale_);

    const Rect plot = content.RestBelow(header.Height() + S(8.0f));
    if (plot.Empty()) return;

    canvas.FillRoundedRect(plot, S(6.0f), theme.surfaceSunken);
    DrawPlotGrid(canvas, plot);

    const auto scale = static_cast<float>(readings_.networkScale);
    DrawSeries(canvas, plot, readings_.traces.networkDown,
               SeriesStyle{theme.seriesNetDown, scale, S(1.6f)}, scrollPhase_);
    // Upload rides over the download as a line only, so neither series hides
    // the other when both are busy.
    DrawSeries(canvas, plot, readings_.traces.networkUp,
               SeriesStyle{theme.seriesNetUp, scale, S(1.4f), 0.10f, false}, scrollPhase_);
}

void Dashboard::DrawColumnHeader(Canvas& canvas, const Rect& box, std::wstring_view label,
                                 Column column, HAlign align) const {
    const Theme& theme = *theme_;
    const bool active = sortColumn_ == column;
    const bool hovered = mouseInside_ && box.Deflate(0.0f, -S(5.0f)).Contains(mouse_);

    Color color = theme.textFaint;
    if (active) color = theme.textSecondary;
    if (hovered) color = theme.textPrimary;

    const TextStyle style = LabelStyle(theme, S(9.5f));
    const float labelWidth = canvas.MeasureString(label, style).width;
    const float arrow = S(7.0f);
    const float gap = S(5.0f);

    // The indicator sits immediately beside the label, on the side away from
    // the alignment edge. Parking it at the edge of the column instead leaves
    // it stranded next to whichever column happens to be adjacent.
    Rect labelBox = box;
    if (active) {
        labelBox = align == HAlign::Left ? box.RestLeft(arrow + gap) : box.RestRight(arrow + gap);
        const float centreX = align == HAlign::Left
                                  ? labelBox.left + labelWidth + gap + arrow * 0.5f
                                  : labelBox.right - labelWidth - gap - arrow * 0.5f;
        DrawSortArrow(canvas, Point{canvas.Snap(centreX), canvas.Snap(box.CenterY())}, arrow,
                      !sortDescending_, color, S(1.3f));
    }

    canvas.DrawString(label, style, labelBox, color, align, VAlign::Middle);
}

void Dashboard::DrawProcessPanel(Canvas& canvas, const Rect& box) {
    const Theme& theme = *theme_;
    const auto& processes = readings_.snapshot.processes;

    // The count says how many are being shown when a filter is on, and how many
    // there are when it is not.
    std::wstring trailing = FormatCount(processes.size()) + L" RUNNING";
    if (!search_.Empty()) {
        trailing = FormatCount(order_.size()) + L" OF " + FormatCount(processes.size());
    }
    if (frozenFade_.Value() > 0.5f) trailing = L"FROZEN  ·  " + trailing;

    const Rect card =
        DrawCard(canvas, box, L"PROCESSES", theme.seriesGpu, trailing, panelHover_[3].Value());
    if (card.Empty()) {
        headersValid_ = false;
        return;
    }

    // The filter box sits between the card title and the column headers, which
    // is where the eye already is when it wants to narrow the list down.
    const float searchHeight = S(26.0f);
    searchBox_ = card.TopSlice(searchHeight);
    const Rect content = card.RestBelow(searchHeight + S(8.0f));

    {
        const float radius = searchHeight * 0.5f;
        const bool active = !search_.Empty();
        canvas.FillRoundedRect(searchBox_, radius, theme.surfaceSunken);
        canvas.StrokeRoundedRect(searchBox_, radius,
                                 active ? theme.seriesGpu.WithAlpha(0.45f) : theme.border,
                                 canvas.Hairline());

        // A magnifier, drawn rather than set in an icon font so it matches the
        // stroke weight of everything around it at any scale.
        const Point lens{searchBox_.left + S(15.0f), canvas.Snap(searchBox_.CenterY())};
        const float lensRadius = S(4.5f);
        canvas.StrokeCircle(lens, lensRadius, theme.textMuted, S(1.4f));
        canvas.StrokeLine(Point{lens.x + lensRadius * 0.72f, lens.y + lensRadius * 0.72f},
                          Point{lens.x + lensRadius * 1.7f, lens.y + lensRadius * 1.7f},
                          theme.textMuted, S(1.4f));

        const Rect field = searchBox_.Deflate(S(28.0f), S(5.0f));
        if (caretClickPending_) {
            search_.PlaceCaret(canvas, field, caretClick_, theme, uiScale_);
            caretClickPending_ = false;
        }
        search_.Draw(canvas, field, theme, uiScale_, seconds_, true);
    }

    const float memoryColumn = S(66.0f);
    const float cpuColumn = S(56.0f);
    // The meter is the first thing to go when the column gets tight: the
    // numbers beside it carry the same information.
    const float meterColumn = content.Width() >= S(400.0f) ? S(78.0f) : 0.0f;
    const float numbersWidth = memoryColumn + cpuColumn + meterColumn;

    const Rect header = content.TopSlice(S(15.0f));
    const Rect nameHeader{header.left, header.top, header.right - numbersWidth - S(10.0f),
                          header.bottom};
    const Rect cpuHeader = header.RestLeft(memoryColumn).RightSlice(cpuColumn);
    const Rect memoryHeader = header.RightSlice(memoryColumn);

    DrawColumnHeader(canvas, nameHeader, L"NAME", Column::Name, HAlign::Left);
    DrawColumnHeader(canvas, cpuHeader, L"CPU", Column::Cpu, HAlign::Right);
    DrawColumnHeader(canvas, memoryHeader, L"MEMORY", Column::Memory, HAlign::Right);

    // Recorded after drawing so a click is tested against exactly the geometry
    // that is on screen, and grown vertically to a comfortable target.
    const float grow = S(5.0f);
    headers_[0] = ColumnHeader{nameHeader.Deflate(0.0f, -grow), Column::Name};
    headers_[1] = ColumnHeader{cpuHeader.Deflate(0.0f, -grow), Column::Cpu};
    headers_[2] = ColumnHeader{memoryHeader.Deflate(0.0f, -grow), Column::Memory};
    headersValid_ = true;

    const Rect underline = header.BottomSlice(canvas.Hairline()).Offset(0.0f, S(3.0f));
    canvas.FillRect(underline, theme.borderSoft);

    const Rect list = content.RestBelow(header.Height() + S(8.0f));
    const float rowHeight = S(22.0f);

    // Room for the scrollbar only when there is something to scroll.
    const auto capacity = static_cast<std::size_t>(std::max(0.0f, list.Height()) / rowHeight);
    const bool scrollable = order_.size() > capacity;
    listBox_ = scrollable ? list.RestLeft(S(10.0f)) : list;

    visibleRows_ = capacity;
    ClampScroll();

    const float offset = std::clamp(scroll_.Value(), 0.0f,
                                    std::max(0.0f, order_.size() * rowHeight - list.Height()));
    firstVisibleRow_ = static_cast<std::size_t>(offset / rowHeight);

    // One extra row so the partially scrolled row at the bottom is drawn.
    const std::size_t rows =
        std::min({capacity + 1, order_.size() - std::min(firstVisibleRow_, order_.size()),
                  easedProcessCpu_.size()});

    const bool byMemory = sortColumn_ == Column::Memory;
    const float busiestCpu = std::max(busiestProcessCpu_.Value(), 0.02f);

    float busiestMemory = 1.0f;
    if (byMemory) {
        // The true maximum, not the first row: sorting ascending puts the
        // smallest process at the top and every meter would overflow.
        for (const ProcessEntry& entry : processes) {
            busiestMemory = std::max(busiestMemory, static_cast<float>(entry.workingSetBytes));
        }
    }

    // Small on purpose. Asking the shell about a path it has not seen can reach
    // the disk, so a screenful fills in over several frames rather than costing
    // one frame the lot of it.
    int iconBudget = 2;

    canvas.PushClip(list);

    for (std::size_t index = 0; index < rows; ++index) {
        const std::size_t absolute = firstVisibleRow_ + index;
        if (absolute >= order_.size()) break;

        const ProcessEntry& entry = processes[order_[absolute]];
        const float top = list.top + absolute * rowHeight - offset;
        const Rect row{listBox_.left, top, listBox_.right, top + rowHeight};
        if (row.bottom < list.top || row.top > list.bottom) continue;

        const bool hovered = mouseInside_ && list.Contains(mouse_) && row.Contains(mouse_);

        if (hovered) {
            canvas.FillRoundedRect(row.Deflate(-S(4.0f), S(0.5f)), S(4.0f),
                                   Color::Hex(0xFFFFFF, 0.055f));
        } else if (absolute % 2 == 1) {
            canvas.FillRoundedRect(row.Deflate(-S(4.0f), S(0.5f)), S(4.0f),
                                   Color::Hex(0xFFFFFF, 0.018f));
        }

        // The application's own icon, from the shell. Rows that have not been
        // resolved yet get the generic one, so nothing ever waits on it.
        const float glyph = S(14.0f);
        const Rect iconBox{canvas.Snap(row.left),
                           canvas.Snap(row.CenterY() - glyph * 0.5f),
                           canvas.Snap(row.left + glyph),
                           canvas.Snap(row.CenterY() + glyph * 0.5f)};
        if (ID2D1Bitmap* icon = icons_.Acquire(canvas, entry.pid, iconBudget)) {
            canvas.DrawBitmap(icon, iconBox, hovered ? 1.0f : 0.88f);
        }

        canvas.DrawString(entry.name, BodyStyle(theme, S(12.0f)),
                          row.RestRight(glyph + S(8.0f)).RestLeft(numbersWidth + S(10.0f)),
                          hovered ? theme.textPrimary : theme.textSecondary, HAlign::Left,
                          VAlign::Middle);

        if (meterColumn > 0.0f) {
            // The meter tracks whichever column the table is sorted by, so it
            // is always saying something about the order you are looking at.
            const float share =
                byMemory ? static_cast<float>(entry.workingSetBytes) / busiestMemory
                         : easedProcessCpu_[index] / busiestCpu;
            const Color fill = byMemory ? theme.seriesMemory : theme.accent;

            const Rect column = row.RestLeft(memoryColumn + cpuColumn)
                                    .RightSlice(meterColumn - S(14.0f));
            const float thickness = S(2.0f);
            DrawMeterBar(canvas,
                         Rect{column.left, canvas.Snap(column.CenterY() - thickness),
                              column.right, canvas.Snap(column.CenterY() + thickness)},
                         share, fill.WithAlpha(0.75f), Color::Hex(0xFFFFFF, 0.05f));
        }

        canvas.DrawString(FormatPercent(easedProcessCpu_[index], 1), ValueStyle(theme, S(12.0f)),
                          row.RestLeft(memoryColumn), theme.textPrimary, HAlign::Right,
                          VAlign::Middle);
        canvas.DrawString(FormatBytesShort(entry.workingSetBytes), ValueStyle(theme, S(12.0f)),
                          row, theme.textMuted, HAlign::Right, VAlign::Middle);
    }

    canvas.PopClip();

    if (scrollable) {
        const float width = S(5.0f);
        const Rect track{list.right - width, list.top, list.right, list.bottom};
        canvas.FillRoundedRect(track, width * 0.5f, Color::Hex(0xFFFFFF, 0.03f));

        const float total = order_.size() * rowHeight;
        const float maximum = std::max(1.0f, total - list.Height());
        const float thumbHeight = std::max(S(26.0f), list.Height() * (list.Height() / total));
        const float travel = std::max(0.0f, list.Height() - thumbHeight);
        const float thumbTop = list.top + travel * std::clamp(offset / maximum, 0.0f, 1.0f);

        scrollbarBox_ = Rect{track.left, thumbTop, track.right, thumbTop + thumbHeight};
        const bool hot = draggingScrollbar_ || (mouseInside_ && track.Contains(mouse_));
        canvas.FillRoundedRect(scrollbarBox_, width * 0.5f,
                               hot ? theme.accent.WithAlpha(0.6f) : theme.borderStrong);
    } else {
        scrollbarBox_ = Rect{};
    }
}

void Dashboard::DrawStatusBar(Canvas& canvas, const Rect& box) const {
    const Theme& theme = *theme_;
    const SystemInfo& system = SystemInfo::Current();

    canvas.FillRect(box, theme.chromeBar);
    canvas.FillRect(box.TopSlice(canvas.Hairline()), theme.borderSoft);

    const Rect inner = box.Deflate(S(theme.pagePadding), 0.0f);
    const TextStyle style = BodyStyle(theme, S(10.5f));

    const float signatureWidth = DrawSignature(canvas, inner, theme, uiScale_);

    const Rect uptime = inner.RestLeft(signatureWidth + S(18.0f)).RightSlice(S(160.0f));
    const Rect machine{inner.left, inner.top, uptime.left - S(12.0f), inner.bottom};

    // A notice from an action the user just took displaces the machine
    // description for a few seconds, because it is the more interesting of the
    // two at that moment and there is only one line to say it on.
    const bool showingNotice = seconds_ < noticeUntil_ && !notice_.empty();
    const std::wstring left =
        showingNotice ? notice_
                      : system.cpuBrand + L"   ·   " + system.osName + L"   ·   " + system.osBuild;

    canvas.DrawString(left, style, machine,
                      showingNotice ? theme.accentSoft : theme.textMuted, HAlign::Left,
                      VAlign::Middle);
    canvas.DrawString(L"UPTIME  " + FormatUptime(readings_.snapshot.uptimeSeconds), style,
                      uptime, theme.textMuted, HAlign::Right, VAlign::Middle);
}

} // namespace sysmon::ui
