#pragma once

#include "Libs/Canvas.h"
#include "Logic/Metrics.h"

#include <vector>

namespace sysmon::ui {

struct SeriesStyle {
    slick::Color line;
    // Value that maps to the top of the plot. Loads use 1.0; rates pass the
    // autoscaled peak the sampler publishes.
    float maximum = 1.0f;
    float lineWidth = 1.6f;
    float fillOpacity = 0.22f;
    bool fill = true;
    bool glow = true;
};

// Faint reference grid, drawn under every series in a plot.
void DrawPlotGrid(slick::Canvas& canvas, const slick::Rect& plot, int rows = 4,
                  int columns = 6);

// scrollPhase runs 0..1 across one sample interval and slides the series left
// by that fraction of a step, so the chart glides continuously at 60 fps
// instead of stepping four times a second.
void DrawSeries(slick::Canvas& canvas, const slick::Rect& plot, const Trace& trace,
                const SeriesStyle& style, float scrollPhase);

// 270 degree dial with the value written in the middle.
void DrawRingGauge(slick::Canvas& canvas, const slick::Rect& area, float value,
                   const slick::Color& accent, const slick::Color& track, float thickness);

struct CoreBarStyle {
    slick::Color low;   // colour at idle
    slick::Color high;  // colour at full load
    slick::Color track;
    slick::TextStyle label;
    slick::Color labelColor;
    // Per-core readouts are drawn only when the columns are wide enough to
    // carry them; on a 32 thread machine they would be unreadable mush.
    bool showLabels = true;
    float maximumBarWidth = 30.0f;
    // Tightened automatically as the core count climbs, so a 32 thread machine
    // does not spend half the panel on gaps.
    float gap = 8.0f;

    // A column taller than this many times its own width stops reading as a
    // meter and starts reading as a skyscraper. The group is centred in
    // whatever space is left over rather than stretched to fill it.
    float maximumAspect = 9.0f;
};

void DrawCoreBars(slick::Canvas& canvas, const slick::Rect& area,
                  const std::vector<float>& cores, const CoreBarStyle& style);

void DrawMeterBar(slick::Canvas& canvas, const slick::Rect& area, float value,
                  const slick::Color& accent, const slick::Color& track);

} // namespace sysmon::ui
