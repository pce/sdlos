#include <metal_stdlib>
using namespace metal;

// Simple fullscreen triangle vertex shader.
// Emits a position and UV (in user location 0) for the fragment shader.
struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

vertex VertOut main0(uint vid [[vertex_id]]) {
    VertOut out;
    float2 pos[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    out.position = float4(pos[vid], 0.0, 1.0);
    // Map from NDC (-1..1) to UV (0..1)
    out.uv = out.position.xy * 0.5 + float2(0.5, 0.5);
    return out;
}
