#include <metal_stdlib>
using namespace metal;

struct NodeShaderParams {
    float u_width;
    float u_height;
    float u_focusX;
    float u_focusY;
    float u_param0;   // distortAmount
    float u_param1;   // distortFalloff
    float u_param2;   // noiseScale
    float u_param3;   // warpAmount (optional if available)
    float u_param4;   // quantizeSteps
    float u_param5;   // grainAmount
    float u_time;
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

float hash21(float2 p) {
    p = fract(p * float2(123.34, 456.21));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

float noise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);

    float a = hash21(i);
    float b = hash21(i + float2(1,0));
    float c = hash21(i + float2(0,1));
    float d = hash21(i + float2(1,1));

    float2 u = f * f * (3.0 - 2.0 * f);

    return mix(mix(a,b,u.x), mix(c,d,u.x), u.y);
}

float fbm(float2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

fragment float4 main0(VertOut in                 [[stage_in]],
                      constant NodeShaderParams& p [[buffer(0)]],
                      texture2d<float> tex        [[texture(0)]],
                      sampler samp                [[sampler(0)]])
{
    float2 uv = in.uv;
    float2 center = float2(p.u_focusX, p.u_focusY);

    float2 res = float2(p.u_width, p.u_height);
    float2 px = 1.0 / max(res, float2(1.0));

    float t = p.u_time * 0.2;

    // --------------------------------------------------
    // 1. RADIAL DISTORTION (bulge / pinch)
    // --------------------------------------------------
    float2 delta = uv - center;
    float dist = length(delta);

    float falloff = pow(dist, p.u_param1); // lens curve
    float2 radialOffset = normalize(delta + 1e-5) * falloff * p.u_param0;

    // --------------------------------------------------
    // 2. DOMAIN WARPED NOISE (important!)
    // --------------------------------------------------
    float2 nUV = uv * p.u_param2;

    float2 warp = float2(
        fbm(nUV + t),
        fbm(nUV + float2(5.2, 1.3) + t)
    );

    warp = (warp - 0.5) * p.u_param3;

    float n = fbm(nUV + warp);

    // convert scalar noise into directional distortion
    float2 noiseOffset = float2(
        fbm(nUV + warp + 3.1),
        fbm(nUV + warp - 2.7)
    );

    noiseOffset = (noiseOffset - 0.5) * p.u_param0;

    // --------------------------------------------------
    // 3. COMBINE DISTORTIONS
    // --------------------------------------------------
    float2 duv = uv + radialOffset + noiseOffset;

    // --------------------------------------------------
    // 4. SAMPLE BASE IMAGE
    // --------------------------------------------------
    float4 col = tex.sample(samp, duv);

    // --------------------------------------------------
    // 5. QUANTIZATION (comic water shading)
    // --------------------------------------------------
    if (p.u_param4 > 1.0) {
        float steps = p.u_param4;
        col.rgb = floor(col.rgb * steps) / steps;
    }

    // --------------------------------------------------
    // 6. GRAIN
    // --------------------------------------------------
    float grain = hash21(uv * res + p.u_time) - 0.5;
    col.rgb += grain * p.u_param5;

    return col;
}
