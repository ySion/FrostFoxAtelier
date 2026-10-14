// Surface, depth and editing passes must compute the same clip position.
// Precise preserves the operation order across separately compiled shaders.
float4 raster_position(float3 position,row_major float4x4 world,row_major float4x4 view_projection) {
    precise float4 world_position=mul(float4(position,1),world);
    precise float4 clip_position=mul(world_position,view_projection);
    return clip_position;
}

float4 raster_viewport_transform; // Logical-to-integer hardware viewport: XY scale, XY clip-w offset.
float4 raster_viewport_position(float4 clip_position) {
    // Keep integer viewports bit-identical, including the shared depth path.
    if(all(raster_viewport_transform==float4(1,1,0,0)))return clip_position;
    precise float4 result=clip_position;
    result.xy=clip_position.xy*raster_viewport_transform.xy+clip_position.w*raster_viewport_transform.zw;
    return result; // Z/W, logical projection and temporal correspondence are unchanged.
}
