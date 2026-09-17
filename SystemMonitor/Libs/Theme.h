#pragma once

#include "Libs/Types.h"

namespace slick {

// One text style = one DirectWrite format. Canvas caches them by value, so
// styles are cheap to build inline at the call site.
struct TextStyle {
    const wchar_t* family = L"Segoe UI";
    float size = 13.0f;
    int weight = 400;           // DWRITE_FONT_WEIGHT
    float letterSpacing = 0.0f; // DIPs added between glyphs
    bool italic = false;
    bool wrap = false;
    bool ellipsis = false;  // trim with an ellipsis instead of clipping
};

// A dark, warm, low-contrast palette in the spirit of the Claude Code terminal:
// near-black clay-tinted surfaces, cream text, a single clay accent doing the
// talking, and desaturated companions for the secondary metric series.
struct Theme {
    // Surfaces
    Color windowBase = Color::Hex(0x161614);
    Color chromeBar = Color::Hex(0x1B1A18);
    Color surface = Color::Hex(0x1E1D1B);
    Color surfaceSunken = Color::Hex(0x141311);
    Color gaugeTrack = Color::Hex(0x302D29);
    Color border = Color::Hex(0x2F2D2A);
    Color borderSoft = Color::Hex(0x26241F);
    Color borderStrong = Color::Hex(0x3C3833);

    // Text
    Color textPrimary = Color::Hex(0xF3EFE6);
    Color textSecondary = Color::Hex(0xA8A296);
    Color textMuted = Color::Hex(0x6B655D);
    Color textFaint = Color::Hex(0x4A453F);

    // Accent
    Color accent = Color::Hex(0xD97757);
    Color accentSoft = Color::Hex(0xE79B7C);
    Color accentDeep = Color::Hex(0xA9502F);

    // Metric series
    Color seriesCpu = Color::Hex(0xD97757);
    Color seriesMemory = Color::Hex(0x5FA69C);
    Color seriesGpu = Color::Hex(0x9B8AC4);
    Color seriesDisk = Color::Hex(0xD3A353);
    Color seriesNetDown = Color::Hex(0x6E9FC9);
    Color seriesNetUp = Color::Hex(0xD97757);

    // Status. Headline readouts shift through these as a metric gets into
    // trouble, so a glance at the number is enough.
    Color warn = Color::Hex(0xD3A353);
    Color critical = Color::Hex(0xE05A4E);
    Color closeHover = Color::Hex(0xC42B1C);

    // Chrome interaction
    Color chromeHover = Color::Hex(0xFFFFFF, 0.07f);
    Color chromePressed = Color::Hex(0xFFFFFF, 0.12f);

    // Typography
    const wchar_t* fontDisplay = L"Segoe UI Variable Display";
    const wchar_t* fontText = L"Segoe UI Variable Text";
    const wchar_t* fontMono = L"Cascadia Mono";

    // Layout
    float cornerRadius = 10.0f;
    float pagePadding = 16.0f;
    float cardPadding = 14.0f;
    float titleBarHeight = 44.0f;
    float statusBarHeight = 30.0f;

    // How far a metric panel dims while the window is not focused. Matches the
    // way the shell desaturates inactive window chrome.
    float inactiveFade = 0.55f;

    static const Theme& Dark();
};

} // namespace slick
