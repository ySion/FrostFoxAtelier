struct Vertex { float3 position; float3 normal; float2 uv; };
struct Triangle { Vertex vertices[3]; uint edge_mask; float3 face_normal; };
struct Instance {
    row_major float4x4 world;
    float3 normal_x;
    float3 normal_y;
    float3 normal_z;
    float selected;
    float4 color;
};
StructuredBuffer<Triangle> triangles;
StructuredBuffer<Instance> instances;
uint first_instance;
uint first_vertex;
row_major float4x4 projection;
SceneOutput main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    vertex_id += first_vertex;
    Triangle face = triangles[vertex_id / 3];
    uint corner = vertex_id % 3;
    Vertex vertex = face.vertices[corner];
    Instance instance = instances[first_instance + instance_id];
    SceneOutput o;
    o.position = mul(mul(mul(float4(vertex.position, 1), instance.world), view), projection);
    float3 normal = display.x == 1 ? face.face_normal : vertex.normal;
    o.normal = normal.x * instance.normal_x + normal.y * instance.normal_y + normal.z * instance.normal_z;
    o.color = instance.color;
    o.uv = vertex.uv;
    o.edge_coordinates = float4(corner == 0, corner == 1, corner == 2, o.position.w);
    o.edge_mask = display.z != 0 ? 7 : face.edge_mask;
    o.selected = instance.selected;
    return o;
}
