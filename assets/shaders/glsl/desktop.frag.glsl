#version 450

// desktop.frag.glsl — Chrome surface: grayscale animated SDF gradient
//                     Vulkan / SPIR-V mirror of desktop.frag.metal.
//
// Technique
// ---------
// Analytic height field → SDF gradient via central finite differences →
// 3D surface normal → chrome BRDF (ambient + diffuse + twin specular lobes) →
// exp-lerp tone-map.  Directional light rotates slowly; no spotlight.
//
// Compilation
//   glslc desktop.frag.glsl -o desktop.frag.spv
//
// SDL3 GPU binding conventions for SPIR-V (fragment stage)
// --------------------------------------------------------
//   Uniform buffers  →  set = 3,  binding = slot
//   (SDL_PushGPUFragmentUniformData(cmd, /*slot=*/0, &data, sizeof(data)))

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// Fragment uniform buffer — slot 0 → set 3, binding 0.
// Layout matches the C++ FragUniform struct (4 × float = 16 bytes).
layout(set = 3, binding = 0) uniform FragUniform {
    float time;
    float pad0;
    float pad1;
    float pad2;
} u;

// ── Height field ────────────────────────────────────────────────────────────
// Four sinusoidal waves with coprime drift speeds → never perfectly repeating.
float h(vec2 p, float t) {
    float v = 0.0;
    v += 0.45 * sin(p.x * 1.05 + t * 0.13) * cos(p.y * 0.80 + t * 0.09);
    v += 0.28 * sin(p.x * 0.65 - p.y * 1.15 + t * 0.10);
    v += 0.18 * cos(p.x * 1.45 + p.y * 0.50 - t * 0.07);
    v += 0.12 * sin(p.y * 1.70 - p.x * 0.35 + t * 0.05);
    return v;
}

// ── SDF gradient — central finite differences ───────────────────────────────
vec2 sdfGrad(vec2 p, float t) {
    const float e = 0.016;
    return vec2(
        h(p + vec2(e, 0.0), t) - h(p - vec2(e, 0.0), t),
        h(p + vec2(0.0, e), t) - h(p - vec2(0.0, e), t)
    ) * (0.5 / e);
}

// ── Chrome BRDF ─────────────────────────────────────────────────────────────
float chromeBRDF(vec3 N, vec3 L) {
    const vec3  V    = vec3(0.0, 0.0, 1.0);
    vec3  H    = normalize(L + V);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    return 0.03
         + 0.10 * NdotL
         + 0.75 * pow(NdotH, 56.0)   // sharp chrome streak
         + 0.20 * pow(NdotH,  9.0);  // broad glow
}

void main()
{
    // Centred coordinates ≈ [-1.6, 1.6]
    vec2  p = (v_uv * 2.0 - 1.0) * 1.6;
    float t = u.time;

    // Surface gradient → 3D normal
    vec2  g = sdfGrad(p, t);
    vec3  N = normalize(vec3(-g.x, -g.y, 1.0));

    // Slowly rotating directional light (≈ one revolution per 114 s)
    float la = t * 0.055;
    vec3 L1 = normalize(vec3( cos(la),          sin(la * 0.71),        2.2));
    // Subtle fill from opposite hemisphere
    vec3 L2 = normalize(vec3(-cos(la + 1.9),   -sin(la * 0.71 + 0.5), 1.6));

    float raw = chromeBRDF(N, L1) + 0.35 * chromeBRDF(N, L2);

    // Exp-lerp tone-map → chrome's sharp dark/bright contrast
    float c    = 1.0 - exp(-raw * 2.4);
    float gray = mix(0.04, 0.90, c);

    // Soft corner fade (not a spotlight)
    float edge = smoothstep(1.45, 0.55, length(p));
    gray *= mix(0.72, 1.0, edge);

    fragColor = vec4(gray, gray, gray, 1.0);
}