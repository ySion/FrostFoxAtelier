Texture2D<float> coverage;
SamplerState coverage_sampler;
float4 tint;
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    float alpha = tint.a * coverage.Sample(coverage_sampler, uv);
    return float4(tint.rgb * alpha, alpha);
}
