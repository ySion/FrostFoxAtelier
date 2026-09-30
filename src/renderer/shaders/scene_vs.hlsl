struct Vertex { float3 position; float3 normal; };
struct Instance {
    row_major float4x4 world;
    float3 normal_x;
    float3 normal_y;
    float3 normal_z;
    float selected;
    float4 color;
};
StructuredBuffer<Vertex> vertices;
StructuredBuffer<Instance> instances;
row_major float4x4 view;
row_major float4x4 projection;
struct Output { float4 position : SV_Position; float3 normal : NORMAL; float4 color : COLOR; };
Output main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    Vertex vertex = vertices[vertex_id];
    Instance instance = instances[instance_id];
    Output o;
    o.position = mul(mul(mul(float4(vertex.position, 1), instance.world), view), projection);
    o.normal = vertex.normal.x * instance.normal_x + vertex.normal.y * instance.normal_y
             + vertex.normal.z * instance.normal_z;
    o.color = float4(lerp(instance.color.rgb, float3(1, 0.58, 0.08), instance.selected * 0.7), 1);
    return o;
}
