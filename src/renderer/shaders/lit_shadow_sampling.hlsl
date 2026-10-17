// Private atlas ABI in shadow_cache.hpp. ShadowCache and this consumer share
// the footprint limit through a uniform populated from the CPU constant.
struct LitShadowFace {
    row_major float4x4 view_projection;
    float4 atlas,depth;
    uint4 meta;
};
struct LitShadowLight { uint4 meta;float4 splits; };
StructuredBuffer<LitShadowFace> lit_shadow_faces;
StructuredBuffer<LitShadowLight> lit_shadow_lights;
Texture2D<float> lit_shadow_static;
Texture2D<float> lit_shadow_dynamic;
uint lit_shadow_enabled,lit_shadow_first_batch;
float lit_shadow_filter_tangent_limit;
uint lit_shadow_atlas_extent;

// World/light anchored taps: no frame-varying rotation or screen jitter.
// Keep the PCSS loops rolled: duplicating receiver correction and cascade
// selection per tap causes excessive DXIL and driver/validation compilation.
static const float2 lit_shadow_disk[16]={
    float2(.1768,0),float2(-.2258,.2068),float2(.0346,-.3938),float2(.2846,.3712),
    float2(-.5222,-.0924),float2(.4947,-.3147),float2(-.1655,.6156),float2(-.3156,-.6076),
    float2(.6846,.2500),float2(-.7127,.2942),float2(.3434,-.7338),float2(.2537,.8080),
    float2(-.7647,-.4432),float2(.8971,-.1972),float2(-.5470,.7831),float2(-.1265,-.9760)};

uint lit_shadow_light_index(uint source) {
    uint analytic=lit_source_counts.x-lit_source_counts.z;
    return source<analytic?source:analytic+source-lit_source_counts.x;
}
bool lit_shadow_batch_contains(uint source) {
    if(lit_shadow_enabled==0)return true;
    return (lit_shadow_lights[lit_shadow_light_index(source)].meta.w&1u)!=0;
}
void lit_shadow_count(uint counter) {
    // Keep wave convergence out of divergent PCSS loops. Per-invocation
    // counters live across the entire filter measured slower on the target GPU.
    InterlockedAdd(lit_ray_counters[counter],1u);
}
uint lit_shadow_cube_face(float3 direction) {
    float3 a=abs(direction);
    if(a.x>=a.y&&a.x>=a.z)return direction.x>=0?0:1;
    if(a.y>=a.z)return direction.y>=0?2:3;
    return direction.z>=0?4:5;
}
float lit_shadow_depth_at(LitShadowFace face,int2 pixel) {
    float depth=lit_shadow_static.Load(int3(pixel,0));
    if((face.meta.x&2u)!=0)depth=max(depth,lit_shadow_dynamic.Load(int3(pixel,0)));
    return depth;
}
bool lit_shadow_project(LitShadowFace face,float3 position,out float2 uv,out float depth) {
    float4 clip=mul(float4(position,1),face.view_projection);
    uv=clip.xy/clip.w*float2(.5,-.5)+.5;depth=clip.z/clip.w;
    return clip.w>0 && all(isfinite(clip)) && all(uv>=-1e-5)&&all(uv<=1.00001) && depth>=0 && depth<=1;
}
void lit_shadow_projection_axes(LitShadowFace face,out float3 x,out float3 y,out float3 z) {
    // The pinned compiler's struct-member scalar indexing transposes these
    // loads relative to mul (confirmed in DXIL). Extract through the same
    // row-vector mul as projection, then assemble ordinary vector components.
    float3 rx=mul(float4(1,0,0,0),face.view_projection).xyz;
    float3 ry=mul(float4(0,1,0,0),face.view_projection).xyz;
    float3 rz=mul(float4(0,0,1,0),face.view_projection).xyz;
    x=float3(rx.x,ry.x,rz.x);y=float3(rx.y,ry.y,rz.y);z=float3(rx.z,ry.z,rz.z);
}
// Device depth of a plane is affine in projected UV, including perspective
// projection. Gradients are per atlas pixel, so all four PCF comparisons and
// blocker classification use the receiver at the fetched texel center.
struct LitShadowReference {
    float depth;
    float2 gradient;
    float fallback_depth;
    float radial; // Point fallback sphere radius; zero for directional maps.
    bool plane;
};
bool lit_shadow_valid_depth(float depth) {return isfinite(depth)&&depth>=0&&depth<=1;}
float lit_shadow_texel_ray_scale(LitShadowFace face,int2 pixel) {
    float2 uv=((float2(pixel)+.5)/float(lit_shadow_atlas_extent)-face.atlas.zw)/face.atlas.xy;
    float2 xy=uv*float2(2,-2)+float2(-1,1);
    // Guard texels remain in this face's expanded raster projection. Their
    // direction can cross a cube edge; max(abs(direction)) would be wrong.
    return sqrt(1+dot(xy,xy));
}
bool lit_shadow_reference_at(LitShadowFace face,int2 pixel,float2 offset, LitShadowReference reference,out float depth) {
    depth=reference.depth+dot(reference.gradient,offset);
    if(reference.plane&&lit_shadow_valid_depth(depth))return true;
    if(reference.plane)lit_shadow_count(81);
    if(reference.radial>0) {
        float n=face.depth.x,f=face.depth.y;
        depth=(-n+n*f*lit_shadow_texel_ray_scale(face,pixel)/reference.radial)/(f-n);
    } else depth=reference.fallback_depth;
    if(lit_shadow_valid_depth(depth))return true;
    lit_shadow_count(78);return false;
}
bool lit_shadow_depth_nearest(LitShadowFace face,float2 uv,LitShadowReference reference,
                             out float depth,out float receiver_depth,out int2 pixel) {
    float2 at=(uv*face.atlas.xy+face.atlas.zw)*float(lit_shadow_atlas_extent);
    pixel=int2(floor(at));depth=0;
    if(!lit_shadow_reference_at(face,pixel,float2(pixel)+.5-at,reference,receiver_depth))return false;
    depth=lit_shadow_depth_at(face,pixel);return true;
}
float lit_shadow_compare(LitShadowFace face,float2 uv,LitShadowReference reference) {
    float2 at=(uv*face.atlas.xy+face.atlas.zw)*float(lit_shadow_atlas_extent)-.5;
    int2 p=int2(floor(at));float2 f=frac(at);
    // Union of static and dynamic occlusion at each tap, before filtering.
    float4 depths;
    if(!lit_shadow_reference_at(face,p,-f,reference,depths.x) ||
       !lit_shadow_reference_at(face,p+int2(1,0),float2(1,0)-f,reference,depths.y) ||
       !lit_shadow_reference_at(face,p+int2(0,1),float2(0,1)-f,reference,depths.z) ||
       !lit_shadow_reference_at(face,p+1,1-f,reference,depths.w))return 0;
    float4 visibility=float4(depths.x>=lit_shadow_depth_at(face,p),
        depths.y>=lit_shadow_depth_at(face,p+int2(1,0)),
        depths.z>=lit_shadow_depth_at(face,p+int2(0,1)),
        depths.w>=lit_shadow_depth_at(face,p+1));
    return lerp(lerp(visibility.x,visibility.y,f.x),lerp(visibility.z,visibility.w,f.x),f.y);
}
float lit_shadow_point_distance(float depth,LitShadowFace face,int2 pixel) {
    float n=face.depth.x,f=face.depth.y;
    float view_z=(n*f)/(depth*(f-n)+n);
    return view_z*lit_shadow_texel_ray_scale(face,pixel);
}
bool lit_shadow_point_tap(LitShadowLight cache,float3 origin,float3 position,float3 normal,float3 direction,
                          out LitShadowFace face,out float2 uv,out LitShadowReference reference) {
    face=lit_shadow_faces[cache.meta.y+lit_shadow_cube_face(direction)];
    uv=0;reference=(LitShadowReference)0;reference.radial=length(position-origin);
    if((face.meta.x&1u)==0){lit_shadow_count(78);return false;}
    // Receiver plane correction prevents a wide filter from comparing every
    // neighboring direction against the center's nonlinear depth.
    float denominator=dot(normal,direction),numerator=dot(normal,position-origin);
    float intersection=abs(denominator)>1e-5?numerator/denominator:-1;
    if(isfinite(intersection)&&intersection>0 &&
        lit_shadow_project(face,origin+direction*intersection,uv,reference.depth)) {
        float2 projected_normal=mul(float4(normal,0),face.view_projection).xy;
        float b=face.depth.x*face.depth.y/(face.depth.y-face.depth.x);
        reference.gradient=(2*b/numerator)*projected_normal*float2(1,-1)/
            (face.atlas.xy*float(lit_shadow_atlas_extent));
        reference.plane=all(isfinite(reference.gradient));
        if(reference.plane)return true;
    }
    // A grazing receiver plane can intersect behind the source or outside the
    // face near/far interval. Revert this tap to radial comparison, never clamp
    // an invalid reference depth into an unrelated occlusion decision.
    lit_shadow_count(81);
    reference.gradient=0;reference.plane=false;
    if(!lit_shadow_project(face,origin+direction*reference.radial,uv,reference.depth)){
        lit_shadow_count(78);return false;
    }
    return true;
}
float lit_shadow_point(LitShadowLight cache,LitLightSource light,LitLightingSurface surface) {
    float3 origin=light.origin_range.xyz,position=surface.position+surface.geometric_normal*lit_controls.y;
    float3 delta=position-origin;float distance=length(delta);float3 direction=delta/max(distance,1e-20);
    float3 tangent,bitangent;lit_basis(direction,tangent,bitangent);
    float radius=light.direction_radius.w;
    LitShadowFace face;float2 uv;LitShadowReference reference;
    if(radius<=0) {
        if(!lit_shadow_point_tap(cache,origin,position,surface.geometric_normal,direction,face,uv,reference))return 0;
        return lit_shadow_compare(face,uv,reference);
    }
    // Search from the finite-source footprint. Clamp is an explicit quality
    // bound shared with CPU residency, never an excuse to read an absent face.
    float near_z=lit_shadow_faces[cache.meta.y+lit_shadow_cube_face(direction)].depth.x;
    float search=radius*max(distance-near_z,0)/max(near_z*distance,1e-20);
    search=min(search,lit_shadow_filter_tangent_limit);
    float local_search=min(radius/max(distance-radius,1e-5),search);
    float blockers=0,blocker_distance=0;
    [loop] for(uint i=0;i<8;++i) {
        // Keep center/local coverage while also looking for off-axis blockers
        // near the source. A single receiver-sized disk misses those blockers.
        float2 offset=i==0?0:lit_shadow_disk[i*2]*(i<4?local_search:(i<6?sqrt(local_search*search):search));
        float3 sample_direction=normalize(direction+tangent*offset.x+bitangent*offset.y);
        if(!lit_shadow_point_tap(cache,origin,position,surface.geometric_normal,sample_direction,face,uv,reference))return 0;
        float depth,receiver_depth;int2 pixel;
        if(!lit_shadow_depth_nearest(face,uv,reference,depth,receiver_depth,pixel))return 0;
        if(depth>receiver_depth && depth>0){blocker_distance+=lit_shadow_point_distance(depth,face,pixel);blockers+=1;}
    }
    if(blockers==0)return 1;
    blocker_distance/=blockers;
    float filter=radius*max(distance-blocker_distance,0)/max(blocker_distance*distance,1e-10);
    if(filter>lit_shadow_filter_tangent_limit)lit_shadow_count(79);
    filter=min(filter,lit_shadow_filter_tangent_limit);
    float visibility=0;
    [loop] for(uint i=0;i<16;++i) {
        float2 offset=lit_shadow_disk[i]*filter;
        float3 sample_direction=normalize(direction+tangent*offset.x+bitangent*offset.y);
        if(!lit_shadow_point_tap(cache,origin,position,surface.geometric_normal,sample_direction,face,uv,reference))return 0;
        visibility+=lit_shadow_compare(face,uv,reference);
    }
    return visibility/16;
}

bool lit_shadow_directional_project(LitShadowLight cache,uint cascade,float3 position,
                                out LitShadowFace face,out float2 uv,out float reference_depth) {
    uv=0;reference_depth=0;face=(LitShadowFace)0;
    [loop] for(uint i=cascade;i<cache.meta.z;++i) {
        face=lit_shadow_faces[cache.meta.y+i];
        if((face.meta.x&1u)!=0 && lit_shadow_project(face,position,uv,reference_depth))return true;
    }
    return false;
}
bool lit_shadow_directional_tap(LitShadowLight cache,uint cascade,float3 position,float3 offset,
                               float3 normal,float3 direction,
                               out LitShadowFace face,out float2 uv,out LitShadowReference reference) {
    reference=(LitShadowReference)0;
    float denominator=dot(normal,direction);
    if(abs(denominator)>1e-5) {
        float3 corrected=offset-direction*(dot(normal,offset)/denominator);
        if(all(isfinite(corrected))&&lit_shadow_directional_project(cache,cascade,position+corrected,face,uv,reference.depth)) {
            // Use the actual selected cascade, including a larger fallback.
            // Orthographic columns are orthogonal scaled light basis vectors.
            float3 x,y,z;lit_shadow_projection_axes(face,x,y,z);
            float3 plane=float3(dot(normal,x)/dot(x,x),dot(normal,y)/dot(y,y),dot(normal,z)/dot(z,z));
            reference.gradient=float2(-2*plane.x,2*plane.y)/plane.z/(face.atlas.xy*float(lit_shadow_atlas_extent));
            reference.fallback_depth=mul(float4(position+offset,1),face.view_projection).z;
            reference.plane=all(isfinite(reference.gradient));
            if(reference.plane)return true;
        }
    }
    lit_shadow_count(81);
    reference.gradient=0;reference.plane=false;
    if(lit_shadow_directional_project(cache,cascade,position+offset,face,uv,reference.depth)) {
        reference.fallback_depth=reference.depth;return true;
    }
    lit_shadow_count(78);return false;
}
float lit_shadow_directional_cascade(LitShadowLight cache,uint cascade,LitLightSource light,LitLightingSurface surface) {
    LitShadowFace face=lit_shadow_faces[cache.meta.y+cascade];
    float3 position=surface.position+surface.geometric_normal*lit_controls.y;
    float2 uv;LitShadowReference center_reference;
    if(!lit_shadow_directional_tap(cache,cascade,position,0,surface.geometric_normal,light.direction_radius.xyz,face,uv,center_reference))return 0;
    if(light.direction_radius.w<=0)return lit_shadow_compare(face,uv,center_reference);
    float3 axis_x,axis_y,axis_z;lit_shadow_projection_axes(face,axis_x,axis_y,axis_z);
    float3 tangent=normalize(axis_x),bitangent=normalize(axis_y);
    float angle=tan(light.direction_radius.w);
    if(angle>lit_shadow_filter_tangent_limit)lit_shadow_count(79);
    angle=min(angle,lit_shadow_filter_tangent_limit);
    float receiver_z=face.depth.y-center_reference.depth*(face.depth.y-face.depth.x);
    float search=angle*max(receiver_z-face.depth.x,0),blockers=0,separation=0;
    float3 normal=surface.geometric_normal;
    [loop] for(uint i=0;i<8;++i) {
        float2 disk=lit_shadow_disk[i*2]*search;
        float3 offset=tangent*disk.x+bitangent*disk.y;
        LitShadowFace tap;float2 tap_uv;LitShadowReference reference;
        if(!lit_shadow_directional_tap(cache,cascade,position,offset,normal,light.direction_radius.xyz,tap,tap_uv,reference))return 0;
        float depth,receiver_depth;int2 pixel;
        if(!lit_shadow_depth_nearest(tap,tap_uv,reference,depth,receiver_depth,pixel))return 0;
        if(depth>receiver_depth && depth>0){separation+=(depth-receiver_depth)*(tap.depth.y-tap.depth.x);blockers+=1;}
    }
    if(blockers==0)return 1;
    float filter=angle*separation/blockers,visibility=0;
    [loop] for(uint i=0;i<16;++i) {
        float2 disk=lit_shadow_disk[i]*filter;
        float3 offset=tangent*disk.x+bitangent*disk.y;
        LitShadowFace tap;float2 tap_uv;LitShadowReference reference;
        if(!lit_shadow_directional_tap(cache,cascade,position,offset,normal,light.direction_radius.xyz,tap,tap_uv,reference))return 0;
        visibility+=lit_shadow_compare(tap,tap_uv,reference);
    }
    return visibility/16;
}
// Cache-only call graph: endpoint RT belongs to the residual shader entry.
float lit_shadow_directional(LitShadowLight cache,LitLightSource light,LitLightingSurface surface) {
    float view_z=mul(float4(surface.position,1),lit_camera_view).z;
    uint cascade=0;[unroll] for(uint i=0;i<3;++i)if(i+1<cache.meta.z&&view_z>cache.splits[i])cascade=i+1;
    float visibility=lit_shadow_directional_cascade(cache,cascade,light,surface);
    if(cascade+1<cache.meta.z) {
        LitShadowFace face=lit_shadow_faces[cache.meta.y+cascade];
        float width=max((cache.splits[cascade]-face.depth.w)*.1,1e-5);
        float blend=saturate((view_z-cache.splits[cascade]+width)/width);
        if(blend>0)visibility=lerp(visibility,lit_shadow_directional_cascade(cache,cascade+1,light,surface),blend);
    }
    return visibility;
}
