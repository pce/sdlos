// cinematic.frag.metal — Film-look default: S-curve + warm grade + grain + vignette
//
// Binding convention (sdlos engine):
//   vertex  buffer(0)   → RectUniform      (48 bytes)
//   fragment buffer(0)  → NodeShaderParams (32 bytes)
//   fragment texture(0) → video/image source
//   fragment sampler(0) → bilinear/clamp sampler
//
// NodeShaderParams:
//   u_param0  overall intensity / mix  (default 1.0;  0 = passthrough)
//   u_param1  film grain amount        (default 0.04; 0 = no grain)
//   u_param2  colour warmth            (default 0.5;  0 = cool, 1 = neutral, 2 = warm)
//   u_time    seconds — drives grain animation

#include <metal_stdlib>
using namespace metal;

struct NodeShaderParams {
    float u_width;
    float u_height;
    float u_focusX;   // unused
    float u_focusY;   // unused
    float u_param0;   // intensity
    float u_param1;   // grain
    float u_param2;   // warmth
    float u_time;
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// ---- helpers ----------------------------------------------------------------

// Scalar hash → [0, 1], cheap but good enough for grain
static float hash11(float2 p, float t)
{
    float3 q = fract(float3(p.xyx) * float3(443.897f, 441.423f, 437.195f) + t);
    q += dot(q, q.yzx + 19.19f);
    return fract((q.x + q.y) * q.z);
}

// Perceptual (Rec-709) luminance
static float luma(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Filmic S-curve — lifts shadows, rolls off highlights; contrast without clipping
static float3 sCurve(float3 c)
{
    // Approximation of the classic photographic S: two smoothstep passes
    // blended so the response is lifted in shadows and compressed in highlights.
    float3 lo = c * c * (3.0f - 2.0f * c);           // smoothstep lift
    float3 hi = 1.0f - (1.0f - c) * (1.0f - c);      // concave roll-off
    // Cross-fade between passes at mid-tone (0.5)
    float3 t  = smoothstep(float3(0.0f), float3(1.0f), c);
    return mix(lo, hi, t);
}

// ---- fragment entry ---------------------------------------------------------

fragment float4 main0(VertOut                   in   [[stage_in]],
                      constant NodeShaderParams& p    [[buffer(0)]],
                      texture2d<float>           tex  [[texture(0)]],
                      sampler                    samp [[sampler(0)]])
{
    float2 uv  = in.uv;
    float3 col = tex.sample(samp, uv).rgb;

    // ---- S-curve tone mapping -------------------------------------------
    // Applied before everything else so grade and grain land on the shaped image.
    float3 graded = sCurve(col);

    // ---- Colour temperature / warmth ------------------------------------
    // warmth = 0   → push toward cool teal/blue
    // warmth = 1   → neutral (original white balance)
    // warmth = 2   → push toward warm amber/orange
    float  warmth = clamp(p.u_param2, 0.0f, 2.0f) - 1.0f;   // [-1, +1]

    // Lift reds + suppress blues for warmth > 0; invert for cool.
    graded.r = saturate(graded.r + warmth * 0.06f);
    graded.g = saturate(graded.g + warmth * 0.02f);
    graded.b = saturate(graded.b - warmth * 0.08f);

    // Slight shadow tint: shadows lean slightly teal (cinematic cross-process).
    float  lum     = luma(graded);
    float  shadow  = 1.0f - smoothstep(0.0f, 0.4f, lum);
    graded.b = saturate(graded.b + shadow * 0.04f);
    graded.r = saturate(graded.r - shadow * 0.02f);

    // ---- Film grain -------------------------------------------------------
    // Two layers: coarse (low-frequency clumps) + fine (pixel-level texture).
    // Animated per-frame via u_time so it never looks frozen.
    float  grainAmt = max(p.u_param1, 0.0f);
    if (grainAmt > 0.0f) {
        // Fine grain — high spatial frequency, small amplitude
        float  fine   = hash11(uv, p.u_time * 0.0173f) - 0.5f;
        // Coarse grain — lower frequency, larger clumps
        float2 coarseUV = floor(uv * float2(p.u_width, p.u_height) * 0.25f)
                          / (float2(p.u_width, p.u_height) * 0.25f);
        float  coarse = hash11(coarseUV, p.u_time * 0.0071f) - 0.5f;

        float  grain  = fine * 0.7f + coarse * 0.3f;

        // Grain is stronger in mid-tones, weaker in shadows and highlights
        // (replicates the response curve of silver-halide film).
        float  midMask = 1.0f - abs(lum * 2.0f - 1.0f);   // peaks at lum = 0.5
        graded += grain * grainAmt * (0.5f + midMask * 0.5f);
    }

    // ---- Vignette ---------------------------------------------------------
    // Soft radial falloff; slightly elliptical (wider than tall) to feel
    // more like a photographic lens.
    float2 vig     = (uv - 0.5f) * float2(1.3f, 1.0f);
    float  vigDist = dot(vig, vig);
    float  vigMask = 1.0f - smoothstep(0.45f, 1.35f, vigDist);
    graded *= vigMask;

    // ---- Blend with source by intensity ----------------------------------
    float intensity = clamp(p.u_param0, 0.0f, 1.0f);
    float3 result   = mix(col, graded, intensity);

    return float4(saturate(result), 1.0f);
}
