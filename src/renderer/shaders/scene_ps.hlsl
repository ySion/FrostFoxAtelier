float4 main(float4 position : SV_Position, float3 normal : NORMAL, float4 color : COLOR) : SV_Target {
    float light = 0.3 + 0.7 * saturate(dot(normalize(normal), normalize(float3(0.4, 0.8, -0.6))));
    return float4(color.rgb * light, 1);
}
