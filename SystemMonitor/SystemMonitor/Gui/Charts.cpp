#include "Gui/Charts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sysmon::ui {
namespace {

using slick::Canvas;
using slick::Color;
using slick::Path;
using slick::Point;
using slick::Rect;

constexpr float kGaugeStartDegrees = -135.0f;
constexpr float kGaugeSweepDegrees = 270.0f;

// Catmull-Rom expressed as a cubic bezier. The vertical clamp is what stops a
// single spike from bowing the curve up through the panel above it.
void AppendSmoothSegments(Path& path, const std::vector<Point>& points) {
    const std::size_t count = points.size();
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const Point& previous = points[i == 0 ? 0 : i - 1];
        const Point& start = points[i];
        const Point& end = points[i + 1];
        const Point& next = points[i + 2 < count ? i + 2 : count - 1];

        const float low = std::min(start.y, end.y);
        const float high = std::max(start.y, end.y);

        Point control1{start.x + (end.x - previous.x) / 6.0f,
                       start.y + (end.y - previous.y) / 6.0f};
        Point control2{end.x - (next.x - start.x) / 6.0f, end.y - (next.y - start.y) / 6.0f};
        control1.y = std::clamp(control1.y, low, high);
        control2.y = std::clamp(control2.y, low, high);

        path.CurveTo(control1, control2, end);
    }
}

std::vector<Point> BuildPoints(const Rect& plot, const Trace& trace, float maximum,
                               float scrollPhase) {
    const std::size_t count = trace.Size();
    std::vector<Point> points;
    if (count < 2) return points;

    const float step = plot.Width() / static_cast<float>(Trace::Capacity() - 1);
    // The newest sample starts one whole step past the right edge and slides in
    // over the sample interval, so the series always covers the full plot.
    const float leadingEdge = plot.right + (1.0f - scrollPhase) * step;
    const float span = std::max(maximum, 1e-6f);

    points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float x = leadingEdge - static_cast<float>(count - 1 - i) * step;
        const float value = std::clamp(trace.At(i) / span, 0.0f, 1.0f);
        points.push_back(Point{x, plot.bottom - value * plot.Height()});
    }
    return points;
}

} // namespace

void DrawPlotGrid(Canvas& canvas, const Rect& plot, int rows, int columns) {
    if (plot.Empty()) return;

    const Color line = Color::Hex(0xFFFFFF, 0.035f);
    const float hairline = canvas.Hairline();

    for (int row = 1; row < rows; ++row) {
        const float y = canvas.Snap(plot.top + plot.Height() * row / static_cast<float>(rows));
        canvas.StrokeLine(Point{plot.left, y}, Point{plot.right, y}, line, hairline);
    }
    for (int column = 1; column < columns; ++column) {
        const float x =
            canvas.Snap(plot.left + plot.Width() * column / static_cast<float>(columns));
        canvas.StrokeLine(Point{x, plot.top}, Point{x, plot.bottom}, line, hairline);
    }
}

void DrawSeries(Canvas& canvas, const Rect& plot, const Trace& trace, const SeriesStyle& style,
                float scrollPhase) {
    const std::vector<Point> points = BuildPoints(plot, trace, style.maximum, scrollPhase);
    if (points.size() < 2) return;

    // Anything past the right edge is deliberate overdraw for the glide.
    canvas.PushClip(plot);

    if (style.fill) {
        Path area = canvas.NewPath();
        area.Begin(Point{points.front().x, plot.bottom}, true);
        area.LineTo(points.front());
        AppendSmoothSegments(area, points);
        area.LineTo(Point{points.back().x, plot.bottom});
        area.Close();
        canvas.FillPathVerticalGradient(area, plot, style.line.WithAlpha(style.fillOpacity),
                                        style.line.WithAlpha(0.0f));
    }

    Path line = canvas.NewPath();
    line.Begin(points.front(), false);
    AppendSmoothSegments(line, points);
    line.Close(false);

    if (style.glow) {
        // A soft wide pass under the crisp stroke: the line reads as lit rather
        // than merely coloured, which is most of the premium feel.
        canvas.StrokePath(line, style.line.WithAlpha(0.11f), style.lineWidth * 3.0f);
    }
    canvas.StrokePath(line, style.line, style.lineWidth);

    canvas.PopClip();
}

void DrawRingGauge(Canvas& canvas, const Rect& area, float value, const Color& accent,
                   const Color& track, float thickness) {
    if (area.Empty()) return;

    const Point centre = area.Center();
    const float radius = std::min(area.Width(), area.Height()) * 0.5f - thickness * 0.5f;
    if (radius <= 0.0f) return;

    const float sweep = kGaugeSweepDegrees * std::clamp(value, 0.0f, 1.0f);

    canvas.StrokeArc(centre, radius, kGaugeStartDegrees, kGaugeSweepDegrees, track, thickness);
    if (sweep > 0.0f) {
        canvas.StrokeArc(centre, radius, kGaugeStartDegrees, sweep, accent.WithAlpha(0.18f),
                         thickness * 2.2f);
        canvas.StrokeArc(centre, radius, kGaugeStartDegrees, sweep, accent, thickness);
    }
}

void DrawCoreBars(Canvas& canvas, const Rect& area, const std::vector<float>& cores,
                  const CoreBarStyle& style) {
    if (cores.empty() || area.Empty()) return;

    const auto count = static_cast<float>(cores.size());
    float gap = style.gap * (count > 32 ? 0.25f : (count > 16 ? 0.5f : 1.0f));
    const float available = std::max(2.0f, (area.Width() - gap * (count - 1.0f)) / count);
    const float width = std::min(available, style.maximumBarWidth);
    const float radius = std::min(width * 0.12f, 5.0f);

    // On a panel much wider than the columns need, the leftover goes into the
    // gaps rather than into the margins. Eight meters marooned in the middle of
    // a very wide card read as a mistake; the same eight spread across it read
    // as a design. Widening the columns instead would turn them into slabs, so
    // the spacing gives way first, up to a point.
    if (count > 1.0f) {
        const float spread = (area.Width() - width * count) / (count - 1.0f);
        gap = std::clamp(spread, gap, width * 0.8f);
    }

    const bool labelled = style.showLabels && width >= 20.0f;
    const float labelHeight = labelled ? style.label.size * 1.5f : 0.0f;

    const float columnHeight =
        std::min(area.Height() - labelHeight, width * style.maximumAspect);
    const float groupTop = area.top + (area.Height() - columnHeight - labelHeight) * 0.5f;
    const Rect columns{area.left, groupTop, area.right, groupTop + columnHeight};

    // Capped columns are centred rather than stretched, so eight cores on a
    // wide panel look composed instead of bloated.
    const float used = width * count + gap * (count - 1.0f);
    const float start = area.left + (area.Width() - used) * 0.5f;

    for (std::size_t index = 0; index < cores.size(); ++index) {
        const float left = start + index * (width + gap);
        const Rect column{left, columns.top, left + width, columns.bottom};
        canvas.FillRoundedRect(column, radius, style.track);

        const float load = std::clamp(cores[index], 0.0f, 1.0f);
        // A sliver of colour at idle keeps the row from looking switched off.
        const float filled = std::max(load * column.Height(), 3.0f);
        const Rect bar{column.left, column.bottom - filled, column.right, column.bottom};

        const Color tip = Color::Lerp(style.low, style.high, load);
        canvas.FillVerticalGradient(bar, tip, Color::Lerp(style.low, tip, 0.35f), radius);

        if (labelled) {
            wchar_t text[8]{};
            swprintf_s(text, L"%d", static_cast<int>(load * 100.0f + 0.5f));
            canvas.DrawString(
                text, style.label,
                Rect{column.left, columns.bottom, column.right, columns.bottom + labelHeight},
                style.labelColor, slick::HAlign::Center, slick::VAlign::Middle);
        }
    }
}

void DrawMeterBar(Canvas& canvas, const Rect& area, float value, const Color& accent,
                  const Color& track) {
    if (area.Empty()) return;

    const float radius = area.Height() * 0.5f;
    canvas.FillRoundedRect(area, radius, track);

    const float filled = std::clamp(value, 0.0f, 1.0f) * area.Width();
    if (filled <= 0.0f) return;

    // Below one full cap the rounded rectangle degenerates into a lens; a plain
    // dot of the right width looks like the intended start of the bar.
    const Rect bar{area.left, area.top, area.left + std::max(filled, area.Height()), area.bottom};
    canvas.FillRoundedRect(bar, radius, accent);
}

} // namespace sysmon::ui
