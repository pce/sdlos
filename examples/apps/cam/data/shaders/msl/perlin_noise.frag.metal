// perlin_noise.frag.metal — UV-space warp using 4-octave FBM gradient noise
//
// Binding convention (sdlos engine):
//   vertex  buffer(0)   → RectUniform      (48 bytes)
//   fragment buffer(0)  → NodeShaderParams (32 bytes)
//   fragment texture(0) → video/image source
//   fragment sampler(0) → bilinear/clamp sampler
//
// NodeShaderParams:
//   u_param0  displacement scale in pixels   (default 30)
//   u_param1  noise spatial frequency        (default 0.02 → maps to ~1.0 cell scale)
//   u_param2  animation speed multiplier     (default 4.0)
//   u_time    seconds

#include <metal_stdlib>
using namespace metal;

struct NodeShaderParams {
    float u_width;
    float u_height;
    float u_focusX;
    float u_focusY;
    float u_param0;
    float u_param1;
    float u_param2;
    float u_time;
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// ---- gradient noise ---------------------------------------------------------

// Per-cell pseudo-random gradient direction
static float2 grad2(float2 i)
{
    // Hash to angle, return unit vector
    float h = fract(sin(dot(i, float2(127.1f, 311.7f))) * 43758.5453f) * 6.28318f;
    return float2(cos(h), sin(h));
}

// Gradient noise — C2 continuous via quintic smoothstep
static float gnoise(float2 p)
{
    float2 i = floor(p);
    float2 f = fract(p);
    // Quintic ease curve — zero first and second derivatives at cell edges
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);

    float va = dot(grad2(i + float2(0.f, 0.f)), f - float2(0.f, 0.f));
    float vb = dot(grad2(i + float2(1.f, 0.f)), f - float2(1.f, 0.f));
    float vc = dot(grad2(i + float2(0.f, 1.f)), f - float2(0.f, 1.f));
    float vd = dot(grad2(i + float2(1.f, 1.f)), f - float2(1.f, 1.f));

    return mix(mix(va, vb, u.x), mix(vc, vd, u.x), u.y);
}

// 4-octave FBM — returns a 2-component displacement vector.
// X and Y channels use different seed offsets so they scroll independently.
static float2 fbm2(float2 p, float t)
{
    float2 acc   = float2(0.f);
    float  amp   = 0.5f;
    float  freq  = 1.0f;

    // Slight rotation between octaves breaks axis-aligned grid artefacts
    // and gives more organic, swirling motion.
    const float2x2 rot = float2x2( 0.8f, -0.6f,
                                   0.6f,  0.8f );

    float2 px = p + float2(t * 0.41f, t * 0.29f);
    float2 py = p + float2(5.2f + t * 0.31f, 1.3f + t * 0.37f);

    for (int i = 0; i < 4; ++i) {
        acc.x += gnoise(px * freq) * amp;
        acc.y += gnoise(py * freq) * amp;
        px     = rot * px;
        py     = rot * py;
        freq  *= 2.0f;
        amp   *= 0.5f;
    }
    return acc;
}

// ---- fragment entry ---------------------------------------------------------

fragment float4 main0(VertOut                   in   [[stage_in]],
                      constant NodeShaderParams& p    [[buffer(0)]],
                      texture2d<float>           tex  [[texture(0)]],
                      sampler                    samp [[sampler(0)]])
{
    float2 uv    = in.uv;
    float  scale = p.u_param0 / max(p.u_width, 1.0f);
    float  freq  = max(p.u_param1, 0.005f) * 50.0f;
    float  speed = max(p.u_param2, 0.1f);
    float  t     = p.u_time * speed * 0.05f;

    // FBM displacement — two independent noise fields for X and Y
    float2 offset = fbm2(uv * freq, t) * scale;

    // Edge-safe clamp so border pixels don't smear
    float2 suv = clamp(uv + offset, 0.0f, 1.0f);

    float3 col = tex.sample(samp, suv).rgb;
    return float4(col, 1.0f);
}
