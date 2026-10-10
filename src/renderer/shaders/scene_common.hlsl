struct SceneOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float4 color : COLOR;
    // Original barycentrics and clip w share perspective interpolation. Their
    // quotient is affine in screen space, even across eye/near-plane clipping.
    float4 edge_coordinates : TEXCOORD0;
    nointerpolation uint edge_mask : TEXCOORD1;
    nointerpolation float selected : TEXCOORD2;
    float2 uv : TEXCOORD3;
};
// Shading, wireframe, triangle edges, pass (surface / lines / depth).
uint4 display;
float2 wire_style;
row_major float4x4 view;

float3 unit_normal(float3 normal) {
    float magnitude = max(abs(normal.x), max(abs(normal.y), abs(normal.z)));
    return magnitude > 0 ? normalize(normal / magnitude) : float3(0, 0, 0);
}
