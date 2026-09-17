#pragma once

#include "Libs/Renderer.h"
#include "Libs/Theme.h"
#include "Libs/Types.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace slick {

enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Middle, Bottom };

// Builds a Direct2D path geometry with a small, readable vocabulary. Charts
// live and die by this, so it stays deliberately tiny.
class Path {
public:
    explicit Path(ID2D1Factory1* factory);

    void Begin(Point start, bool filled);
    void LineTo(Point p);
    void CurveTo(Point control1, Point control2, Point end);
    void Close(bool closed = true);

    ID2D1PathGeometry* Geometry() const { return geometry_.Get(); }
    bool Valid() const { return geometry_ && !open_; }

private:
    ComPtr<ID2D1PathGeometry> geometry_;
    ComPtr<ID2D1GeometrySink> sink_;
    bool open_ = false;
};

// A thin, opinionated drawing surface over ID2D1DeviceContext.
//
// Everything is expressed in DIPs and plain Color values; brushes, gradient
// stop collections and text formats are cached behind the scenes so a frame
// costs allocations only when something genuinely new appears on screen.
class Canvas {
public:
    explicit Canvas(Renderer& renderer);

    void Clear(const Color& color);

    void FillRect(const Rect& rect, const Color& color);
    void FillRoundedRect(const Rect& rect, float radius, const Color& color);
    void StrokeRoundedRect(const Rect& rect, float radius, const Color& color,
                           float thickness = 1.0f);
    void FillVerticalGradient(const Rect& rect, const Color& top, const Color& bottom,
                              float radius = 0.0f);
    void FillCircle(Point center, float radius, const Color& color);
    void StrokeCircle(Point center, float radius, const Color& color, float thickness = 1.0f);
    void StrokeLine(Point a, Point b, const Color& color, float thickness = 1.0f);

    // Angles are degrees clockwise from twelve o clock, which is how a gauge
    // is naturally described.
    void StrokeArc(Point center, float radius, float startDegrees, float sweepDegrees,
                   const Color& color, float thickness, bool roundCaps = true);

    // Drawn with linear filtering, so a shell icon handed over at one size
    // still looks right when the row it sits in is a different one.
    void DrawBitmap(ID2D1Bitmap* bitmap, const Rect& destination, float opacity = 1.0f);

    // Premultiplied BGRA, top row first. Returns null if the device refuses it.
    ComPtr<ID2D1Bitmap> CreateBitmap(const void* pixels, unsigned width, unsigned height);

    void FillPath(const Path& path, const Color& color);
    void FillPathVerticalGradient(const Path& path, const Rect& span, const Color& top,
                                  const Color& bottom);
    void StrokePath(const Path& path, const Color& color, float thickness);

    void DrawString(std::wstring_view text, const TextStyle& style, const Rect& box,
                    const Color& color, HAlign h = HAlign::Left, VAlign v = VAlign::Top);
    Size MeasureString(std::wstring_view text, const TextStyle& style,
                       float maxWidth = 100000.0f);

    void PushClip(const Rect& rect);
    void PopClip();
    void PushOpacity(float opacity);
    void PopOpacity();

    Path NewPath() { return Path(factory_.Get()); }
    Size Viewport() const { return renderer_->LogicalSize(); }
    ID2D1DeviceContext* Dc() const { return renderer_->Dc(); }

    float Scale() const { return renderer_->Scale(); }

    // One physical pixel, expressed in DIPs. Hairlines drawn at this thickness
    // stay exactly one pixel wide at every scale factor.
    float Hairline() const { return 1.0f / renderer_->Scale(); }

    // Rounds a DIP coordinate onto the physical pixel grid, which is what keeps
    // thin strokes and small glyphs crisp instead of smeared across two pixels.
    float Snap(float value) const {
        const float scale = renderer_->Scale();
        return std::round(value * scale) / scale;
    }

private:
    struct TextKey {
        std::wstring text;
        std::wstring family;
        float size;
        int weight;
        float spacing;
        float maxWidth;
        bool italic;
        bool wrap;
        bool ellipsis;

        bool operator<(const TextKey& other) const;
    };

    ID2D1SolidColorBrush* Solid(const Color& color);
    ID2D1StrokeStyle* RoundStroke();
    ID2D1LinearGradientBrush* Gradient(const Color& top, const Color& bottom);
    IDWriteTextFormat* Format(const TextStyle& style);
    IDWriteTextLayout* Layout(std::wstring_view text, const TextStyle& style, float maxWidth);

    // Resolves a preferred family against the installed fonts once, so the UI
    // degrades to Segoe UI or Consolas instead of a random fallback face.
    const std::wstring& ResolveFamily(const wchar_t* requested);

    Renderer* renderer_ = nullptr;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID2D1SolidColorBrush> solid_;
    ComPtr<ID2D1StrokeStyle> roundStroke_;
    ComPtr<IDWriteFontCollection> fonts_;

    std::map<std::uint64_t, ComPtr<ID2D1LinearGradientBrush>> gradients_;
    std::map<std::wstring, ComPtr<IDWriteTextFormat>> formats_;
    std::map<TextKey, ComPtr<IDWriteTextLayout>> layouts_;
    std::map<std::wstring, std::wstring> familyCache_;
    int clipDepth_ = 0;
};

} // namespace slick
