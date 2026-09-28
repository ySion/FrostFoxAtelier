float4 bounds;
float4 uv_bounds;
float2 target_size;
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD; };
Output main(uint id : SV_VertexID) {
    const float2 corners[6] = { float2(0,0), float2(1,0), float2(0,1), float2(0,1), float2(1,0), float2(1,1) };
    float2 corner = corners[id];
    float2 pixel = bounds.xy + corner * bounds.zw;
    Output o;
    o.position = float4(pixel.x / target_size.x * 2 - 1, 1 - pixel.y / target_size.y * 2, 0, 1);
    o.uv = uv_bounds.xy + corner * uv_bounds.zw;
    return o;
}
