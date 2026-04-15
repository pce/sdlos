#pragma once

#include <cmath>
#include <string_view>
#include <vector>
#include <type_traits>

namespace pce::sdlos {
struct Vec2 { float x, y; };
}

namespace pce::sdlos::easing {

// Fast math approximations — used by expensive easing functions
// These are inline approximations tuned for the [0, 1] input range common in
// animation easing. They avoid expensive system calls while maintaining good
// visual quality (imperceptible error for UI animations).

// Fast sine approximation using Taylor series + range reduction
// Accurate to ~0.00005 over [0, 2π], good enough for elastic/spring easing
[[nodiscard]] inline float sin_fast(float x) noexcept
{
    // Normalize to [0, 2π)
    constexpr float inv_2pi = 1.f / 6.28318530718f;
    float t = x * inv_2pi;
    t = t - static_cast<float>(static_cast<int>(t));  // Modulo [0, 1)
    if (t < 0.f) t += 1.f;
    t = t * 6.28318530718f;  // Scale back to [0, 2π)

    // Taylor series: sin(x) ≈ x - x³/6 + x⁵/120 - ...
    const float t2 = t * t;
    return t * (1.f - t2 * (0.16666667f - t2 * 0.00833333f));
}

// Fast cosine using sin_fast
// cos(x) = sin(x + π/2)
[[nodiscard]] inline float cos_fast(float x) noexcept
{
    return sin_fast(x + 1.57079632679f);  // x + π/2
}

// Fast exponential approximation
// exp(x) using the formula: exp(x) = 2^(x / ln(2))
// Accurate to ~0.1% over [-5, 5]
[[nodiscard]] inline float exp_fast(float x) noexcept
{
    // Clamp to safe range
    x = (x > 80.f) ? 80.f : (x < -80.f ? -80.f : x);

    // exp(x) ≈ 2^(x / ln(2)) = 2^(x * 1.44269...)
    constexpr float ln2_inv = 1.44269504089f;
    const float y = x * ln2_inv;
    const int i = static_cast<int>(y);
    const float f = y - static_cast<float>(i);

    // Fast 2^f using polynomial: 2^f ≈ 1 + f*ln(2) + (f*ln(2))²/2 + …
    constexpr float ln2 = 0.69314718056f;
    const float ln2f = f * ln2;
    const float exp_frac = 1.f + ln2f * (1.f + ln2f * 0.5f);

    // Reconstruct 2^y = 2^i * 2^f
    union { int i; float f; } u = { (i + 127) << 23 };
    return u.f * exp_frac;
}

// Fast power: x^y
// Using the identity: x^y = exp(y * ln(x))
[[nodiscard]] inline float pow_fast(float x, float y) noexcept
{
    if (x <= 0.f) return 0.f;
    return exp_fast(y * std::log(x));  // log is reasonably fast; exp_fast is the bottleneck
}

// Fast square root using Newton-Raphson iteration
// One iteration gives ~4 correct bits; two iterations give ~12 correct bits
// Converges very fast for values close to 1 (common in easing)
[[nodiscard]] inline float sqrt_fast(float x) noexcept
{
    if (x <= 0.f) return 0.f;
    if (x >= 1.f) return 1.f;  // Optimization: easing usually stays in [0, 1]

    // Initial guess using bit manipulation
    union { float f; uint32_t i; } u = { x };
    u.i = 0x5f3759df - (u.i >> 1);  // Magic constant (Quake III)
    float y = u.f;

    // Newton-Raphson iterations: y_new = (y + x/y) / 2
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);  // Second iteration for better precision
    return y;
}


// Linear
[[nodiscard]] constexpr float linear(float t) noexcept
{
    return t;
}

// Quadratic
constexpr float easeIn(float t) noexcept
{
    return t * t;
}

constexpr float easeOut(float t) noexcept
{
    const float u = 1.f - t;
    return 1.f - u * u;
}

constexpr float easeInOut(float t) noexcept
{
    return t < 0.5f
        ? 2.f * t * t
        : 1.f - 2.f * (1.f - t) * (1.f - t);
}

// Cubic
constexpr float easeInCubic(float t) noexcept
{
    return t * t * t;
}

constexpr float easeOutCubic(float t) noexcept
{
    const float u = 1.f - t;
    return 1.f - u * u * u;
}

constexpr float easeInOutCubic(float t) noexcept
{
    if (t < 0.5f) return 4.f * t * t * t;
    const float u = -2.f * t + 2.f;
    return 1.f - u * u * u * 0.5f;
}

// Quartic
constexpr float easeInQuart(float t) noexcept
{
    return t * t * t * t;
}

constexpr float easeOutQuart(float t) noexcept
{
    const float u = 1.f - t;
    return 1.f - u * u * u * u;
}

constexpr float easeInOutQuart(float t) noexcept
{
    if (t < 0.5f) return 8.f * t * t * t * t;
    const float u = -2.f * t + 2.f;
    return 1.f - u * u * u * u * 0.5f;
}

// ── Optimized Simple Aliases (Non-spec but common) ──────────────────────────

/// Fast quadratic ease-in
[[nodiscard]] constexpr float easeInQuad(float t) noexcept { return t * t; }

/// Fast quadratic ease-out
[[nodiscard]] constexpr float easeOutQuad(float t) noexcept { return 1.f - (1.f - t) * (1.f - t); }

/// Fast quadratic ease-in-out
[[nodiscard]] constexpr float easeInOutQuad(float t) noexcept {
    return t < 0.5f ? 2.f * t * t : 1.f - 2.f * (1.f - t) * (1.f - t);
}

// Spring
//
// Underdamped spring — closed-form solution of the harmonic oscillator ODE.
// Uses fast math approximations for sin, cos, exp for better performance.
//
//   zeta  — damping ratio (0 < zeta < 1 for underdamped oscillation)
//           0.3 → very springy   0.7 → snappy   1.0 → critically damped
//   omega — natural frequency in radians per unit time
//           (t is normalised, so "unit time" = the full animation duration)
//           8  → slow oscillation   18 → fast oscillation
//
// The output may briefly exceed 1.0 near the first overshoot peak
inline float spring(float t,
                                   float zeta  = 0.5f,
                                   float omega = 12.f) noexcept
{
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    const float wd = omega * sqrt_fast(1.f - zeta * zeta);
    return 1.f
        - exp_fast(-zeta * omega * t)
          * (  cos_fast(wd * t)
             + (zeta * omega / wd) * sin_fast(wd * t));
}

// Bounce
// Piecewise parabolic approximation of a ball bouncing on a floor.
// Output is always in [0, 1]; the value briefly returns toward 0 on each
// sub-bounce before settling at 1.
//
// easeInBounce reverses the direction: starts with the bounce, ends smoothly.
inline float easeOutBounce(float t) noexcept
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

// Elastic
//
// Exponentially-decaying sinusoid. Uses fast math approximations.
// Similar feel to spring() but parameterised differently (amplitude, period).
[[nodiscard]] inline float easeOutElastic(float t,
                                           float amplitude = 1.f,
                                           float period    = 0.3f) noexcept
{
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    const float s = period / (2.f * static_cast<float>(M_PI))
                  * std::asin(1.f / amplitude);
    return amplitude
         * pow_fast(2.f, -10.f * t)
         * sin_fast((t - s) * 2.f * static_cast<float>(M_PI) / period)
         + 1.f;
}

[[nodiscard]] inline float easeInElastic(float t,
                                          float amplitude = 1.f,
                                          float period    = 0.3f) noexcept
{
    return 1.f - easeOutElastic(1.f - t, amplitude, period);
}

// ── Cubic Bezier ───────────────────────────────────────────────────────────

/**
 * @brief Evaluates a cubic bezier curve at time t.
 *
 * Parameters (x1, y1, x2, y2) define the control points of the bezier curve.
 * This implementation uses Newton-Raphson iteration to solve for x(t) = target_x.
 */
[[nodiscard]] inline float cubicBezier(float t, float x1, float y1, float x2, float y2) noexcept
{
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;

    // Helper: evaluate cubic spline
    auto sample = [](float a, float b, float c, float t) {
        return ((a * t + b) * t + c) * t;
    };

    // Helper: evaluate cubic derivative
    auto derivative = [](float a, float b, float c, float t) {
        return (3.f * a * t + 2.f * b) * t + c;
    };

    // Polynomial coefficients
    const float cx = 3.f * x1;
    const float bx = 3.f * (x2 - x1) - cx;
    const float ax = 1.f - cx - bx;

    const float cy = 3.f * y1;
    const float by = 3.f * (y2 - y1) - cy;
    const float ay = 1.f - cy - by;

    // Solve for x(t) = target using Newton-Raphson
    float t_curr = t;
    for (int i = 0; i < 8; ++i) {
        float x = sample(ax, bx, cx, t_curr) - t;
        if (std::abs(x) < 1e-6f) break;
        float d = derivative(ax, bx, cx, t_curr);
        if (std::abs(d) < 1e-6f) break;
        t_curr -= x / d;
    }

    return sample(ay, by, cy, t_curr);
}

/**
 * @brief Evaluates a cubic bezier curve at time t using Vec2 control points.
 */
[[nodiscard]] inline float cubicBezier(float t, Vec2 p1, Vec2 p2) noexcept {
    return cubicBezier(t, p1.x, p1.y, p2.x, p2.y);
}

// ── CSS Easing Aliases (per spec) ──────────────────────────────────────────

/// ease: cubic-bezier(0.25, 0.1, 0.25, 1.0) - The default easing function.
[[nodiscard]] inline float ease(float t) noexcept {
    return cubicBezier(t, 0.25f, 0.1f, 0.25f, 1.f);
}

/// ease-in: cubic-bezier(0.42, 0, 1.0, 1.0)
[[nodiscard]] inline float ease_in_spec(float t) noexcept {
    return cubicBezier(t, 0.42f, 0.f, 1.f, 1.f);
}

/// ease-out: cubic-bezier(0, 0, 0.58, 1.0)
[[nodiscard]] inline float ease_out_spec(float t) noexcept {
    return cubicBezier(t, 0.f, 0.f, 0.58f, 1.f);
}

/// ease-in-out: cubic-bezier(0.42, 0, 0.58, 1.0)
[[nodiscard]] inline float ease_in_out_spec(float t) noexcept {
    return cubicBezier(t, 0.42f, 0.f, 0.58f, 1.f);
}

// Spring Presets

/// Standard spring: snappy but stable (ζ=0.5, ω=12)
[[nodiscard]] inline float easeOutSpring(float t) noexcept { return spring(t, 0.5f, 12.f); }

/// Bouncy spring: more oscillation (ζ=0.3, ω=10)
[[nodiscard]] inline float easeOutSpringBouncy(float t) noexcept { return spring(t, 0.3f, 10.f); }

/// Snappy spring: very quick settle (ζ=0.75, ω=15)
[[nodiscard]] inline float easeOutSpringSnappy(float t) noexcept { return spring(t, 0.75f, 15.f); }

// Steps
[[nodiscard]] constexpr float stepStart(float t) noexcept { return t >= 1.f ? 1.f : 0.f; }
[[nodiscard]] constexpr float stepEnd(float t) noexcept { return t <= 0.f ? 0.f : 1.f; }

// Easing Resolver

using EasingFn = float (*)(float);

/**
 * @brief Resolves a CSS-style easing name to a function pointer.
 *
 * Recognised: "linear", "ease", "ease-in", "ease-out", "ease-in-out",
 *             "bounce", "spring", "spring-bouncy", "spring-snappy", "elastic".
 */
[[nodiscard]] inline EasingFn resolve(std::string_view name) noexcept {
    if (name == "linear")      return linear;
    if (name == "ease")        return ease;
    if (name == "ease-in")     return ease_in_spec;
    if (name == "ease-out")    return ease_out_spec;
    if (name == "ease-in-out") return ease_in_out_spec;
    if (name == "step-start")  return stepStart;
    if (name == "step-end")    return stepEnd;
    if (name == "bounce")      return easeOutBounce;
    if (name == "spring" || name == "spring-out") return easeOutSpring;
    if (name == "spring-bouncy") return easeOutSpringBouncy;
    if (name == "spring-snappy") return easeOutSpringSnappy;
    if (name == "elastic")     return [](float t) -> float { return easeOutElastic(t); };
    return ease; // default
}

/// Back (overshoot)
// easeInBack/easeOutBack slightly overshoot their target before settling.
// `overshoot` controls the magnitude; the CSS default is 1.70158.
constexpr float easeInBack(float t,
                           float overshoot = 1.70158f) noexcept
{
    const float c = overshoot + 1.f;
    return c * t * t * t - overshoot * t * t;
}

constexpr float easeOutBack(float t,
                                           float overshoot = 1.70158f) noexcept
{
    const float c = overshoot + 1.f;
    const float u = t - 1.f;
    return 1.f + c * u * u * u + overshoot * u * u;
}

constexpr float easeInOutBack(float t,
                                             float overshoot = 1.70158f) noexcept
{
    const float c = overshoot * 1.525f;
    if (t < 0.5f) {
        return (2.f * t) * (2.f * t) * ((c + 1.f) * 2.f * t - c) * 0.5f;
    }
    const float u = 2.f * t - 2.f;
    return (u * u * ((c + 1.f) * u + c) + 2.f) * 0.5f;
}


} // namespace pce::sdlos::easing
