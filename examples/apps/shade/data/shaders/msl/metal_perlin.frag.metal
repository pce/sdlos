#include <metal_stdlib>
using namespace metal;

// ---------- uniforms (unchanged layout) ----------
struct NodeShaderParams {
    float u_width;
    float u_height;
    float u_focusX;
    float u_focusY;
    float u_param0;   // noise scale
    float u_param1;   // distortion strength
    float u_param2;   // metallic intensity
    float u_time;
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// ---------- hash / noise helpers ----------
float hash21(float2 p) {
    p = fract(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);

    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));

    float2 u = f * f * (3.0 - 2.0 * f);

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Fractal Brownian Motion (multi-octave Perlin-style)
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

// ---------- fragment ----------
fragment float4 main0(VertOut in                 [[stage_in]],
                      constant NodeShaderParams& p [[buffer(0)]],
                      texture2d<float> tex        [[texture(0)]],
                      sampler samp                [[sampler(0)]])
{
    float2 uv = in.uv;

    float2 resolution = float2(p.u_width, p.u_height);
    float2 px = 1.0 / max(resolution, float2(1.0));

    // ---------- animated noise field ----------
    float t = p.u_time * 0.2;

    float2 nUV = uv * (p.u_param0 * 4.0) + float2(t, t * 0.7);
    float n = fbm(nUV);

    // ---------- UV distortion (liquid metal feel) ----------
    float2 distort = float2(
        fbm(nUV + float2(2.3, 1.7)),
        fbm(nUV + float2(-1.2, 3.4))
    );

    distort = (distort - 0.5) * 2.0;

    float2 duv = uv + distort * p.u_param1 * 0.02;

    float4 base = tex.sample(samp, duv);

    // ---------- derive pseudo normal from noise ----------
    float h  = fbm(nUV);
    float hx = fbm(nUV + float2(px.x, 0.0));
    float hy = fbm(nUV + float2(0.0, px.y));

    float3 normal = normalize(float3(h - hx, h - hy, 0.2));

    // ---------- lighting (metallic look) ----------
    float3 lightDir = normalize(float3(0.4, 0.6, 1.0));
    float3 viewDir  = float3(0.0, 0.0, 1.0);

    float diff = max(dot(normal, lightDir), 0.0);

    // Fresnel
    float fres = pow(1.0 - max(dot(normal, viewDir), 0.0), 3.0);

    // Specular highlight
    float3 halfVec = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfVec), 0.0), 64.0);

    // ---------- metallic shading ----------
    float3 metalColor = mix(base.rgb, float3(0.75, 0.8, 0.9), p.u_param2);

    float3 color =
        metalColor * (0.3 + diff * 0.7) +
        spec * 1.5 +
        fres * 0.6;

    return float4(color, base.a);
}
