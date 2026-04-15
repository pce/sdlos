#pragma once

#include <type_traits>
#include <cmath>

namespace pce::sdlos {

// RGBA color type (float components [0, 1])
//
// Used for animating color properties in widgets.
// Example:
//   Animated<RGBAf> background_color;
//   background_color.transition(RGBAf{1.f, 0.f, 0.f, 1.f}, 500.f, easing::easeOut);
struct RGBAf {
    float r = 0.f;  ///< Red channel [0, 1]
    float g = 0.f;  ///< Green channel [0, 1]
    float b = 0.f;  ///< Blue channel [0, 1]
    float a = 1.f;  ///< Alpha channel [0, 1]

    /// Component-wise equality
    bool operator==(const RGBAf &other) const noexcept {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }

    /// Component-wise addition
    RGBAf operator+(const RGBAf &other) const noexcept {
        return {r + other.r, g + other.g, b + other.b, a + other.a};
    }

    /// Component-wise subtraction
    RGBAf operator-(const RGBAf &other) const noexcept {
        return {r - other.r, g - other.g, b - other.b, a - other.a};
    }

    /// Component-wise multiplication (useful for lerp)
    RGBAf operator*(const RGBAf &other) const noexcept {
        return {r * other.r, g * other.g, b * other.b, a * other.a};
    }

    /// Scalar multiplication
    RGBAf operator*(float scalar) const noexcept {
        return {r * scalar, g * scalar, b * scalar, a * scalar};
    }
};

}  // namespace pce::sdlos

