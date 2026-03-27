#include <metal_stdlib>
using namespace metal;

// ui_rect.frag.metal — Solid-colour fragment shader for UI rectangles.
//
// Receives a single push uniform at [[buffer(0)]] carrying the RGBA colour.
// The vertex shader (ui_rect.vert.metal) already handled clipping to the
// correct screen-space quad; we just output the colour here.
//
// Alpha is passed through as-is.  The pipeline must be created with
// premultiplied-alpha or straight-alpha blending depending on the caller;
// for the desktop overlay we use straight-alpha (SRC_ALPHA / ONE_MINUS_SRC_ALPHA).

struct ColorUniform {
    float r;
    float g;
    float b;
    float a;
};

// The position interpolant is the only thing the vertex stage emits.
struct VertOut {
    float4 position [[position]];
};

fragment float4 main0(VertOut        in [[stage_in]],
                      constant ColorUniform& c [[buffer(0)]])
{
    return float4(c.r, c.g, c.b, c.a);
}
