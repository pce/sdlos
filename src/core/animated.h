#pragma once

// animated.h — Animated<T>: a value type that interpolates between two values
// over a fixed duration using a chosen easing function.
//
// Design principles
//   - Value type — stored inside widget state (e.g. ButtonState, InputBoxState).
//     Not a global system, not a registry, not heap-allocated on its own.
//   - Pure evaluation — current() has no side effects; safe to call multiple
//     times per frame, from draw() or update().
//   - SDL_GetTicks() is the clock — no threading, no scheduler.
//   - Dirty-marking is the caller's responsibility.  While !finished(), whoever
//     owns the animated node calls tree.markDirty(handle) each frame to keep
//     the draw callback firing.  When finished() the node settles at `to` and
//     goes quiet until something triggers a new transition().
//
// Example: fade-in on reveal
//   // Inside widget state struct:
//   Animated<float> opacity;
//
//   // On reveal (e.g. in an onreveal EventBus handler):
//   opacity.transition(1.f, 300.f, easing::easeOut);
//
//   // In update() (keeps the node dirty while animating):
//   if (!state->opacity.finished())
//       tree.markDirty(handle);
//
//   // In draw():
//   const float op = state->opacity.current();
//   ctx.drawRect(ax, ay, w, h, r, g, b, op);
//
// Supported T
//   float, double — built-in lerp below.
//   Any type for which pce::sdlos::lerp(T, T, float) → T is defined.
//   Extend lerp() for custom colour/vector types as needed.

#include "easing.h"
#include <SDL3/SDL.h>     // SDL_GetTicks()
#include <type_traits>

namespace pce::sdlos {



/**
 * @brief Lerp
 *
 * @return T result
 */
/**
 * @brief Lerp
 *
 * @return T result
 */
/**
 * @brief Lerp
 *
 * @return T result
 */
template<typename T>
inline T lerp(T a, T b, T t) noexcept
{
    return std::fma(t, b - a, a);
}


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
    bool operator==(const RGBAf& other) const noexcept {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }

    /// Component-wise addition
    RGBAf operator+(const RGBAf& other) const noexcept {
        return {r + other.r, g + other.g, b + other.b, a + other.a};
    }

    /// Component-wise subtraction
    RGBAf operator-(const RGBAf& other) const noexcept {
        return {r - other.r, g - other.g, b - other.b, a - other.a};
    }

    /// Scalar multiplication
    RGBAf operator*(float scalar) const noexcept {
        return {r * scalar, g * scalar, b * scalar, a * scalar};
    }
};

// Specialized lerp for RGBAf — component-wise interpolation
//
// Example:
//   RGBAf from{1.f, 0.f, 0.f, 1.f};
//   RGBAf to{0.f, 1.f, 0.f, 1.f};
//   RGBAf mid = lerp(from, to, 0.5f);  // {0.5f, 0.5f, 0.f, 1.f}
/**
 * @brief Lerp
 *
 * @param a  Alpha channel component [0, 1]
 * @param b  Blue channel component [0, 1]
 * @param t  Interpolation parameter in [0, 1]
 *
 * @return RGBAf result
 */
/**
 * @brief Lerp
 *
 * @param a  Alpha channel component [0, 1]
 * @param b  Blue channel component [0, 1]
 * @param t  Interpolation parameter in [0, 1]
 *
 * @return RGBAf result
 */
template<>
inline RGBAf lerp(RGBAf a, RGBAf b, float t) noexcept
{
    return a + (b - a) * t;
}



/**
 * @brief Inv lerp
 *
 * @param a  Alpha channel component [0, 1]
 * @param b  Blue channel component [0, 1]
 * @param v  32-bit floating-point scalar
 *
 * @return Computed floating-point value
 */
inline float inv_lerp(float a, float b, float v) noexcept
{
    return (v - a) / (b - a);
}


template<typename T>
struct Animated {

    T       from        = T{};
    T       to          = T{};
    double  start_ms    = 0.0;
    float   duration_ms = 200.f;

    /// Easing function pointer — points to one of the functions in easing.h.
    /// Default: easeInOut (quadratic, symmetric).
    float (*ease_fn)(float) = easing::easeInOut;


    /**
     * @brief Animated<t>  — Default: settled at T{} with no pending animation.
     */
    Animated() = default;

    // Construct already settled at a given value (no animation plays).
    /**
     * @brief Animated<t>
     *
     * @param initial  Iterator position
     */
    explicit Animated(T initial)
        : from(initial), to(initial), start_ms(0.0), duration_ms(0.f)
    {}


    /**
     * @brief Current
     *       Evaluate the animated value at an explicit timestamp (milliseconds).
     *        Returns `from` before start_ms, `to` after start_ms + duration_ms,
     * and the interpolated value in between.
     *
     * @param now_ms  Width in logical pixels
     *
     * @return T result
     */
    T current(double now_ms) const
    {
        if (duration_ms <= 0.f || now_ms >= start_ms + static_cast<double>(duration_ms))
            return to;
        if (now_ms <= start_ms)
            return from;

        const float t = static_cast<float>(
            (now_ms - start_ms) / static_cast<double>(duration_ms));

        // Saturate t to [0, 1] in case of floating-point edge cases.
        const float ts = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);

        return lerp(from, to, ease_fn(ts));
    }

    // Evaluate using SDL_GetTicks() as the clock.
    // Most draw/update callbacks should use this overload.
    /**
     * @brief Current
     *
     * @return T result
     */
    T current() const
    {
        return current(static_cast<double>(SDL_GetTicks()));
    }


    /**
     * @brief Finished
     *
     * @param now_ms  Width in logical pixels
     *
     * @return true on success, false on failure
     */
    bool finished(double now_ms) const noexcept
    {
        return now_ms >= start_ms + static_cast<double>(duration_ms);
    }

    /**
     * @brief Finished
     *
     * @return true on success, false on failure
     */
    bool finished() const noexcept
    {
        return finished(static_cast<double>(SDL_GetTicks()));
    }

    /**
     * @brief Transition
     *
     * @param target  new destination value
     * @param dur_ms  ms to transition from `from` to `target`
     * @param ease    easing function pointer — points to one of the functions in easing.h.
     *                 Default: easeInOut (quadratic, symmetric).
     * @param now_ms  Width in logical pixels
     *
     * @warning Parameter 'ease' is a non-const raw pointer — Raw pointer parameter —
     *          ownership is ambiguous; consider std::span (non-owning view),
     *          std::unique_ptr (transfer), or const T* (borrow)
     */
    void transition(T target,
                    float dur_ms   = 200.f,
                    float (*ease)(float) = easing::easeInOut,
                    double now_ms  = static_cast<double>(SDL_GetTicks()))
    {
        from        = current(now_ms);   // snapshot where we are right now
        to          = static_cast<T&&>(target);
        start_ms    = now_ms;
        duration_ms = dur_ms;
        ease_fn     = ease;
    }


    /**
     * @brief Sets Convenience: jump immediately to `value` with no animation.
     *
     * @param value  Operand value
     */
    void set(T value) noexcept
    {
        from = to = value;
        duration_ms = 0.f;
    }
};

// Common Animated aliases
//
// Convenience type aliases for commonly-animated properties.
// extendable with Animated<Vec2f>, Animated<Vec3f>, etc.

using AnimatedFloat  = Animated<float>;
using AnimatedDouble = Animated<double>;
using AnimatedColor  = Animated<RGBAf>;  ///< Animated RGBA color (e.g. background, text color)

} // namespace pce::sdlos
