float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Linear colors: the existing sRGB render target performs encoding.
    return float4(lerp(float3(.11, .17, .20), float3(.44, .50, .51), saturate(uv.y)), 1);
}
