Texture2D<float4> color_image;
SamplerState image_sampler;
float4 main(UiVertex input) : SV_Target {
    UiInstance item = ui_instances[input.instance];
    float4 sample = color_image.Sample(image_sampler, input.uv);
    float4 fill = float4(sample.rgb * item.tint.rgb * item.tint.a, sample.a * item.tint.a);
    fill += float4(item.image_background.rgb * item.image_background.a, item.image_background.a) * (1 - fill.a);
    return shade_box(item, input.position.xy, fill);
}
