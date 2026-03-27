// ui_sdf.frag.wgsl — SDF-based UI element: rounded rectangle, border, drop shadow.
//
// WebGPU / WGSL equivalent of ui_sdf.frag.glsl (#version 450).
// Phase 3 replacement for ui_rect.frag.wgsl.
//
// One fragment shader, one draw call per element — handles fill, border, and
// soft drop shadow simultaneously using a 2D signed distance function.
//
// Usage
// -----
//   SDL3 GPU accepts WGSL source directly for its WebGPU backend.
//   No offline compilation step is required (unlike GLSL → SPIR-V).
//
//   Push the SDF uniform before drawing:
//     SDL_PushGPUFragmentUniformData(cmd, /*slot=*/0, &fu, sizeof(fu));
//
//   The vertex quad MUST be inflated by (shadow_spread + shadow_blur * 2) on
//   all sides so the shadow has room to render outside the rectangle bounds.
//   The C++ side (RenderContext::drawRoundedRect / drawShadow) is responsible
//   for this inflation.
//
// SDL3 GPU binding conventions — WebGPU / WGSL
// --------------------------------------------
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
// ── Why SDF for UI? ──────────────────────────────────────────────────────────
//
//   A Signed Distance Function returns the signed distance from any point to
//   the nearest surface of a shape.  Negative inside, positive outside, zero
//   exactly on the boundary.
//
//   For a rectangle centred at the origin with half-extents b and corner
//   radius r, the 2D SDF is (Inigo Quilez, 2018):
//
//     sdRoundBox(p, b, r):
//       q = abs(p) - b + r
//       return length(max(q, 0)) + min(max(q.x, q.y), 0) - r
//
//   From this single scalar field every UI effect falls out for free:
//
//     filled rect     →  smoothstep(+0.5, -0.5, sdf)
//     border          →  smoothstep(+0.5, -0.5, abs(sdf) - half_border)
//     drop shadow     →  soft_alpha( sdRoundBox(p - offset, b + spread, r) )
//     inner shadow    →  soft_alpha(-sdf)   inside the shape
//     glow / outline  →  exp(-max(sdf, 0) / blur)
//     anti-aliasing   →  built in — smoothstep straddles the zero crossing
//
// ── Drop shadow is NOT a "rect that grows" ───────────────────────────────────
//
//   A shadow is the same SDF evaluated at a displaced, slightly enlarged query
//   point, fed through a soft falloff function:
//
//     shadow_sdf   = sdRoundBox(p - offset, b + spread, radius)
//     shadow_alpha = exp( -max(shadow_sdf, 0)^2 / blur^2 )
//
//   The exp(-x²) term approximates a Gaussian blur analytically — no separate
//   blur pass, no texture lookup, no extra draw call.  The result is
//   indistinguishable from a real Gaussian for typical UI blur radii.
//
//   Key parameters:
//     offset  (ox, oy)  — directional shift in pixels; (4, 4) = bottom-right
//     spread            — expands the shadow beyond the rect edges in pixels
//     blur              — controls the softness (sigma of the Gaussian approx)
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
//   Compositing is performed in premultiplied-alpha space internally for
//   correct blending, then converted back to straight alpha for output.
//   The SDL3 GPU pipeline blend state must be:
//     src_color_blendfactor = SRC_ALPHA
//     dst_color_blendfactor = ONE_MINUS_SRC_ALPHA
//     color_blend_op        = ADD
//     src_alpha_blendfactor = ONE
//     dst_alpha_blendfactor = ONE_MINUS_SRC_ALPHA
//     alpha_blend_op        = ADD

// ── Uniform buffer (fragment stage, slot 0) ───────────────────────────────────
//
// 80 bytes total.  Must match the C++ struct SDFFragUniform byte-for-byte.
//
//   struct SDFFragUniform {
//       float w, h;
//       float radius, border_width;
//       float shadow_blur, shadow_spread;
//       float shadow_ox, shadow_oy;      // 32 bytes above
//       float fill_r,   fill_g,   fill_b,   fill_a;
//       float border_r, border_g, border_b, border_a;
//       float shadow_r, shadow_g, shadow_b, shadow_a;
//   };  // 80 bytes total
//   static_assert(sizeof(SDFFragUniform) == 80);
//
// WGSL uniform address space layout rules:
//   - f32 members are 4 bytes, 4-byte aligned — no implicit padding between
//     scalars here since all are f32.
//   - vec4f members are 16 bytes, 16-byte aligned — placed at 32-byte offset
//     naturally (8 × f32 = 32 bytes precede them), so no padding is inserted.
//   - Total: 8 × f32 (32 B) + 3 × vec4f (48 B) = 80 bytes.

struct SDFFragUniform {
    // ── Rect dimensions ───────────────────────────────────────────────────
    // Used to convert v_uv [0,1]² into centre-relative pixel coordinates.
    w: f32,              // rect width  in pixels  (matches value pushed to vert)
    h: f32,              // rect height in pixels

    // ── SDF geometry ──────────────────────────────────────────────────────
    radius:       f32,   // corner radius in pixels; 0 = sharp right-angle
    border_width: f32,   // border thickness in pixels; 0 = no border drawn

    // ── Drop shadow ───────────────────────────────────────────────────────
    //   blur    — softness: larger = more diffuse.  0 = hard shadow edge.
    //   spread  — expand shadow beyond rect edges.  Negative = shrink.
    //   ox, oy  — pixel offset.  Positive x = right.  Positive y = down.
    shadow_blur:   f32,
    shadow_spread: f32,
    shadow_ox:     f32,
    shadow_oy:     f32,

    // ── Colours (RGBA straight alpha, [0,1]) ──────────────────────────────
    fill_color:   vec4f,   // interior fill
    border_color: vec4f,   // border ring (ignored when border_width == 0)
    shadow_color: vec4f,   // drop shadow (ignored when shadow_color.a == 0)
}

@group(3) @binding(0) var<uniform> u: SDFFragUniform;

// ── Fragment input / output ───────────────────────────────────────────────────

struct FragIn {
    @location(0) v_uv: vec2f,
}

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
// The radius is clamped to min(b.x, b.y) so that very large radii never
// produce incorrect (non-circular) corners.

fn sdRoundBox(p: vec2f, b: vec2f, r: f32) -> f32 {
    // Clamp radius so it never exceeds the smallest half-extent.
    let rc = min(r, min(b.x, b.y));
    let q  = abs(p) - b + rc;
    return length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - rc;
}

// ── Soft shadow falloff ───────────────────────────────────────────────────────
//
// Approximates a Gaussian blur analytically from the SDF value.
//
// For a hard shadow (blur < 0.5): step function at sdf == 0.
// For a soft shadow: exponential decay — matches a Gaussian with sigma ≈ blur/2.
//
//   alpha = exp( -max(sdf, 0)^2 / blur^2 )
//
// Properties:
//   - alpha = 1 when sdf <= 0  (fully inside / below the shadow shape)
//   - alpha → 0 as sdf → +inf  (far outside, fully transparent)
//   - Smooth, differentiable, correct convex falloff
//   - No texture lookup, no multi-pass blur required
//
// Comparison to a true Gaussian: for sdf > 0 the difference is negligible
// at typical UI blur radii (4–32 px).  Inside the shape (sdf < 0) both
// produce alpha = 1, so the distinction only matters at the soft edge.

fn shadowFalloff(sdf: f32, blur: f32) -> f32 {
    if blur < 0.5 {
        // Hard shadow: sharp step at the SDF boundary.
        return select(0.0, 1.0, sdf < 0.0);
    }
    let d = max(sdf, 0.0);
    return exp(-(d * d) / (blur * blur));
}

// ── Main ─────────────────────────────────────────────────────────────────────

@fragment
fn fs_main(in: FragIn) -> @location(0) vec4f {

    // Convert v_uv [0,1]² to centre-relative pixel coordinates.
    // Origin is at the rect centre; ±(w/2, h/2) are the corner extents.
    let p         = (in.v_uv - vec2f(0.5)) * vec2f(u.w, u.h);
    let half_size = vec2f(u.w, u.h) * 0.5;

    // ── 1. Drop shadow ────────────────────────────────────────────────────
    //
    // Query the SDF at the shadow's displaced, enlarged position.
    // The shadow shape is the same rounded rect, shifted by the offset and
    // expanded by `spread` pixels on all sides.
    let shadow_p   = p - vec2f(u.shadow_ox, u.shadow_oy);
    let shadow_b   = half_size + u.shadow_spread;
    let shadow_sdf = sdRoundBox(shadow_p, shadow_b, u.radius);
    let shadow_a   = shadowFalloff(shadow_sdf, u.shadow_blur) * u.shadow_color.a;

    // ── 2. Fill ───────────────────────────────────────────────────────────
    //
    // Smooth step over a 1-pixel window centred on sdf == 0.
    // Gives sub-pixel anti-aliasing without MSAA.
    //
    //   sdf < -0.5  →  fully inside  (alpha = 1)
    //   sdf > +0.5  →  fully outside (alpha = 0)
    //   between     →  smooth ramp   (anti-aliased edge)
    //
    // Note: smoothstep(edge0, edge1, x) with edge0 > edge1 is well-defined
    // in both WGSL and GLSL — it simply inverts the ramp direction.
    let rect_sdf = sdRoundBox(p, half_size, u.radius);
    let fill_a   = smoothstep(0.5, -0.5, rect_sdf) * u.fill_color.a;

    // ── 3. Border ─────────────────────────────────────────────────────────
    //
    // The border straddles the SDF zero-crossing:
    //   abs(sdf) < half_border  →  on the border ring
    //
    // smoothstep with a 1-pixel window on each edge gives a clean AA border
    // at any subpixel position.
    var border_a = 0.0;
    if u.border_width > 0.0 {
        let half_bw = u.border_width * 0.5;
        // Inner edge of the border (negative sdf = inside the rect).
        let inner = smoothstep(-half_bw - 0.5, -half_bw + 0.5, rect_sdf);
        // Outer edge of the border (positive sdf = outside the rect).
        let outer = smoothstep( half_bw + 0.5,  half_bw - 0.5, rect_sdf);
        border_a  = inner * outer * u.border_color.a;
    }

    // ── Compositing (back to front) ───────────────────────────────────────
    //
    // Standard "over" operator in premultiplied-alpha space:
    //   result = src_premul + dst_premul * (1 - src.a)
    //
    // We composite in premultiplied space for correctness, then convert back
    // to straight alpha before output so the pipeline blend state
    // (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) handles the final composite onto the
    // framebuffer correctly.

    // 1. Shadow layer (lowest / furthest back).
    var color = vec4f(u.shadow_color.rgb * shadow_a, shadow_a);

    // 2. Fill composited over shadow.
    let fill_premul = vec4f(u.fill_color.rgb * fill_a, fill_a);
    color = fill_premul + color * (1.0 - fill_a);

    // 3. Border composited over fill.
    let border_premul = vec4f(u.border_color.rgb * border_a, border_a);
    color = border_premul + color * (1.0 - border_a);

    // ── Premultiplied → straight alpha conversion ─────────────────────────
    //
    // The pipeline blend state expects straight (un-premultiplied) alpha in
    // the fragment output.  Divide RGB by alpha to undo premultiplication.
    // Guard against division by zero for fully transparent fragments.
    if color.a > 0.0001 {
        color = vec4f(color.rgb / color.a, color.a);
    }

    return color;
}
