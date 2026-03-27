#version 450

// ui_rect.frag.glsl — Solid-colour rectangle fragment shader.
//
// Phase 1 equivalent of ui_rect.frag.metal.
// Used for flat filled rectangles — backgrounds, separators, overlays.
// No SDF, no rounding, no shadow.  Fast and simple.
//
// Compilation
// -----------
//   glslc ui_rect.frag.glsl -o ui_rect.frag.spv
//
// SDL3 GPU binding conventions (fragment stage)
// ---------------------------------------------
//   Uniform buffers  →  set = 3,  binding = slot
//   Samplers         →  set = 2,  binding = slot
//
// C++ push path:
//   SDL_PushGPUFragmentUniformData(cmd, /*slot=*/0, &fu, sizeof(fu));
//
// Alpha compositing
// -----------------
//   Output is straight (un-premultiplied) alpha.
//   The pipeline blend state must be:
//     src_color = SRC_ALPHA
//     dst_color = ONE_MINUS_SRC_ALPHA
//     src_alpha = ONE
//     dst_alpha = ONE_MINUS_SRC_ALPHA
//   This matches the Metal pipeline created in SDLRenderer::CreateUIPipelines().

// ── Uniform buffer (fragment stage, slot 0) ──────────────────────────────────
//
// 16 bytes — one vec4.
// std140: vec4 is already 16-byte aligned, no padding needed.

layout(set = 3, binding = 0) uniform ColorUniform {
    float r;
    float g;
    float b;
    float a;
} u;

// ── Inputs ───────────────────────────────────────────────────────────────────

layout(location = 0) in vec2 v_uv;   // [0,0]→[1,1], not used here

// ── Output ───────────────────────────────────────────────────────────────────

layout(location = 0) out vec4 out_color;

// ── Entry point ──────────────────────────────────────────────────────────────

void main()
{
    out_color = vec4(u.r, u.g, u.b, u.a);
}
