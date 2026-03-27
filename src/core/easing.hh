#pragma once

// easing.hh — stateless constexpr easing functions.
//
// All functions map a normalised progress value t ∈ [0, 1] to an output
// value that is also nominally in [0, 1], though spring() and bounce() may
// briefly exceed 1 for overshoot / bounce effects.
//
// Usage:
//   #include "core/easing.hh"
//   float t = ...; // 0 → 1
//   float v = pce::sdlos::easing::easeOut(t);
//
// File location: src/core/easing.hh (header-only, no .cxx needed)

#include <cmath>

namespace pce::sdlos::easing {

// ── Linear ────────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr float linear(float t) noexcept
{
    return t;
}

// ── Quadratic ─────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr float easeIn(float t) noexcept
{
    return t * t;
}

[[nodiscard]] constexpr float easeOut(float t) noexcept
{
    const float u = 1.f - t;
    return 1.f - u * u;
}

[[nodiscard]] constexpr float easeInOut(float t) noexcept
{
    return t < 0.5f
        ? 2.f * t * t
        : 1.f - 2.f * (1.f - t) * (1.f - t);
}

// ── Cubic ─────────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr float easeInCubic(float t) noexcept
{
    return t * t * t;
}

[[nodiscard]] constexpr float easeOutCubic(float t) noexcept
{
    const float u = 1.f - t;
    return 1.f - u * u * u;
}

[[nodiscard]] constexpr float easeInOutCubic(float t) noexcept
{
    if (t < 0.5f) return 4.f * t * t * t;
    const float u = -2.f * t + 2.f;
    return 1.f - u * u * u * 0.5f;
}

// ── Quartic ───────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr float easeInQuart(float t) noexcept
{
    return t * t * t * t;
}

[[nodiscard]] constexpr float easeOutQuart(float t) noexcept
{
    const float u = 1.f - t;
    return 1.f - u * u * u * u;
}

[[nodiscard]] constexpr float easeInOutQuart(float t) noexcept
{
    if (t < 0.5f) return 8.f * t * t * t * t;
    const float u = -2.f * t + 2.f;
    return 1.f - u * u * u * u * 0.5f;
}

// ── Back (overshoot) ──────────────────────────────────────────────────────────
//
// easeInBack/easeOutBack slightly overshoot their target before settling.
// `overshoot` controls the magnitude; the CSS default is 1.70158.

[[nodiscard]] constexpr float easeInBack(float t,
                                         float overshoot = 1.70158f) noexcept
{
    const float c = overshoot + 1.f;
    return c * t * t * t - overshoot * t * t;
}

[[nodiscard]] constexpr float easeOutBack(float t,
                                          float overshoot = 1.70158f) noexcept
{
    const float c = overshoot + 1.f;
    const float u = t - 1.f;
    return 1.f + c * u * u * u + overshoot * u * u;
}

[[nodiscard]] constexpr float easeInOutBack(float t,
                                            float overshoot = 1.70158f) noexcept
{
    const float c = overshoot * 1.525f;
    if (t < 0.5f) {
        return (2.f * t) * (2.f * t) * ((c + 1.f) * 2.f * t - c) * 0.5f;
    }
    const float u = 2.f * t - 2.f;
    return (u * u * ((c + 1.f) * u + c) + 2.f) * 0.5f;
}

// ── Spring ────────────────────────────────────────────────────────────────────
//
// Underdamped spring — closed-form solution of the harmonic oscillator ODE.
// No numerical integration; as cheap as easeOut.
//
//   zeta  — damping ratio (0 < zeta < 1 for underdamped oscillation)
//           0.3 → very springy   0.7 → snappy   1.0 → critically damped
//   omega — natural frequency in radians per unit time
//           (t is normalised, so "unit time" = the full animation duration)
//           8  → slow oscillation   18 → fast oscillation
//
// Typical SwiftUI-feel: zeta=0.5, omega=12
// The output may briefly exceed 1.0 near the first overshoot peak.

[[nodiscard]] inline float spring(float t,
                                  float zeta  = 0.5f,
                                  float omega = 12.f) noexcept
{
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    const float wd = omega * std::sqrt(1.f - zeta * zeta);
    return 1.f
        - std::exp(-zeta * omega * t)
          * (  std::cos(wd * t)
             + (zeta * omega / wd) * std::sin(wd * t));
}

// ── Bounce ────────────────────────────────────────────────────────────────────
//
// Piecewise parabolic approximation of a ball bouncing on a floor.
// Output is always in [0, 1]; the value briefly returns toward 0 on each
// sub-bounce before settling at 1.
//
// easeInBounce reverses the direction: starts with the bounce, ends smoothly.

[[nodiscard]] inline float easeOutBounce(float t) noexcept
{
    constexpr float n  = 7.5625f;
    constexpr float d  = 2.75f;
    if (t < 1.f / d) {
        return n * t * t;
    } else if (t < 2.f / d) {
        t -= 1.5f / d;
        return n * t * t + 0.75f;
    } else if (t < 2.5f / d) {
        t -= 2.25f / d;
        return n * t * t + 0.9375f;
    } else {
        t -= 2.625f / d;
        return n * t * t + 0.984375f;
    }
}

[[nodiscard]] inline float easeInBounce(float t) noexcept
{
    return 1.f - easeOutBounce(1.f - t);
}

[[nodiscard]] inline float easeInOutBounce(float t) noexcept
{
    return t < 0.5f
        ? (1.f - easeOutBounce(1.f - 2.f * t)) * 0.5f
        : (1.f + easeOutBounce(2.f * t - 1.f)) * 0.5f;
}

// ── Elastic ───────────────────────────────────────────────────────────────────
//
// Exponentially-decaying sinusoid.  Similar feel to spring() but parameterised
// differently (amplitude, period in normalised time).

[[nodiscard]] inline float easeOutElastic(float t,
                                          float amplitude = 1.f,
                                          float period    = 0.3f) noexcept
{
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    const float s = period / (2.f * static_cast<float>(M_PI))
                  * std::asin(1.f / amplitude);
    return amplitude
         * std::pow(2.f, -10.f * t)
         * std::sin((t - s) * 2.f * static_cast<float>(M_PI) / period)
         + 1.f;
}

[[nodiscard]] inline float easeInElastic(float t,
                                         float amplitude = 1.f,
                                         float period    = 0.3f) noexcept
{
    return 1.f - easeOutElastic(1.f - t, amplitude, period);
}

} // namespace pce::sdlos::easing
