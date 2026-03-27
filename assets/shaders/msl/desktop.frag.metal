#include <metal_stdlib>
using namespace metal;

// Chrome surface: grayscale animated SDF gradient
//
// Technique
// An analytic height field h(p,t) is defined as a sum of sinusoidal waves
// drifting at different rates.  The surface gradient (= 2D surface normal)
// is evaluated via central finite differences:
//
//   grad = float2( h(p+dx,t) - h(p-dx,t),
//                  h(p+dy,t) - h(p-dy,t) ) / (2·ε)
//
// The gradient is lifted into a 3D normal and fed to a chrome BRDF:
// dark ambient + shallow diffuse + tight specular ribbon + broad halo.
// An exp-lerp tone-map ( 1 - exp(-raw·k) ) gives chrome's sharp
// light/dark contrast.  The directional light rotates very slowly;
// no radial spot, no point light.
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

// Height field
// Four sinusoidal waves with coprime drift speeds → never perfectly repeating.
static float h(float2 p, float t) {
    float v  = 0.0;
    v += 0.45 * sin(p.x * 1.05 + t * 0.13) * cos(p.y * 0.80 + t * 0.09);
    v += 0.28 * sin(p.x * 0.65 - p.y * 1.15 + t * 0.10);
    v += 0.18 * cos(p.x * 1.45 + p.y * 0.50 - t * 0.07);
    v += 0.12 * sin(p.y * 1.70 - p.x * 0.35 + t * 0.05);
    return v;
}

// SDF gradient — central finite differences
static float2 sdfGrad(float2 p, float t) {
    const float e = 0.016;
    float2 dx = float2(e, 0.0);
    float2 dy = float2(0.0, e);
    return float2(
        h(p + dx, t) - h(p - dx, t),
        h(p + dy, t) - h(p - dy, t)
    ) * (0.5 / e);
}

// Chrome BRDF
// Orthographic view (V fixed at +Z).  Returns raw (un-tonemapped) luminance.
static float chromeBRDF(float3 N, float3 L) {
    const float3 V = float3(0.0, 0.0, 1.0);
    float3 H    = normalize(L + V);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    // ambient + shallow diffuse + tight highlight ribbon + broad soft halo
    return 0.03
         + 0.10 * NdotL
         + 0.75 * pow(NdotH, 56.0)   // sharp chrome streak
         + 0.20 * pow(NdotH,  9.0);  // broad glow
}

fragment float4 main0(VertOut in [[stage_in]], constant FragUniform& u [[buffer(0)]]) {
    // Centred coordinates  ≈ [-1.6, 1.6]
    float2 p = (in.uv * 2.0 - 1.0) * 1.6;
    float  t = u.time;

    // Surface gradient → 3D normal
    float2 g = sdfGrad(p, t);
    float3 N = normalize(float3(-g.x, -g.y, 1.0));

    // Slowly rotating directional light (≈ one revolution per 114 s)
    float  la = t * 0.055;
    float3 L1 = normalize(float3( cos(la),           sin(la * 0.71),        2.2));
    // Subtle fill light from the opposite hemisphere
    float3 L2 = normalize(float3(-cos(la + 1.9),    -sin(la * 0.71 + 0.5), 1.6));

    float raw = chromeBRDF(N, L1)
              + 0.35 * chromeBRDF(N, L2);

    // Exp-lerp tone-map: smooth transition from near-black to bright ribbon
    float c    = 1.0 - exp(-raw * 2.4);
    float gray = mix(0.04, 0.90, c);

    // Soft edge fade toward corners (not a spotlight — purely geometric cutoff)
    float edge = smoothstep(1.45, 0.55, length(p));
    gray *= mix(0.72, 1.0, edge);

    return float4(gray, gray, gray, 1.0);
}