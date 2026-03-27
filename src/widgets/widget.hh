#pragma once

// widget.hh — Core value types for sdlos::widgets.
//
// Namespace : sdlos::widgets
// File      : src/widgets/widget.hh
//
// A "widget" in sdlos is purely a NodeHandle — a reference to a subtree of
// RenderNode entries inside a RenderTree.  Widget factory functions allocate
// those nodes, attach draw/update callbacks, wire Signals, and return the
// root NodeHandle.  There is no Widget base class, no vtable, no heap
// allocation beyond what RenderTree itself manages.
//
// This header defines the small value types that appear in every widget
// config struct (Color, Edges, FontStyle) and the Widget type alias so
// call-sites read naturally:
//
//   Widget box = widgets::inputBox(tree, { .placeholder = "Search…" });
//   tree.appendChild(root, box);

#include "../render_tree.hh"   // pce::sdlos: NodeHandle, RenderTree, Signal

#include <cstdint>
#include <string>

namespace pce::sdlos::widgets {

// ---------------------------------------------------------------------------
// Widget — a NodeHandle that happens to be a widget root.
//
// Using a named alias (not a distinct type) keeps it assignment-compatible
// with plain NodeHandle so tree manipulation APIs still accept widgets
// without a cast.
// ---------------------------------------------------------------------------

using Widget = NodeHandle;

// ---------------------------------------------------------------------------
// Color — linear RGBA in [0, 1].
//
// All factory helpers are constexpr so widget configs can be declared as
// compile-time constants.  The `hex` factory accepts the familiar 0–255 byte
// range from design tools and CSS colour pickers.
// ---------------------------------------------------------------------------

struct Color {
    float r = 1.f;
    float g = 1.f;
    float b = 1.f;
    float a = 1.f;

    // ---- Named constants -------------------------------------------------

    [[nodiscard]] static constexpr Color white()        noexcept { return {1.f, 1.f, 1.f, 1.f}; }
    [[nodiscard]] static constexpr Color black()        noexcept { return {0.f, 0.f, 0.f, 1.f}; }
    [[nodiscard]] static constexpr Color clear()        noexcept { return {0.f, 0.f, 0.f, 0.f}; }

    // ---- Factory helpers -------------------------------------------------

    /// Construct from four floats in [0, 1].
    [[nodiscard]] static constexpr Color rgba(float r, float g, float b, float a = 1.f) noexcept
    {
        return {r, g, b, a};
    }

    /// Construct from four bytes in [0, 255].
    [[nodiscard]] static constexpr Color hex(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept
    {
        return {
            static_cast<float>(r) / 255.f,
            static_cast<float>(g) / 255.f,
            static_cast<float>(b) / 255.f,
            static_cast<float>(a) / 255.f,
        };
    }

    /// Return a copy with a different alpha.
    [[nodiscard]] constexpr Color withAlpha(float alpha) const noexcept
    {
        return {r, g, b, alpha};
    }

    // ---- Apple / iOS design system colours (approximate) ----------------

    static constexpr Color systemBlue()     noexcept { return hex(0x0a, 0x84, 0xff); }
    static constexpr Color systemGray()     noexcept { return hex(0x8e, 0x8e, 0x93); }
    static constexpr Color systemGray2()    noexcept { return hex(0x63, 0x63, 0x65); }
    static constexpr Color systemGray6()    noexcept { return hex(0x1c, 0x1c, 0x1e); }
    static constexpr Color label()          noexcept { return hex(0xff, 0xff, 0xff); }
    static constexpr Color secondaryLabel() noexcept { return hex(0xeb, 0xeb, 0xf5, 0x99); }
    static constexpr Color fillTertiary()   noexcept { return hex(0x76, 0x76, 0x80, 0x3d); }
    static constexpr Color separator()      noexcept { return hex(0x54, 0x54, 0x58, 0xff); }
};

// ---------------------------------------------------------------------------
// Edges — inset / padding distances for top / right / bottom / left sides.
//
// Mirrors CSS box-model padding and SwiftUI's EdgeInsets.
// ---------------------------------------------------------------------------

struct Edges {
    float top    = 0.f;
    float right  = 0.f;
    float bottom = 0.f;
    float left   = 0.f;

    // ---- Factory helpers -------------------------------------------------

    /// Same value on all four sides.
    [[nodiscard]] static constexpr Edges all(float v) noexcept
    {
        return {v, v, v, v};
    }

    /// Horizontal (left+right = h) and vertical (top+bottom = v).
    [[nodiscard]] static constexpr Edges xy(float h, float v) noexcept
    {
        return {v, h, v, h};
    }

    /// Horizontal only (top/bottom = 0).
    [[nodiscard]] static constexpr Edges horizontal(float h) noexcept
    {
        return {0.f, h, 0.f, h};
    }

    /// Vertical only (left/right = 0).
    [[nodiscard]] static constexpr Edges vertical(float v) noexcept
    {
        return {v, 0.f, v, 0.f};
    }
};

// ---------------------------------------------------------------------------
// FontStyle — font selection hint passed to TextRenderer / drawText.
//
// The actual font file resolution happens inside TextRenderer.  This struct
// is a lightweight descriptor so widget configs can express intent without
// depending on a concrete font-loading type.
// ---------------------------------------------------------------------------

struct FontStyle {
    float       size   = 17.f;     // point size
    std::string family = "";       // empty → system default
    bool        bold   = false;
    bool        italic = false;

    // ---- Common presets --------------------------------------------------

    [[nodiscard]] static FontStyle body()       { return {17.f}; }
    [[nodiscard]] static FontStyle title()      { return {22.f, "", true}; }
    [[nodiscard]] static FontStyle caption()    { return {12.f}; }
    [[nodiscard]] static FontStyle monospace()  { return {14.f, "monospace"}; }
};

} // namespace pce::sdlos::widgets
