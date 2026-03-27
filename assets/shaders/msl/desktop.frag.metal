#include <metal_stdlib>
using namespace metal;

// desktop.frag.metal
// Fragment shader (MSL) for a fullscreen procedural animated background.
// - Expects a small uniform buffer at [[buffer(0)]] with a float `time` in .x
// - Vertex shader should provide UVs at [[user(locn0)]]

struct FragUniform {
    float time;
    float pad0;
    float pad1;
    float pad2;
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// Simple hashing function for 2D coordinates
static inline float hash21(float2 p) {
    // Classic value-hash trick
    return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453123);
}

// Smooth value noise (bilinear interpolation)
static inline float noise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);

    float a = hash21(i + float2(0.0, 0.0));
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));

    float2 u = f * f * (3.0 - 2.0 * f); // smoothstep interpolation
    float lerp_ab = mix(a, b, u.x);
    float lerp_cd = mix(c, d, u.x);
    return mix(lerp_ab, lerp_cd, u.y);
}

// FBM (fractal Brownian motion)
static inline float fbm(float2 p) {
    float val = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        val += amp * noise(p);
        p = p * 2.0 + float2(1.0, 0.5);
        amp *= 0.5;
    }
    return val;
}

fragment float4 main0(VertOut in [[stage_in]], constant FragUniform& u [[buffer(0)]]) {
    // UV is expected in 0..1; center and scale for nicer detail
    float2 uv = in.uv * 2.0 - 1.0;
    float t = u.time;

    // Animated FBM field
    float n = fbm(uv * 1.8 + float2(t * 0.15, t * 0.08));

    // Create an appealing color gradient driven by noise
    float3 colA = float3(0.08, 0.18, 0.45);
    float3 colB = float3(0.95, 0.62, 0.28);

    float blend = smoothstep(-0.1, 0.9, n);
    float3 color = mix(colA, colB, blend);

    // Slight vignetting
    float vign = 1.0 - 0.35 * length(in.uv - float2(0.5, 0.5));
    color *= vign;

    return float4(color, 1.0);
}
