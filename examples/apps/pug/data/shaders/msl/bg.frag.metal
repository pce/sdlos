#include <metal_stdlib>
using namespace metal;

// bg.frag.metal — animated FBM noise background.
// No input samplers; generates colour entirely from UV coordinates and time.
//
// Bucket-C uniform block — cbuffer fields in ALPHABETICAL ORDER (slots 0,1,2):
//   scale  [0]   noise frequency / zoom level  (default 1.5)
//   speed  [1]   animation speed multiplier    (default 0.4)
//   time   [2]   wall-clock seconds (auto-injected by CompiledGraph::execute)

struct BgParams {
    float scale;   // slot 0 — alphabetical
    float speed;   // slot 1
    float time;    // slot 2
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// ── Noise helpers ─────────────────────────────────────────────────────────────

static float hash21(float2 p)
{
    p = fract(p * float2(127.1f, 311.7f));
    p += dot(p, p + 19.19f);
    return fract(p.x * p.y);
}

static float vnoise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = fract(p);
    const float2 u = f * f * (3.0f - 2.0f * f);   // smoothstep
    return mix(
        mix(hash21(i),                    hash21(i + float2(1.0f, 0.0f)), u.x),
        mix(hash21(i + float2(0.0f, 1.0f)), hash21(i + float2(1.0f, 1.0f)), u.x),
        u.y);
}

static float fbm(float2 p)
{
    float v = 0.0f, a = 0.5f;
    for (int i = 0; i < 5; ++i) {
        v += a * vnoise(p);
        p  = p * 2.03f + float2(0.31f, 0.73f);
        a *= 0.5f;
    }
    return v;
}

// ── Fragment shader ───────────────────────────────────────────────────────────

fragment float4 main0(
    VertOut            in  [[stage_in]],
    constant BgParams& p   [[buffer(0)]])
{
    const float2 uv = in.uv;
    const float  t  = p.time * (0.25f + p.speed * 0.4f);
    const float2 q  = uv * max(p.scale, 0.01f);

    // Two overlapping FBM layers — offset by time for fluid motion.
    const float n = fbm(q + float2(t * 0.11f,  t * 0.07f))
                  + 0.55f * fbm(q * 2.4f + float2(-t * 0.15f, t * 0.04f));

    // Map noise [0..1.5] → deep-blue/indigo/violet colour band.
    const float3 dark   = float3(0.02f, 0.03f, 0.16f);  // deep navy
    const float3 mid    = float3(0.35f, 0.28f, 0.78f);  // soft indigo
    const float3 bright = float3(0.06f, 0.05f, 0.22f);  // dark violet

    float3 col = mix(mix(dark, mid, saturate(n * 1.2f)),
                     bright, saturate(n * n * 0.8f));

    // Subtle colour shimmer tied to time.
    col += 0.03f * float3(sin(t * 1.1f), sin(t * 0.65f + 1.0f), sin(t * 1.7f));

    return float4(col, 1.0f);
}
