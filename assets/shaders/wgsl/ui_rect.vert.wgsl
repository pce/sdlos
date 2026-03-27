// ui_rect.vert.wgsl — WebGPU vertex shader for UI rectangles.
//
// WGSL mirror of ui_rect.vert.glsl (#version 450) and ui_rect.vert.metal.
// Targets the SDL3 GPU WebGPU backend (Emscripten / WASM).
//
// No vertex buffer: all six vertices of the quad are synthesised from
// @builtin(vertex_index) (0..5) and the pushed RectUniform alone.
//
// Compilation
// -----------
//   None required.  SDL3 GPU's WebGPU backend accepts WGSL source text
//   directly — no offline compilation step, unlike SPIR-V.
//
// SDL3 GPU binding conventions for WGSL / WebGPU
// -----------------------------------------------
//   These group indices mirror the Vulkan SPIR-V set indices used in the
//   companion .glsl files so the C++ binding code is identical across backends.
//
//   Vertex stage
//     Samplers / textures  →  group = 0,  binding = slot
//     Uniform buffers      →  group = 1,  binding = slot   ← this shader
//     Storage buffers      →  group = 2,  binding = slot
//
//   Fragment stage
//     Samplers / textures  →  group = 2,  binding = slot
//     Uniform buffers      →  group = 3,  binding = slot
//     Storage buffers      →  group = 4,  binding = slot
//
// C++ push path (SDL3 GPU):
//   SDL_PushGPUVertexUniformData(cmd, /*slot=*/0, &u, sizeof(u));
//
// Coordinate system
// -----------------
//   Input  : pixel space, top-left origin  (SDL / screen coords).
//   Output : WebGPU NDC, top-left origin   (y = +1 at top of viewport).
//
//   WebGPU NDC matches Metal — y = +1 at the top, y = -1 at the bottom.
//   This is the OPPOSITE of Vulkan NDC.
//
//   Conversion:
//     ndc.x =  (px / vw) * 2.0 - 1.0
//     ndc.y = -(py / vh) * 2.0 + 1.0   ← flip Y (same sign as Metal shader)
//
// Quad topology — two CCW triangles, 6 vertices, no vertex buffer
// ---------------------------------------------------------------
//   pixel-space corners:
//     A = (x,   y  )   B = (x+w, y  )
//     C = (x+w, y+h)   D = (x,   y+h)
//
//   tri 0 : A B D   →  vid 0, 1, 2
//   tri 1 : B C D   →  vid 3, 4, 5
//
// UV layout
// ---------
//   [0,0] at top-left of the rect, [1,1] at bottom-right.
//   Passed to the fragment stage via @location(0).

// ── Uniform buffer (vertex stage, group 1 binding 0) ─────────────────────────
//
// 32 bytes — eight f32 values.
// Must match the C++ struct RectUniform pushed by
// RenderContext::drawRect() and RenderContext::drawText().
//
// WGSL struct layout rules (for uniform address space):
//   Each f32 occupies 4 bytes with 4-byte alignment.
//   All members are f32 here so no padding is inserted by the runtime.
//   The two _pad members exist to keep the size at exactly 32 bytes,
//   matching the C++ struct layout across all backends.

struct RectUniform {
    x:    f32,   // left edge,          pixels
    y:    f32,   // top edge,           pixels
    w:    f32,   // width,              pixels
    h:    f32,   // height,             pixels
    vw:   f32,   // viewport width,     pixels
    vh:   f32,   // viewport height,    pixels
    pad0: f32,   // reserved
    pad1: f32,   // reserved
}

@group(1) @binding(0) var<uniform> u: RectUniform;

// ── Vertex output ─────────────────────────────────────────────────────────────

struct VertOut {
    @builtin(position) position: vec4f,
    @location(0)       uv:       vec2f,   // [0,0]→[1,1] across the rect
}

// ── Entry point ───────────────────────────────────────────────────────────────

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VertOut {

    // ── Four pixel-space corners ──────────────────────────────────────────
    let A = vec2f(u.x,       u.y      );   // top-left
    let B = vec2f(u.x + u.w, u.y      );   // top-right
    let C = vec2f(u.x + u.w, u.y + u.h);   // bottom-right
    let D = vec2f(u.x,       u.y + u.h);   // bottom-left

    // ── Corresponding UV corners ──────────────────────────────────────────
    let uvA = vec2f(0.0, 0.0);   // top-left
    let uvB = vec2f(1.0, 0.0);   // top-right
    let uvC = vec2f(1.0, 1.0);   // bottom-right
    let uvD = vec2f(0.0, 1.0);   // bottom-left

    // ── Six-vertex flat arrays — index by vertex_index ────────────────────
    //   tri 0 : A B D   →  vid 0, 1, 2
    //   tri 1 : B C D   →  vid 3, 4, 5
    var pos = array<vec2f, 6>(A, B, D,  B, C, D);
    var uvs = array<vec2f, 6>(uvA, uvB, uvD,  uvB, uvC, uvD);

    let p = pos[vid];

    // ── Pixel space → WebGPU NDC ──────────────────────────────────────────
    //   x: [0, vw] → [-1, +1]
    //   y: [0, vh] → [+1, -1]  (flip: screen-top == NDC +1, like Metal)
    var out: VertOut;
    out.uv = uvs[vid];
    out.position = vec4f(
         (p.x / u.vw) * 2.0 - 1.0,
        -(p.y / u.vh) * 2.0 + 1.0,
        0.0,
        1.0,
    );
    return out;
}
