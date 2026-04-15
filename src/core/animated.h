#pragma once

#include "easing.h"
#include "rgba.h"

#include <SDL3/SDL.h>  // SDL_GetTicks()

#include <cmath>
#include <type_traits>


namespace pce::sdlos {

template <typename T>
struct Animated;

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
template <typename T, typename U>
inline T lerp(T a, T b, U t) noexcept {
    return a + (b - a) * t;
}

// Specialized lerp for floats to use fma
/**
 * @brief Lerp
 *
 * @param a  Alpha channel component [0, 1]
 * @param b  Blue channel component [0, 1]
 * @param t  Interpolation parameter in [0, 1]
 *
 * @return Computed floating-point value
 */
template <>
inline float lerp(float a, float b, float t) noexcept {
    return std::fma(t, b - a, a);
}

/**
 * @brief Lerp
 *
 * @param a  Alpha channel component [0, 1]
 * @param b  Blue channel component [0, 1]
 * @param t  Interpolation parameter in [0, 1]
 *
 * @return double result
 */
template <>
inline double lerp(double a, double b, double t) noexcept {
    return std::fma(static_cast<double>(t), b - a, a);
}


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
template <>
inline RGBAf lerp(RGBAf a, RGBAf b, float t) noexcept {
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
inline float inv_lerp(float a, float b, float v) noexcept {
    return (v - a) / (b - a);
}

template <typename T>
struct Animated {
    T from            = T{};
    T to              = T{};
    double start_ms   = 0.0;
    float duration_ms = 200.f;

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
        : from(initial)
        , to(initial)
        , start_ms(0.0)
        , duration_ms(0.f) {}

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
    T current(double now_ms) const {
        if (duration_ms <= 0.f || now_ms >= start_ms + static_cast<double>(duration_ms))
            return to;
        if (now_ms <= start_ms)
            return from;

        const float t = static_cast<float>((now_ms - start_ms) / static_cast<double>(duration_ms));

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
    T current() const { return current(static_cast<double>(SDL_GetTicks())); }

    /**
     * @brief Finished
     *
     * @param now_ms  Width in logical pixels
     *
     * @return true on success, false on failure
     */
    bool finished(double now_ms) const noexcept {
        return now_ms >= start_ms + static_cast<double>(duration_ms);
    }

    /**
     * @brief Finished
     *
     * @return true on success, false on failure
     */
    bool finished() const noexcept { return finished(static_cast<double>(SDL_GetTicks())); }

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
    void transition(
        T target,
        float dur_ms         = 200.f,
        float (*ease)(float) = easing::easeInOut,
        double now_ms        = static_cast<double>(SDL_GetTicks())) {
        from        = current(now_ms);  // snapshot where we are right now
        to          = static_cast<T &&>(target);
        start_ms    = now_ms;
        duration_ms = dur_ms;
        ease_fn     = ease;
    }

    /**
     * @brief Sets Convenience: jump immediately to `value` with no animation.
     *
     * @param value  Operand value
     */
    void set(T value) noexcept {
        from = to   = value;
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

}  // namespace pce::sdlos
