#version 450

// Animated FBM cloud field (blue/yellow palette).
// Previously named desktop.frag.glsl.  Kept for reference / easy swap-in.
//
// Compilation
//   glslc clouds.frag.glsl -o clouds.frag.spv
//
// SDL3 GPU binding: fragment UBO slot 0 → set 3, binding 0.

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(set = 3, binding = 0) uniform FragUniform {
    float time;
    float pad0;
    float pad1;
    float pad2;
} u;

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.11369, 0.13787));
    return fract(sin(dot(p3, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i + vec2(0.0, 0.0));
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2 u2 = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u2.x), mix(c, d, u2.x), u2.y);
}

float fbm(vec2 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        v   += amp * noise(p);
        p    = p * 2.0 + vec2(1.0, 0.5);
        amp *= 0.5;
    }
    return v;
}

void main()
{
    vec2  uv = v_uv * 2.0 - 1.0;
    float t  = u.time;
    float n  = fbm(uv * 1.8 + vec2(t * 0.15, t * 0.08));

    vec3 colA  = vec3(0.08, 0.18, 0.45);
    vec3 colB  = vec3(0.95, 0.62, 0.28);
    vec3 color = mix(colA, colB, smoothstep(-0.1, 0.9, n));

    float vign = 1.0 - 0.35 * length(v_uv - vec2(0.5));
    color *= vign;

    fragColor = vec4(color, 1.0);
}

