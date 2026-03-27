#include <metal_stdlib>
using namespace metal;

// ui_rect.vert.metal — Vertex shader for solid-colour and textured UI rectangles.
//
// Design: no vertex buffer.
// All six vertices of the quad are generated purely from `vertex_id` (0..5)
// and a push uniform that describes the rectangle in pixel space.
//
// SDL3 GPU push path:
//   SDL_PushGPUVertexUniformData(cmd, /*slot=*/0, &u, sizeof(u));
//
// Coordinate system
// -----------------
//   Input  : pixel space, top-left origin  (matches SDL / screen coords).
//   Output : Metal NDC, bottom-left origin (y = +1 at top of viewport).
//
//   Conversion:
//     ndc.x =  (px / viewport_w) * 2 - 1
//     ndc.y = -(py / viewport_h) * 2 + 1   ← flip Y
//
// UV coordinates
// --------------
//   UVs span [0,0] (top-left) → [1,1] (bottom-right) across the rect.
//   They are emitted as [[user(locn0)]] so fragment shaders can either
//   ignore them (solid rect) or sample a texture (text / image).
//
// Quad topology (two CCW triangles, 6 vertices)
// ---------------------------------------------
//   pixel-space corners:
//     A = (x,   y  )   B = (x+w, y  )
//     C = (x+w, y+h)   D = (x,   y+h)
//
//   tri 0: A B D   (vid 0,1,2)
//   tri 1: B C D   (vid 3,4,5)

// Must match the C++ struct pushed by RenderContext::drawRect() /
// RenderContext::drawText().
// Total = 32 bytes (eight floats × 4 B each).
struct RectUniform {
    float x;       // left edge,   pixels
    float y;       // top edge,    pixels
    float w;       // width,       pixels
    float h;       // height,      pixels
    float vw;      // viewport width,  pixels
    float vh;      // viewport height, pixels
    float _pad0;
    float _pad1;
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];   // [0,0]→[1,1] across the rect
};

// Helper: pixel → NDC
static inline float2 px_to_ndc(float2 p, float vw, float vh)
{
    return float2(
         (p.x / vw) * 2.0f - 1.0f,
        -(p.y / vh) * 2.0f + 1.0f    // flip Y: screen-top == NDC +1
    );
}

vertex VertOut main0(uint                  vid [[vertex_id]],
                     constant RectUniform&  r  [[buffer(0)]])
{
    // Four pixel-space corners.
    const float2 A = float2(r.x,       r.y      );   // top-left
    const float2 B = float2(r.x + r.w, r.y      );   // top-right
    const float2 C = float2(r.x + r.w, r.y + r.h);   // bottom-right
    const float2 D = float2(r.x,       r.y + r.h);   // bottom-left

    // Corresponding UVs for each corner.
    const float2 uvA = float2(0.0f, 0.0f);   // top-left
    const float2 uvB = float2(1.0f, 0.0f);   // top-right
    const float2 uvC = float2(1.0f, 1.0f);   // bottom-right
    const float2 uvD = float2(0.0f, 1.0f);   // bottom-left

    // Six-vertex flat arrays covering both triangles (CCW winding).
    //   tri 0 : A B D   →  vid 0,1,2
    //   tri 1 : B C D   →  vid 3,4,5
    const float2 verts[6] = { A, B, D,   B, C, D };
    const float2 uvs[6]   = { uvA, uvB, uvD,   uvB, uvC, uvD };

    VertOut out;
    out.position = float4(px_to_ndc(verts[vid], r.vw, r.vh), 0.0f, 1.0f);
    out.uv       = uvs[vid];
    return out;
}
