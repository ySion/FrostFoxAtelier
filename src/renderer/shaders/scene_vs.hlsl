StructuredBuffer<uint> draw_instances;
uint first_instance;

SceneOutput main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    Vertex vertex = vertices[vertex_id];
    uint index = draw_instances[first_instance + instance_id];
    Instance instance = instances[index];
    SceneOutput o;
    o.position = raster_position(vertex.position,instance.world,scene_view_projection);
    o.far_clip = o.position.z-camera_far_depth*o.position.w;
    o.normal = vertex.normal.x * instance.normal_x + vertex.normal.y * instance.normal_y + vertex.normal.z * instance.normal_z;
    o.tangent=float4(material_transform_tangent(vertex.tangent.xyz,instance.world),vertex.tangent.w*instance.tangent_sign);
    o.eye_position=mul(mul(float4(vertex.position,1),instance.world),view).xyz;
    o.world_position=mul(float4(vertex.position,1),instance.world).xyz;
    o.uv = vertex.uv;
    o.instance = index;
    o.clip_w = o.position.w;
    o.selected = instance.selected;
    o.position = raster_viewport_position(o.position);
    return o;
}
