float4 main(UiVertex input) : SV_Target {
    StructuredBuffer<UiInstance> ui_instances = ResourceDescriptorHeap[atelier_safe_desc(g_ui_instances_srv)];
    UiInstance item = ui_instances[input.instance];
    float2 p = input.position.xy;
    uint4 clip = uint4(item.clip.x & 0xffff, item.clip.x >> 16, item.clip.y & 0xffff, item.clip.y >> 16);
    if (any(p < float2(clip.xy)) || any(p >= float2(clip.zw)))
        discard;
    uint shading = item.shading & 7;
    float4 fill = float4(item.tint.rgb * item.tint.a, item.tint.a);
    if (shading == ui_capsule) {
        float2 a = item.uv_bounds.xy;
        float2 ab = item.uv_bounds.zw - a;
        float span = dot(ab, ab);
        float t = span > 0 ? saturate(dot(p - a, ab) / span) : 0;
        return fill * saturate(item.radius.x + .5 - length(p - (a + ab * t)));
    }
    if (shading == ui_node_shape)
        return shade_node_shape(item, p, fill);
    if (shading == ui_arc_sector)
        return shade_arc_sector(item, p, fill);
    if (shading != ui_solid) {
        Texture2D<float4> ui_tex = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(item.shading >> 3))];
        SamplerState ui_sampler = SamplerDescriptorHeap[0];
        float4 sample = ui_tex.SampleLevel(ui_sampler, input.uv, 0);
        if (shading == ui_coverage) {
            float alpha = item.tint.a * sample.r;
            fill = float4(item.tint.rgb * alpha, alpha);
        } else {
            fill = float4(sample.rgb * item.tint.rgb * item.tint.a, sample.a * item.tint.a);
            fill += float4(item.image_background.rgb * item.image_background.a, item.image_background.a) * (1 - fill.a);
        }
    }
    return shade_box(item, p, fill);
}
