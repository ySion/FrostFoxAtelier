#pragma once

namespace rhi_acceptance {

inline constexpr char compute_source[] = R"(
cbuffer RootConstants : register(b0, space0) {
    uint seed;
    uint bias;
    uint values_uav;
    uint mesh_args_uav;
    uint draw_args_uav;
};

[numthreads(1, 1, 1)]
void main() {
    RWStructuredBuffer<uint> values = ResourceDescriptorHeap[values_uav];
    RWStructuredBuffer<uint> mesh_args = ResourceDescriptorHeap[mesh_args_uav];
    RWStructuredBuffer<uint> draw_args = ResourceDescriptorHeap[draw_args_uav];
    values[0] = seed + bias;
    values[1] = 0;
    values[2] = 0;
    values[3] = 1;
    mesh_args[0] = 1;
    mesh_args[1] = 1;
    mesh_args[2] = 1;
    draw_args[0] = 3;
    draw_args[1] = 1;
    draw_args[2] = 0;
    draw_args[3] = 0;
}
)";

inline constexpr char mesh_source[] = R"(
struct Vertex {
    float4 position : SV_Position;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(out vertices Vertex verts[3], out indices uint3 triangles[1]) {
    SetMeshOutputCounts(3, 1);
    verts[0].position = float4(-1, -1, 0.75, 1);
    verts[1].position = float4(-1, 3, 0.75, 1);
    verts[2].position = float4(3, -1, 0.75, 1);
    triangles[0] = uint3(0, 1, 2);
}
)";

inline constexpr char vertex_source[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = id == 0 ? float2(-1, -1) : id == 1 ? float2(-1, 3) : float2(3, -1);
    return float4(p, 0.25, 1);
}
)";

inline constexpr char mesh_pixel_source[] = R"(
cbuffer RootConstants : register(b0, space0) {
    uint values_srv;
};

float4 main() : SV_Target {
    StructuredBuffer<uint> values = ResourceDescriptorHeap[values_srv];
    return float4(float(values[0]) / 255.0, 0, 0, 1);
}
)";

inline constexpr char vertex_pixel_source[] = R"(
cbuffer RootConstants : register(b0, space0) {
    uint values_srv;
};

float4 main() : SV_Target {
    StructuredBuffer<uint> values = ResourceDescriptorHeap[values_srv];
    return float4(0, float(values[0]) / 255.0, 0, 1);
}
)";

} // namespace rhi_acceptance
