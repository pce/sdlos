#include <metal_stdlib>
using namespace metal;

// ui_sdf.frag.metal — SDF-based UI element: rounded rectangle, border, drop shadow.
//
// Phase 3 replacement for ui_rect.frag.metal.
// One fragment shader, one draw call per element — handles fill, border, and
// soft drop shadow simultaneously using a 2D signed distance function.

struct SDFFragUniform {
    float w, h;
    float radius, border_width;
    float shadow_blur, shadow_spread;
    float shadow_ox, shadow_oy;      // 32 bytes above
    float4 fill_color;
    float4 border_color;
    float4 shadow_color;
};  // 80 bytes total

struct VertOut {
    float4 position [[position]];
    float2 uv [[user(locn0)]];
};

static float sdRoundBox(float2 p, float2 b, float r) {
    float rc = min(r, min(b.x, b.y));
    float2 q = abs(p) - b + rc;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rc;
}

static float shadowFalloff(float sdf, float blur) {
    if (blur < 0.5) {
        return sdf < 0.0 ? 1.0 : 0.0;
    }
    float d = max(sdf, 0.0);
    return exp(-(d * d) / (blur * blur));
}

fragment float4 main0(VertOut in [[stage_in]],
                      constant SDFFragUniform& u [[buffer(0)]])
{
    float2 p = (in.uv - 0.5) * float2(u.w, u.h);
    float2 half_size = float2(u.w, u.h) * 0.5;

    // 1. Drop shadow
    float2 shadow_p = p - float2(u.shadow_ox, u.shadow_oy);
    float2 shadow_b = half_size + u.shadow_spread;
    float shadow_sdf = sdRoundBox(shadow_p, shadow_b, u.radius);
    float shadow_a = shadowFalloff(shadow_sdf, u.shadow_blur) * u.shadow_color.a;

    // 2. Fill
    float rect_sdf = sdRoundBox(p, half_size, u.radius);
    float fill_a = smoothstep(0.5, -0.5, rect_sdf) * u.fill_color.a;

    // 3. Border
    float border_a = 0.0;
    if (u.border_width > 0.0) {
        float half_bw = u.border_width * 0.5;
        float inner = smoothstep(-half_bw - 0.5, -half_bw + 0.5, rect_sdf);
        float outer = smoothstep( half_bw + 0.5,  half_bw - 0.5, rect_sdf);
        border_a = inner * outer * u.border_color.a;
    }

    // Compositing (premultiplied space)
    float4 color = float4(u.shadow_color.rgb * shadow_a, shadow_a);
    float4 fill_premul = float4(u.fill_color.rgb * fill_a, fill_a);
    color = fill_premul + color * (1.0 - fill_a);
    float4 border_premul = float4(u.border_color.rgb * border_a, border_a);
    color = border_premul + color * (1.0 - border_a);

    // Back to straight alpha
    if (color.a > 0.0001) {
        color.rgb /= color.a;
    }

    return color;
}

