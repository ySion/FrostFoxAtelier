UiVertex main(uint id : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = { float2(0,0), float2(1,0), float2(0,1), float2(0,1), float2(1,0), float2(1,1) };
    float2 corner = corners[id];
    UiInstance item = ui_instances[first_instance + instance];
    float2 pixel = item.bounds.xy + corner * item.bounds.zw;
    UiVertex o;
    o.position = float4(pixel.x / target_size.x * 2 - 1, 1 - pixel.y / target_size.y * 2, 0, 1);
    o.uv = item.uv_bounds.xy + corner * item.uv_bounds.zw;
    o.instance = first_instance + instance;
    return o;
}
