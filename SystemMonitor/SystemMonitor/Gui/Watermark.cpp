#include "Gui/Watermark.h"

#include <algorithm>

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

constexpr const wchar_t* kAuthor = L"bxzx";
constexpr const wchar_t* kCredit = L"CRAFTED BY";

TextStyle CreditStyle(const Theme& theme, float scale) {
    return TextStyle{theme.fontText, 8.8f * scale, 600, 1.0f * scale};
}

TextStyle AuthorStyle(const Theme& theme, float scale) {
    return TextStyle{theme.fontText, 12.0f * scale, 700, 0.8f * scale};
}

} // namespace

void DrawMark(Canvas& canvas, Point centre, float radius, float stroke, const Color& color) {
    Path diamond = canvas.NewPath();
    diamond.Begin(Point{centre.x, centre.y - radius}, false);
    diamond.LineTo(Point{centre.x + radius, centre.y});
    diamond.LineTo(Point{centre.x, centre.y + radius});
    diamond.LineTo(Point{centre.x - radius, centre.y});
    diamond.Close();
    canvas.StrokePath(diamond, color, stroke);

    // The pulse runs across the waist of the diamond. Proportions are taken
    // from the icon art so the mark reads the same at every size it appears at.
    const float run = radius * 0.69f;
    const float rise = radius * 0.43f;

    Path pulse = canvas.NewPath();
    pulse.Begin(Point{centre.x - run, centre.y}, false);
    pulse.LineTo(Point{centre.x - run * 0.40f, centre.y});
    pulse.LineTo(Point{centre.x - run * 0.16f, centre.y - rise});
    pulse.LineTo(Point{centre.x + run * 0.18f, centre.y + rise});
    pulse.LineTo(Point{centre.x + run * 0.42f, centre.y});
    pulse.LineTo(Point{centre.x + run, centre.y});
    pulse.Close(false);
    canvas.StrokePath(pulse, color, stroke);
}

float DrawSignature(Canvas& canvas, const Rect& area, const Theme& theme, float scale) {
    const TextStyle credit = CreditStyle(theme, scale);
    const TextStyle author = AuthorStyle(theme, scale);

    const float creditWidth = canvas.MeasureString(kCredit, credit).width;
    const float authorWidth = canvas.MeasureString(kAuthor, author).width;

    const float markRadius = 5.0f * scale;
    const float padding = 9.0f * scale;
    const float gap = 7.0f * scale;

    const float contentWidth = creditWidth + gap + markRadius * 2.0f + gap * 0.7f + authorWidth;
    const float badgeWidth = contentWidth + padding * 2.0f;
    const float badgeHeight = std::min(area.Height() - 2.0f * scale, 21.0f * scale);

    const float centreY = canvas.Snap(area.CenterY());
    const Rect badge{area.right - badgeWidth, centreY - badgeHeight * 0.5f, area.right,
                     centreY + badgeHeight * 0.5f};

    // A pill rather than loose text: it reads as a maker's mark stamped on the
    // product instead of a line of status text that happens to say a name.
    canvas.FillRoundedRect(badge, badgeHeight * 0.5f, theme.surface);
    canvas.StrokeRoundedRect(badge, badgeHeight * 0.5f, theme.border, canvas.Hairline());

    float cursor = badge.left + padding;
    canvas.DrawString(kCredit, credit, Rect{cursor, badge.top, cursor + creditWidth, badge.bottom},
                      theme.textFaint, HAlign::Left, VAlign::Middle);
    cursor += creditWidth + gap;

    DrawMark(canvas, Point{cursor + markRadius, centreY}, markRadius, 1.15f * scale,
             theme.accent);
    cursor += markRadius * 2.0f + gap * 0.7f;

    canvas.DrawString(kAuthor, author, Rect{cursor, badge.top, badge.right, badge.bottom},
                      theme.textPrimary, HAlign::Left, VAlign::Middle);

    return badgeWidth;
}

void DrawGhostSignature(Canvas& canvas, const Rect& plot, const Theme& theme) {
    // Scaled to the panel so the mark keeps the same visual weight whether the
    // window is at its minimum size or maximised on a large display.
    const float size = std::clamp(plot.Width() * 0.032f, 18.0f, 34.0f);
    const TextStyle style{theme.fontDisplay, size, 700, size * 0.16f};
    const Rect box = plot.Deflate(size * 0.7f, size * 0.5f);

    // Low enough to disappear into the panel at a glance, high enough to still
    // be there after a screen recording has been through a codec twice.
    canvas.DrawString(kAuthor, style, box, theme.textPrimary.WithAlpha(0.045f), HAlign::Right,
                      VAlign::Top);
}

} // namespace sysmon::ui
