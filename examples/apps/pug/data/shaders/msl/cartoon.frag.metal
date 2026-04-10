#include <metal_stdlib>
using namespace metal;

// ALPHABETICAL ORDER in the struct to match pipeline.pug keys.
// pass#cartoon(shader="cartoon" threshold="0.1" time="0.0")
struct CartoonUniforms {
    float threshold;
    float time;
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// Simple luma helper
static float luma(float3 c) {
    return dot(c, float3(0.299f, 0.587f, 0.114f));
}

fragment float4 main0(VertOut in [[stage_in]],
                     constant CartoonUniforms& u [[buffer(0)]],
                     texture2d<float> tex [[texture(0)]],
                     sampler s [[sampler(0)]]) {

    float2 uv = in.uv;
    float2 size = float2(tex.get_width(), tex.get_height());
    float2 px = 1.0f / size;

    // Sample core color
    float4 base = tex.sample(s, uv);

    // Sobel edge detection on luma
    float tl = luma(tex.sample(s, uv + float2(-px.x,  px.y)).rgb);
    float tc = luma(tex.sample(s, uv + float2( 0.0,  px.y)).rgb);
    float tr = luma(tex.sample(s, uv + float2( px.x,  px.y)).rgb);
    float ml = luma(tex.sample(s, uv + float2(-px.x,  0.0)).rgb);
    float mr = luma(tex.sample(s, uv + float2( px.x,  0.0)).rgb);
    float bl = luma(tex.sample(s, uv + float2(-px.x, -px.y)).rgb);
    float bc = luma(tex.sample(s, uv + float2( 0.0, -px.y)).rgb);
    float br = luma(tex.sample(s, uv + float2( px.x, -px.y)).rgb);

    float gx = -tl - 2.0f*ml - bl + tr + 2.0f*mr + br;
    float gy = -tl - 2.0f*tc - tr + bl + 2.0f*bc + br;

    float edge = sqrt(gx*gx + gy*gy);

    // Apply threshold (parameter from pug)
    float edge_mask = smoothstep(u.threshold, u.threshold + 0.1f, edge);

    // Posterize base color (simple 4-level quantization)
    float3 posterized = floor(base.rgb * 4.0f) / 4.0f;

    // Mix posterized color with black edges
    float3 col = mix(posterized, float3(0.0f), edge_mask);

    return float4(col, base.a);
}
