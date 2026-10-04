struct SceneOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float4 color : COLOR;
    noperspective float3 barycentric : TEXCOORD0;
    nointerpolation uint edge_mask : TEXCOORD1;
    nointerpolation float selected : TEXCOORD2;
};
// Shading, wireframe, triangle edges, pass (surface / lines / depth).
uint4 display;
float2 wire_style;
row_major float4x4 view;

float3 unit_normal(float3 normal) {
    float magnitude = max(abs(normal.x), max(abs(normal.y), abs(normal.z)));
    return magnitude > 0 ? normalize(normal / magnitude) : float3(0, 0, 0);
}
