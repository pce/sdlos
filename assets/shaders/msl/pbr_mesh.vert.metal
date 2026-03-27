#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
};

struct VertPush {
    float4x4 mvp;    // 64 bytes — combined model-view-projection
    float4x4 model;  // 64 bytes — model matrix for world-space normal transform
};

struct VertOut {
    float4 position     [[position]];
    float3 world_pos;
    float3 world_normal;
    float2 uv;
};

vertex VertOut main0(
    VertexIn in          [[stage_in]],
    constant VertPush& push [[buffer(0)]]
) {
    VertOut out;

    float4 world_pos4 = push.model * float4(in.position, 1.0);
    out.world_pos     = world_pos4.xyz;
    out.position      = push.mvp * float4(in.position, 1.0);

    // Upper 3x3 of model matrix for normal transform (rotation only — no non-uniform scale)
    float3x3 normal_mat = float3x3(
        push.model[0].xyz,
        push.model[1].xyz,
        push.model[2].xyz
    );
    out.world_normal = normalize(normal_mat * in.normal);

    out.uv = in.uv;

    return out;
}
