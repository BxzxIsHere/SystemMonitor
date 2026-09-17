#pragma once

#include "Libs/Canvas.h"
#include "Libs/Theme.h"

namespace sysmon::ui {

// The author mark, in two deliberately different registers.
//
// DrawSignature is the readable one: a small badge that belongs to the design
// rather than sitting on top of it, carrying the same diamond and pulse the
// caption and the application icon use. DrawGhostSignature is the quiet one,
// set into a chart at an alpha that survives video compression but never
// competes with the data.

// Draws right-aligned inside the given area and returns the width it used, so
// the caller can lay out whatever sits beside it.
float DrawSignature(slick::Canvas& canvas, const slick::Rect& area, const slick::Theme& theme,
                    float scale);

void DrawGhostSignature(slick::Canvas& canvas, const slick::Rect& plot,
                        const slick::Theme& theme);

// The mark on its own: an outlined diamond with a pulse through it. Shared so
// the caption, the badge and anything else all trace the same geometry.
void DrawMark(slick::Canvas& canvas, slick::Point centre, float radius, float stroke,
              const slick::Color& color);

} // namespace sysmon::ui
