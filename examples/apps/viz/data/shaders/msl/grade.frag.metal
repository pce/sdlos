#include <metal_stdlib>
using namespace metal;

// =============================================================================
// grade.frag.metal  —  color grading post-process pass
//
// FrameGraph pass: #grade
// Reads from:      bg_color  (rgba16f intermediate, sampler slot 0)
// Writes to:       swapchain
//
// Bucket-C uniforms (alphabetical slot order — must match pipeline.pug attrs):
//   slot 0  exposure    — linear exposure multiplier  (default 1.0)
//   slot 1  gamma       — display gamma               (default 2.2)
//   slot 2  saturation  — colour saturation scale     (default 1.05)
//
// The alphabetical ordering is enforced by FrameGraph::compile() which sorts
// the PassDesc::params StyleMap keys before assigning slots.  The shader's
// cbuffer must declare fields in the same order.
//
// Tone mapping:  Reinhard (simple, perceptually pleasant for FBM scenes).
// Colour space:  linear HDR in → gamma-corrected LDR out.
// =============================================================================

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// Bucket-C params — packed floats, alphabetical key order.
// exposure=0, gamma=1, saturation=2
struct GradeParams {
    float exposure;    // slot 0
    float gamma;       // slot 1
    float saturation;  // slot 2
    float _pad;        // 16-byte alignment
};

// ── Helpers ───────────────────────────────────────────────────────────────────

// Reinhard tone mapping — maps HDR [0,∞) to LDR [0,1).
inline float3 reinhard(float3 x)
{
    return x / (x + float3(1.0f));
}

// Luminance (BT.709 coefficients).
inline float luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Saturation adjustment: lerp between greyscale and colour.
inline float3 adjust_saturation(float3 c, float sat)
{
    float lum = luminance(c);
    return mix(float3(lum), c, sat);
}

// ── Fragment entry ────────────────────────────────────────────────────────────

fragment float4 main0(
    VertOut              in   [[stage_in]],
    constant GradeParams& p   [[buffer(0)]],
    texture2d<float>     tex  [[texture(0)]],
    sampler              smp  [[sampler(0)]])
{
    // Sample the HDR intermediate buffer.
    float3 hdr = tex.sample(smp, in.uv).rgb;

    // 1. Exposure — scale linear HDR before tone mapping.
    //    Clamp to a sane range to avoid NaN / Inf from extreme values.
    float exp_val = clamp(p.exposure, 0.01f, 16.0f);
    hdr *= exp_val;

    // 2. Reinhard tone mapping — HDR → LDR [0, 1].
    float3 ldr = reinhard(hdr);

    // 3. Saturation adjustment (applied in linear space before gamma).
    float sat = clamp(p.saturation, 0.0f, 4.0f);
    ldr = adjust_saturation(ldr, sat);
    ldr = clamp(ldr, 0.0f, 1.0f);

    // 4. Gamma correction — linear → display gamma.
    //    Standard sRGB uses gamma ≈ 2.2.  Clamp to [0.5, 4.0] to avoid
    //    divide-by-zero or inversion from pathological CSS values.
    float gam = clamp(p.gamma, 0.5f, 4.0f);
    ldr = pow(ldr, float3(1.0f / gam));

    return float4(ldr, 1.0f);
}
