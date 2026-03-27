#include <metal_stdlib>
using namespace metal;

// =============================================================================
// fullscreen.vert.metal  —  shared full-screen triangle vertex shader
//
// Used by every FrameGraph pass.  No vertex buffer required — three vertices
// are generated from SV_VertexID (vertex_id) and cover the entire viewport.
//
// Triangle layout (clip space):
//
//   vid=2  (-1, +3)
//    │╲
//    │  ╲   covers [-1,-1] to [+1,+1]
//    │    ╲  with one overdraw triangle
//    │      ╲
//   (-1,-1)──(+3,-1)  vid=1
//   vid=0
//
// UV output:  [0,0] at top-left, [1,1] at bottom-right (Metal convention).
// =============================================================================

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

vertex VertOut main0(uint vid [[vertex_id]])
{
    // Reconstruct clip-space position from vertex index.
    // Three verts, one giant triangle that covers the entire NDC square.
    const float2 pos[3] = {
        float2(-1.0f, -1.0f),   // bottom-left
        float2( 3.0f, -1.0f),   // bottom-right (far past clip)
        float2(-1.0f,  3.0f),   // top-left     (far past clip)
    };

    VertOut out;
    out.position = float4(pos[vid], 0.0f, 1.0f);

    // UV: map NDC [-1,1] → [0,1].
    // Metal's clip-space Y is +1 at top, so we flip Y for a standard
    // top-left origin UV space used by the post-process samplers.
    out.uv = float2(
         pos[vid].x * 0.5f + 0.5f,
        -pos[vid].y * 0.5f + 0.5f
    );

    return out;
}
