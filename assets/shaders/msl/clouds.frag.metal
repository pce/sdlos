#include <metal_stdlib>
using namespace metal;

// clouds.frag.metal
// Fragment shader: animated FBM (fractal Brownian motion) cloud field.
// Blue–yellow colour palette.  Previously named desktop.frag.metal.
//
// Uniform at [[buffer(0)]]: float time in .x

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

static inline float hash21(float2 p) {
    return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453123);
}

static inline float noise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);

    float a = hash21(i + float2(0.0, 0.0));
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));

    float2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

static inline float fbm(float2 p) {
    float val = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        val += amp * noise(p);
        p    = p * 2.0 + float2(1.0, 0.5);
        amp *= 0.5;
    }
    return val;
}

fragment float4 main0(VertOut in [[stage_in]], constant FragUniform& u [[buffer(0)]]) {
    float2 uv = in.uv * 2.0 - 1.0;
    float  t  = u.time;

    float n = fbm(uv * 1.8 + float2(t * 0.15, t * 0.08));

    float3 colA  = float3(0.08, 0.18, 0.45);
    float3 colB  = float3(0.95, 0.62, 0.28);
    float3 color = mix(colA, colB, smoothstep(-0.1, 0.9, n));

    float vign = 1.0 - 0.35 * length(in.uv - float2(0.5, 0.5));
    color *= vign;

    return float4(color, 1.0);
}

