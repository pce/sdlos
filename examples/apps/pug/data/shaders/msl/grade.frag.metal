#include <metal_stdlib>
using namespace metal;

// grade.frag.metal — colour grading: exposure / gamma / saturation.
// Reads vignette_buffer; writes to the swapchain (final output).
//
// Bucket-C uniform block — cbuffer fields in ALPHABETICAL ORDER (slots 0,1,2):
//   exposure    [0]  EV offset      (1.0 = neutral)
//   gamma       [1]  display gamma  (2.2 = sRGB)
//   saturation  [2]  colour saturation (1.0 = neutral)

struct GradeParams {
    float exposure;    // slot 0 — alphabetical
    float gamma;       // slot 1
    float saturation;  // slot 2
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

fragment float4 main0(
    VertOut               in   [[stage_in]],
    constant GradeParams& p    [[buffer(0)]],
    texture2d<float>      tex  [[texture(0)]],
    sampler               smp  [[sampler(0)]])
{
    float4      hdr = tex.sample(smp, in.uv);
    float3      col = hdr.rgb;

    // Exposure: multiply by the EV offset.
    col *= max(p.exposure, 0.0f);

    // Saturation: lerp between luminance and full colour.
    // Rec. 709 luminance weights.
    const float lum = dot(col, float3(0.2126f, 0.7152f, 0.0722f));
    col = mix(float3(lum), col, max(p.saturation, 0.0f));

    // Gamma correction: linearise HDR → display-referred.
    col = pow(max(col, 0.0f), float3(1.0f / max(p.gamma, 0.001f)));

    return float4(col, hdr.a);
}
