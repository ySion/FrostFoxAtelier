// Surface, depth and editing passes must compute the same clip position.
// Precise preserves the operation order across separately compiled shaders.
float4 raster_position(float3 position, row_major float4x4 world, row_major float4x4 view_projection) {
    precise float4 world_position = mul(float4(position, 1), world);
    precise float4 clip_position = mul(world_position, view_projection);
    return clip_position;
}

float4 raster_viewport_position(float4 clip_position, float4 viewport_transform) {
    // Keep integer viewports bit-identical, including the shared depth path.
    if (all(viewport_transform == float4(1, 1, 0, 0)))
        return clip_position;
    precise float4 result = clip_position;
    result.xy = clip_position.xy * viewport_transform.xy + clip_position.w * viewport_transform.zw;
    return result;
}
