// Private atlas ABI in shadow_cache.hpp. ShadowCache's residency bounds
// include the one-texel normal offset used here.
struct LitShadowFace {
    row_major float4x4 view_projection;
    float4 atlas,depth;
    uint4 meta;
};
struct LitShadowLight { uint4 meta;float4 splits; };
StructuredBuffer<LitShadowFace> lit_shadow_faces;
StructuredBuffer<LitShadowLight> lit_shadow_lights; // Indexed by light source.
Texture2D<float> lit_shadow_static;
Texture2D<float> lit_shadow_dynamic;
SamplerComparisonState lit_shadow_sampler; // GREATER_EQUAL: the receiver is at or before the stored occluder.
uint lit_shadow_atlas_extent;

void lit_shadow_count(uint counter) {
    InterlockedAdd(lit_ray_counters[counter],1u);
}
uint lit_shadow_cube_face(float3 direction) {
    float3 a=abs(direction);
    if(a.x>=a.y&&a.x>=a.z)return direction.x>=0?0:1;
    if(a.y>=a.z)return direction.y>=0?2:3;
    return direction.z>=0?4:5;
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
// Device depth of the receiver plane is affine in atlas pixels, including
// perspective projection; gradient is per atlas pixel. Without a usable plane
// the gradient is zero and every texel compares the projected depth.
struct LitShadowReference {
    float depth;
    float2 gradient;
};
// 3x3 tent: four bilinear taps half a texel apart, gathered as four 2x2
// comparison blocks so static and dynamic occlusion combine per texel before
// filtering. Each block compares the plane depth at its centre with no slope
// bias: along the light, a one-texel normal offset w gives w/cos(theta) of depth,
// while the block's texels need at most .71*w*tan(theta), less at every angle.
float lit_shadow_compare(LitShadowFace face,float2 uv,LitShadowReference reference) {
    float extent=float(lit_shadow_atlas_extent);
    float2 at=(uv*face.atlas.xy+face.atlas.zw)*extent;
    float2 base=floor(at-1),f=at-1-base;
    float4 wx=float4(1-f.x,1,f.x,0)*.5,wy=float4(1-f.y,1,f.y,0)*.5;
    bool dynamic=(face.meta.x&2u)!=0;
    float visibility=0;
    [unroll] for(uint block=0;block<4;++block) {
        uint2 b=uint2(block&1,block>>1)*2;
        float2 corner=base+float2(b)+1;
        float depth=reference.depth+dot(reference.gradient,corner-at);
        // Components: x=(0,1) y=(1,1) z=(1,0) w=(0,0) within the block.
        float4 lit=lit_shadow_static.GatherCmpRed(lit_shadow_sampler,corner/extent,depth);
        if(dynamic)lit=min(lit,lit_shadow_dynamic.GatherCmpRed(lit_shadow_sampler,corner/extent,depth));
        float2 x=float2(wx[b.x],wx[b.x+1]),y=float2(wy[b.y],wy[b.y+1]);
        visibility+=dot(lit,float4(x.x*y.y,x.y*y.y,x.y*y.x,x.x*y.x));
    }
    return visibility;
}
float lit_shadow_point(LitShadowLight cache,LitLightSource light,LitLightingSurface surface) {
    // Faces are selected and projected from the origin they were drawn from,
    // which lags a moving light while its redraw waits for budget.
    float3 origin=cache.splits.xyz,normal=surface.geometric_normal;
    float3 delta=surface.position-origin;
    LitShadowFace face=lit_shadow_faces[cache.meta.y+lit_shadow_cube_face(delta)];
    // One interior texel at this distance; each face spans 90 degrees.
    float3 position=surface.position+normal*(2*length(delta)*face.depth.z);
    delta=position-origin;
    face=lit_shadow_faces[cache.meta.y+lit_shadow_cube_face(delta)];
    if((face.meta.x&1u)==0){lit_shadow_count(78);return 0;}
    float2 uv;LitShadowReference reference=(LitShadowReference)0;
    if(!lit_shadow_project(face,position,uv,reference.depth)){lit_shadow_count(78);return 0;}
    float numerator=dot(normal,delta);
    if(abs(numerator)>1e-6*length(delta)) {
        float2 projected_normal=mul(float4(normal,0),face.view_projection).xy;
        float b=face.depth.x*face.depth.y/(face.depth.y-face.depth.x);
        reference.gradient=(2*b/numerator)*projected_normal*float2(1,-1)/(face.atlas.xy*float(lit_shadow_atlas_extent));
    }
    if(!all(isfinite(reference.gradient))){lit_shadow_count(81);reference.gradient=0;}
    return lit_shadow_compare(face,uv,reference);
}
float lit_shadow_directional_cascade(LitShadowLight cache,uint cascade,LitLightSource light,LitLightingSurface surface) {
    float3 normal=surface.geometric_normal,x,y,z;
    LitShadowFace face=lit_shadow_faces[cache.meta.y+cascade];
    lit_shadow_projection_axes(face,x,y,z);
    // Orthographic columns scale by 1/radius: one interior texel in world units.
    float3 position=surface.position+normal*(2*face.depth.z/max(length(x),1e-20));
    float2 uv;LitShadowReference reference=(LitShadowReference)0;
    bool projected=false;
    // A larger cascade covers receivers just outside the selected one.
    [loop] for(uint i=cascade;i<cache.meta.z&&!projected;++i) {
        face=lit_shadow_faces[cache.meta.y+i];
        projected=(face.meta.x&1u)!=0&&lit_shadow_project(face,position,uv,reference.depth);
    }
    if(!projected){lit_shadow_count(78);return 0;}
    lit_shadow_projection_axes(face,x,y,z);
    float3 plane=float3(dot(normal,x)/dot(x,x),dot(normal,y)/dot(y,y),dot(normal,z)/dot(z,z));
    reference.gradient=float2(-2*plane.x,2*plane.y)/plane.z/(face.atlas.xy*float(lit_shadow_atlas_extent));
    if(!all(isfinite(reference.gradient))){lit_shadow_count(81);reference.gradient=0;}
    return lit_shadow_compare(face,uv,reference);
}
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
