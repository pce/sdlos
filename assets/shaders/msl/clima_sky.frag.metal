#include <metal_stdlib>
using namespace metal;

struct FragUniform {
    float time;     // animation seconds
    float weather;  // 0–4 (fractional = cross-fade between adjacent states)
    float blend;    // reserved
    float pad;
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// ── Noise / FBM ───────────────────────────────────────────────────────────────
static float hash2(float2 p) {
    return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}
static float vnoise(float2 p) {
    float2 i = floor(p), f = fract(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i),               hash2(i + float2(1,0)), u.x),
               mix(hash2(i + float2(0,1)), hash2(i + float2(1,1)), u.x), u.y);
}
static float fbm(float2 p, int oct) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < oct; ++i) { v += a * vnoise(p); p = p * 2.0 + float2(1.7, 9.2); a *= 0.5; }
    return v;
}

// ── Helpers ──────────────────────────────────────────────────────────────────
static float3 sunGlow(float2 uv, float t) {
    float2 sp  = float2(0.78 + 0.018 * sin(t * 0.05), 0.24);
    float  d   = length(uv - sp);
    return (exp(-d * 7.0) * 1.6 + exp(-d * 1.8) * 0.45) * float3(1.0, 0.96, 0.80);
}
static float rainStreak(float2 uv, float t, float density) {
    float2 q  = fract((uv + float2(-0.18, 0.9) * t * 0.28) * float2(density, density * 0.5));
    return smoothstep(0.96, 1.0, q.y) * smoothstep(0.993, 1.0, q.x);
}
static float vignette(float2 uv) {
    float2 v = (uv - 0.5) * 2.0;
    return 1.0 - 0.28 * dot(v, v);
}

// ── Weather sky functions ─────────────────────────────────────────────────────

// 0 — Sunny: deep blue zenith → warm horizon, wispy clouds, sun glow
static float3 skySunny(float2 uv, float t) {
    float y = uv.y;  // 1=top, 0=bottom
    float3 col = y > 0.5
        ? mix(float3(0.40, 0.65, 0.90), float3(0.08, 0.36, 0.72), (y - 0.5) * 2.0)
        : mix(float3(0.88, 0.80, 0.60), float3(0.40, 0.65, 0.90),  y        * 2.0);
    // wispy clouds
    float cn = fbm(uv * float2(3.6, 2.2) + float2(t * 0.04, 0), 4);
    col = mix(col, float3(1.0, 0.99, 0.97), smoothstep(0.60, 0.78, cn) * y * 0.75);
    // horizon haze
    col = mix(col, float3(0.96, 0.88, 0.68), exp(-y * 5.0) * 0.35);
    // sun
    col += sunGlow(uv, t);
    return col;
}

// 1 — Rainy: dark blue-gray, heavy clouds, rain streaks
static float3 skyRainy(float2 uv, float t) {
    float y = uv.y;
    float3 col = mix(float3(0.10, 0.14, 0.22), float3(0.05, 0.08, 0.14), y);
    float cn = fbm(uv * float2(2.6, 1.9) + float2(t * 0.07, t * 0.02), 5);
    col = mix(col, float3(0.19, 0.23, 0.32), smoothstep(0.38, 0.72, cn) * 0.88);
    col += float3(0.45, 0.55, 0.70) * rainStreak(uv, t, 42.0);
    return col;
}

// 2 — Snowy: pale blue-white, soft cloud diffusion, snow particles
static float3 skySnowy(float2 uv, float t) {
    float y = uv.y;
    float3 col = mix(float3(0.88, 0.93, 0.98), float3(0.55, 0.68, 0.82), y);
    float cn = fbm(uv * float2(2.0, 1.5) + float2(t * 0.02, 0), 4);
    col = mix(col, float3(0.97, 0.98, 1.00), smoothstep(0.42, 0.70, cn) * 0.55);
    // snow particles
    float2 sc   = floor(uv * float2(62.0, 85.0) + float2(t * 0.6, t * 0.9));
    float  snow = step(0.974, hash2(sc)) * step(0.5, uv.y); // only upper half
    col += float3(1.0) * snow * 0.9;
    return col;
}

// 3 — Hail: near-black green-tinted storm, fast turbulent clouds, heavy streaks, lightning
static float3 skyHail(float2 uv, float t) {
    float y = uv.y;
    float3 col = mix(float3(0.06, 0.09, 0.06), float3(0.02, 0.04, 0.02), y);
    float cn = fbm(uv * float2(3.2, 2.6) + float2(t * 0.14, t * 0.06), 6);
    col = mix(col, float3(0.09, 0.14, 0.09), smoothstep(0.30, 0.76, cn) * 0.92);
    col += float3(0.28, 0.38, 0.28) * rainStreak(uv, t * 1.6, 56.0);
    // rare lightning flash
    float flash = step(0.998, hash2(float2(floor(t * 0.25), 7.3)));
    col += float3(0.7, 0.85, 1.0) * flash * (1.0 - y) * 0.55;
    return col;
}

// 4 — Cloudy: mid gray-blue, dense layered overcast
static float3 skyCloudy(float2 uv, float t) {
    float y = uv.y;
    float3 col = mix(float3(0.52, 0.58, 0.66), float3(0.32, 0.40, 0.51), y);
    float cn1 = fbm(uv * float2(2.3, 1.7) + float2(t * 0.03, 0), 5);
    float cn2 = fbm(uv * float2(3.6, 2.6) + float2(-t * 0.02, t * 0.01), 4);
    float cloud = smoothstep(0.36, 0.65, mix(cn1, cn2, 0.45));
    col = mix(col, float3(0.76, 0.79, 0.84), cloud * 0.72);
    return col;
}

// ── Main ─────────────────────────────────────────────────────────────────────
fragment float4 main0(VertOut in [[stage_in]], constant FragUniform& u [[buffer(0)]]) {
    float2 uv = in.uv;
    float  t  = u.time;
    float  w  = clamp(u.weather, 0.0, 4.0);
    int    wi = int(floor(w));
    float  wt = fract(w);

    // Evaluate current and next state (avoids array + runtime-index path)
    float3 colA, colB;
    if      (wi == 0) { colA = skySunny(uv, t); colB = skyRainy(uv, t); }
    else if (wi == 1) { colA = skyRainy(uv, t); colB = skySnowy(uv, t); }
    else if (wi == 2) { colA = skySnowy(uv, t); colB = skyHail (uv, t); }
    else if (wi == 3) { colA = skyHail (uv, t); colB = skyCloudy(uv, t); }
    else              { colA = skyCloudy(uv, t); colB = colA; }

    float3 color = mix(colA, colB, smoothstep(0.0, 1.0, wt));

    // Vignette + filmic tone-map (Reinhard variant, soft shoulder)
    color *= vignette(uv);
    color  = color / (color + 0.55) * 1.55;

    return float4(saturate(color), 1.0);
}
