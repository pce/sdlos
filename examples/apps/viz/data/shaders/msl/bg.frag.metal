#include <metal_stdlib>
using namespace metal;

// =============================================================================
// bg.frag.metal  —  animated FBM noise background
//
// FrameGraph pass: #bg
// Writes to:       bg_color  (rgba16f intermediate)
//
// Bucket-C uniforms (alphabetical slot order — must match pipeline.pug attrs):
//   slot 0  scale    — spatial frequency of the noise   (default 1.5)
//   slot 1  speed    — animation rate multiplier         (default 0.4)
//   slot 2  time     — wall-clock seconds, injected by execute() each frame
//
// The alphabetical ordering is enforced by FrameGraph::compile() which sorts
// the PassDesc::params StyleMap keys before assigning slots.  The shader's
// cbuffer must declare fields in the same order.
// =============================================================================

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// Bucket-C params — packed floats, alphabetical key order.
// scale=0, speed=1, time=2
struct BgParams {
    float scale;   // slot 0
    float speed;   // slot 1
    float time;    // slot 2
    float _pad;    // alignment
};

// ── Noise primitives ──────────────────────────────────────────────────────────

float hash21(float2 p)
{
    float3 p3 = fract(float3(p.xyx) * float3(0.1031f, 0.11369f, 0.13787f));
    p3 += dot(p3, p3.yzx + 19.19f);
    return fract((p3.x + p3.y) * p3.z);
}

float vnoise(float2 p)
{
    float2 i = floor(p);
    float2 f = fract(p);
    float2 u = f * f * (3.0f - 2.0f * f);   // smoothstep

    float a = hash21(i + float2(0.0f, 0.0f));
    float b = hash21(i + float2(1.0f, 0.0f));
    float c = hash21(i + float2(0.0f, 1.0f));
    float d = hash21(i + float2(1.0f, 1.0f));

    return mix(mix(a, b, u.x),
               mix(c, d, u.x), u.y);
}

// 5-octave fBm — returns [0, 1]
float fbm(float2 p)
{
    float v = 0.0f;
    float a = 0.5f;
    float2 shift = float2(100.0f);
    float2x2 rot = float2x2(
        cos(0.5f), sin(0.5f),
       -sin(0.5f), cos(0.5f));

    for (int i = 0; i < 5; ++i) {
        v += a * vnoise(p);
        p  = rot * p * 2.0f + shift;
        a *= 0.5f;
    }
    return v;
}

// ── Fragment entry ────────────────────────────────────────────────────────────

fragment float4 main0(
    VertOut              in  [[stage_in]],
    constant BgParams&   p   [[buffer(0)]])
{
    float2 uv = in.uv;

    // Animated domain warp: two layers of fBm, one offset by time.
    float t  = p.time * p.speed;
    float sc = max(p.scale, 0.1f);

    float2 q = float2(
        fbm(uv * sc + float2(0.0f,  0.0f) + t * 0.3f),
        fbm(uv * sc + float2(5.2f,  1.3f) + t * 0.2f)
    );

    float2 r = float2(
        fbm(uv * sc + 4.0f * q + float2(1.7f, 9.2f) + t * 0.15f),
        fbm(uv * sc + 4.0f * q + float2(8.3f, 2.8f) + t * 0.10f)
    );

    float n = fbm(uv * sc + 4.0f * r + t * 0.05f);

    // Colour ramp: deep blue-purple → amber-gold → soft white
    float3 col = mix(
        mix(float3(0.08f, 0.12f, 0.35f),   // deep blue
            float3(0.55f, 0.25f, 0.60f),   // purple
            clamp(n * 2.0f, 0.0f, 1.0f)),
        mix(float3(0.55f, 0.25f, 0.60f),   // purple
            float3(0.92f, 0.78f, 0.50f),   // amber
            clamp(n * 2.0f - 1.0f, 0.0f, 1.0f)),
        smoothstep(0.3f, 0.7f, n));

    // Subtle vignette — darken corners.
    float2 vig_uv = uv * 2.0f - 1.0f;
    float  vig    = 1.0f - dot(vig_uv * 0.5f, vig_uv * 0.5f);
    col *= pow(max(vig, 0.0f), 0.4f);

    return float4(col, 1.0f);
}
