#pragma once

#include "../render_tree.hh"   // NodeHandle, RenderTree, Signal

#include <cstdint>
#include <string>

namespace pce::sdlos::widgets {

// Named alias (not a distinct type) keeps assignment-compatible with plain
// NodeHandle — tree manipulation APIs accept widgets without a cast.
using Widget = NodeHandle;

// Linear RGBA in [0, 1].  Factory helpers are constexpr so configs can be
// compile-time constants.
struct Color {
    float r = 1.f;
    float g = 1.f;
    float b = 1.f;
    float a = 1.f;

    [[nodiscard]] static constexpr Color white() noexcept { return {1.f, 1.f, 1.f, 1.f}; }
    [[nodiscard]] static constexpr Color black() noexcept { return {0.f, 0.f, 0.f, 1.f}; }
    [[nodiscard]] static constexpr Color clear() noexcept { return {0.f, 0.f, 0.f, 0.f}; }

    [[nodiscard]] static constexpr Color rgba(float r, float g, float b, float a = 1.f) noexcept
    {
        return {r, g, b, a};
    }

    // Bytes in [0, 255].
    [[nodiscard]] static constexpr Color hex(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept
    {
        return {
            static_cast<float>(r) / 255.f,
            static_cast<float>(g) / 255.f,
            static_cast<float>(b) / 255.f,
            static_cast<float>(a) / 255.f,
        };
    }

    [[nodiscard]] constexpr Color withAlpha(float alpha) const noexcept
    {
        return {r, g, b, alpha};
    }

    // Apple/iOS dark-mode palette (approximate).
    static constexpr Color systemBlue()     noexcept { return hex(0x0a, 0x84, 0xff); }
    static constexpr Color systemGray()     noexcept { return hex(0x8e, 0x8e, 0x93); }
    static constexpr Color systemGray2()    noexcept { return hex(0x63, 0x63, 0x65); }
    static constexpr Color systemGray6()    noexcept { return hex(0x1c, 0x1c, 0x1e); }
    static constexpr Color label()          noexcept { return hex(0xff, 0xff, 0xff); }
    static constexpr Color secondaryLabel() noexcept { return hex(0xeb, 0xeb, 0xf5, 0x99); }
    static constexpr Color fillTertiary()   noexcept { return hex(0x76, 0x76, 0x80, 0x3d); }
    static constexpr Color separator()      noexcept { return hex(0x54, 0x54, 0x58, 0xff); }
};

// CSS box-model padding / SwiftUI EdgeInsets.
struct Edges {
    float top    = 0.f;
    float right  = 0.f;
    float bottom = 0.f;
    float left   = 0.f;

    [[nodiscard]] static constexpr Edges all(float v) noexcept        { return {v, v, v, v}; }
    [[nodiscard]] static constexpr Edges xy(float h, float v) noexcept { return {v, h, v, h}; }
    [[nodiscard]] static constexpr Edges horizontal(float h) noexcept  { return {0.f, h, 0.f, h}; }
    [[nodiscard]] static constexpr Edges vertical(float v) noexcept    { return {v, 0.f, v, 0.f}; }
};

// Lightweight font descriptor; actual resolution happens inside TextRenderer.
struct FontStyle {
    float       size   = 17.f;  // points
    std::string family = "";    // empty → system default
    bool        bold   = false;
    bool        italic = false;

    [[nodiscard]] static FontStyle body()      { return {17.f}; }
    [[nodiscard]] static FontStyle title()     { return {22.f, "", true}; }
    [[nodiscard]] static FontStyle caption()   { return {12.f}; }
    [[nodiscard]] static FontStyle monospace() { return {14.f, "monospace"}; }
};

} // namespace pce::sdlos::widgets
