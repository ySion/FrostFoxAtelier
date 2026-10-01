Texture2D<float> coverage;
SamplerState coverage_sampler;
float4 main(UiVertex input) : SV_Target {
    UiInstance item = ui_instances[input.instance];
    float alpha = item.tint.a * coverage.Sample(coverage_sampler, input.uv);
    return shade_box(item, input.position.xy, float4(item.tint.rgb * alpha, alpha));
}
