// ui_rect.frag.wgsl — Solid-colour rectangle fragment shader (WebGPU / WGSL).
//
// WebGPU equivalent of ui_rect.frag.glsl (#version 450) and
// ui_rect.frag.metal.  Used for flat filled rectangles — backgrounds,
// separators, overlays.  No SDF, no rounding, no shadow.
//
// Usage
// -----
//   SDL3 GPU accepts WGSL source directly for its WebGPU backend.
//   No offline compilation step is required (unlike GLSL → SPIR-V).
//
//   Push the colour uniform before drawing:
//     SDL_PushGPUFragmentUniformData(cmd, /*slot=*/0, &color, sizeof(color));
//
// SDL3 GPU binding conventions — WebGPU / WGSL
// --------------------------------------------
//   SDL3 GPU maps its abstract "uniform buffer slots" to WebGPU bind groups
//   using the same numeric scheme as its Vulkan/SPIR-V binding layout:
//
//   Fragment stage
//     Samplers / textures  →  group = 2,  binding = slot
//     Uniform buffers      →  group = 3,  binding = slot   ← this shader
//     Storage buffers      →  group = 4,  binding = slot
//
//   Vertex stage
//     Samplers / textures  →  group = 0,  binding = slot
//     Uniform buffers      →  group = 1,  binding = slot
//     Storage buffers      →  group = 2,  binding = slot
//
// Uniform layout
// --------------
//   16 bytes (one vec4).  Must match the C++ struct pushed by
//   RenderContext::drawRect():
//
//     struct ColorUniform {
//         float r, g, b, a;   // 16 bytes, no padding needed
//     };
//
//   WebGPU uniform buffer alignment rules: the struct is 16 bytes and
//   vec4-aligned — no padding is inserted by the driver.
//
// Alpha compositing
// -----------------
//   Output is straight (un-premultiplied) alpha.
//   The SDL3 GPU pipeline blend state should be:
//     src_color_blendfactor = SRC_ALPHA
//     dst_color_blendfactor = ONE_MINUS_SRC_ALPHA
//     color_blend_op        = ADD
//     src_alpha_blendfactor = ONE
//     dst_alpha_blendfactor = ONE_MINUS_SRC_ALPHA
//     alpha_blend_op        = ADD
//   This matches the Metal and Vulkan pipeline configurations.

// ── Uniform buffer (fragment stage, slot 0) ──────────────────────────────────

struct ColorUniform {
    r: f32,
    g: f32,
    b: f32,
    a: f32,
}

@group(3) @binding(0) var<uniform> u: ColorUniform;

// ── Fragment entry ────────────────────────────────────────────────────────────
//
// v_uv is passed from the vertex stage but unused here — the entire rect
// receives the same flat colour.  It is declared so the interface matches
// ui_rect.vert.wgsl and avoids pipeline layout mismatches.

@fragment
fn fs_main(@location(0) v_uv: vec2f) -> @location(0) vec4f {
    return vec4f(u.r, u.g, u.b, u.a);
}
