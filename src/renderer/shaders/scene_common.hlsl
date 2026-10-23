struct SceneOutput {
    precise float4 position : SV_Position;
    float3 normal : NORMAL;
    // Perspective SV_Barycentrics divided by the interpolated clip w are affine
    // in screen space, even across eye/near-plane clipping.
    float clip_w : TEXCOORD0;
    nointerpolation uint instance : TEXCOORD1;
    nointerpolation float selected : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 tangent:TEXCOORD5;
    float3 eye_position:TEXCOORD6;
};
// The shared indexed mesh: welded vertices read by SV_VertexID, one record per
// triangle at first_triangle plus the draw's SV_PrimitiveID.
struct Vertex { float3 position; float3 normal; float2 uv; float4 tangent; };
// Oriented geometric normal; material slot in bits 0-28, polygon edge mask in bits 29-31.
struct Face { float3 normal; uint slot_edges; };
struct Instance {
    row_major float4x4 world;
    float3 normal_x;
    float3 normal_y;
    float3 normal_z;
    float selected;
    float tangent_sign;
};
StructuredBuffer<Vertex> vertices;
StructuredBuffer<Face> faces;
uint first_triangle;
StructuredBuffer<Instance> instances;
// Shading, wireframe, triangle edges, pass (surface / wire lines / depth / studio overlay).
uint4 display;
float2 wire_style;
row_major float4x4 view;
row_major float4x4 projection;
row_major float4x4 scene_view_projection;

float3 unit_normal(float3 normal) {
    float magnitude = max(abs(normal.x), max(abs(normal.y), abs(normal.z)));
    return magnitude > 0 ? normalize(normal / magnitude) : float3(0, 0, 0);
}
