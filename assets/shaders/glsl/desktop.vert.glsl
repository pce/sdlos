#version 450

// Fullscreen triangle vertex shader (Vulkan / SPIR-V).
// Mirror of desktop.vert.metal: no vertex buffer, geometry from gl_VertexIndex.
//
// Compilation
//   glslc desktop.vert.glsl -o desktop.vert.spv
//
// Three vertices cover the entire viewport with a single oversized triangle:
//   vid 0 → (-1, -1)  bottom-left  →  UV (0, 0)
//   vid 1 → ( 3, -1)  bottom-right →  UV (2, 0)   ← off-screen, clips to edge
//   vid 2 → (-1,  3)  top-left     →  UV (0, 2)   ← off-screen, clips to edge
// The rasteriser clips the triangle to the viewport; every on-screen fragment
// receives an interpolated UV in [0,1]×[0,1].
//
// Vulkan NDC note: y = -1 at the top of the viewport (opposite of Metal/WebGPU).
// For the chrome SDF background the exact UV direction is irrelevant because
// length(p) is radially symmetric. Keep the same formula as desktop.vert.metal
// so the two fragment shaders remain interchangeable.
//
// No uniform buffers required — pure vertex-index geometry.

layout(location = 0) out vec2 v_uv;

void main()
{
    const vec2 pos[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );

    vec2 p   = pos[gl_VertexIndex];
    gl_Position = vec4(p, 0.0, 1.0);

    // Map NDC [-1,+1] → UV [0,1].  Screen-centre → UV (0.5, 0.5).
    v_uv = p * 0.5 + vec2(0.5);
}

