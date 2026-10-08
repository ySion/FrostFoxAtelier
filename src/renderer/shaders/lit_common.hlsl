// Shared contract: LH +Y up, row vectors, D3D device Z [0,1]. All images
// cover the window; lit_viewport selects the scene rectangle in physical pixels.
// HDR and all temporal signals are scene linear, before display exposure.
// Motion.xy = previous window UV - current window UV; z = previous device Z
// minus current device Z (a FP16 residual). Decode with current R32 scene depth
// to retain near/far precision; w = stable object/camera correspondence.
// Jitter is in viewport pixel units.
static const float LIT_PI = 3.14159265359;
uint4 lit_size_frame; // width,height,sequence,history valid
float4 lit_viewport; // x,y,width,height
row_major float4x4 lit_view_projection;
row_major float4x4 lit_inverse_view_projection;
row_major float4x4 lit_previous_view_projection;
float4 lit_camera_position;
float4 lit_environment; // RGB radiance, reserved
uint4 lit_counts; // lights,shadow slots,samples,effect flags
float4 lit_controls; // exposure multiplier,shadow bias,AO radius,SSR max distance
float4 lit_temporal; // history weight,roughness threshold,light-change reactive, reserved
float4 lit_jitter; // current xy,previous xy; viewport pixel units

struct LitLight {
    float4 position_range;
    float4 direction_radius;
    float4 color_intensity;
    float4 right_width;
    float4 up_height;
    uint4 meta; // kind(point=0,rect=1,dir=2),shadow slot/~0,flags(two-sided=1,casts-shadow=2),stable hash
};
struct LitInstance {
    row_major float4x4 world;
    row_major float4x4 previous_world;
    float4 normal_x, normal_y, normal_z;
    float4 base_roughness;
    float4 emissive_metallic;
    uint4 geometry; // global RT vertex offset,vertex count,history valid,stable hash
};
struct LitVertex { float3 position; float3 normal; };
StructuredBuffer<LitLight> lit_lights;
StructuredBuffer<LitInstance> lit_instances;
StructuredBuffer<LitVertex> lit_rt_vertices;
RaytracingAccelerationStructure lit_scene;
RWStructuredBuffer<uint> lit_ray_counters; // 0/1 = visibility/probe low words, 2/3 = high words.
Texture2D<float> lit_depth;
Texture2D<float4> lit_normal_roughness;
Texture2D<float4> lit_base_metallic;
Texture2D<float4> lit_emissive;
Texture2D<float4> lit_motion;
SamplerState lit_linear_sampler;

bool lit_inside(uint2 p) {
    return all(p < lit_size_frame.xy) && all(float2(p)+.5 >= lit_viewport.xy) &&
        all(float2(p)+.5 < lit_viewport.xy+lit_viewport.zw);
}
float lit_previous_device_depth(uint2 pixel,float4 motion) {
    return lit_depth.Load(int3(pixel,0))+motion.z;
}
float3 lit_world(float2 window_uv, float device_z) {
    float2 local=(window_uv*lit_size_frame.xy-lit_viewport.xy)/lit_viewport.zw;
    float4 h=mul(float4(local*float2(2,-2)+float2(-1,1),device_z,1),lit_inverse_view_projection);
    return h.xyz/h.w;
}
float2 lit_window_uv(float4 clip) {
    float2 uv=clip.xy/clip.w*float2(.5,-.5)+.5;
    return (uv*lit_viewport.zw+lit_viewport.xy)/lit_size_frame.xy;
}
float3 lit_view_direction(float2 uv) {
    // Near/far unprojection covers perspective and orthographic projections.
    return normalize(lit_world(uv,0)-lit_world(uv,.9999));
}
uint lit_hash(uint x) { x^=x>>16; x*=0x7feb352d; x^=x>>15; x*=0x846ca68b; return x^(x>>16); }
uint lit_light_seed(uint2 pixel,uint light_index) {
    uint sequence=lit_counts.z==64?0:lit_size_frame.z;
    return lit_hash(pixel.x+pixel.y*lit_size_frame.x+sequence*7919+light_index*104729);
}
float lit_random(inout uint seed) { seed=lit_hash(seed); return (seed>>8)*(1.0/16777216.0); }
float3 lit_safe_normal(float3 n) { return n*rsqrt(max(dot(n,n),1e-20)); }
void lit_basis(float3 n,out float3 tangent,out float3 bitangent) {
    tangent=normalize(cross(abs(n.y)<.99?float3(0,1,0):float3(1,0,0),n)); bitangent=cross(n,tangent);
}
float3 lit_brdf(float3 base,float roughness,float metallic,float3 n,float3 v,float3 l) {
    float nl=saturate(dot(n,l)),nv=max(saturate(dot(n,v)),1e-4);
    float3 h=lit_safe_normal(v+l); float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    float a=max(roughness*roughness,.0025),a2=a*a;
    float d=a2/(LIT_PI*pow(nh*nh*(a2-1)+1,2));
    float gv=2*nv/(nv+sqrt(a2+(1-a2)*nv*nv));
    float gl=2*nl/(nl+sqrt(a2+(1-a2)*nl*nl));
    float3 f0=lerp(.04.xxx,base,metallic),f=f0+(1-f0)*pow(1-vh,5);
    return ((1-f)*base*(1-metallic)/LIT_PI+d*gv*gl*f/max(4*nl*nv,1e-5))*nl;
}
void lit_count_rays(uint kind) {
    uint active_rays=WaveActiveCountBits(true);
    if(WaveIsFirstLane()) {
        uint before;InterlockedAdd(lit_ray_counters[kind],active_rays,before);
        if(before>0xffffffffu-active_rays)InterlockedAdd(lit_ray_counters[kind+2],1);
    }
}
bool lit_visible(float3 p,float3 n,float3 l,float distance) {
    lit_count_rays(0);
    RayDesc ray; ray.Origin=p+n*lit_controls.y; ray.Direction=l;
    ray.TMin=lit_controls.y; ray.TMax=max(ray.TMin,distance-lit_controls.y*2);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_FORCE_OPAQUE|RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(lit_scene,0,255,ray); while(query.Proceed()) {}
    return query.CommittedStatus()==COMMITTED_NOTHING;
}
// Returns incoming radiance integrated against the sampling solid angle.
// Rectangle uses uniform area samples, including its emitting cosine and area.
float3 lit_sample_light(LitLight light,float3 p,inout uint seed,out float3 l,out float distance) {
    uint kind=light.meta.x; float3 q=light.position_range.xyz;
    if(kind==2) {
        l=-normalize(light.direction_radius.xyz);
        if(light.direction_radius.w>0) {
            float3 t,b; lit_basis(l,t,b); float r=sqrt(lit_random(seed))*tan(light.direction_radius.w);
            float phi=2*LIT_PI*lit_random(seed); l=normalize(l+r*(cos(phi)*t+sin(phi)*b));
        }
        distance=1e5; return light.color_intensity.rgb*light.color_intensity.w;
    }
    if(kind==1) q+=(lit_random(seed)-.5)*light.right_width.xyz*light.right_width.w+
                       (lit_random(seed)-.5)*light.up_height.xyz*light.up_height.w;
    else if(light.direction_radius.w>0) {
        float z=2*lit_random(seed)-1,phi=2*LIT_PI*lit_random(seed);
        q+=light.direction_radius.w*float3(sqrt(1-z*z)*cos(phi),z,sqrt(1-z*z)*sin(phi));
    }
    float3 delta=q-p; distance=length(delta); l=delta/max(distance,1e-5);
    float cutoff=saturate(1-pow(distance/max(light.position_range.w,1e-3),4)); cutoff*=cutoff;
    float attenuation=cutoff/max(distance*distance,max(.0025,light.direction_radius.w*light.direction_radius.w));
    if(kind==1) {
        float cosine=dot(normalize(light.direction_radius.xyz),-l);
        attenuation*= ((light.meta.z&1)!=0?abs(cosine):saturate(cosine))*light.right_width.w*light.up_height.w;
    }
    return light.color_intensity.rgb*light.color_intensity.w*attenuation;
}
struct LitSurface { float3 position,normal,base,emissive; float roughness,metallic; };
LitSurface lit_hit_surface(uint instance_id,uint primitive,float2 bary,float3 position) {
    LitInstance instance=lit_instances[instance_id]; uint offset=instance.geometry.x+primitive*3;
    LitVertex a=lit_rt_vertices[offset],b=lit_rt_vertices[offset+1],c=lit_rt_vertices[offset+2];
    float3 n=a.normal*(1-bary.x-bary.y)+b.normal*bary.x+c.normal*bary.y;
    LitSurface s; s.position=position;
    s.normal=lit_safe_normal(n.x*instance.normal_x.xyz+n.y*instance.normal_y.xyz+n.z*instance.normal_z.xyz);
    s.base=instance.base_roughness.rgb; s.roughness=instance.base_roughness.w;
    s.emissive=instance.emissive_metallic.rgb; s.metallic=instance.emissive_metallic.w; return s;
}
