#include "shadow_cache.hpp"
#include "math/math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace atelier::renderer::detail {
namespace {
using V=math::Vector3d;
using M=math::Matrix4d;
constexpr std::uint32_t cell_size=128,grid_size=shadow_atlas_extent/cell_size;
constexpr std::uint32_t no_slot=~0u;
void check(bool condition,const char* message) {
    if(!condition)throw Error{{rhi::ErrorCode::invalid_argument,kGfxResult_InvalidParameter,message},false};
}
M matrix(const std::array<float,16>& input) {
    M result;for(std::size_t i=0;i<16;++i)result.elements[i]=input[i];return result;
}
std::array<float,16> packed(const M& input) {
    std::array<float,16> result;
    for(std::size_t i=0;i<16;++i){result[i]=static_cast<float>(input.elements[i]);
        check(std::isfinite(result[i]),"Shadow projection exceeds GPU float representation");}
    return result;
}
double dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
double length(V a){return std::sqrt(dot(a,a));}
V cross(V a,V b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
V unit(V a){const auto l=length(a);check(l>0&&std::isfinite(l),"Invalid shadow basis");return a/l;}
V corner(const ShadowBounds& b,std::uint32_t i) {
    return {i&1?b.maximum[0]:b.minimum[0],i&2?b.maximum[1]:b.minimum[1],i&4?b.maximum[2]:b.minimum[2]};
}
double distance_squared(const ShadowBounds& b,V p) {
    double result=0;for(std::size_t axis=0;axis<3;++axis){
        const double d=std::max(std::max(double(b.minimum[axis])-p[axis],p[axis]-b.maximum[axis]),0.0);result+=d*d;}
    return result;
}
double far_distance(const ShadowBounds& b,V p) {
    double result=0;for(std::uint32_t i=0;i<8;++i)result=std::max(result,length(corner(b,i)-p));return result;
}
// Homogeneous plane rejection is conservative for boxes crossing the eye or
// near plane, unlike projecting only the corners and dividing by w.
bool intersects(const ShadowBounds& b,const M& vp,double padding=0) {
    std::array<bool,6> outside{true,true,true,true,true,true};
    for(std::uint32_t i=0;i<8;++i){const auto p=corner(b,i);
        const auto q=math::Vector4d{p.x,p.y,p.z,1}*vp;
        const double w=q.w*(1+padding);
        // The GPU uses FP32 matrices/world positions. Include dot-product
        // cancellation error instead of assuming the double CPU plane is exact.
        double scale=1;
        for(std::size_t column=0;column<4;++column)scale=std::max(scale,
            std::abs(p.x*vp.elements[column])+std::abs(p.y*vp.elements[4+column])+
            std::abs(p.z*vp.elements[8+column])+std::abs(vp.elements[12+column]));
        const double epsilon=1e-5*scale;
        outside[0]&=q.x< -w-epsilon;outside[1]&=q.x>w+epsilon;
        outside[2]&=q.y< -w-epsilon;outside[3]&=q.y>w+epsilon;
        outside[4]&=q.z< -epsilon;outside[5]&=q.z>q.w+epsilon;
    }
    return std::ranges::none_of(outside,[](bool value){return value;});
}
ShadowBounds expanded(const ShadowBounds& b,double amount) {
    ShadowBounds result;
    for(std::size_t i=0;i<3;++i){
        // Outward rounding keeps the float bound conservative.
        result.minimum[i]=std::nextafter(static_cast<float>(double(b.minimum[i])-amount),-std::numeric_limits<float>::infinity());
        result.maximum[i]=std::nextafter(static_cast<float>(double(b.maximum[i])+amount), std::numeric_limits<float>::infinity());
    }
    return result;
}
double ceil_power_two(double value) {
    check(value>0&&std::isfinite(value),"Invalid shadow projection extent");
    return std::exp2(std::ceil(std::log2(value)));
}
M view(V origin,V forward,V up) {
    const auto z=unit(forward),x=unit(cross(up,z)),y=cross(z,x);
    return {{x.x,y.x,z.x,0,x.y,y.y,z.y,0,x.z,y.z,z.z,0,-dot(origin,x),-dot(origin,y),-dot(origin,z),1}};
}
M point_projection(V origin,std::uint32_t face,double near_z,double far_z) {
    constexpr std::array<V,6> directions{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
    constexpr std::array<V,6> ups{{{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}}};
    const M projection{{1,0,0,0,0,1,0,0,0,0,-near_z/(far_z-near_z),1,0,0,near_z*far_z/(far_z-near_z),0}};
    return view(origin,directions[face],ups[face])*projection;
}
M expanded_projection(M vp,std::uint32_t size) {
    const double scale=double(size-2*shadow_tile_border)/size;
    for(std::size_t row=0;row<4;++row){vp.elements[row*4]*=scale;vp.elements[row*4+1]*=scale;}
    return vp;
}
struct Request {
    std::uint32_t kind{},size{128},count{};
    bool endpoint_visibility{};
    double projected_diameter{};
    std::array<bool,6> needed{};
    std::array<M,6> projection{};
    std::array<std::array<double,2>,6> depth{};
    std::array<float,4> splits{};
    std::array<float,4> starts{};
    // Orthographic matrices are snapped at the allocated resolution.
    V axis_x{},axis_y{},axis_z{};
    std::array<V,4> centers{};
    std::array<double,4> radii{};
};
M request_projection(const Request& request,std::uint32_t face,std::uint32_t size) {
    if(request.kind==1)return request.projection[face];
    const auto x=request.axis_x,y=request.axis_y,z=request.axis_z;
    const auto center=request.centers[face];const double radius=request.radii[face];
    const double texel=2*radius/(size-2*shadow_tile_border);
    const double cx=std::floor(dot(center,x)/texel+.5)*texel;
    const double cy=std::floor(dot(center,y)/texel+.5)*texel;
    const double near_z=request.depth[face][0],far_z=request.depth[face][1];
    return {{x.x/radius,y.x/radius,-z.x/(far_z-near_z),0,
             x.y/radius,y.y/radius,-z.y/(far_z-near_z),0,
             x.z/radius,y.z/radius,-z.z/(far_z-near_z),0,
             -cx/radius,-cy/radius,far_z/(far_z-near_z),1}};
}
V unproject(const M& inverse,double x,double y,double z) {
    const auto p=math::Vector4d{x,y,z,1}*inverse;
    check(std::isfinite(p.w)&&p.w!=0,"Shadow planning needs a finite camera near plane");
    return {p.x/p.w,p.y/p.w,p.z/p.w};
}
Request point_request(const Frame& frame,const GpuLightSource& source,std::span<const ShadowCaster> receivers,const M& camera_view) {
    Request request;request.kind=1;request.count=6;
    const V origin{source.origin_range[0],source.origin_range[1],source.origin_range[2]};
    // Match the FP32 minimum in lit_local_range before widening to double.
    const double effective_range=std::max(source.origin_range[3],.001f);
    const double support=effective_range+source.direction_radius[3];
    const double rounding_margin=std::max(1e-4,1e-5*std::max(std::max(support,std::abs(origin.x)),std::max(std::abs(origin.y),std::abs(origin.z))));
    const double conservative_support=support+rounding_margin;
    const double receiver_bias=std::abs(double(frame.view.lit.shadow_bias));
    const double endpoint_radius=std::max(double(source.direction_radius[3]),receiver_bias*2)+rounding_margin;
    const double far_z=ceil_power_two((conservative_support+receiver_bias)*(1+shadow_filter_tangent_limit));
    const double near_z=std::max(1e-30,std::min(double(shadow_maximum_near),far_z*shadow_near_ratio));
    for(std::uint32_t face=0;face<request.count;++face){request.projection[face]=point_projection(origin,face,near_z,far_z);request.depth[face]={near_z,far_z};}
    for(const auto& receiver:receivers){
        // Recomputed from current bounds/radius each frame, independently of
        // quantized depth projections. Test before range rejection so even an
        // unusually large normal bias cannot hide endpoint-kernel demand.
        request.endpoint_visibility|=distance_squared(receiver.bounds,origin)<=endpoint_radius*endpoint_radius;
        if(distance_squared(receiver.bounds,origin)>conservative_support*conservative_support)continue;
        // PCSS directions extend beyond the receiver's center direction. The
        // shader shares this maximum tangent footprint; no delayed feedback is
        // needed to make neighbouring directions resident in the current frame.
        // The normal offset also increases the distance used by the angular
        // footprint: b + (d+b)*limit, not merely d*limit+b.
        const auto bounds=expanded(receiver.bounds,receiver_bias+
            (far_distance(receiver.bounds,origin)+receiver_bias)*shadow_filter_tangent_limit+rounding_margin);
        for(std::uint32_t face=0;face<6;++face)
            request.needed[face]=request.needed[face]||intersects(bounds,request.projection[face],.04);
    }
    const auto eye=math::Vector4d{origin.x,origin.y,origin.z,1}*camera_view;
    const auto projection=matrix(frame.camera.projection);
    const double pixels=std::max(double(frame.scene_viewport.width)*std::abs(projection.elements[0]),
        double(frame.scene_viewport.height)*std::abs(projection.elements[5]));
    const bool perspective=std::abs(projection.elements[11])>0;
    const double diameter=perspective?pixels*support/std::max(eye.z-support,1e-6):pixels*support;
    request.projected_diameter=diameter;
    request.size=diameter>512?512u:(diameter>192?256u:128u);
    return request;
}
Request directional_request(const Frame& frame,const GpuLightSource& source,std::span<const ShadowCaster> receivers,
                            std::span<const ShadowCaster> casters,const M& camera_view) {
    Request request;request.kind=2;request.count=4;request.size=1024;
    if(receivers.empty())return request;
    const auto inverse_projection=math::try_inverse(matrix(frame.camera.projection));
    const auto inverse_view=math::try_inverse(camera_view);
    check(bool(inverse_projection)&&bool(inverse_view),"Invalid shadow camera inverse");
    std::array<V,4> near_corners,rays;
    double camera_near=std::numeric_limits<double>::infinity(),camera_far=-camera_near;
    for(std::uint32_t i=0;i<4;++i){const double x=i&1?1:-1,y=i&2?1:-1;
        near_corners[i]=unproject(*inverse_projection,x,y,1);
        const auto far_clip=math::Vector4d{x,y,0,1}*(*inverse_projection);
        check(std::isfinite(far_clip.x)&&std::isfinite(far_clip.y)&&std::isfinite(far_clip.z)&&std::isfinite(far_clip.w),
            "Invalid shadow camera far direction");
        // H.xyz-N*H.w is a ray even when the reversed-Z far endpoint is at
        // infinity. Retain the previous division order for finite projections.
        const V homogeneous{far_clip.x,far_clip.y,far_clip.z};
        if(far_clip.w!=0){const auto endpoint=homogeneous/far_clip.w;
            rays[i]=endpoint-near_corners[i];camera_far=std::max(camera_far,endpoint.z);}
        else {rays[i]=homogeneous-near_corners[i]*far_clip.w;camera_far=std::numeric_limits<double>::infinity();}
        camera_near=std::min(camera_near,near_corners[i].z);}
    check(camera_far>camera_near,"Invalid shadow camera depth interval");
    double receiver_far=camera_near;
    for(const auto& receiver:receivers)for(std::uint32_t i=0;i<8;++i){const auto p=corner(receiver.bounds,i);
        receiver_far=std::max(receiver_far,(math::Vector4d{p.x,p.y,p.z,1}*camera_view).z);}
    double receiver_limit=ceil_power_two(std::max(receiver_far,1e-3));
    // A receiver touching a power-of-two near plane must still leave a
    // nonzero cascade interval; finite camera far clipping remains authoritative.
    if(receiver_limit<=camera_near)receiver_limit=ceil_power_two(std::max(std::abs(camera_near),1e-3)*2);
    const double far_z=std::min(camera_far,receiver_limit);
    check(far_z>camera_near,"Invalid shadow receiver depth interval");
    request.axis_z=unit({source.direction_radius[0],source.direction_radius[1],source.direction_radius[2]});
    request.axis_x=unit(cross(std::abs(request.axis_z.y)<.95?V{0,1,0}:V{1,0,0},request.axis_z));
    request.axis_y=cross(request.axis_z,request.axis_x);
    // Stable world-space depth planes include off-camera casters. Quantization
    // avoids rebuilding the static layer for small motion inside scene bounds.
    double low=std::numeric_limits<double>::infinity(),high=-low;
    for(const auto& caster:casters)for(std::uint32_t i=0;i<8;++i){const double z=dot(corner(caster.bounds,i),request.axis_z);
        low=std::min(low,z);high=std::max(high,z);}
    const double step=ceil_power_two(std::max(high-low,1.0))/16;
    const double receiver_bias=std::abs(double(frame.view.lit.shadow_bias));
    low=(std::floor((low-receiver_bias)/step)-1)*step;high=(std::ceil((high+receiver_bias)/step)+1)*step;
    double previous=camera_near;
    constexpr std::array<double,4> fractions{1.0/64,1.0/16,1.0/4,1};
    for(std::uint32_t face=0;face<4;++face){
        const double end=camera_near+(far_z-camera_near)*fractions[face];
        const double start=face==0?camera_near:previous-.1*(previous-camera_near);
        std::array<V,8> corners;V center{};
        for(std::uint32_t i=0;i<8;++i){const auto ray=rays[i%4];
            const double depth=i<4?start:end;check(ray.z!=0,"Invalid shadow frustum ray");
            const auto camera=near_corners[i%4]+ray*((depth-near_corners[i%4].z)/ray.z);
            corners[i]=math::transform_point(camera,*inverse_view);center=center+corners[i];}
        center=center/8.0;double radius=0;for(const auto& p:corners)radius=std::max(radius,length(p-center));
        // The margin covers snapping at the minimum 128 resolution as well as
        // the bounded soft-shadow footprint. It is independent of allocation.
        const double filter_margin=(high-low)*std::min(std::tan(double(source.direction_radius[3])),double(shadow_filter_tangent_limit));
        radius=(std::max(.01,std::ceil(radius*16)/16)+filter_margin+receiver_bias)*1.04;
        request.centers[face]=center;request.radii[face]=radius;request.depth[face]={low,high};
        request.starts[face]=static_cast<float>(start);request.splits[face]=static_cast<float>(end);request.needed[face]=true;previous=end;
    }
    return request;
}
}

struct ShadowCache::State {
    struct Face {
        std::uint32_t slot{no_slot},size{},generation{};
        std::array<float,16> projection{};
        std::uint64_t used{},pinned{};
        bool stationary_valid{},dynamic_valid{},dynamic_nonempty{};
    };
    struct Light {std::array<Face,6> faces;std::uint32_t preferred_size{};};
    struct ReservationUndo {Face* face;Face previous;};
    std::vector<Light> lights;
    std::vector<ReservationUndo> reservation_undo;
    std::vector<Light> previous_lights;
    std::vector<Request> requests;
    std::vector<std::uint32_t> sizes;
    std::vector<ShadowCaster> receivers;
    std::vector<ShadowBatch> recycled_batches;
    std::array<bool,grid_size*grid_size> occupied{};
    ShadowFramePlan plan;
    std::uint64_t scene{},sequence{},batch_serial{1};
    std::uint32_t local_count{},analytic_count{},next_generation{};
    bool initialized{},pending{},plan_ready{};

    static std::uint32_t cache_index(std::uint32_t source,std::uint32_t local,std::uint32_t analytic) {
        return source<analytic?source:(source>=local?analytic+source-local:no_slot);
    }
    std::uint32_t cache_index(std::uint32_t source) const {return cache_index(source,local_count,analytic_count);}
    std::uint32_t generation(){if(++next_generation==0)++next_generation;return next_generation;}

    void mark(const Face& face,bool value) {
        if(face.slot==no_slot)return;
        const auto x=face.slot%grid_size,y=face.slot/grid_size,n=face.size/cell_size;
        for(std::uint32_t row=0;row<n;++row)for(std::uint32_t column=0;column<n;++column)
            occupied[(y+row)*grid_size+x+column]=value;
    }
    void release(Face& face){mark(face,false);face.slot=no_slot;face.stationary_valid=face.dynamic_valid=false;}
    bool allocate(Face& face,std::uint32_t size) {
        const auto n=size/cell_size;
        for(std::uint32_t y=0;y+n<=grid_size;y+=n)for(std::uint32_t x=0;x+n<=grid_size;x+=n){
            bool free=true;for(std::uint32_t row=0;row<n&&free;++row)for(std::uint32_t column=0;column<n;++column)
                if(occupied[(y+row)*grid_size+x+column]){free=false;break;}
            if(!free)continue;
            face.slot=y*grid_size+x;face.size=size;face.stationary_valid=face.dynamic_valid=false;
            face.generation=generation();mark(face,true);return true;
        }
        return false;
    }
    bool evict() {
        Face* oldest=nullptr;
        for(auto& light:lights)for(auto& face:light.faces)
            if(face.slot!=no_slot&&face.pinned!=batch_serial&&(!oldest||face.used<oldest->used))oldest=&face;
        if(!oldest)return false;
        reservation_undo.push_back({oldest,*oldest});release(*oldest);return true;
    }
    bool reserve(std::uint32_t source,const Request& request,std::uint32_t size) {
        reservation_undo.clear();
        source=cache_index(source);
        bool resident=true;
        for(std::uint32_t i=0;i<request.count;++i)if(request.needed[i])
            resident&=lights[source].faces[i].slot!=no_slot&&lights[source].faces[i].size==size;
        if(resident) {
            for(std::uint32_t i=0;i<request.count;++i)if(request.needed[i]) {
                lights[source].faces[i].pinned=batch_serial;lights[source].faces[i].used=sequence;
            }
            return true;
        }
        // At most six faces are one atomic reservation. Rollback is CPU-only,
        // before jobs or descriptors are published; never overwrite this batch.
        const auto saved_occupied=occupied;
        auto& light=lights[source];
        // lights never resizes during a reservation. Record every needed face
        // before pinning, then only record actual evictions; keep journal capacity
        // across calls instead of copying the complete light table per source.
        for(std::uint32_t i=0;i<request.count;++i)if(request.needed[i])
            reservation_undo.push_back({&light.faces[i],light.faces[i]});
        for(std::uint32_t i=0;i<request.count;++i)if(request.needed[i])light.faces[i].pinned=batch_serial;
        for(std::uint32_t i=0;i<request.count;++i)if(request.needed[i]){
            auto& face=light.faces[i];
            if(face.slot!=no_slot&&face.size!=size)release(face);
            if(face.slot==no_slot){
                while(!allocate(face,size))if(!evict()){
                    for(auto undo=reservation_undo.rbegin();undo!=reservation_undo.rend();++undo)*undo->face=undo->previous;
                    occupied=saved_occupied;reservation_undo.clear();return false;
                }
            }
            face.pinned=batch_serial;face.used=sequence;
        }
        reservation_undo.clear();return true;
    }
    ShadowBatch& add_batch(std::size_t sources) {
        if(recycled_batches.empty())plan.batches.emplace_back();
        else {plan.batches.push_back(std::move(recycled_batches.back()));recycled_batches.pop_back();}
        auto& batch=plan.batches.back();batch.sources.clear();batch.faces.clear();batch.updates.clear();
        batch.lights.assign(sources,{});
        batch.endpoint_visibility=false;
        batch.include_emitters=plan.batches.size()==1;++batch_serial;
        if(batch_serial==0){batch_serial=1;for(auto& light:lights)for(auto& face:light.faces)face.pinned=0;}
        return batch;
    }
    void append(std::uint32_t source,const Request& request,std::uint32_t size,std::span<const ShadowCaster> casters) {
        auto& batch=plan.batches.back();batch.sources.push_back(source);
        batch.endpoint_visibility|=request.endpoint_visibility;
        auto& gpu_light=batch.lights[cache_index(source)];gpu_light.meta={request.kind,static_cast<std::uint32_t>(batch.faces.size()),request.count,1};
        gpu_light.splits=request.splits;
        for(std::uint32_t i=0;i<request.count;++i){
            batch.faces.emplace_back();auto& gpu=batch.faces.back();gpu.meta={0,0,source,i};
            if(!request.needed[i])continue;
            ++plan.stats.requested_faces;
            auto& face=lights[cache_index(source)].faces[i];
            const auto vp=request_projection(request,i,size),draw_vp=expanded_projection(vp,size);
            const auto projection=packed(vp);
            if(face.projection!=projection){face.projection=projection;face.stationary_valid=face.dynamic_valid=false;
                face.generation=generation();}
            const std::array<std::uint32_t,4> rect{(face.slot%grid_size)*cell_size,(face.slot/grid_size)*cell_size,size,size};
            const bool reused=face.stationary_valid&&face.dynamic_valid;
            for(const auto layer:{ShadowLayer::stationary,ShadowLayer::dynamic}){
                bool& valid=layer==ShadowLayer::stationary?face.stationary_valid:face.dynamic_valid;
                if(valid)continue;
                ShadowUpdate job;job.face=static_cast<std::uint32_t>(batch.faces.size()-1);job.layer=layer;
                job.rect=rect;job.view_projection=packed(draw_vp);
                for(const auto& caster:casters)if(caster.dynamic==(layer==ShadowLayer::dynamic)&&intersects(caster.bounds,draw_vp))
                    job.instances.push_back(caster.instance);
                if(layer==ShadowLayer::dynamic){face.dynamic_nonempty=!job.instances.empty();++plan.stats.dynamic_updates;}
                else ++plan.stats.static_updates;
                plan.stats.rendered_instances+=static_cast<std::uint32_t>(job.instances.size());
                batch.updates.push_back(std::move(job));valid=true;
            }
            if(reused)++plan.stats.reused_faces;
            gpu.view_projection=projection;
            const float inverse=1.f/shadow_atlas_extent;
            gpu.atlas={float(size-2*shadow_tile_border)*inverse,float(size-2*shadow_tile_border)*inverse,
                float(rect[0]+shadow_tile_border)*inverse,float(rect[1]+shadow_tile_border)*inverse};
            gpu.depth={static_cast<float>(request.depth[i][0]),static_cast<float>(request.depth[i][1]),1.f/float(size-2*shadow_tile_border),request.kind==2?request.starts[i]:0};
            check(std::isfinite(gpu.depth[0])&&std::isfinite(gpu.depth[1])&&gpu.depth[1]>gpu.depth[0],
                "Shadow near/far planes exceed GPU float representation");
            gpu.meta={1u|(face.dynamic_nonempty?2u:0u),face.generation,source,i};
        }
    }
};

ShadowCache::ShadowCache():state_(std::make_unique<State>()){}
ShadowCache::~ShadowCache()=default;
void ShadowCache::invalidate() noexcept {
    auto& s=*state_;s.lights.clear();s.previous_lights.clear();s.occupied.fill(false);s.plan.batches.clear();
    s.recycled_batches.clear();s.initialized=false;s.pending=false;s.plan_ready=false;
}
void ShadowCache::commit() noexcept {if(state_->plan_ready)state_->pending=false;}
const ShadowFramePlan& ShadowCache::prepare(const Frame& frame,const LightFrame& sources,
    std::span<const ShadowCaster> casters,std::span<const ShadowChange> changes,std::uint64_t sequence) {
    auto& s=*state_;
    if(s.plan_ready&&s.initialized&&s.scene==frame.scene_identity&&s.sequence==sequence)return s.plan;
    // An unconsumed plan is never GPU-valid. It can be abandoned safely without
    // allowing its scheduled writes to masquerade as completed cache contents.
    if(s.pending||!s.initialized||s.scene!=frame.scene_identity)invalidate();
    s.recycled_batches.swap(s.plan.batches);s.plan.batches.clear();s.plan.stats={};
    s.scene=frame.scene_identity;s.sequence=sequence;s.initialized=true;s.pending=true;s.plan_ready=false;
    const auto previous_local=s.local_count,previous_analytic=s.analytic_count;
    s.local_count=sources.local_count;s.analytic_count=sources.local_count-sources.emitter_count;
    s.previous_lights.swap(s.lights);auto& previous=s.previous_lights;
    s.lights.assign(std::size_t(s.analytic_count)+sources.directional_count,{});
    for(std::uint32_t i=0;i<s.lights.size();++i){
        const auto source=i<s.analytic_count?i:s.local_count+i-s.analytic_count;
        if(source>=sources.current_to_previous.size())continue;
        const auto old_source=sources.current_to_previous[source];
        if(old_source==no_slot)continue;
        const auto old=State::cache_index(old_source,previous_local,previous_analytic);
        if(old<previous.size())s.lights[i]=std::move(previous[old]);
    }
    s.occupied.fill(false);for(const auto& light:s.lights)for(const auto& face:light.faces)s.mark(face,true);
    for(auto& light:s.lights)for(auto& face:light.faces)if(face.slot!=no_slot){
        const auto vp=expanded_projection(matrix(face.projection),face.size);
        for(const auto& change:changes){
            if(!((change.old_present&&intersects(change.old_bounds,vp))||(change.new_present&&intersects(change.new_bounds,vp))))continue;
            if(change.static_changed)face.stationary_valid=false;
            if(change.dynamic_changed)face.dynamic_valid=false;
        }
    }
    const auto camera_view=matrix(frame.camera.view),camera_vp=camera_view*matrix(frame.camera.projection);
    auto& receivers=s.receivers;receivers.clear();receivers.reserve(casters.size());
    const double camera_padding=2.0/std::max(1.f,std::min(frame.scene_viewport.width,frame.scene_viewport.height));
    for(const auto& caster:casters){
        for(std::size_t axis=0;axis<3;++axis)check(std::isfinite(caster.bounds.minimum[axis])&&std::isfinite(caster.bounds.maximum[axis])&&
            caster.bounds.minimum[axis]<=caster.bounds.maximum[axis],"Invalid shadow caster bounds");
        check(caster.instance<frame.meshes.size(),"Shadow caster instance exceeds current frame");
        if(intersects(caster.bounds,camera_vp,camera_padding))receivers.push_back(caster);
    }
    auto& requests=s.requests;requests.assign(s.lights.size(),{});
    auto& sizes=s.sizes;sizes.assign(s.lights.size(),0);
    std::uint64_t required_cells=0;
    const auto cell_count=[](const Request& request,std::uint32_t size){
        return std::uint64_t(std::count(request.needed.begin(),request.needed.end(),true))*(size/cell_size)*(size/cell_size);};
    for(std::uint32_t index=0;index<s.lights.size();++index){
        const auto source=index<s.analytic_count?index:s.local_count+index-s.analytic_count;
        const auto& light=sources.sources[source];
        if(light.meta[0]==1){requests[index].endpoint_visibility=true;continue;}
        if((light.meta[1]&2u)==0)continue;
        requests[index]=light.meta[0]==0?point_request(frame,light,receivers,camera_view):
            directional_request(frame,light,receivers,casters,camera_view);
        if(requests[index].kind==1){
            auto& request=requests[index];const auto previous_size=s.lights[index].preferred_size;
            const auto diameter=request.projected_diameter;
            if(previous_size==512&&diameter>=384)request.size=512;
            else if(previous_size==256)request.size=diameter>576?512u:(diameter<144?128u:256u);
            else if(previous_size==128)request.size=diameter>576?512u:(diameter>224?256u:128u);
            s.lights[index].preferred_size=request.size;
        }
        sizes[index]=requests[index].size;required_cells+=cell_count(requests[index],sizes[index]);
    }
    // Fit the entire current demand before resorting to sequential batches.
    // Point faces are reduced first; the sun otherwise loses near detail for
    // many unrelated small lamps. The same demand gives the same tier choices.
    while(required_cells>grid_size*grid_size){
        std::size_t selected=sizes.size();std::uint64_t largest=0;
        for(std::uint32_t kind:{1u,2u}){
            for(std::size_t i=0;i<sizes.size();++i)if(requests[i].kind==kind&&sizes[i]>cell_size){
                const auto cost=cell_count(requests[i],sizes[i]);
                if(cost>largest){largest=cost;selected=i;}}
            if(selected!=sizes.size())break;
        }
        if(selected==sizes.size())break;
        required_cells-=largest;sizes[selected]/=2;required_cells+=cell_count(requests[selected],sizes[selected]);
    }
    s.add_batch(s.lights.size());
    for(std::uint32_t index=0;index<s.lights.size();++index){
        const auto source=index<s.analytic_count?index:s.local_count+index-s.analytic_count;
        const auto& light=sources.sources[source];
        if((light.meta[1]&2u)==0||light.meta[0]==1){
            s.append(source,requests[index],0,casters);continue;}
        const auto& request=requests[index];std::uint32_t size=sizes[index];
        while(!s.reserve(source,request,size)){
            if(size>cell_size){size/=2;continue;}
            // One light's six minimum-size faces always fit an empty atlas.
            // A new batch releases previous batch pins, not shadow semantics.
            check(!s.plan.batches.back().sources.empty(),"Shadow atlas cannot fit a minimum light allocation");
            s.add_batch(s.lights.size());++s.plan.stats.pressure_batches;size=sizes[index];
        }
        if(size<request.size)++s.plan.stats.resolution_downgrades;
        bool near_caster=false;
        if(request.kind==1){const V origin{light.origin_range[0],light.origin_range[1],light.origin_range[2]};
            const double near_z=request.depth[0][0];
            for(const auto& caster:casters)if(distance_squared(caster.bounds,origin)<=near_z*near_z){near_caster=true;break;}
            if(near_caster)for(bool needed:request.needed)if(needed)++s.plan.stats.near_plane_faces;
        }
        s.append(source,request,size,casters);
        if(near_caster){auto& batch=s.plan.batches.back();const auto first=batch.lights[index].meta[1];
            for(std::uint32_t face=0;face<request.count;++face)if(request.needed[face])batch.faces[first+face].meta[0]|=4;}
    }
    s.plan_ready=true;return s.plan;
}
}
