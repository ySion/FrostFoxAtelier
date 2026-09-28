float4 tint;
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    return float4(tint.rgb * tint.a, tint.a);
}
