UiVertex main(uint id : SV_VertexID, uint instance : SV_InstanceID) {
    StructuredBuffer<UiInstance> ui_instances = ResourceDescriptorHeap[atelier_safe_desc(g_ui_instances_srv)];
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
    float2 corner = corners[id];
    uint idx = g_first_instance + instance;
    UiInstance item = ui_instances[idx];
    float2 pixel = item.bounds.xy + corner * item.bounds.zw;
    UiVertex o;
    o.position = float4(pixel.x / g_target_size.x * 2 - 1, 1 - pixel.y / g_target_size.y * 2, 0, 1);
    o.uv = item.uv_bounds.xy + corner * item.uv_bounds.zw;
    o.instance = idx;
    return o;
}
