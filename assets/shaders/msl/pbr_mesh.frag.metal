#include <metal_stdlib>
using namespace metal;

// Must match the vertex shader output exactly.
struct VertOut {
    float4 position     [[position]];
    float3 world_pos;
    float3 world_normal;
    float2 uv;
};

// 96 bytes — driven by CSS properties and scene lighting.
struct FragPush {
    float4 base_color;   // CSS 'color' → albedo (rgba)
    float4 emissive;     // CSS '--emissive' (rgb) + intensity (a)
    float4 light_dir_i;  // xyz = normalized world dir toward surface, w = intensity
    float4 light_color;  // rgb directional light color, w = 0
    float4 cam_pos;      // xyz = camera world position, w = 0
    float  roughness;    // CSS '--roughness'
    float  metallic;     // CSS '--metallic'
    float  hover_t;      // 0..1; >0 when node is CSS :hover (driven by border-width > 0)
    float  opacity;      // CSS 'opacity'
};

fragment float4 main0(
    VertOut          in   [[stage_in]],
    constant FragPush& push [[buffer(0)]])
{
    // ------------------------------------------------------------------ basis
    float3 N = normalize(in.world_normal);
    float3 L = normalize(-push.light_dir_i.xyz);   // direction *toward* the light
    float3 V = normalize(push.cam_pos.xyz - in.world_pos);
    float3 H = normalize(L + V);

    float light_intensity = push.light_dir_i.w;

    // ------------------------------------------------------------------ terms
    // 1. Lambertian diffuse
    float  NdotL    = max(dot(N, L), 0.0);
    float3 diffuse  = NdotL
                    * push.base_color.rgb
                    * push.light_color.rgb
                    * light_intensity;

    // 2. Blinn-Phong specular
    //    shininess remaps roughness: roughness=0 → shininess≈1024, roughness=1 → shininess=1
    float  shininess = pow(2.0, (1.0 - push.roughness) * 10.0);
    float  NdotH     = max(dot(N, H), 0.0);
    float3 specular  = push.metallic
                     * pow(NdotH, shininess)
                     * push.light_color.rgb
                     * light_intensity;

    // 3. Ambient (constant low-level fill)
    float3 ambient   = push.base_color.rgb * 0.07;

    // 4. Emissive
    float3 emissive  = push.emissive.rgb * push.emissive.a;

    // 5. Hover rim — cool blue highlight on silhouette edges
    float  rim_factor = pow(1.0 - saturate(dot(N, V)), 3.0) * push.hover_t;
    float3 rim_color  = float3(0.34, 0.65, 1.0);
    float3 rim        = rim_color * rim_factor;

    // ------------------------------------------------------------------ final
    float3 color = ambient + diffuse + specular + emissive + rim;
    float  alpha = push.base_color.a * push.opacity;

    return float4(color, alpha);
}
