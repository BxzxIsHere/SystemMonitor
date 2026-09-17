#include "Libs/Canvas.h"

#include <d2d1_1helper.h>
#include <dwrite_1.h>

#include <algorithm>
#include <cmath>

namespace slick {
namespace {

constexpr float kPi = 3.14159265358979f;

// Past this many distinct strings the layout cache is doing more harm than
// good, so it is dropped wholesale rather than evicted one entry at a time.
constexpr std::size_t kLayoutCacheLimit = 400;

D2D1_COLOR_F ToD2D(const Color& c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}


D2D1_RECT_F ToD2D(const Rect& r) {
    return D2D1::RectF(r.left, r.top, r.right, r.bottom);
}

D2D1_POINT_2F ToD2D(Point p) {
    return D2D1::Point2F(p.x, p.y);
}

std::uint32_t Quantise(const Color& c) {
    auto channel = [](float v) {
        return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (channel(c.r) << 24) | (channel(c.g) << 16) | (channel(c.b) << 8) | channel(c.a);
}

// Degrees clockwise from twelve o clock, in a y-down coordinate system.
Point OnCircle(Point centre, float radius, float degrees) {
    const float radians = (degrees - 90.0f) * kPi / 180.0f;
    return Point{centre.x + radius * std::cos(radians), centre.y + radius * std::sin(radians)};
}

const wchar_t* FallbackFor(const std::wstring& family) {
    if (family == L"Segoe UI Variable Display") return L"Segoe UI Variable Text";
    if (family == L"Segoe UI Variable Text") return L"Segoe UI";
    if (family == L"Cascadia Mono") return L"Consolas";
    if (family == L"Consolas") return L"Courier New";
    return nullptr;
}

} // namespace

Path::Path(ID2D1Factory1* factory) {
    Check(factory->CreatePathGeometry(geometry_.GetAddressOf()), "CreatePathGeometry");
}

void Path::Begin(Point start, bool filled) {
    Check(geometry_->Open(sink_.ReleaseAndGetAddressOf()), "ID2D1PathGeometry::Open");
    sink_->BeginFigure(ToD2D(start),
                       filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    open_ = true;
}

void Path::LineTo(Point p) {
    if (sink_) sink_->AddLine(ToD2D(p));
}

void Path::CurveTo(Point control1, Point control2, Point end) {
    if (!sink_) return;
    sink_->AddBezier(D2D1::BezierSegment(ToD2D(control1), ToD2D(control2), ToD2D(end)));
}

void Path::Close(bool closed) {
    if (!sink_) return;
    sink_->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    Check(sink_->Close(), "ID2D1GeometrySink::Close");
    sink_.Reset();
    open_ = false;
}

bool Canvas::TextKey::operator<(const TextKey& other) const {
    if (size != other.size) return size < other.size;
    if (weight != other.weight) return weight < other.weight;
    if (spacing != other.spacing) return spacing < other.spacing;
    if (maxWidth != other.maxWidth) return maxWidth < other.maxWidth;
    if (italic != other.italic) return italic < other.italic;
    if (wrap != other.wrap) return wrap < other.wrap;
    if (ellipsis != other.ellipsis) return ellipsis < other.ellipsis;
    if (family != other.family) return family < other.family;
    return text < other.text;
}

Canvas::Canvas(Renderer& renderer) : renderer_(&renderer) {
    ComPtr<ID2D1Factory> base;
    renderer_->Dc()->GetFactory(base.GetAddressOf());
    Check(base.As(&factory_), "ID2D1Factory as ID2D1Factory1");
    Check(renderer_->Dc()->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                                 solid_.GetAddressOf()),
          "CreateSolidColorBrush");
    renderer_->Dwrite()->GetSystemFontCollection(fonts_.GetAddressOf(), FALSE);
}

ID2D1StrokeStyle* Canvas::RoundStroke() {
    if (!roundStroke_) {
        auto props = D2D1::StrokeStyleProperties();
        props.startCap = D2D1_CAP_STYLE_ROUND;
        props.endCap = D2D1_CAP_STYLE_ROUND;
        Check(factory_->CreateStrokeStyle(props, nullptr, 0, roundStroke_.GetAddressOf()),
              "CreateStrokeStyle(round)");
    }
    return roundStroke_.Get();
}

ID2D1SolidColorBrush* Canvas::Solid(const Color& color) {
    solid_->SetColor(ToD2D(color));
    solid_->SetOpacity(1.0f);
    return solid_.Get();
}

ID2D1LinearGradientBrush* Canvas::Gradient(const Color& top, const Color& bottom) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(Quantise(top)) << 32) | Quantise(bottom);

    auto found = gradients_.find(key);
    if (found != gradients_.end()) return found->second.Get();

    const D2D1_GRADIENT_STOP stops[] = {
        {0.0f, ToD2D(top)},
        {1.0f, ToD2D(bottom)},
    };

    ComPtr<ID2D1GradientStopCollection> collection;
    Check(renderer_->Dc()->CreateGradientStopCollection(stops, ARRAYSIZE(stops),
                                                        collection.GetAddressOf()),
          "CreateGradientStopCollection");

    ComPtr<ID2D1LinearGradientBrush> brush;
    Check(renderer_->Dc()->CreateLinearGradientBrush(
              D2D1::LinearGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F()),
              collection.Get(), brush.GetAddressOf()),
          "CreateLinearGradientBrush");

    gradients_.emplace(key, brush);
    return brush.Get();
}

const std::wstring& Canvas::ResolveFamily(const wchar_t* requested) {
    std::wstring wanted = requested ? requested : L"Segoe UI";

    auto cached = familyCache_.find(wanted);
    if (cached != familyCache_.end()) return cached->second;

    std::wstring candidate = wanted;
    while (fonts_) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts_->FindFamilyName(candidate.c_str(), &index, &exists)) && exists) {
            break;
        }
        const wchar_t* next = FallbackFor(candidate);
        if (!next) {
            candidate = L"Segoe UI";
            break;
        }
        candidate = next;
    }

    return familyCache_.emplace(std::move(wanted), std::move(candidate)).first->second;
}

IDWriteTextFormat* Canvas::Format(const TextStyle& style) {
    const std::wstring& family = ResolveFamily(style.family);

    std::wstring key = family;
    key += L'|';
    key += std::to_wstring(static_cast<int>(style.size * 100.0f));
    key += L'|';
    key += std::to_wstring(style.weight);
    key += style.italic ? L"|i" : L"|r";
    key += style.wrap ? L"|w" : L"|n";
    key += style.ellipsis ? L"|e" : L"|c";

    auto found = formats_.find(key);
    if (found != formats_.end()) return found->second.Get();

    ComPtr<IDWriteTextFormat> format;
    Check(renderer_->Dwrite()->CreateTextFormat(
              family.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(style.weight),
              style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
              DWRITE_FONT_STRETCH_NORMAL, style.size, L"", format.GetAddressOf()),
          "CreateTextFormat");

    format->SetWordWrapping(style.wrap ? DWRITE_WORD_WRAPPING_WRAP
                                       : DWRITE_WORD_WRAPPING_NO_WRAP);

    if (style.ellipsis) {
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(renderer_->Dwrite()->CreateEllipsisTrimmingSign(format.Get(),
                                                                      sign.GetAddressOf()))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, sign.Get());
        }
    }

    formats_.emplace(std::move(key), format);
    return format.Get();
}

IDWriteTextLayout* Canvas::Layout(std::wstring_view text, const TextStyle& style,
                                  float maxWidth) {
    TextKey key{std::wstring(text), ResolveFamily(style.family), style.size,
                style.weight,       style.letterSpacing,        maxWidth,
                style.italic,       style.wrap,                 style.ellipsis};

    auto found = layouts_.find(key);
    if (found != layouts_.end()) return found->second.Get();

    if (layouts_.size() > kLayoutCacheLimit) layouts_.clear();

    ComPtr<IDWriteTextLayout> layout;
    Check(renderer_->Dwrite()->CreateTextLayout(key.text.c_str(),
                                                static_cast<UINT32>(key.text.size()),
                                                Format(style), maxWidth, 100000.0f,
                                                layout.GetAddressOf()),
          "CreateTextLayout");

    if (style.letterSpacing != 0.0f) {
        ComPtr<IDWriteTextLayout1> spaced;
        if (SUCCEEDED(layout.As(&spaced))) {
            const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(key.text.size())};
            spaced->SetCharacterSpacing(0.0f, style.letterSpacing, 0.0f, all);
        }
    }

    return layouts_.emplace(std::move(key), layout).first->second.Get();
}

void Canvas::Clear(const Color& color) {
    renderer_->Dc()->Clear(ToD2D(color));
}

void Canvas::FillRect(const Rect& rect, const Color& color) {
    if (rect.Empty() || color.a <= 0.0f) return;
    renderer_->Dc()->FillRectangle(ToD2D(rect), Solid(color));
}

void Canvas::FillRoundedRect(const Rect& rect, float radius, const Color& color) {
    if (rect.Empty() || color.a <= 0.0f) return;
    if (radius <= 0.0f) return FillRect(rect, color);
    const float limit = std::min(rect.Width(), rect.Height()) * 0.5f;
    renderer_->Dc()->FillRoundedRectangle(
        D2D1::RoundedRect(ToD2D(rect), std::min(radius, limit), std::min(radius, limit)),
        Solid(color));
}

void Canvas::StrokeRoundedRect(const Rect& rect, float radius, const Color& color,
                               float thickness) {
    if (rect.Empty() || color.a <= 0.0f) return;
    // Half a stroke is drawn on each side of the path, so nudging inwards keeps
    // the whole outline inside the rectangle it is describing.
    const Rect inset = rect.Deflate(thickness * 0.5f);
    const float limit = std::min(inset.Width(), inset.Height()) * 0.5f;
    const float r = std::max(0.0f, std::min(radius - thickness * 0.5f, limit));
    renderer_->Dc()->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(inset), r, r), Solid(color),
                                          thickness);
}

void Canvas::FillVerticalGradient(const Rect& rect, const Color& top, const Color& bottom,
                                  float radius) {
    if (rect.Empty()) return;
    auto* brush = Gradient(top, bottom);
    brush->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
    brush->SetEndPoint(D2D1::Point2F(rect.left, rect.bottom));

    if (radius > 0.0f) {
        const float limit = std::min(rect.Width(), rect.Height()) * 0.5f;
        const float r = std::min(radius, limit);
        renderer_->Dc()->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), r, r), brush);
    } else {
        renderer_->Dc()->FillRectangle(ToD2D(rect), brush);
    }
}

void Canvas::FillCircle(Point centre, float radius, const Color& color) {
    if (radius <= 0.0f || color.a <= 0.0f) return;
    renderer_->Dc()->FillEllipse(D2D1::Ellipse(ToD2D(centre), radius, radius), Solid(color));
}

void Canvas::StrokeCircle(Point centre, float radius, const Color& color, float thickness) {
    if (radius <= 0.0f || color.a <= 0.0f) return;
    renderer_->Dc()->DrawEllipse(D2D1::Ellipse(ToD2D(centre), radius, radius), Solid(color),
                                 thickness);
}

void Canvas::StrokeLine(Point a, Point b, const Color& color, float thickness) {
    if (color.a <= 0.0f) return;
    renderer_->Dc()->DrawLine(ToD2D(a), ToD2D(b), Solid(color), thickness);
}

void Canvas::StrokeArc(Point centre, float radius, float startDegrees, float sweepDegrees,
                       const Color& color, float thickness, bool roundCaps) {
    if (radius <= 0.0f || color.a <= 0.0f) return;
    // A single arc segment cannot describe a closed circle, and a hair under a
    // full turn is visually identical.
    sweepDegrees = std::clamp(sweepDegrees, -359.9f, 359.9f);
    if (std::abs(sweepDegrees) < 0.05f) return;

    D2D1_ARC_SEGMENT arc{};
    arc.point = ToD2D(OnCircle(centre, radius, startDegrees + sweepDegrees));
    arc.size = D2D1::SizeF(radius, radius);
    arc.rotationAngle = 0.0f;
    arc.sweepDirection = sweepDegrees >= 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                              : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    arc.arcSize = std::abs(sweepDegrees) > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;

    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    Check(factory_->CreatePathGeometry(geometry.GetAddressOf()), "CreatePathGeometry(arc)");
    Check(geometry->Open(sink.GetAddressOf()), "ID2D1PathGeometry::Open(arc)");
    sink->BeginFigure(ToD2D(OnCircle(centre, radius, startDegrees)),
                      D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(arc);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    Check(sink->Close(), "ID2D1GeometrySink::Close(arc)");

    renderer_->Dc()->DrawGeometry(geometry.Get(), Solid(color), thickness,
                                  roundCaps ? RoundStroke() : nullptr);
}

void Canvas::FillPath(const Path& path, const Color& color) {
    if (!path.Valid() || color.a <= 0.0f) return;
    renderer_->Dc()->FillGeometry(path.Geometry(), Solid(color));
}

void Canvas::FillPathVerticalGradient(const Path& path, const Rect& span, const Color& top,
                                      const Color& bottom) {
    if (!path.Valid()) return;
    auto* brush = Gradient(top, bottom);
    brush->SetStartPoint(D2D1::Point2F(span.left, span.top));
    brush->SetEndPoint(D2D1::Point2F(span.left, span.bottom));
    renderer_->Dc()->FillGeometry(path.Geometry(), brush);
}

void Canvas::StrokePath(const Path& path, const Color& color, float thickness) {
    if (!path.Valid() || color.a <= 0.0f) return;
    renderer_->Dc()->DrawGeometry(path.Geometry(), Solid(color), thickness);
}

void Canvas::DrawBitmap(ID2D1Bitmap* bitmap, const Rect& destination, float opacity) {
    if (!bitmap || destination.Empty() || opacity <= 0.0f) return;
    renderer_->Dc()->DrawBitmap(bitmap, ToD2D(destination), opacity,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

ComPtr<ID2D1Bitmap> Canvas::CreateBitmap(const void* pixels, unsigned width, unsigned height) {
    if (!pixels || width == 0 || height == 0) return nullptr;

    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(renderer_->Dc()->CreateBitmap(D2D1::SizeU(width, height), pixels, width * 4,
                                             properties, bitmap.GetAddressOf()))) {
        return nullptr;
    }
    return bitmap;
}

void Canvas::DrawString(std::wstring_view text, const TextStyle& style, const Rect& box,
                        const Color& color, HAlign h, VAlign v) {
    if (text.empty() || color.a <= 0.0f) return;

    auto* format = Format(style);
    format->SetTextAlignment(h == HAlign::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                             : h == HAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                   : DWRITE_TEXT_ALIGNMENT_TRAILING);
    format->SetParagraphAlignment(v == VAlign::Top      ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                  : v == VAlign::Middle ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                        : DWRITE_PARAGRAPH_ALIGNMENT_FAR);

    if (style.letterSpacing == 0.0f) {
        // Fast path: no layout object, which matters because most of the text
        // on screen is a number that changes every frame.
        renderer_->Dc()->DrawText(text.data(), static_cast<UINT32>(text.size()), format,
                                  ToD2D(box), Solid(color), D2D1_DRAW_TEXT_OPTIONS_NONE,
                                  DWRITE_MEASURING_MODE_NATURAL);
        return;
    }

    auto* layout = Layout(text, style, box.Width());
    layout->SetMaxWidth(box.Width());
    layout->SetMaxHeight(box.Height());
    layout->SetTextAlignment(format->GetTextAlignment());
    layout->SetParagraphAlignment(format->GetParagraphAlignment());
    renderer_->Dc()->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout, Solid(color),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
}

Size Canvas::MeasureString(std::wstring_view text, const TextStyle& style, float maxWidth) {
    if (text.empty()) return Size{};

    auto* layout = Layout(text, style, maxWidth);
    layout->SetMaxWidth(maxWidth);
    layout->SetMaxHeight(100000.0f);

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return Size{};
    return Size{metrics.widthIncludingTrailingWhitespace, metrics.height};
}

void Canvas::PushClip(const Rect& rect) {
    renderer_->Dc()->PushAxisAlignedClip(ToD2D(rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ++clipDepth_;
}

void Canvas::PopClip() {
    if (clipDepth_ <= 0) return;
    renderer_->Dc()->PopAxisAlignedClip();
    --clipDepth_;
}

void Canvas::PushOpacity(float opacity) {
    renderer_->Dc()->PushLayer(
        D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                              std::clamp(opacity, 0.0f, 1.0f)),
        nullptr);
}

void Canvas::PopOpacity() {
    renderer_->Dc()->PopLayer();
}

} // namespace slick
