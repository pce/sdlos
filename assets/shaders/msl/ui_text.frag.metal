#include <metal_stdlib>
using namespace metal;

// ui_text.frag.metal — Fragment shader for SDL_ttf-sourced text textures.
//
// The vertex stage (ui_rect.vert.metal) generates a pixel-aligned quad whose
// UV coordinates span [0,0]→[1,1] across the entire texture.  This shader
// samples that texture and multiplies its alpha by a tint colour so the caller
// can colour placeholder text differently from active input text.
//
// Binding layout (must match SDL3 GPU pipeline create info):
//   fragment sampler  slot 0 → text atlas / pre-rendered glyph surface
//   fragment uniform  slot 0 → TintUniform  (rgba tint, 16 bytes)
//
// SDL3 GPU push path:
//   SDL_PushGPUFragmentUniformData(cmd, /*slot=*/0, &tint, sizeof(tint));
//
// Alpha compositing
// -----------------
// The texture produced by TTF_RenderText_Blended contains straight (un-pre-
// multiplied) alpha in the A channel and the glyph colour in RGB.  We
// multiply the sampled RGBA by the tint so the caller can:
//   • dim placeholder text  (tint.a < 1)
//   • recolour it           (tint.rgb ≠ white)
//   • use it as-is          (tint = {1,1,1,1})
//
// The blending state on the pipeline is expected to be:
//   src = SRC_ALPHA,  dst = ONE_MINUS_SRC_ALPHA  (straight alpha over)

// ── Uniforms ────────────────────────────────────────────────────────────────

// 16-byte tint pushed via SDL_PushGPUFragmentUniformData.
struct TintUniform {
    float r;
    float g;
    float b;
    float a;   // overall opacity multiplier
};

// ── Vertex stage output (must match ui_rect.vert.metal) ─────────────────────

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// ── Fragment entry ───────────────────────────────────────────────────────────

fragment float4 main0(VertOut               in      [[stage_in]],
                      texture2d<float>      atlas   [[texture(0)]],
                      sampler               samp    [[sampler(0)]],
                      constant TintUniform& tint    [[buffer(0)]])
{
    float4 texel = atlas.sample(samp, in.uv);

    // Multiply glyph colour + alpha by the caller-supplied tint.
    return texel * float4(tint.r, tint.g, tint.b, tint.a);
}
