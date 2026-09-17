#pragma once

#include <algorithm>
#include <cstdint>

namespace slick {

// Geometry is expressed in DIPs (device independent pixels) throughout the UI.
// The renderer hands Direct2D the window DPI, so layout code never multiplies
// by a scale factor -- only raw mouse input is converted, once, on the way in.

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct Size {
    float width = 0.0f;
    float height = 0.0f;
};

struct Rect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    static Rect FromXYWH(float x, float y, float w, float h) {
        return Rect{x, y, x + w, y + h};
    }

    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
    float CenterX() const { return (left + right) * 0.5f; }
    float CenterY() const { return (top + bottom) * 0.5f; }
    Point Center() const { return Point{CenterX(), CenterY()}; }
    Size GetSize() const { return Size{Width(), Height()}; }
    bool Empty() const { return Width() <= 0.0f || Height() <= 0.0f; }

    bool Contains(Point p) const {
        return p.x >= left && p.x < right && p.y >= top && p.y < bottom;
    }

    Rect Deflate(float dx, float dy) const {
        return Rect{left + dx, top + dy, right - dx, bottom - dy};
    }
    Rect Deflate(float d) const { return Deflate(d, d); }

    Rect Offset(float dx, float dy) const {
        return Rect{left + dx, top + dy, right + dx, bottom + dy};
    }

    // Slices measured from an edge; the caller keeps the remainder by calling
    // the matching Rest* helper, which is how every panel splits its space.
    Rect TopSlice(float h) const { return Rect{left, top, right, top + h}; }
    Rect BottomSlice(float h) const { return Rect{left, bottom - h, right, bottom}; }
    Rect LeftSlice(float w) const { return Rect{left, top, left + w, bottom}; }
    Rect RightSlice(float w) const { return Rect{right - w, top, right, bottom}; }

    Rect RestBelow(float h) const { return Rect{left, top + h, right, bottom}; }
    Rect RestAbove(float h) const { return Rect{left, top, right, bottom - h}; }
    Rect RestRight(float w) const { return Rect{left + w, top, right, bottom}; }
    Rect RestLeft(float w) const { return Rect{left, top, right - w, bottom}; }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static Color Hex(std::uint32_t rgb, float alpha = 1.0f) {
        return Color{((rgb >> 16) & 0xFF) / 255.0f,
                     ((rgb >> 8) & 0xFF) / 255.0f,
                     (rgb & 0xFF) / 255.0f,
                     alpha};
    }

    Color WithAlpha(float alpha) const { return Color{r, g, b, alpha}; }

    // Scales alpha rather than replacing it, so a colour that is already
    // translucent stays relatively translucent when a panel fades.
    Color Fade(float factor) const { return Color{r, g, b, a * factor}; }

    static Color Lerp(const Color& from, const Color& to, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return Color{from.r + (to.r - from.r) * t,
                     from.g + (to.g - from.g) * t,
                     from.b + (to.b - from.b) * t,
                     from.a + (to.a - from.a) * t};
    }
};

} // namespace slick
