#include <metal_stdlib>
using namespace metal;

// NodeShaderParams — must match render_tree.hh (32 bytes / 8 floats).
struct NodeShaderParams {
    float u_width;    // node width  in physical pixels
    float u_height;   // node height in physical pixels
    float u_focusX;   // normalised focus X  [0..1]
    float u_focusY;   // normalised focus Y  [0..1]
    float u_param0;   // param control 0  (intensity / scale)
    float u_param1;   // param control 1  (speed / amount)
    float u_param2;   // param control 2  (reserved)
    float u_time;     // seconds since app start
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};
// Animated plasma wash
//   param0  intensity  (0 – 2)  colour saturation / brightness scale
//   param1  speed      (0 – 1)  additional animation multiplier

fragment float4 main0(
    VertOut                    in   [[stage_in]],
    constant NodeShaderParams& p    [[buffer(0)]],
    texture2d<float>           tex  [[texture(0)]],
    sampler                    smp  [[sampler(0)]])
{
    float2 uv   = in.uv;
    float4 base = tex.sample(smp, uv);
    if (base.a < 0.01) return float4(0.0);

    float s = max(p.u_param0, 0.01);
    float t = p.u_time * (0.40 + p.u_param1 * 0.60);

    float r = 0.5 + 0.5 * sin(uv.x * 5.0 * s + t);
    float g = 0.5 + 0.5 * sin(uv.y * 4.0 * s + t + 2.094);
    float b = 0.5 + 0.5 * sin((uv.x + uv.y) * 4.0 * s + t + 4.189);

    return float4(r, g, b, base.a);
}
