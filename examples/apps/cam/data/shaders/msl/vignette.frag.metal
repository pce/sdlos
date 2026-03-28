// vignette.frag.metal — Vignette + per-pixel film grain (no scanline stripes)
//
// Binding convention (sdlos engine):
//   vertex  buffer(0)   → RectUniform      (48 bytes)
//   fragment buffer(0)  → NodeShaderParams (32 bytes)
//   fragment texture(0) → video/image source
//   fragment sampler(0) → bilinear/clamp sampler
//
// NodeShaderParams:
//   u_param0  vignette strength  (default 0.8;  0 = off,  1 = full)
//   u_param1  grain amplitude    (default 0.02; 0 = no grain)
//   u_param2  (reserved)
//   u_time    seconds — seeds per-frame grain animation

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

// Per-pixel white-noise hash — returns [0, 1].
// Input is pixel-space coordinate + a time seed so grain changes every frame.
static float hash21(float2 p)
{
    float3 q = fract(float3(p.xyx) * float3(443.897f, 441.423f, 437.195f));
    q += dot(q, q.yzx + 19.19f);
    return fract((q.x + q.y) * q.z);
}

fragment float4 main0(VertOut                   in   [[stage_in]],
                      constant NodeShaderParams& p    [[buffer(0)]],
                      texture2d<float>           tex  [[texture(0)]],
                      sampler                    samp [[sampler(0)]])
{
    float2 uv  = in.uv;
    float3 col = tex.sample(samp, uv).rgb;

    // ---- per-pixel film grain -------------------------------------------
    // Unique coordinate per pixel (pixel-space), mixed with a time seed so
    // the pattern changes every frame — no visible spatial structure.
    float2 pixCoord = uv * float2(p.u_width, p.u_height);
    float  grain    = hash21(pixCoord + float2(p.u_time * 59.7f, p.u_time * 31.3f));
    grain = (grain - 0.5f) * p.u_param1;   // centre on zero, scale by amplitude
    col  += grain;

    // ---- radial vignette ------------------------------------------------
    // vigCoord maps UV to [-1, 1] on both axes; corners reach ~1.41.
    float2 vigCoord = (uv - 0.5f) * 2.0f;
    float  dist     = length(vigCoord);

    // smoothstep ramp: inner edge = (1 - strength), outer edge fixed at 1.5.
    // At strength 0 the inner edge = 1.0, ramp never fires → no darkening.
    // At strength 1 the inner edge = 0.0, ramp starts at the centre.
    float inner    = 1.0f - saturate(p.u_param0);
    float vignette = 1.0f - smoothstep(inner, 1.5f, dist) * p.u_param0;

    col *= saturate(vignette);

    return float4(saturate(col), 1.0f);
}
