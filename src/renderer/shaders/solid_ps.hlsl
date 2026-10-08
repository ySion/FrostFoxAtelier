float4 main(UiVertex input) : SV_Target {
    UiInstance item = ui_instances[input.instance];
    float4 fill = float4(item.tint.rgb * item.tint.a, item.tint.a);
    if (item.reserved.x > .5) {
        // Viewport overlay capsule: uv_bounds holds the physical endpoints and
        // radius.x the half width, so one instance draws an anti-aliased stroke.
        float2 p = input.position.xy;
        float2 a = item.uv_bounds.xy, ab = item.uv_bounds.zw - a;
        float span = dot(ab, ab);
        float t = span > 0 ? saturate(dot(p - a, ab) / span) : 0;
        return fill * saturate(item.radius.x + .5 - length(p - (a + ab * t)));
    }
    return shade_box(item, input.position.xy, fill);
}
