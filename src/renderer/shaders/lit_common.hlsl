// Shared contract: LH +Y up, row vectors, finite reversed D3D Z: near=1,
// far/background=0. Depth correspondence is FP32 throughout. All images
// cover the window; lit_viewport selects the scene rectangle in physical pixels.
// HDR and all temporal signals are scene linear, before display exposure.
// Motion.xy = previous window UV - current window UV, including sample jitter;
// z = raster coverage (one on geometry, clear zero on background);
// w = stable object/camera and unchanged material correspondence. lit_previous_depth holds the
// expected previous camera device Z of the current surface as R32_FLOAT.
// Jitter is in viewport pixel units.
static const float LIT_PI = 3.14159265359;
uint4 lit_size_frame; // width,height,sequence,history valid
float4 lit_viewport; // x,y,width,height
uint4 lit_screen_dispatch; // 16-aligned pixel origin and exclusive end in window coordinates; lit_inside rejects the margin.
row_major float4x4 lit_view_projection;
row_major float4x4 lit_camera_view;
row_major float4x4 lit_inverse_view_projection;
row_major float4x4 lit_previous_view_projection;
row_major float4x4 lit_previous_inverse_view_projection;
float4 lit_environment; // RGB radiance, reserved
uint lit_effect_flags; // shadows, DDGI, AO, reflections, TAA, bloom, local GI
uint lit_history_invalid; // Per-signal incompatibility: direct=1, reflection=2, AO=4, final TAA=8, SSIL=16.
float4 lit_controls; // exposure multiplier,shadow bias,AO radius,SSR max distance
float4 lit_temporal; // history weight,roughness threshold,radiance change,visibility geometry change
float4 lit_jitter; // current xy,previous xy; viewport pixel units

struct LitInstance {
    row_major float4x4 world;
    row_major float4x4 previous_world;
    float4 normal_x, normal_y, normal_z;
    uint4 geometry; // RT face offset, stable key low, history valid, stable key high; zero key has no correspondence.
};
// Shares the uploaded geometry::Vertex layout with the studio raster path.
// UV and slot lookup are identical in primary and secondary surface evaluation.
struct LitVertex { float3 position; float3 normal; float2 uv; float4 tangent; };
struct LitRayTriangle { LitVertex vertices[3]; float3 geometric_normal; uint closed; uint material_slot; uint closure_first,closure_count,reserved; };
StructuredBuffer<LitInstance> lit_instances;
StructuredBuffer<LitRayTriangle> lit_rt_triangles;
StructuredBuffer<uint> lit_closed_slots;
RaytracingAccelerationStructure lit_scene;
RWStructuredBuffer<uint> lit_ray_counters; // Four low words followed by their four high words.
Texture2D<float> lit_depth;
Texture2D<float> lit_previous_depth;
Texture2D<uint2> lit_surface_id;
Texture2D<uint2> lit_history_surface_id;
Texture2D<float4> lit_normal_roughness;
Texture2D<float2> lit_geometry_normal;
Texture2D<float2> lit_history_geometry_normal;
float2 lit_encode_normal(float3 n) {
    n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-20);
    float2 signs=float2(n.x>=0?1:-1,n.y>=0?1:-1);
    return n.z>=0?n.xy:(1-abs(n.yx))*signs;
}
float3 lit_decode_normal(float2 value) {
    float3 n=float3(value,1-abs(value.x)-abs(value.y));
    float t=saturate(-n.z);n.xy+=float2(n.x>=0?-t:t,n.y>=0?-t:t);
    return normalize(n);
}
float3 lit_geometry_normal_at(uint2 pixel) {return lit_decode_normal(lit_geometry_normal.Load(int3(pixel,0)));}
Texture2D<float4> lit_base_metallic;
Texture2D<float4> lit_emissive;
Texture2D<float4> lit_motion;
SamplerState lit_linear_sampler;

// A valid motion belongs to the current object; it does not identify the
// previous tap. All temporal consumers must validate both halves of its key.
// Callers validate pixel bounds and then apply their signal's geometry gates.
bool lit_history_surface_matches(uint2 current_pixel, uint2 previous_pixel) {
    uint2 identity = lit_surface_id.Load(int3(current_pixel, 0));
    return lit_motion.Load(int3(current_pixel, 0)).w > .5 && any(identity != 0) &&
        all(identity == lit_history_surface_id.Load(int3(previous_pixel, 0)));
}

bool lit_inside(uint2 p) {
    return all(p < lit_size_frame.xy) && all(float2(p)+.5 >= lit_viewport.xy) &&
        all(float2(p)+.5 < lit_viewport.xy+lit_viewport.zw);
}
bool lit_screen_pixel(uint2 thread, out uint2 pixel) {
    pixel = thread + lit_screen_dispatch.xy;
    return all(pixel < lit_screen_dispatch.zw);
}
float lit_depth_near_value() { return 1; }
float lit_depth_far_value() { return 0; }
bool lit_is_background_depth(float depth) { return depth == lit_depth_far_value(); }
float lit_previous_device_depth(uint2 pixel,float4 motion) {
    return lit_previous_depth.Load(int3(pixel,0));
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
    // The same near-to-interior ray covers finite perspective and orthographic
    // projections without magnifying far-plane reconstruction roundoff.
    float middle=lerp(lit_depth_near_value(),lit_depth_far_value(),.5);
    return normalize(lit_world(uv,lit_depth_near_value())-lit_world(uv,middle));
}
uint lit_hash(uint x) { x^=x>>16; x*=0x7feb352d; x^=x>>15; x*=0x846ca68b; return x^(x>>16); }
float lit_random(inout uint seed) { seed=lit_hash(seed); return (seed>>8)*(1.0/16777216.0); }
float3 lit_safe_normal(float3 n) {
    float largest = max(abs(n.x), max(abs(n.y), abs(n.z)));
    if (largest == 0) return 0;
    float3 scaled = n / largest;
    return scaled * rsqrt(dot(scaled, scaled));
}
void lit_basis(float3 n,out float3 tangent,out float3 bitangent) {
    tangent=normalize(cross(abs(n.y)<.99?float3(0,1,0):float3(1,0,0),n)); bitangent=cross(n,tangent);
}
void lit_brdf_components(float3 base,float roughness,float metallic,float3 n,float3 v,float3 l,
                         out float3 diffuse,out float3 specular) {
    float nl=saturate(dot(n,l)),nv=max(saturate(dot(n,v)),1e-4);
    float3 h=lit_safe_normal(v+l); float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    float a=max(roughness*roughness,.0025),a2=a*a;
    float d=a2/(LIT_PI*pow(nh*nh*(a2-1)+1,2));
    float gv=2*nv/(nv+sqrt(a2+(1-a2)*nv*nv));
    float gl=2*nl/(nl+sqrt(a2+(1-a2)*nl*nl));
    float3 f0=lerp(.04.xxx,base,metallic),f=f0+(1-f0)*pow(1-vh,5);
    diffuse=(1-f)*base*(1-metallic)/LIT_PI*nl;
    specular=d*gv*gl*f/max(4*nl*nv,1e-5)*nl;
}
float3 lit_brdf(float3 base,float roughness,float metallic,float3 n,float3 v,float3 l) {
    float3 diffuse,specular;
    lit_brdf_components(base,roughness,metallic,n,v,l,diffuse,specular);
    return diffuse+specular;
}
// Four disjoint counters, with high words in the second half. Classification
// avoids a second atomic increment for every visibility query.
uint lit_visibility_counter; // direct=0, probe shading=2, receiver gather=3
void lit_count_rays(uint kind) {
    uint active_rays=WaveActiveCountBits(true);
    if(WaveIsFirstLane()) {
        uint before;InterlockedAdd(lit_ray_counters[kind],active_rays,before);
        if(before>0xffffffffu-active_rays)InterlockedAdd(lit_ray_counters[kind+4],1);
    }
}
float2 lit_triangle_uv(LitRayTriangle face,float2 bary) {
    return face.vertices[0].uv*(1-bary.x-bary.y)+face.vertices[1].uv*bary.x+face.vertices[2].uv*bary.y;
}
MaterialSurface lit_ray_material(uint instance_id,LitRayTriangle face) {
    return material_surfaces[material_indices[material_offsets[instance_id]+face.material_slot]];
}
bool lit_accept_candidate(uint instance_id,uint primitive,float2 bary) {
    LitRayTriangle face=lit_rt_triangles[lit_instances[instance_id].geometry.x+primitive];
    return material_covered(lit_ray_material(instance_id,face),lit_triangle_uv(face,bary),0,0,0,false);
}
bool lit_visible(float3 p,float3 n,float3 l,float distance) {
    lit_count_rays(lit_visibility_counter);
    RayDesc ray; ray.Origin=p+n*lit_controls.y; ray.Direction=l;
    ray.TMin=lit_controls.y; ray.TMax=max(ray.TMin,distance-lit_controls.y*2);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(lit_scene,0,255,ray); while(query.Proceed()) { if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE &&
        lit_accept_candidate(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),query.CandidateTriangleBarycentrics()))query.CommitNonOpaqueTriangleHit(); }
    return query.CommittedStatus()==COMMITTED_NOTHING;
}
bool lit_visible_target(float3 p, float3 n, float3 target) {
    float bias = max(lit_controls.y, 1e-5);
    RayDesc ray;
    ray.Origin = p + n * bias;
    // Re-aim after offsetting the receiver: an unmodified direction may hit
    // the sampled emitter before TMax, particularly at grazing angles.
    float3 delta = target - ray.Origin;
    float distance = length(delta);
    if (distance <= bias * 2) return true;
    ray.Direction = delta / distance;
    ray.TMin = bias;
    ray.TMax = distance - bias;
    lit_count_rays(lit_visibility_counter);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(lit_scene, 0, 255, ray); while(query.Proceed()) { if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE &&
        lit_accept_candidate(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),query.CandidateTriangleBarycentrics()))query.CommitNonOpaqueTriangleHit(); }
    return query.CommittedStatus() == COMMITTED_NOTHING;
}
struct LitSurface { float3 position,normal,geometric_normal,base; float roughness,metallic; bool closed,double_sided; };
LitSurface lit_hit_surface(uint instance_id,uint primitive,float2 bary,float3 position,float footprint) {
    LitInstance instance=lit_instances[instance_id];
    LitRayTriangle face=lit_rt_triangles[instance.geometry.x+primitive];
    LitVertex a=face.vertices[0],b=face.vertices[1],c=face.vertices[2];
    float3 n=a.normal*(1-bary.x-bary.y)+b.normal*bary.x+c.normal*bary.y;
    LitSurface s; s.position=position;
    s.normal=lit_safe_normal(n.x*instance.normal_x.xyz+n.y*instance.normal_y.xyz+n.z*instance.normal_z.xyz);
    float3 gn=face.geometric_normal;
    s.geometric_normal=lit_safe_normal(gn.x*instance.normal_x.xyz+gn.y*instance.normal_y.xyz+gn.z*instance.normal_z.xyz);
    MaterialSurface surface=lit_ray_material(instance_id,face);
    s.closed=face.closed!=0&&surface.flags.x==0;
    if(s.closed) [loop] for(uint i=0;i<face.closure_count;++i) {
        uint slot=lit_closed_slots[face.closure_first+i];
        if(slot==face.material_slot)continue;
        if(material_surfaces[material_indices[material_offsets[instance_id]+slot]].flags.x!=0) {s.closed=false;break;}
    }
    float3 edge1=mul(float4(b.position-a.position,0),instance.world).xyz;
    float3 edge2=mul(float4(c.position-a.position,0),instance.world).xyz;
    float uv_density=max(length(b.uv-a.uv)/max(length(edge1),1e-6),length(c.uv-a.uv)/max(length(edge2),1e-6));
    MaterialSample material=material_evaluate(surface,lit_triangle_uv(face,bary),0,0,footprint*uv_density,false);
    float4 tangent=a.tangent*(1-bary.x-bary.y)+b.tangent*bary.x+c.tangent*bary.y;
    tangent.xyz=material_transform_tangent(tangent.xyz,instance.world);tangent.w*=instance.normal_x.w;
    s.normal=material_normal(surface,lit_triangle_uv(face,bary),0,0,footprint*uv_density,false,s.normal,tangent,material.roughness);
    s.base=material.base;s.roughness=material.roughness;s.metallic=material.metallic;
    s.double_sided=surface.flags.y!=0;return s;
}
