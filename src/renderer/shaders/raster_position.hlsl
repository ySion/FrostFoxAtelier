// Surface, depth and editing passes must compute the same clip position.
// Precise preserves the operation order across separately compiled shaders.
float4 raster_position(float3 position,row_major float4x4 world,row_major float4x4 view_projection) {
    precise float4 world_position=mul(float4(position,1),world);
    precise float4 clip_position=mul(world_position,view_projection);
    return clip_position;
}
