#pragma once

// animated.hh — Animated<T>: a value type that interpolates between two values
// over a fixed duration using a chosen easing function.
//
// Design principles (from architecture.md / roadmap)
// ───────────────────────────────────────────────────
//   • Value type — stored inside widget state (e.g. ButtonState, InputBoxState).
//     Not a global system, not a registry, not heap-allocated on its own.
//   • Pure evaluation — current() has no side effects; safe to call multiple
//     times per frame, from draw() or update().
//   • SDL_GetTicks() is the clock — no threading, no scheduler.
//   • Dirty-marking is the caller's responsibility.  While !finished(), whoever
//     owns the animated node calls tree.markDirty(handle) each frame to keep
//     the draw callback firing.  When finished() the node settles at `to` and
//     goes quiet until something triggers a new transition().
//
// Example — fade-in on reveal
// ───────────────────────────
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
// ───────────
//   float, double — built-in lerp below.
//   Any type for which pce::sdlos::lerp(T, T, float) → T is defined.
//   Extend lerp() for custom colour/vector types as needed.

#include "easing.hh"
#include <SDL3/SDL.h>     // SDL_GetTicks()
#include <type_traits>

namespace pce::sdlos {

// ── lerp ─────────────────────────────────────────────────────────────────────
// Free-function lerp — specialise or overload for custom types.

[[nodiscard]] inline float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

[[nodiscard]] inline double lerp(double a, double b, float t) noexcept
{
    return a + (b - a) * static_cast<double>(t);
}

// ── Animated<T> ───────────────────────────────────────────────────────────────

template<typename T>
struct Animated {

    T       from        = T{};
    T       to          = T{};
    double  start_ms    = 0.0;
    float   duration_ms = 200.f;

    // Easing function pointer — points to one of the functions in easing.hh.
    // Default: easeInOut (quadratic, symmetric).
    float (*ease_fn)(float) = easing::easeInOut;

    // ── Construction ─────────────────────────────────────────────────────────

    // Default: settled at T{} with no pending animation.
    Animated() = default;

    // Construct already settled at a given value (no animation plays).
    explicit Animated(T initial)
        : from(initial), to(initial), start_ms(0.0), duration_ms(0.f)
    {}

    // ── Evaluation (pure — no side effects) ──────────────────────────────────

    // Evaluate the animated value at an explicit timestamp (milliseconds).
    // Returns `from` before start_ms, `to` after start_ms + duration_ms,
    // and the interpolated value in between.
    [[nodiscard]] T current(double now_ms) const
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
    [[nodiscard]] T current() const
    {
        return current(static_cast<double>(SDL_GetTicks()));
    }

    // ── Finished predicate ───────────────────────────────────────────────────

    [[nodiscard]] bool finished(double now_ms) const noexcept
    {
        return now_ms >= start_ms + static_cast<double>(duration_ms);
    }

    [[nodiscard]] bool finished() const noexcept
    {
        return finished(static_cast<double>(SDL_GetTicks()));
    }

    // ── Transition ───────────────────────────────────────────────────────────

    // Start a new transition from the current interpolated value to `target`.
    // Picks up from wherever the animation currently is so there is no visual
    // jump when transition() is called mid-flight.
    //
    //   target     — new destination value
    //   dur_ms     — duration in milliseconds (default 200 ms)
    //   ease       — easing function (default easeInOut)
    //
    // Pass now_ms explicitly if you want a deferred start or need deterministic
    // behaviour in tests.  Normally just call the SDL_GetTicks() overload.
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

    // Convenience: jump immediately to `value` with no animation.
    void set(T value) noexcept
    {
        from = to = value;
        duration_ms = 0.f;
    }
};

// ── Common Animated aliases ───────────────────────────────────────────────────
//
// Add more as needed (e.g. Animated<RGBAf> once a colour lerp is defined).

using AnimatedFloat  = Animated<float>;
using AnimatedDouble = Animated<double>;

} // namespace pce::sdlos
