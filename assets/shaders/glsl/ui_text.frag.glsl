#version 450

// ui_text.frag.glsl — Glyph texture sampling with RGBA tint.
//
// Vulkan / SPIR-V equivalent of ui_text.frag.metal.
// Samples a pre-rendered glyph texture (produced by SDL_ttf + TextRenderer)
// and multiplies it by a caller-supplied tint colour.
//
// Compilation
// -----------
//   glslc ui_text.frag.glsl -o ui_text.frag.spv
//
// SDL3 GPU binding conventions (fragment stage)
// ---------------------------------------------
//   Samplers / textures  →  set = 2,  binding = slot
//   Uniform buffers      →  set = 3,  binding = slot
//
// C++ push paths:
//   SDL_PushGPUFragmentUniformData(cmd, /*slot=*/0, &tint, sizeof(tint));
//   SDL_GPUSamplerBinding sb{ .texture = gt.texture, .sampler = sampler };
//   SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);
//
// Texture format
// --------------
//   The texture is R8G8B8A8_UNORM produced by TextRenderer::ensureTexture().
//   TTF_RenderText_Blended() outputs straight (un-premultiplied) alpha, and
//   the surface is converted to SDL_PIXELFORMAT_RGBA32 before GPU upload, so
//   byte order in memory is R, G, B, A — matching R8G8B8A8_UNORM.
//
// Tint semantics
// --------------
//   out_color = texture(atlas, v_uv) * tint
//
//   Common usage:
//     tint = (1,1,1,1)        — render glyph in its original colour (white)
//     tint = (0,0,0,0.55)     — placeholder text (greyed out, semi-transparent)
//     tint = (r,g,b,1)        — recolour the glyph to (r,g,b)
//
//   The glyph texture from TTF_RenderText_Blended is rendered in pure white
//   by convention (TextRenderer passes SDL_Color{255,255,255,255} to TTF).
//   This means tint.rgb directly controls the rendered text colour.
//
// Alpha compositing
// -----------------
//   Output is straight (un-premultiplied) alpha.
//   Pipeline blend state must be:
//     src_color = SRC_ALPHA,  dst_color = ONE_MINUS_SRC_ALPHA,  op = ADD
//     src_alpha = ONE,        dst_alpha = ONE_MINUS_SRC_ALPHA,  op = ADD
//
// Coordinate system
// -----------------
//   v_uv is [0,0] at top-left → [1,1] at bottom-right of the glyph quad.
//   The vertex shader (ui_rect.vert.glsl) positions the quad at the exact
//   pixel dimensions of the GlyphTexture { width, height } returned by
//   TextRenderer::ensureTexture(), so no UV scaling is needed here.

// ── Sampler (fragment stage, slot 0) ─────────────────────────────────────────
//
// Combined image sampler — the texture and sampler are bound together.
// Matches SDL_GPUSamplerBinding { .texture, .sampler } in C++.
// The sampler is created by TextRenderer::createSampler():
//   min/mag filter: LINEAR   →  smooth glyph scaling
//   address mode:   CLAMP    →  no edge bleed from neighbouring atlas entries

layout(set = 2, binding = 0) uniform sampler2D atlas;

// ── Uniform buffer (fragment stage, slot 0) ───────────────────────────────────
//
// 16 bytes — one vec4.
//
// C++ mirror struct (must match byte-for-byte):
//
//   struct TintUniform {
//       float r, g, b, a;
//   };
//   static_assert(sizeof(TintUniform) == 16);

layout(set = 3, binding = 0) uniform TintUniform {
    float r;
    float g;
    float b;
    float a;   // overall opacity multiplier; multiply into glyph alpha
} tint;

// ── Inputs / outputs ─────────────────────────────────────────────────────────

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

// ── Entry point ──────────────────────────────────────────────────────────────

void main()
{
    // Sample the glyph atlas at the interpolated UV.
    // The texture carries RGBA: R/G/B are the glyph colour (white by default),
    // A is the glyph coverage/opacity (0 = background, 1 = fully covered).
    vec4 texel = texture(atlas, v_uv);

    // Multiply by tint: this lets the caller set glyph colour and opacity
    // independently of the texture content.
    out_color = texel * vec4(tint.r, tint.g, tint.b, tint.a);
}
