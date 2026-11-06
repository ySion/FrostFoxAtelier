Texture2D<float4> cavity_normals;
Texture2D<float4> cavity_color;
Texture2D<float> cavity_depth;
row_major float4x4 cavity_inverse_projection;
float4 cavity_viewport;
float4 cavity_controls; // ridge, valley, radius in physical pixels, unused

bool cavity_inside(int2 p) {
    float2 center=float2(p)+.5;
    return all(center>=cavity_viewport.xy)&&all(center<cavity_viewport.xy+cavity_viewport.zw);
}
float3 cavity_position(float2 pixel,float depth) {
    float2 uv=(pixel-cavity_viewport.xy)/cavity_viewport.zw;
    float4 position=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),cavity_inverse_projection);
    return position.xyz/position.w;
}
float cavity_length(float3 value) {
    float scale=max(abs(value.x),max(abs(value.y),abs(value.z)));
    return scale>0?scale*length(value/scale):0;
}
float cavity_curvature(int2 p,float3 position,float3 normal,float pixel_size,int radius) {
    if(radius==0)return 0;
    static const int2 directions[4]={int2(1,0),int2(-1,0),int2(0,1),int2(0,-1)};
    float result=0;
    [unroll] for(uint i=0;i<4;++i) {
        int2 q=p+directions[i]*radius;
        if(!cavity_inside(q))continue;
        float4 other=cavity_normals.Load(int3(q,0));
        if(other.w==0)continue; // Background and cutout holes are not a second surface.
        if(!all(isfinite(other.xyz))||dot(other.xyz,other.xyz)<.5)continue;
        float3 q_normal=normalize(other.xyz);
        float3 delta=cavity_position(float2(q)+.5,cavity_depth.Load(int3(q,0)))-position;
        float tolerance=pixel_size*radius*4;
        if(!all(isfinite(delta))||abs(dot(delta,normal))>tolerance||abs(dot(delta,q_normal))>tolerance)continue;
        // Signed screen-space normal divergence: outward bending is a ridge,
        // inward bending a valley. Coplanar triangulation contributes zero.
        result+=dot((q_normal.xy-normal.xy)*float2(1,-1),float2(directions[i]));
    }
    return result*.25;
}
float4 main(float4 screen:SV_Position):SV_Target {
    int2 p=int2(screen.xy);
    if(!cavity_inside(p))discard;
    float4 sample=cavity_normals.Load(int3(p,0));
    if(sample.w==0)discard;
    float3 color=cavity_color.Load(int3(p,0)).rgb;
    if(!all(isfinite(sample.xyz))||dot(sample.xyz,sample.xyz)<.5)return float4(color,1);
    float depth=cavity_depth.Load(int3(p,0));
    float3 position=cavity_position(screen.xy,depth),normal=normalize(sample.xyz);
    float pixel_size=max(cavity_length(cavity_position(screen.xy+float2(1,0),depth)-position),
                         cavity_length(cavity_position(screen.xy+float2(0,1),depth)-position));
    if(!all(isfinite(position))||!isfinite(pixel_size)||pixel_size<=0)return float4(color,1);
    int lower=int(floor(cavity_controls.z)),upper=int(ceil(cavity_controls.z));
    float curvature=cavity_curvature(p,position,normal,pixel_size,lower);
    if(upper!=lower)curvature=lerp(curvature,cavity_curvature(p,position,normal,pixel_size,upper),frac(cavity_controls.z));
    // Ignore normal quantization and keep default enhancement restrained.
    float ridge=saturate(max(curvature-.002,0)*cavity_controls.x*1.5)*.35;
    float valley=saturate(max(-curvature-.002,0)*cavity_controls.y*1.5)*.5;
    // Surface color is rendered directly into the MRT attachment. Only geometry
    // reaches this composition; the already drawn background is left intact.
    return float4(lerp(color*(1-valley),float3(1,1,1),ridge),1);
}
