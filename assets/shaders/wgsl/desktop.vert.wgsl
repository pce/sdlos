// desktop.vert.wgsl — Fullscreen triangle vertex shader for WebGPU / WASM.
//
// No vertex buffer. Three vertices generated from @builtin(vertex_index)
// cover the entire viewport; the rasteriser clips to the visible quad.
//
// WebGPU NDC: y = +1 at top of viewport (same convention as Metal, opposite Vulkan).
// The formula   ndc.y = pos.y * 0.5 + 0.5   maps NDC +1 → UV 1.0 (top).
// The chrome SDF is radially symmetric so UV direction is irrelevant visually.
//
// No uniform buffers needed — pure vertex-index geometry.

struct VertOut {
    @builtin(position) position: vec4f,
    @location(0)       uv:       vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VertOut {
    // Oversized triangle: three vertices suffice to cover the full viewport.
    var pos = array<vec2f, 3>(
        vec2f(-1.0, -1.0),
        vec2f( 3.0, -1.0),
        vec2f(-1.0,  3.0),
    );

    let p = pos[vid];

    var out: VertOut;
    out.position = vec4f(p, 0.0, 1.0);
    // NDC [-1, +1] → UV [0, 1].  Centre → UV (0.5, 0.5).
    out.uv = p * 0.5 + vec2f(0.5);
    return out;
}

