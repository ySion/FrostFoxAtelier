struct ViewportOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
ViewportOutput main(uint vertex_id : SV_VertexID) {
    ViewportOutput result;
    result.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    result.position = float4(result.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return result;
}
