// contrast.frag.metal — Contrast / brightness / saturation adjustment
//
// Binding convention (sdlos engine — must match NodeShaderParams in render_tree.hh):
//   vertex  buffer(0)   → RectUniform      (set by ui_rect.vert.metal, 48 bytes)
//   fragment buffer(0)  → NodeShaderParams (32 bytes)
//   fragment texture(0) → source image     (webcam frame)
//   fragment sampler(0) → bilinear/clamp sampler
//
// NodeShaderParams layout (8 × float32 = 32 bytes):
//   [0] u_width   node width  in physical px
//   [1] u_height  node height in physical px
//   [2] u_focusX  (unused)
//   [3] u_focusY  (unused)
//   [4] u_param0  contrast multiplier  (1.0 = neutral, 1.5 = high contrast)
//   [5] u_param1  brightness offset   (-0.2 .. +0.2,  0.0 = neutral)
//   [6] u_param2  saturation          ( 1.0 = neutral, 0.0 = greyscale, 2.0 = vivid)
//   [7] u_time    (unused)
//
// Processing order:
//   1. Saturation  — mix between Rec-709 luminance and full colour
//   2. Contrast    — scale around the perceptual midpoint (0.5)
//   3. Brightness  — additive offset
//   4. Clamp to [0, 1] and return, preserving original alpha

#include <metal_stdlib>
using namespace metal;

// ---------- uniform struct (must match NodeShaderParams exactly) -------------
struct NodeShaderParams {
    float u_width;
    float u_height;
    float u_focusX;
    float u_focusY;
    float u_param0;   // contrast multiplier
    float u_param1;   // brightness offset
    float u_param2;   // saturation
    float u_time;     // unused
};

// ---------- vertex output (produced by ui_rect.vert.metal) ------------------
struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// ---------- fragment entry (entry point: main0) -----------------------------
fragment float4 main0(VertOut                   in    [[stage_in]],
                      constant NodeShaderParams& p     [[buffer(0)]],
                      texture2d<float>           tex   [[texture(0)]],
                      sampler                    samp  [[sampler(0)]])
{
    float4 color = tex.sample(samp, in.uv);
    float3 rgb   = color.rgb;

    // ---- 1. Saturation -------------------------------------------------------
    // Rec-709 luminance weights (matches sRGB / HDR display primaries).
    float luma = dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
    // p.u_param2 == 0 → pure greyscale; 1 → original; 2 → hyper-saturated.
    rgb = mix(float3(luma), rgb, p.u_param2);

    // ---- 2. Contrast ---------------------------------------------------------
    // Scale each channel around the perceptual midpoint (0.5).
    // p.u_param0 == 1 → identity; > 1 → more contrast; < 1 → flatter image.
    rgb = (rgb - 0.5f) * p.u_param0 + 0.5f;

    // ---- 3. Brightness -------------------------------------------------------
    // Simple additive lift / crush.  Typical range: -0.2 .. +0.2.
    rgb += p.u_param1;

    // ---- 4. Clamp and return -------------------------------------------------
    return float4(saturate(rgb), color.a);
}
