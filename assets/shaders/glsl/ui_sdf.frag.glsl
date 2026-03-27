#version 450

// ui_sdf.frag.glsl — SDF-based UI element: rounded rectangle, border, drop shadow.
//
// Phase 3 replacement for ui_rect.frag.glsl.
// One fragment shader, one draw call per element — handles fill, border, and
// soft drop shadow simultaneously using a 2D signed distance function.
//
// Compilation
// -----------
//   glslc ui_sdf.frag.glsl -o ui_sdf.frag.spv
//
// SDL3 GPU binding conventions (fragment stage)
// ---------------------------------------------
//   Uniform buffers  →  set = 3,  binding = slot
//
// C++ push path:
//   SDL_PushGPUFragmentUniformData(cmd, /*slot=*/0, &fu, sizeof(fu));
//
// ── Why SDF for UI? ──────────────────────────────────────────────────────────
//
//   A Signed Distance Function returns the signed distance from any point to
//   the nearest surface of a shape.  Negative inside, positive outside, zero
//   exactly on the boundary.
//
//   For a rectangle centred at the origin with half-extents b and corner
//   radius r, the 2D SDF is (Inigo Quilez, 2018):
//
//     float sdRoundBox(vec2 p, vec2 b, float r) {
//         vec2 q = abs(p) - b + r;
//         return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
//     }
//
//   From this single scalar field every UI effect falls out for free:
//
//     filled rect     →  smoothstep(+0.5, -0.5, sdf)
//     border          →  smoothstep(+0.5, -0.5, abs(sdf) - half_border)
//     drop shadow     →  soft_alpha( sdRoundBox(p - offset, b + spread, r) )
//     inner shadow    →  soft_alpha(-sdf)   inside the shape
//     glow / outline  →  exp(-max(sdf, 0.0) / blur)
//     anti-aliasing   →  built in — smoothstep straddles the zero crossing
//
// ── Relationship to Inigo Quilez's 3D SDF library ────────────────────────────
//
//   IQ's library (iquilezles.org/articles/distfunctions) covers 3D primitives
//   for ray-marching: sdBoxFrame, sdPlane, sdCapsule, etc.  Those operate in
//   3D world space and are evaluated per ray, not per fragment of a flat quad.
//
//   For 2D UI we use the *2D* equivalents — same maths, one dimension dropped.
//   sdRoundBox above is the authoritative 2D form.  The 3D sdBoxFrame is NOT
//   used here; a 2D "box frame" (border) is simply abs(sdf) < half_border.
//
// ── Drop shadow is NOT a "rect that grows" ───────────────────────────────────
//
//   A shadow is the same SDF evaluated at a displaced, slightly enlarged query
//   point, fed through a soft falloff function:
//
//     shadow_sdf = sdRoundBox(p - offset,  b + spread,  radius)
//     shadow_alpha = exp( -max(shadow_sdf, 0.0)^2 / blur^2 )
//
//   The "exp(-x²)" term approximates a Gaussian blur analytically — no
//   separate blur pass, no texture lookup, no extra draw call.  The result
//   is indistinguishable from a real Gaussian for typical UI blur radii.
//
//   Key parameters:
//     offset  (ox, oy)  — directional shift in pixels; (4, 4) = bottom-right
//     spread            — expands the shadow beyond the rect edges in pixels
//     blur              — controls the softness (sigma of the Gaussian approx)
//
//   The vertex quad MUST be inflated by (spread + blur * 2) on all sides so
//   the shadow has room to render outside the rectangle bounds.  The C++ side
//   is responsible for inflating the quad; this shader assumes it is already
//   large enough.
//
// ── Coordinate system ────────────────────────────────────────────────────────
//
//   v_uv comes in as [0,0] (top-left) → [1,1] (bottom-right) across the quad.
//
//   The SDF expects coordinates centred at the rectangle centre.  We convert:
//
//     p_local = (v_uv - 0.5) * vec2(u.w, u.h)
//
//   This gives p_local in pixels, with (0,0) at the rect centre and
//   ±(w/2, h/2) at the corners.
//
// ── Compositing order (back to front) ────────────────────────────────────────
//
//   1. Drop shadow   (behind everything, soft Gaussian falloff outside rect)
//   2. Fill          (solid fill inside the rounded rect boundary)
//   3. Border        (drawn at the SDF zero-crossing, straddles the edge)
//
//   This matches CSS box-shadow / background-color / border stacking order.
//
// ── Alpha compositing ─────────────────────────────────────────────────────────
//
//   All colours are straight (un-premultiplied) alpha.
//   Pipeline blend state must be:
//     src_color = SRC_ALPHA,  dst_color = ONE_MINUS_SRC_ALPHA,  op = ADD
//     src_alpha = ONE,        dst_alpha = ONE_MINUS_SRC_ALPHA,  op = ADD

// ── Uniform buffer (fragment stage, slot 0) ───────────────────────────────────
//
// Total: 80 bytes.  Within the 128-byte Vulkan minimum push constant budget.
//
// std140 layout:
//   All scalars are float (4 bytes each, 4-byte aligned).
//   vec4 members are 16-byte aligned — placed at 32-byte offset; natural.
//
// The C++ mirror struct (must match byte-for-byte):
//
//   struct SDFFragUniform {
//       float w, h;
//       float radius, border_width;
//       float shadow_blur, shadow_spread;
//       float shadow_ox, shadow_oy;      // 32 bytes above
//       float fill_r,   fill_g,   fill_b,   fill_a;
//       float border_r, border_g, border_b, border_a;
//       float shadow_r, shadow_g, shadow_b, shadow_a;
//   };                                   // 80 bytes total
//   static_assert(sizeof(SDFFragUniform) == 80);

layout(set = 3, binding = 0) uniform SDFFragUniform {

    // ── Rect dimensions ───────────────────────────────────────────────────
    // Needed to convert v_uv [0,1]² to centre-relative pixel coordinates.

    float w;              // rect width  in pixels  (same as pushed to vert)
    float h;              // rect height in pixels

    // ── SDF geometry ──────────────────────────────────────────────────────

    float radius;         // corner radius in pixels; 0 = sharp right-angle
    float border_width;   // border thickness in pixels; 0 = no border drawn

    // ── Drop shadow ───────────────────────────────────────────────────────
    //
    //  blur    — softness: larger = more diffuse.  0 = hard shadow edge.
    //  spread  — expand shadow beyond rect edges.  Negative = shrink.
    //  ox, oy  — pixel offset.  Positive x = right.  Positive y = down.

    float shadow_blur;
    float shadow_spread;
    float shadow_ox;
    float shadow_oy;

    // ── Colours (RGBA straight alpha, [0,1]) ──────────────────────────────

    vec4 fill_color;      // interior fill
    vec4 border_color;    // border ring (ignored when border_width == 0)
    vec4 shadow_color;    // drop shadow (ignored when shadow_color.a == 0)

} u;

// ── Inputs / outputs ─────────────────────────────────────────────────────────

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

// ── 2D rounded rectangle SDF ─────────────────────────────────────────────────
//
// Returns the signed distance from point `p` to the boundary of a rounded
// rectangle centred at the origin with half-extents `b` and corner radius `r`.
//
//   < 0  →  inside the shape
//   = 0  →  on the boundary
//   > 0  →  outside the shape
//
// Reference: Inigo Quilez, "2D distance functions"
//   https://iquilezles.org/articles/distfunctions2d
//
// The formula works because:
//   1. abs(p) - b  maps p into the first quadrant relative to the rounded
//      corner centre at (b.x - r, b.y - r).
//   2. length(max(q, 0))  measures the distance outside the corner arc.
//   3. min(max(q.x, q.y), 0)  handles interior points (negative distance).
//   4. Subtracting r shifts the zero-crossing to the actual rounded edge.

float sdRoundBox(vec2 p, vec2 b, float r)
{
    // Clamp radius so it never exceeds the smallest half-extent.
    // Without this, very large radii produce incorrect (non-circular) corners.
    r = min(r, min(b.x, b.y));

    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// ── Soft shadow falloff ───────────────────────────────────────────────────────
//
// Approximates a Gaussian blur analytically from the SDF value.
//
// For a hard shadow (blur == 0): step function at sdf == 0.
// For a soft shadow: exponential decay — matches a Gaussian with sigma ≈ blur/2.
//
// The approximation:
//   alpha = exp( -max(sdf, 0)^2 / blur^2 )
//
// Properties:
//   - alpha = 1 when sdf <= 0  (fully inside / below the shadow shape)
//   - alpha → 0 as sdf → +∞   (far outside, fully transparent)
//   - Smooth, differentiable, correct convex falloff
//   - No texture lookup, no multi-pass blur required
//
// Comparison to a true Gaussian convolution: for sdf > 0 the difference is
// negligible at typical UI blur radii (4–32 px).  Inside the shape (sdf < 0)
// both produce alpha = 1, so the distinction only matters at the edge.

float shadowFalloff(float sdf, float blur)
{
    if (blur < 0.5) {
        // Hard shadow: sharp step at the boundary.
        return sdf < 0.0 ? 1.0 : 0.0;
    }
    float d = max(sdf, 0.0);
    return exp(-(d * d) / (blur * blur));
}

// ── Main ─────────────────────────────────────────────────────────────────────

void main()
{
    // Convert v_uv [0,1]² to centre-relative pixel coordinates.
    // Origin is at the rect centre; ±(w/2, h/2) are the corner extents.
    vec2 p = (v_uv - 0.5) * vec2(u.w, u.h);

    // Half-extents of the rect (SDF convention: b = half-size vector).
    vec2 half_size = vec2(u.w, u.h) * 0.5;

    // ── 1. Drop shadow ────────────────────────────────────────────────────
    //
    // Query the SDF at the shadow's displaced, enlarged position.
    // The shadow shape is the same rounded rect, shifted by the offset and
    // expanded by `spread` pixels on all sides.

    vec2  shadow_p   = p - vec2(u.shadow_ox, u.shadow_oy);
    vec2  shadow_b   = half_size + u.shadow_spread;
    float shadow_sdf = sdRoundBox(shadow_p, shadow_b, u.radius);
    float shadow_a   = shadowFalloff(shadow_sdf, u.shadow_blur) * u.shadow_color.a;

    // ── 2. Fill ───────────────────────────────────────────────────────────
    //
    // Smooth step over a 1-pixel window centred on sdf == 0.
    // This gives sub-pixel anti-aliasing without MSAA.
    //
    //   sdf < -0.5  →  fully inside  (alpha = 1)
    //   sdf > +0.5  →  fully outside (alpha = 0)
    //   between     →  smooth ramp   (anti-aliased edge)

    float rect_sdf  = sdRoundBox(p, half_size, u.radius);
    float fill_a    = smoothstep(0.5, -0.5, rect_sdf) * u.fill_color.a;

    // ── 3. Border ─────────────────────────────────────────────────────────
    //
    // The border straddles the SDF zero-crossing:
    //   abs(sdf) < half_border  →  on the border
    //
    // smoothstep with a 1-pixel window on each edge gives a clean AA border
    // that looks correct at any subpixel position.

    float border_a = 0.0;
    if (u.border_width > 0.0) {
        float half_bw  = u.border_width * 0.5;
        // Inner edge of the border (negative sdf = inside the rect).
        float inner = smoothstep(-half_bw - 0.5, -half_bw + 0.5, rect_sdf);
        // Outer edge of the border (positive sdf = outside the rect).
        float outer = smoothstep( half_bw + 0.5,  half_bw - 0.5, rect_sdf);
        border_a = inner * outer * u.border_color.a;
    }

    // ── Compositing (back to front) ───────────────────────────────────────
    //
    // Standard "over" operator:  result = src + dst * (1 - src.a)
    //
    // We work in premultiplied space internally for correct blending, then
    // convert back to straight alpha for the output (the pipeline blend state
    // handles the final over-composite onto the framebuffer).

    // Start with the shadow layer.
    vec4 color = vec4(u.shadow_color.rgb * shadow_a, shadow_a);

    // Composite fill over shadow.
    vec4 fill = vec4(u.fill_color.rgb * fill_a, fill_a);
    color = fill + color * (1.0 - fill_a);

    // Composite border over fill.
    vec4 border = vec4(u.border_color.rgb * border_a, border_a);
    color = border + color * (1.0 - border_a);

    // Convert premultiplied → straight alpha for the output.
    // The SDL3 GPU blend state (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) expects
    // straight alpha in the fragment output.
    if (color.a > 0.0001) {
        color.rgb /= color.a;
    }

    out_color = color;
}
