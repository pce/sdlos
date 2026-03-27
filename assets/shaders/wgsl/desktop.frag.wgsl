//  Chrome surface: grayscale animated SDF gradient.
//                     WebGPU / WASM mirror of desktop.frag.metal.
//
// Technique
// Analytic height field → SDF gradient via central finite differences →
// 3D surface normal → chrome BRDF (ambient + diffuse + twin specular lobes) →
// exp-lerp tone-map.  Directional light rotates slowly; no spotlight.
//
// SDL3 GPU binding conventions for WGSL / WebGPU
//   Fragment uniform buffers  →  group = 3,  binding = slot
//   (SDL_PushGPUFragmentUniformData(cmd, /*slot=*/0, &data, sizeof(data)))
//
// Layout matches the C++ FragUniform struct (4 × f32 = 16 bytes).

struct FragUniform {
    time: f32,
    pad0: f32,
    pad1: f32,
    pad2: f32,
}

@group(3) @binding(0) var<uniform> u: FragUniform;

// Height field
// Four sinusoidal waves with coprime drift speeds → never perfectly repeating.
fn h(p: vec2f, t: f32) -> f32 {
    var v: f32 = 0.0;
    v += 0.45 * sin(p.x * 1.05 + t * 0.13) * cos(p.y * 0.80 + t * 0.09);
    v += 0.28 * sin(p.x * 0.65 - p.y * 1.15 + t * 0.10);
    v += 0.18 * cos(p.x * 1.45 + p.y * 0.50 - t * 0.07);
    v += 0.12 * sin(p.y * 1.70 - p.x * 0.35 + t * 0.05);
    return v;
}

// SDF gradient:  central finite differences
//                with small step → smooth normals,
//                           no aliasing artifacts.
fn sdfGrad(p: vec2f, t: f32) -> vec2f {
    let e: f32 = 0.016;
    return vec2f(
        h(p + vec2f(e, 0.0), t) - h(p - vec2f(e, 0.0), t),
        h(p + vec2f(0.0, e), t) - h(p - vec2f(0.0, e), t),
    ) * (0.5 / e);
}

//  Chrome BRDF
// Orthographic view (V fixed at +Z).  Returns raw (un-tonemapped) luminance.
fn chromeBRDF(N: vec3f, L: vec3f) -> f32 {
    let V    = vec3f(0.0, 0.0, 1.0);
    let H    = normalize(L + V);
    let NdotL = max(dot(N, L), 0.0);
    let NdotH = max(dot(N, H), 0.0);
    return 0.03
         + 0.10 * NdotL
         + 0.75 * pow(NdotH, 56.0)   // sharp chrome streak
         + 0.20 * pow(NdotH,  9.0);  // broad glow
}

// Fragment entry point
struct FragIn {
    @location(0) uv: vec2f,
}

@fragment
fn fs_main(in: FragIn) -> @location(0) vec4f {
    // Centred coordinates ≈ [-1.6, 1.6]
    let p = (in.uv * 2.0 - vec2f(1.0)) * 1.6;
    let t = u.time;

    // Surface gradient → 3D normal
    let g = sdfGrad(p, t);
    let N = normalize(vec3f(-g.x, -g.y, 1.0));

    // Slowly rotating directional light (≈ one revolution per 114 s)
    let la = t * 0.055;
    let L1 = normalize(vec3f( cos(la),          sin(la * 0.71),        2.2));
    // Subtle fill from opposite hemisphere
    let L2 = normalize(vec3f(-cos(la + 1.9),   -sin(la * 0.71 + 0.5), 1.6));

    let raw = chromeBRDF(N, L1) + 0.35 * chromeBRDF(N, L2);

    // Exp-lerp tone-map → chrome's sharp dark/bright contrast
    let c    = 1.0 - exp(-raw * 2.4);
    var gray = mix(0.04, 0.90, c);

    // Soft corner fade (not a spotlight — purely geometric cutoff)
    let edge = smoothstep(1.45, 0.55, length(p));
    gray *= mix(0.72, 1.0, edge);

    return vec4f(gray, gray, gray, 1.0);
}

