// Animated FBM cloud field (blue/yellow palette).
// Previously the default desktop background.  Kept for reference / easy swap-in.
//
// SDL3 GPU binding: fragment UBO slot 0 → group 3, binding 0.

struct FragUniform {
    time: f32,
    pad0: f32,
    pad1: f32,
    pad2: f32,
}

@group(3) @binding(0) var<uniform> u: FragUniform;

fn hash21(p: vec2f) -> f32 {
    let p3 = fract(vec3f(p.x, p.y, p.x) * vec3f(0.1031, 0.11369, 0.13787));
    return fract(sin(dot(p3, vec3f(12.9898, 78.233, 37.719))) * 43758.5453);
}

fn noise(p: vec2f) -> f32 {
    let i  = floor(p);
    let f  = fract(p);
    let a  = hash21(i + vec2f(0.0, 0.0));
    let b  = hash21(i + vec2f(1.0, 0.0));
    let c  = hash21(i + vec2f(0.0, 1.0));
    let d  = hash21(i + vec2f(1.0, 1.0));
    let u2 = f * f * (vec2f(3.0) - 2.0 * f);
    return mix(mix(a, b, u2.x), mix(c, d, u2.x), u2.y);
}

fn fbm(p_in: vec2f) -> f32 {
    var p   = p_in;
    var v   = 0.0f;
    var amp = 0.5f;
    for (var i = 0; i < 5; i++) {
        v   += amp * noise(p);
        p    = p * 2.0 + vec2f(1.0, 0.5);
        amp *= 0.5;
    }
    return v;
}

struct FragIn { @location(0) uv: vec2f }

@fragment
fn fs_main(in: FragIn) -> @location(0) vec4f {
    let uv  = in.uv * 2.0 - vec2f(1.0);
    let t   = u.time;
    let n   = fbm(uv * 1.8 + vec2f(t * 0.15, t * 0.08));

    let colA  = vec3f(0.08, 0.18, 0.45);
    let colB  = vec3f(0.95, 0.62, 0.28);
    let color = mix(colA, colB, smoothstep(-0.1, 0.9, n));

    let vign  = 1.0 - 0.35 * length(in.uv - vec2f(0.5));
    return vec4f(color * vign, 1.0);
}

