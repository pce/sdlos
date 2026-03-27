#include <metal_stdlib>
using namespace metal;

// vignette.frag.metal — radial vignette + subtle edge pulse.
// Reads bg_color; writes to vignette_buffer.
//
// Bucket-C uniform block — cbuffer fields in ALPHABETICAL ORDER (slots 0,1):
//   intensity  [0]  vignette strength  (0 = off, 1 = strong)  default 0.6
//   time       [1]  wall-clock seconds (auto-injected by execute)

struct VigParams {
    float intensity;  // slot 0 — alphabetical
    float time;       // slot 1
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

fragment float4 main0(
    VertOut            in   [[stage_in]],
    constant VigParams& p   [[buffer(0)]],
    texture2d<float>   tex  [[texture(0)]],
    sampler            smp  [[sampler(0)]])
{
    const float4 base = tex.sample(smp, in.uv);

    // Radial vignette: darkens towards the edges.
    const float2 d = in.uv - 0.5f;
    const float  r = dot(d, d) * 2.2f;
    float vig = 1.0f - r * max(p.intensity, 0.0f);

    // Subtle pulsing edge shimmer — only visible at higher intensity.
    vig += 0.006f * p.intensity * sin(p.time * 1.3f);

    return float4(base.rgb * clamp(vig, 0.0f, 1.0f), base.a);
}
