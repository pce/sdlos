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
// Radial animated rings
//   param0  frequency  (0 – 2)  ring density (higher = tighter rings)
//   param1  speed      (0 – 1)  additional animation multiplier

fragment float4 main0(
    VertOut                    in   [[stage_in]],
    constant NodeShaderParams& p    [[buffer(0)]],
    texture2d<float>           tex  [[texture(0)]],
    sampler                    smp  [[sampler(0)]])
{
    float2 uv     = in.uv;
    float4 base   = tex.sample(smp, uv);
    if (base.a < 0.01) return float4(0.0);

    float2 centre = float2(p.u_focusX, p.u_focusY);
    float  freq   = max(p.u_param0, 0.01);
    float  dist   = length(uv - centre) * 8.0 * freq;
    float  ring   = 0.5 + 0.5 * sin(dist - p.u_time * (1.5 + p.u_param1));

    float3 cool = float3(0.10, 0.30, 0.95);
    float3 warm = float3(0.95, 0.55, 0.10);

    return float4(mix(cool, warm, ring), base.a);
}
