#include <metal_stdlib>
using namespace metal;

// Shared full-screen triangle vertex shader used by every FrameGraph pass.
// Generates NDC positions and UV coordinates from the vertex index alone —
// no vertex buffer needed.  SDL_GPU draws 3 vertices per pass.

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

vertex VertOut main0(uint vid [[vertex_id]])
{
    // Full-screen triangle trick: three hard-coded vertices cover the
    // entire NDC clip space without any vertex buffer allocation.
    const float2 pos[3] = {
        float2(-1.0,  1.0),
        float2( 3.0,  1.0),
        float2(-1.0, -3.0),
    };
    // Matching UV coordinates: (0,0) at top-left, (1,1) at bottom-right.
    const float2 uv[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0),
    };
    VertOut out;
    out.position = float4(pos[vid], 0.0, 1.0);
    out.uv       = uv[vid];
    return out;
}
