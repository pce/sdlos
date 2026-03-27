#version 450

// ui_rect.vert.glsl — SDL3 GPU / Vulkan vertex shader for UI rectangles.
//
// Mirror of ui_rect.vert.metal — same design, GLSL dialect for SPIR-V.
//
// Compilation
// -----------
//   glslc ui_rect.vert.glsl -o ui_rect.vert.spv
//
// SDL3 GPU binding conventions for SPIR-V
// ----------------------------------------
// SDL3 GPU maps its abstract "uniform buffer slots" to fixed Vulkan descriptor
// sets.  Use these set indices exactly or SDL3 GPU will silently misbind:
//
//   Vertex stage
//     Samplers / textures  →  set = 0,  binding = slot
//     Uniform buffers      →  set = 1,  binding = slot   ← this shader
//     Storage buffers      →  set = 2,  binding = slot
//
//   Fragment stage
//     Samplers / textures  →  set = 2,  binding = slot
//     Uniform buffers      →  set = 3,  binding = slot
//     Storage buffers      →  set = 4,  binding = slot
//
// C++ push path (SDL3 GPU):
//   SDL_PushGPUVertexUniformData(cmd, /*slot=*/0, &u, sizeof(u));
//
// Coordinate system
// -----------------
//   Input  : pixel space, top-left origin  (SDL / screen coords).
//   Output : Vulkan NDC, top-left origin   (y = -1 at top of viewport).
//
//   Conversion:
//     ndc.x =  (px / vw) * 2 - 1
//     ndc.y =  (py / vh) * 2 - 1   ← Vulkan y is NOT flipped vs Metal
//
//   NOTE: Metal NDC has y=+1 at the top; Vulkan NDC has y=-1 at the top.
//   The Metal shader uses  -(py/vh)*2 + 1;  this shader uses  (py/vh)*2 - 1.
//   Both produce the same on-screen result because the swapchain presentation
//   already handles the per-backend flip.  SDL3 GPU normalises this; if
//   rects appear upside-down, swap the sign here and re-check the Metal file.
//
// Quad topology — two CCW triangles, 6 vertices, no vertex buffer
// ---------------------------------------------------------------
//   pixel-space corners:
//     A = (x,   y  )   B = (x+w, y  )
//     C = (x+w, y+h)   D = (x,   y+h)
//
//   tri 0 : A B D   →  vid 0,1,2
//   tri 1 : B C D   →  vid 3,4,5
//
// UV layout
// ---------
//   [0,0] at top-left of the rect, [1,1] at bottom-right.
//   Passed to the fragment stage via location 0.
//   The SDF fragment shader converts UV to center-relative pixel coordinates:
//     p_local = (uv - 0.5) * vec2(w, h)

// ── Uniform buffer (vertex stage, slot 0) ───────────────────────────────────
//
// Must be 32 bytes to match the C++ struct RectUniform pushed by
// RenderContext::drawRect() and RenderContext::drawText().
//
// std140 layout rules: each float is 4 bytes, struct is tightly packed here
// because all members are scalar float — no padding inserted by std140.

layout(set = 1, binding = 0) uniform RectUniform {
    float x;       // left edge,          pixels
    float y;       // top edge,           pixels
    float w;       // width,              pixels
    float h;       // height,             pixels
    float vw;      // viewport width,     pixels
    float vh;      // viewport height,    pixels
    float _pad0;   // reserved
    float _pad1;   // reserved
} u;

// ── Outputs ─────────────────────────────────────────────────────────────────

layout(location = 0) out vec2 v_uv;   // [0,0]→[1,1] across the rect

// ── Entry point ─────────────────────────────────────────────────────────────

void main()
{
    // Four pixel-space corners.
    vec2 A = vec2(u.x,       u.y      );   // top-left
    vec2 B = vec2(u.x + u.w, u.y      );   // top-right
    vec2 C = vec2(u.x + u.w, u.y + u.h);   // bottom-right
    vec2 D = vec2(u.x,       u.y + u.h);   // bottom-left

    // Corresponding UV corners.
    vec2 uvA = vec2(0.0, 0.0);
    vec2 uvB = vec2(1.0, 0.0);
    vec2 uvC = vec2(1.0, 1.0);
    vec2 uvD = vec2(0.0, 1.0);

    // Six-vertex flat arrays — index by gl_VertexIndex.
    vec2 pos[6] = vec2[6]( A, B, D,   B, C, D );
    vec2 uvs[6] = vec2[6]( uvA, uvB, uvD,   uvB, uvC, uvD );

    vec2 p = pos[gl_VertexIndex];
    v_uv = uvs[gl_VertexIndex];

    // Pixel space → Vulkan NDC.
    // x: [0, vw] → [-1, +1]
    // y: [0, vh] → [-1, +1]  (Vulkan: -1 at top, +1 at bottom)
    gl_Position = vec4(
        (p.x / u.vw) * 2.0 - 1.0,
        (p.y / u.vh) * 2.0 - 1.0,
        0.0,
        1.0
    );
}
