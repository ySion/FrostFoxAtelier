float4 main(UiVertex input) : SV_Target {
    UiInstance item = ui_instances[input.instance];
    return shade_box(item, input.position.xy, float4(item.tint.rgb * item.tint.a, item.tint.a));
}
