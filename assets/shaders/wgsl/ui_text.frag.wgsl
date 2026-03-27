// ui_text.frag.wgsl — WebGPU fragment shader for SDL_ttf-sourced text textures.
//
// WGSL equivalent of ui_text.frag.metal and the planned ui_text.frag.glsl.
// Targets the SDL3 GPU WebGPU backend (Emscripten / WASM).
//
// The vertex stage (ui_rect.vert.wgsl) generates a pixel-aligned quad whose
// UV coordinates span [0,0] → [1,1] across the entire glyph texture.  This
// shader samples that texture and multiplies by a caller-supplied tint so
// placeholder text and active text can be coloured independently.
//
// Usage
// -----
//   SDL3 GPU accepts WGSL source text directly for the WebGPU backend.
//   No offline compilation is required.
//
//   Push the tint uniform before drawing:
//     SDL_PushGPUFragmentUniformData(cmd, /*slot=*/0, &tint, sizeof(tint));
//
//   Bind the glyph texture and sampler before the draw call:
//     SDL_BindGPUFragmentSamplers(cmd, 0, &texture_sampler_binding, 1);
//
// SDL3 GPU binding conventions — WebGPU / WGSL
// --------------------------------------------
//   Fragment stage
//     Samplers / textures  →  group = 2,  binding = slot
//     Uniform buffers      →  group = 3,  binding = slot
//     Storage buffers      →  group = 4,  binding = slot
//
//   Vertex stage
//     Samplers / textures  →  group = 0,  binding = slot
//     Uniform buffers      →  group = 1,  binding = slot
//     Storage buffers      →  group = 2,  binding = slot
//
// Texture & sampler binding layout
// ---------------------------------
//   WebGPU requires textures and samplers to be declared as separate
//   bindings, unlike Vulkan's combined image sampler.  SDL3 GPU's WebGPU
//   backend splits each sampler slot into:
//
//     group = 2,  binding = slot          ← texture_2d<f32>
//     group = 2,  binding = slot + 16     ← sampler
//
//   For slot 0 (the text atlas):
//     group = 2,  binding = 0   → atlas  (texture_2d<f32>)
//     group = 2,  binding = 16  → samp   (sampler)
//
//   This split is an implementation detail of SDL3 GPU's WebGPU backend.
//   The C++ call site uses the same SDL3 GPU sampler binding API as other
//   backends; the WGSL shader must declare bindings at these indices to match.
//
// Uniform layout
// --------------
//   16 bytes (four f32).  Must match the C++ struct:
//
//     struct TintUniform {
//         float r, g, b, a;   // 16 bytes, 4-byte aligned, no padding
//     };
//
// Source texture format
// ---------------------
//   The texture is produced by TTF_RenderText_Blended → SDL_Surface →
//   transfer buffer → GPU texture (see TextRenderer::ensureTexture).
//   It contains straight (un-premultiplied) RGBA: full glyph colour in RGB,
//   coverage mask in A.
//
// Alpha compositing
// -----------------
//   Output is straight (un-premultiplied) alpha.
//   Pipeline blend state must be:
//     src_color_blendfactor = SRC_ALPHA
//     dst_color_blendfactor = ONE_MINUS_SRC_ALPHA
//     color_blend_op        = ADD
//     src_alpha_blendfactor = ONE
//     dst_alpha_blendfactor = ONE_MINUS_SRC_ALPHA
//     alpha_blend_op        = ADD
//   This matches the Metal and Vulkan pipeline configurations.
//
// Tint semantics
// --------------
//   The sampled texel RGBA is component-wise multiplied by the tint:
//
//     out = texel * vec4f(tint.r, tint.g, tint.b, tint.a)
//
//   Common uses:
//     • White tint  {1,1,1,1}  — render as-is (default glyph colour).
//     • Grey tint   {r,g,b,α}  — dim placeholder text.
//     • Colour tint {r,g,b,1}  — recolour a white-on-transparent atlas.
//     • Alpha tint  {1,1,1,α}  — fade the entire text element.

// ── Texture & sampler (fragment stage, slot 0) ───────────────────────────────

@group(2) @binding(0)  var atlas: texture_2d<f32>;
@group(2) @binding(16) var samp:  sampler;

// ── Uniform buffer (fragment stage, slot 0) ──────────────────────────────────

struct TintUniform {
    r: f32,   // tint red   channel  [0, 1]
    g: f32,   // tint green channel  [0, 1]
    b: f32,   // tint blue  channel  [0, 1]
    a: f32,   // tint alpha / overall opacity multiplier  [0, 1]
}

@group(3) @binding(0) var<uniform> tint: TintUniform;

// ── Fragment entry ────────────────────────────────────────────────────────────

@fragment
fn fs_main(@location(0) v_uv: vec2f) -> @location(0) vec4f {

    // Sample the glyph texture at the interpolated UV.
    // v_uv spans [0,0] (top-left of glyph quad) → [1,1] (bottom-right).
    let texel = textureSample(atlas, samp, v_uv);

    // Multiply glyph colour and coverage by the caller-supplied tint.
    // This lets the caller dim, recolour or fade the text without uploading
    // a new texture.
    return texel * vec4f(tint.r, tint.g, tint.b, tint.a);
}
