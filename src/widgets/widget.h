#pragma once

#include "../render_tree.h"   // NodeHandle, RenderTree, Signal

#include <any>
#include <memory>

#include <cstdint>
#include <string>

namespace pce::sdlos::widgets {

template<typename StateT>
struct WidgetView {
    RenderTree& tree;
    NodeHandle  handle;

    /**
     * @brief Valid
     *
     * @return true on success, false on failure
     */
    bool valid() const noexcept { return handle.valid(); }
    /* implicit */ operator NodeHandle() const noexcept { return handle; }

protected:
    /**
     * @brief Returns state
     *
     * @return Pointer to the result, or nullptr on failure
     */
    StateT* getState(this auto& self) noexcept
    {
        RenderNode* n = self.tree.node(self.handle);
        if (!n) [[unlikely]] return nullptr;
        auto* sp = std::any_cast<std::shared_ptr<StateT>>(&n->state);
        return (sp && *sp) ? sp->get() : nullptr;
    }
};


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


    /**
     * @brief Black
     *
     * @return Color result
     */
    static constexpr Color white() noexcept { return {1.f, 1.f, 1.f, 1.f}; }
    /**
     * @brief Black
     *
     * @return Color result
     */
    static constexpr Color black() noexcept { return {0.f, 0.f, 0.f, 1.f}; }
    /**
     * @brief Clears
     *
     * @return Color result
     */
    static constexpr Color clear() noexcept { return {0.f, 0.f, 0.f, 0.f}; }

    /**
     * @brief Rgba
     *
     * @param r  Red channel component [0, 1]
     * @param g  Green channel component [0, 1]
     * @param b  Blue channel component [0, 1]
     * @param a  Alpha channel component [0, 1]
     *
     * @return Color result
     */
    static constexpr Color rgba(float r, float g, float b, float a = 1.f) noexcept
    {
        return {r, g, b, a};
    }


    /**
     * @brief Hex Bytes in [0, 255].
     *
     * @param r  Red channel component [0, 1]
     * @param g  Green channel component [0, 1]
     * @param b  Blue channel component [0, 1]
     * @param a  Alpha channel component [0, 1]
     *
     * @return Color result
     */
    static constexpr Color hex(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept
    {
        return {
            static_cast<float>(r) / 255.f,
            static_cast<float>(g) / 255.f,
            static_cast<float>(b) / 255.f,
            static_cast<float>(a) / 255.f,
        };
    }

    /**
     * @brief With alpha
     *
     * @param alpha  Opacity value [0, 1]
     *
     * @return Color result
     */
    constexpr Color withAlpha(float alpha) const noexcept
    {
        return {r, g, b, alpha};
    }

    /**
     * @brief System blue
     *
     * @return Color result
     */
    static constexpr Color systemBlue()     noexcept { return hex(0x0a, 0x84, 0xff); }
    /**
     * @brief System gray
     *
     * @return Color result
     */
    static constexpr Color systemGray()     noexcept { return hex(0x8e, 0x8e, 0x93); }
    /**
     * @brief System gray2
     *
     * @return Color result
     */
    static constexpr Color systemGray2()    noexcept { return hex(0x63, 0x63, 0x65); }
    /**
     * @brief System gray6
     *
     * @return Color result
     */
    static constexpr Color systemGray6()    noexcept { return hex(0x1c, 0x1c, 0x1e); }
    /**
     * @brief Label
     *
     * @return Color result
     */
    static constexpr Color label()          noexcept { return hex(0xff, 0xff, 0xff); }
    /**
     * @brief Secondary label
     *
     * @return Color result
     */
    static constexpr Color secondaryLabel() noexcept { return hex(0xeb, 0xeb, 0xf5, 0x99); }
    /**
     * @brief Fill tertiary
     *
     * @return Color result
     */
    static constexpr Color fillTertiary()   noexcept { return hex(0x76, 0x76, 0x80, 0x3d); }
    /**
     * @brief Separator
     *
     * @return Color result
     */
    static constexpr Color separator()      noexcept { return hex(0x54, 0x54, 0x58, 0xff); }
};

// CSS box-model padding / SwiftUI inspired EdgeInsets.
struct Edges {
    float top    = 0.f;
    float right  = 0.f;
    float bottom = 0.f;
    float left   = 0.f;


    /**
     * @brief All
     *
     * @param v  32-bit floating-point scalar
     *
     * @return Edges result
     */
     static constexpr Edges all(float v) noexcept        { return {v, v, v, v}; }
    /**
     * @brief Xy
     *
     * @param h  Opaque resource handle
     * @param v  32-bit floating-point scalar
     *
     * @return Edges result
     */
      static constexpr Edges xy(float h, float v) noexcept { return {v, h, v, h}; }
     /**
      * @brief Horizontal
      *
      * @param h  Opaque resource handle
      *
      * @return Edges result
      */
      static constexpr Edges horizontal(float h) noexcept  { return {0.f, h, 0.f, h}; }

     /**
      * @brief Vertical
      *
      * @param v  32-bit floating-point scalar
      *
      * @return Edges result
      */
      static constexpr Edges vertical(float v) noexcept    { return {v, 0.f, v, 0.f}; }
};

// Lightweight font descriptor; actual resolution happens inside TextRenderer.
struct FontStyle {
    float       size   = 17.f;  // points
    std::string family = "";    // empty → system default
    bool        bold   = false;
    bool        italic = false;

    /**
     * @brief Body
     *
     * @return FontStyle result
     */
    static FontStyle body()      { return {17.f}; }
    /**
     * @brief Title
     *
     * @return FontStyle result
     */
    static FontStyle title()     { return {22.f, "", true}; }
    /**
     * @brief Caption
     *
     * @return FontStyle result
     */
    static FontStyle caption()   { return {12.f}; }
    /**
     * @brief Monospace
     *
     * @return FontStyle result
     */
    static FontStyle monospace() { return {14.f, "monospace"}; }
};

} // namespace pce::sdlos::widgets
