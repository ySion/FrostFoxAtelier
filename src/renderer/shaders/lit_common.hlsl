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
row_major float4x4 lit_view_projection;
row_major float4x4 lit_inverse_view_projection;
row_major float4x4 lit_previous_view_projection;
row_major float4x4 lit_previous_inverse_view_projection;
float4 lit_camera_position;
float4 lit_environment; // RGB radiance, reserved
uint4 lit_counts; // lights,shadow slots,samples,effect flags
uint lit_history_invalid; // Per-signal incompatibility: direct=1, reflection=2, AO=4, final TAA=8, SSIL=16.
float4 lit_controls; // exposure multiplier,shadow bias,AO radius,SSR max distance
float4 lit_temporal; // history weight,roughness threshold,radiance change,visibility geometry change
float4 lit_jitter; // current xy,previous xy; viewport pixel units

struct LitLight {
    float4 position_range;
    float4 direction_radius;
    float4 color_intensity;
    float4 right_width;
    float4 up_height;
    uint4 meta; // kind(point=0,rect=1,dir=2),shadow slot/~0,flags(two-sided=1,casts-shadow=2),stable hash
};
// Derived from emissive mesh triangles; the material defines emission sidedness.
// The probability is the selected face's PMF, not its alias acceptance.
struct LitEmitter {
    float4 origin_area;
    float4 edge1_probability;
    float4 edge2_accept;
    float3 normal;
    uint alias;
    uint instance,primitive;
    uint2 reserved;
};
uint lit_emitter_count;
struct LitInstance {
    row_major float4x4 world;
    row_major float4x4 previous_world;
    float4 normal_x, normal_y, normal_z;
    uint4 geometry; // global RT face offset,reserved,history valid,stable hash
};
// Shares the uploaded geometry::Vertex layout with the studio raster path.
// UV and slot lookup are identical in primary and secondary surface evaluation.
struct LitVertex { float3 position; float3 normal; float2 uv; float4 tangent; };
struct LitRayTriangle { LitVertex vertices[3]; float3 geometric_normal; uint closed; uint material_slot; uint closure_first,closure_count,reserved; };
StructuredBuffer<LitLight> lit_lights;
StructuredBuffer<LitEmitter> lit_emitters;
StructuredBuffer<LitInstance> lit_instances;
StructuredBuffer<LitRayTriangle> lit_rt_triangles;
StructuredBuffer<uint> lit_closed_slots;
RaytracingAccelerationStructure lit_scene;
RWStructuredBuffer<uint> lit_ray_counters; // Four low words followed by their four high words.
Texture2D<float> lit_depth;
Texture2D<float> lit_previous_depth;
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

bool lit_inside(uint2 p) {
    return all(p < lit_size_frame.xy) && all(float2(p)+.5 >= lit_viewport.xy) &&
        all(float2(p)+.5 < lit_viewport.xy+lit_viewport.zw);
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
uint lit_light_seed(uint2 pixel,uint light_index) {
    uint sequence=lit_counts.z==64?0:lit_size_frame.z;
    return lit_hash(pixel.x+pixel.y*lit_size_frame.x+sequence*7919+light_index*104729);
}
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
uint lit_emitter_sample_count() {
    return lit_emitter_count > 0 ? clamp(lit_counts.z, 4u, 64u) : 0;
}
// Returns Le / solid-angle PDF, including face selection and uniform
// area sampling. The receiver cosine belongs exclusively to lit_brdf_components.
float3 lit_sample_emitter(float3 p, inout uint seed, out float3 direction, out float3 target) {
    direction = 0; target = p;
    if (lit_emitter_count == 0) return 0;
    uint index = min(uint(lit_random(seed) * lit_emitter_count), lit_emitter_count - 1);
    LitEmitter emitter = lit_emitters[index];
    if (lit_random(seed) >= emitter.edge2_accept.w) emitter = lit_emitters[emitter.alias];
    float root = sqrt(lit_random(seed)), split = lit_random(seed);
    target = emitter.origin_area.xyz + root * (1 - split) * emitter.edge1_probability.xyz +
        root * split * emitter.edge2_accept.xyz;
    float3 delta = target - p;
    float distance_squared = dot(delta, delta);
    float area = emitter.origin_area.w, probability = emitter.edge1_probability.w;
    if (distance_squared <= 0 || !isfinite(distance_squared) || area <= 0 || probability <= 0) return 0;
    direction = delta * rsqrt(distance_squared);
    float3 normal = lit_safe_normal(emitter.normal);
    LitRayTriangle face=lit_rt_triangles[lit_instances[emitter.instance].geometry.x+emitter.primitive];
    MaterialSurface surface=lit_ray_material(emitter.instance,face);
    float2 uv=lit_triangle_uv(face,float2(root*(1-split),root*split));
    if(!material_covered(surface,uv,0,0,0,false))return 0;
    float cosine=dot(normal,-direction);
    cosine=surface.flags.z!=0?abs(cosine):saturate(cosine);
    float3 emission=surface.emissive_metallic.rgb*material_read(surface,4,uv,0,0,0,false).rgb;
    return max(emission,0)*(area*cosine/(probability*distance_squared));
}
void lit_emitter_lighting(float3 p, float3 n, float3 geometric_normal, float3 v, float3 base, float roughness, float metallic,
                          bool shadows, bool include_specular, inout uint seed,
                          out float3 diffuse_result, out float3 specular_result) {
    diffuse_result = 0; specular_result = 0;
    uint samples = lit_emitter_sample_count();
    uint candidates = lit_counts.z == 64 ? 1u : 4u;
    [loop] for (uint sample_index = 0; sample_index < samples; ++sample_index) {
        float3 selected_target = p, selected_diffuse = 0, selected_specular = 0;
        float weight_sum = 0, selected_weight = 0;
        [loop] for (uint candidate = 0; candidate < candidates; ++candidate) {
            float3 direction, target;
            float3 incoming = lit_sample_emitter(p, seed, direction, target);
            if (dot(n, direction) <= 0 || dot(geometric_normal,direction)<=0 || !any(incoming > 0)) continue;
            float3 diffuse, specular;
            lit_brdf_components(base, roughness, metallic, n, v, direction, diffuse, specular);
            diffuse *= incoming;
            specular = include_specular ? specular * incoming : 0;
            // These contributions already include the proposal PDF inverse,
            // so luminance is the RIS weight, not a second light-selection PDF.
            float weight = dot(diffuse + specular, float3(.2126, .7152, .0722));
            if (weight <= 0) continue;
            weight_sum += weight;
            bool replace = selected_weight == 0;
            // One-candidate reference mode consumes no reservoir random value.
            if (!replace) replace = lit_random(seed) * weight_sum < weight;
            if (replace) {
                selected_target = target;
                selected_diffuse = diffuse; selected_specular = specular;
                selected_weight = weight;
            }
        }
        if (selected_weight <= 0) continue;
        if (shadows && !lit_visible_target(p, geometric_normal, selected_target)) continue;
        // Zero-contribution candidates still count in M. Only the selected
        // candidate traces visibility; no previous visibility is reused.
        float correction = (weight_sum / selected_weight) / candidates;
        diffuse_result += selected_diffuse * correction;
        specular_result += selected_specular * correction;
    }
    // Rejected and shadowed samples remain in the denominator.
    diffuse_result /= max(samples, 1u);
    specular_result /= max(samples, 1u);
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
