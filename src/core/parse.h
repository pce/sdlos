/**
 * @file parse.h
 * @brief Portable locale-independent float parsing.
 *
 * std::from_chars for floating-point types is unavailable before
 * iOS 26.0 / macOS 16.0 (Apple fall-2025 SDK).  Integer overloads
 * have been available since iOS 13.0 / macOS 10.15 and are fine as-is.
 *
 * Usage
 * -----
 *   // Simple: string_view → float (returns fallback on failure)
 *   float v = pce::sdlos::parse_float("3.14");
 *   float v = pce::sdlos::parse_float(sv, 1.0f);   // fallback = 1.0f
 *
 *   // Pointer-pair form (matches std::from_chars, also returns advance ptr)
 *   auto [ptr, ok] = pce::sdlos::parse_float(first, last, value);
 *   if (!ok) { ... }
 */
#pragma once

#include <cstdlib>    // strtof
#include <cstring>    // memcpy
#include <string_view>

// ── Availability detection ────────────────────────────────────────────────────
// Float overloads of std::from_chars were added to Apple libc++ in the
// fall-2025 SDK (iOS 26 / macOS 16 / tvOS 26).  On all other platforms
// (Linux, Windows, WASM) they have been available since C++17.
#if defined(__APPLE__)
#  include <Availability.h>
#  if  (defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && \
        __IPHONE_OS_VERSION_MIN_REQUIRED  >= 260000) || \
       (defined(__MAC_OS_X_VERSION_MIN_REQUIRED)   && \
        __MAC_OS_X_VERSION_MIN_REQUIRED   >= 160000) || \
       (defined(__TV_OS_VERSION_MIN_REQUIRED)       && \
        __TV_OS_VERSION_MIN_REQUIRED      >= 260000)
#    define SDLOS_HAS_FLOAT_FROM_CHARS 1
#  endif
#else
#  define SDLOS_HAS_FLOAT_FROM_CHARS 1
#endif

#if defined(SDLOS_HAS_FLOAT_FROM_CHARS)
#  include <charconv>
#  include <system_error>   // std::errc
#endif

// ─────────────────────────────────────────────────────────────────────────────

namespace pce::sdlos {

/// Result returned by the pointer-pair overload — mirrors std::from_chars_result.
struct ParseFloatResult {
    const char *ptr;  ///< One past the last parsed character (or `first` on failure)
    bool        ok;   ///< True when at least one digit was consumed
};

// ── Pointer-pair overload ─────────────────────────────────────────────────────
// Parse a float from the range [first, last).
// On success sets `value` and returns {end_of_number, true}.
// On failure leaves `value` unchanged and returns {first, false}.
[[nodiscard]]
inline ParseFloatResult parse_float(
    const char *first,
    const char *last,
    float       &value) noexcept
{
#if defined(SDLOS_HAS_FLOAT_FROM_CHARS)
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return {ptr, ec == std::errc{}};
#else
    // Fallback: copy to null-terminated stack buffer → strtof.
    // strtof is locale-independent for the "C" locale (SDL apps never change it).
    const std::size_t len = static_cast<std::size_t>(last - first);
    char buf[64];
    const std::size_t n = len < sizeof(buf) - 1u ? len : sizeof(buf) - 1u;
    std::memcpy(buf, first, n);
    buf[n] = '\0';
    char *end = nullptr;
    const float result = std::strtof(buf, &end);
    if (end == buf) return {first, false};   // no digits consumed
    value = result;
    return {first + static_cast<std::size_t>(end - buf), true};
#endif
}

// ── string_view convenience overload ─────────────────────────────────────────
// Returns `fallback` when the input is empty or unparseable.
[[nodiscard]]
inline float parse_float(std::string_view s, float fallback = 0.f) noexcept {
    if (s.empty()) return fallback;
    float v = fallback;
    (void)parse_float(s.data(), s.data() + s.size(), v);
    return v;
}

} // namespace pce::sdlos
