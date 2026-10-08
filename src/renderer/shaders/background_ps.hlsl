float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Linear colors: the existing sRGB render target performs encoding.
    // Calibrated to the Houdini 22 scene view: #657f8c at the top to #bec6c6 at the bottom.
    return float4(lerp(float3(.13, .212, .262), float3(.515, .565, .565), saturate(uv.y)), 1);
}
