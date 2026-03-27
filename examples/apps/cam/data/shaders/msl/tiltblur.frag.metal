// tiltblur.frag.metal — Circular defocus / tilt-shift blur
//
// Binding convention (sdlos engine — must match NodeShaderParams in render_tree.hh):
//   vertex  buffer(0)   → RectUniform      (set by ui_rect.vert, 48 bytes)
//   fragment buffer(0)  → NodeShaderParams (32 bytes)
//   fragment texture(0) → source image
//   fragment sampler(0) → bilinear/clamp sampler
//
// NodeShaderParams layout (8 × float32 = 32 bytes):
//   [0] u_width   node width  in physical px
//   [1] u_height  node height in physical px
//   [2] u_focusX  normalised focus point X [0..1]
//   [3] u_focusY  normalised focus point Y [0..1]
//   [4] u_param0  blurScale — max defocus radius in px at the screen edge
//   [5] u_param1  (reserved — near/far falloff)
//   [6] u_param2  (reserved)
//   [7] u_time    seconds (used as per-frame jitter seed)
//
// The blur radius at any pixel grows linearly with distance from (u_focusX,
// u_focusY), reaching u_param0 pixels at the screen edge.  A 16-point
// Poisson disk is randomly rotated per pixel (hash of uv + time) to break
// up visible grid patterns without temporal smoothing.

#include <metal_stdlib>
using namespace metal;

// ---------- uniform struct (must match NodeShaderParams exactly) --------------
struct NodeShaderParams {
    float u_width;
    float u_height;
    float u_focusX;
    float u_focusY;
    float u_param0;   // blurScale
    float u_param1;   // reserved
    float u_param2;   // reserved
    float u_time;
};

// ---------- vertex output (produced by ui_rect.vert.metal) -------------------
struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// ---------- 16-point Poisson disk (unit disk, radius ≈ 1) --------------------
constant float2 POISSON[16] = {
    float2(-0.32621f, -0.40581f), float2(-0.84014f, -0.07358f),
    float2(-0.69591f,  0.45714f), float2(-0.20335f,  0.62072f),
    float2( 0.96234f, -0.19498f), float2( 0.47343f, -0.48003f),
    float2( 0.51946f,  0.76702f), float2( 0.18546f, -0.89312f),
    float2( 0.50743f,  0.06443f), float2( 0.89642f,  0.41246f),
    float2(-0.32194f,  0.93262f), float2(-0.79156f,  0.59771f),
    float2(-0.10125f, -0.33044f), float2( 0.63421f, -0.79326f),
    float2( 0.36220f,  0.09012f), float2(-0.02072f, -0.67683f)
};

// Per-pixel rotation jitter — breaks Poisson disk grid patterns.
static float hash21(float2 p) {
    float3 q = fract(float3(p.xyx) * float3(123.34f, 456.21f, 789.12f));
    q += dot(q, q.yzx + 19.19f);
    return fract((q.x + q.y) * q.z);
}

// ---------- fragment entry (entry point: main0) ------------------------------
fragment float4 main0(VertOut                   in    [[stage_in]],
                      constant NodeShaderParams& p     [[buffer(0)]],
                      texture2d<float>           tex   [[texture(0)]],
                      sampler                    samp  [[sampler(0)]])
{
    float2 uv = in.uv;

    // Distance from focus centre — scale X by aspect ratio so the defocus
    // disc is visually round even on non-square viewports.
    float  aspect  = p.u_width / max(p.u_height, 1.0f);
    float2 delta   = (uv - float2(p.u_focusX, p.u_focusY)) * float2(aspect, 1.0f);
    float  dist    = length(delta);

    // Blur radius grows linearly with distance; capped at 64 px.
    float  radius  = clamp(dist * p.u_param0, 0.0f, 64.0f);

    float4 center = tex.sample(samp, uv);
    if (radius < 0.5f)
        return center;   // inside focal zone — no blur

    // UV step size for one pixel in each axis.
    float2 px = 1.0f / float2(max(p.u_width, 1.0f), max(p.u_height, 1.0f));

    // Rotate the Poisson disk randomly per pixel using the time seed.
    float  angle = hash21(uv + float2(p.u_time * 0.001f)) * 6.28318f;
    float  cosA  = cos(angle), sinA = sin(angle);
    float2x2 rot = float2x2(cosA, -sinA, sinA, cosA);

    float4 accum = center;   // centre sample always included (weight 1.0)
    float  wsum  = 1.0f;

    for (int i = 0; i < 16; ++i) {
        float2 jitter = rot * POISSON[i];
        float2 suv    = uv + jitter * (radius * px);

        // Discard samples outside [0, 1] to prevent border bleed.
        if (any(suv < float2(0.0f)) || any(suv > float2(1.0f))) continue;

        float4 s = tex.sample(samp, suv);

        // Gaussian spatial weight: smooth falloff toward outer ring.
        float  r2 = dot(jitter, jitter);
        float  w  = exp(-r2 * 2.0f);
        accum += s * w;
        wsum  += w;
    }

    float4 result = accum / wsum;
    return float4(result.rgb, center.a);   // preserve original alpha
}
