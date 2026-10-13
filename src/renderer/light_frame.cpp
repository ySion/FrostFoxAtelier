#include "light_frame.hpp"
#include "math/math.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace atelier::renderer::detail {
namespace {
void check(bool condition,const char* message) {
    if(!condition)throw Error{{rhi::ErrorCode::invalid_argument,kGfxResult_InvalidParameter,message},false};
}
float gpu_float(double value) {
    check(std::isfinite(value)&&std::abs(value)<=std::numeric_limits<float>::max(),
        "Light source exceeds GPU float representation");
    return static_cast<float>(value);
}
void source_capacity(std::size_t count) {
    // GFX structured buffers use uint32 element counts and uint64 byte sizes.
    // Reserve ~0 as the invalid index; allocation limits remain RHI-owned.
    check(count<no_light_source && count<=std::numeric_limits<std::size_t>::max()/sizeof(GpuLightSource),
        "Light source buffer exceeds addressable storage");
}
double luminance(const std::array<float,4>& value) {
    return .2126*value[0]+.7152*value[1]+.0722*value[2];
}
double emission_luminance(const material::Description& surface) {
    const auto& image=surface.texture(material::Input::emissive).image;
    const auto mean=image?image->mean_linear():std::array<float,3>{1,1,1};
    const auto& emission=surface.parameters.emissive;
    return .2126*emission[0]*mean[0]+.7152*emission[1]*mean[1]+.0722*emission[2]*mean[2];
}
GpuLightSource analytic_source(const Light& light) {
    GpuLightSource result;
    result.origin_range={light.position[0],light.position[1],light.position[2],light.range};
    result.direction_radius={light.direction[0],light.direction[1],light.direction[2],light.source_radius};
    for(std::size_t axis=0;axis<3;++axis) {
        result.radiance[axis]=gpu_float(double(light.color[axis])*light.intensity);
        if(light.kind==LightKind::rectangle) {
            result.edge1_area[axis]=gpu_float(double(light.right[axis])*light.width);
            result.edge2[axis]=gpu_float(double(light.up[axis])*light.height);
        }
    }
    result.edge1_area[3]=light.kind==LightKind::rectangle?gpu_float(double(light.width)*light.height):0;
    result.meta={static_cast<std::uint32_t>(light.kind),(light.two_sided?1u:0u)|(light.casts_shadow?2u:0u),no_light_source,0};
    return result;
}
double analytic_power(const GpuLightSource& source) {
    const double flux=source.meta[0]==1
        ?std::numbers::pi*source.edge1_area[3]*((source.meta[1]&1u)?2:1)
        :4*std::numbers::pi;
    return std::max(0.,luminance(source.radiance)*flux);
}
std::size_t mix_hash(std::size_t hash,std::size_t value) noexcept {
    return hash^(value+std::size_t{0x9e3779b9u}+(hash<<6)+(hash>>2));
}
}

std::size_t LightFrame::KeyHash::operator()(const ObjectKey& key) const noexcept {
    return mix_hash(std::hash<std::uint64_t>{}(key.value),key.legacy);
}
std::size_t LightFrame::KeyHash::operator()(const SourceKey& key) const noexcept {
    auto hash=std::hash<std::uint64_t>{}(key.owner);
    hash=mix_hash(hash,std::hash<const geometry::Mesh*>{}(key.geometry));
    return mix_hash(mix_hash(hash,key.primitive),key.kind);
}

void LightFrame::clear() {
    sources.clear();previous_sources.clear();proposals.clear();
    current_to_previous.clear();previous_to_current.clear();
    objects_.clear();object_order_.clear();next_object_order_.clear();
    analytic_.clear();next_analytic_.clear();analytic_keys_.clear();next_analytic_keys_.clear();
    identities_.clear();previous_identities_.clear();descriptions_.clear();previous_descriptions_.clear();
    geometry_owners_.clear();previous_geometry_owners_.clear();previous_lookup_.clear();
    powers_.clear();scaled_.clear();actual_.clear();below_.clear();above_.clear();
    local_count=directional_count=emitter_count=0;scene_identity_=frame_id_=0;
    valid_=advance_previous_=changed=false;
}

void LightFrame::update(const Frame& frame,const MaterialFrame& materials,std::uint64_t sequence) {
    try {update_impl(frame,materials,sequence);}
    catch(...) {clear();throw;}
}

void LightFrame::update_impl(const Frame& frame,const MaterialFrame& materials,std::uint64_t sequence) {
    if(valid_&&scene_identity_!=frame.scene_identity)clear();
    source_capacity(frame.lights.size());
    bool rebuild=!valid_;
    next_analytic_.clear();next_analytic_keys_.clear();
    // Keep the same order inside each partition. Stable identity, not the
    // packed index, determines temporal correspondence after any reorder.
    for(bool directional:{false,true})for(const auto& light:frame.lights) {
        if((light.kind==LightKind::directional)!=directional)continue;
        next_analytic_.push_back(analytic_source(light));
        next_analytic_keys_.push_back({light.stable_key,nullptr,0,static_cast<std::uint32_t>(light.kind)});
    }
    rebuild|=next_analytic_!=analytic_ || next_analytic_keys_!=analytic_keys_;
    analytic_.swap(next_analytic_);analytic_keys_.swap(next_analytic_keys_);

    for(auto& [key,object]:objects_){(void)key;object.used=false;}
    next_object_order_.clear();
    check(materials.offsets.size()>=frame.meshes.size(),"Light snapshot has no matching material frame");
    for(std::size_t index=0;index<frame.meshes.size();++index) {
        const auto& mesh=frame.meshes[index];
        const auto slots=mesh.geometry->materials().size();
        const auto first=std::size_t(materials.offsets[index]);
        check(first<=materials.descriptions.size() && slots<=materials.descriptions.size()-first &&
              first<=materials.indices.size() && slots<=materials.indices.size()-first,
              "Light snapshot material range is invalid");
        bool emitting=false;
        for(std::size_t slot=0;slot<slots;++slot)emitting|=emission_luminance(materials.surface(index,slot))>0;
        if(!emitting)continue;
        const ObjectKey key{mesh.stable_key?mesh.stable_key:static_cast<std::uint64_t>(index),mesh.stable_key==0};
        auto [at,added]=objects_.try_emplace(key);auto& object=at->second;
        check(!object.used,"Duplicate emissive object identity");object.used=true;
        next_object_order_.push_back(key);
        bool object_changed=added||object.geometry!=mesh.geometry||object.world!=mesh.world||object.descriptions.size()!=slots;
        if(!object_changed)for(std::size_t slot=0;slot<slots;++slot) {
            object_changed|=object.descriptions[slot]!=materials.surface(index,slot)||
                object.material_indices[slot]!=materials.indices[first+slot];
        }
        if(object_changed){rebuild_object(object,mesh,materials,index);rebuild=true;}
    }
    for(auto it=objects_.begin();it!=objects_.end();) {
        if(it->second.used){++it;continue;}it=objects_.erase(it);rebuild=true;
    }
    rebuild|=object_order_!=next_object_order_;object_order_.swap(next_object_order_);
    // Several passes may request the same immutable frame snapshot. Preserve
    // both its previous-frame data and its publication flag on repeated calls.
    if(!rebuild && sequence==frame_id_)return;
    changed=rebuild||advance_previous_;
    if(!rebuild) {
        frame_id_=sequence;
        if(advance_previous_) {
            previous_sources=sources;
            current_to_previous.resize(sources.size());previous_to_current.resize(sources.size());
            for(std::size_t i=0;i<sources.size();++i) {
                const auto index=identities_[i].key.owner?static_cast<std::uint32_t>(i):no_light_source;
                current_to_previous[i]=previous_to_current[i]=index;
            }
            previous_geometry_owners_.clear();previous_descriptions_.clear();previous_identities_.clear();
            advance_previous_=false;
        }
        return;
    }

    previous_sources.swap(sources);sources.clear();
    previous_identities_.swap(identities_);identities_.clear();
    previous_descriptions_.swap(descriptions_);descriptions_.clear();
    previous_geometry_owners_.swap(geometry_owners_);geometry_owners_.clear();
    powers_.clear();emitter_count=directional_count=0;
    const auto append=[&](const GpuLightSource& source,Identity identity,double power) {
        source_capacity(sources.size()+1);sources.push_back(source);identities_.push_back(identity);
        if(source.meta[0]!=2)powers_.push_back(power);
    };
    for(std::size_t i=0;i<analytic_.size();++i)if(analytic_[i].meta[0]!=2)
        append(analytic_[i],{analytic_keys_[i]},analytic_power(analytic_[i]));
    for(const auto& key:object_order_) {
        const auto& object=objects_.at(key);const auto first=descriptions_.size();
        check(first+object.descriptions.size()<no_light_source,"Light material snapshot exceeds addressable storage");
        descriptions_.insert(descriptions_.end(),object.descriptions.begin(),object.descriptions.end());
        geometry_owners_.push_back(object.geometry);
        for(const auto& emitter:object.emitters) {
            append(emitter.source,{{key.legacy?0:key.value,object.geometry.get(),emitter.primitive,3},
                static_cast<std::uint32_t>(first+emitter.material_slot)},emitter.power);
            ++emitter_count;
        }
    }
    local_count=static_cast<std::uint32_t>(sources.size());
    for(std::size_t i=0;i<analytic_.size();++i)if(analytic_[i].meta[0]==2) {
        append(analytic_[i],{analytic_keys_[i]},0);++directional_count;
    }
    build_proposals();map_previous();
    scene_identity_=frame.scene_identity;frame_id_=sequence;valid_=true;advance_previous_=true;
}

void LightFrame::rebuild_object(CachedObject& object,const MeshInstance& mesh,const MaterialFrame& materials,std::size_t index) {
    object.geometry=mesh.geometry;object.world=mesh.world;
    const auto slots=mesh.geometry->materials().size();object.descriptions.resize(slots);object.material_indices.resize(slots);
    for(std::size_t slot=0;slot<slots;++slot) {
        object.descriptions[slot]=materials.surface(index,slot);
        object.material_indices[slot]=materials.indices[materials.offsets[index]+slot];
        check(object.material_indices[slot]<materials.surfaces.size(),"Emissive source material index is invalid");
    }
    math::Matrix4d world;for(std::size_t i=0;i<16;++i)world.elements[i]=mesh.world[i];
    const auto inverse=math::try_inverse(world);check(bool(inverse),"Emissive source transform is singular");
    object.emitters.clear();const auto vertices=mesh.geometry->vertices();
    for(std::size_t first=0;first<vertices.size();first+=3) {
        const auto slot=mesh.geometry->material_slot(first/3);const auto& surface=object.descriptions[slot];
        const double emission=emission_luminance(surface);if(emission<=0)continue;
        const auto* vertex=vertices.data()+first;
        const auto position=[&](std::size_t i) {return math::Vector3d{vertex[i].position[0],vertex[i].position[1],vertex[i].position[2]};};
        const auto a=math::transform_point(position(0),world),b=math::transform_point(position(1),world),c=math::transform_point(position(2),world);
        const auto edge1=b-a,edge2=c-a;const double area=.5*math::length(math::cross(edge1,edge2));
        if(area==0)continue;
        const float gpu_area=gpu_float(area);if(gpu_area==0)continue;
        auto normal=math::cross(position(1)-position(0),position(2)-position(0));
        const math::Vector3d authored{double(vertex[0].normal[0])+vertex[1].normal[0]+vertex[2].normal[0],
            double(vertex[0].normal[1])+vertex[1].normal[1]+vertex[2].normal[1],double(vertex[0].normal[2])+vertex[1].normal[2]+vertex[2].normal[2]};
        if(math::dot(normal,authored)<0)normal=normal*-1.;
        const double local_length=math::length(normal);if(local_length==0)continue;normal=normal/local_length;
        // The inverse transpose preserves the authored emitting front under
        // mirrors. A cross of world edges alone would reverse that front.
        std::array<double,3> transformed{};
        for(std::size_t axis=0;axis<3;++axis)transformed[axis]=normal.x*inverse->elements[axis*4]+
            normal.y*inverse->elements[axis*4+1]+normal.z*inverse->elements[axis*4+2];
        const double normal_length=std::hypot(transformed[0],transformed[1],transformed[2]);
        check(std::isfinite(normal_length)&&normal_length>0,"Emissive source normal is invalid");
        CachedEmitter emitter;emitter.primitive=static_cast<std::uint32_t>(first/3);emitter.material_slot=slot;
        auto& source=emitter.source;
        source.origin_range={gpu_float(a.x),gpu_float(a.y),gpu_float(a.z),0};
        source.edge1_area={gpu_float(edge1.x),gpu_float(edge1.y),gpu_float(edge1.z),gpu_area};
        source.edge2={gpu_float(edge2.x),gpu_float(edge2.y),gpu_float(edge2.z),0};
        for(std::size_t axis=0;axis<3;++axis){source.direction_radius[axis]=gpu_float(transformed[axis]/normal_length);
            source.radiance[axis]=surface.parameters.emissive[axis];}
        source.uv01={vertex[0].uv[0],vertex[0].uv[1],vertex[1].uv[0],vertex[1].uv[1]};
        source.uv2={vertex[2].uv[0],vertex[2].uv[1],0,0};
        source.meta={3,(surface.parameters.emission_two_sided?1u:0u)|2u,object.material_indices[slot],0};
        emitter.power=area*emission*std::numbers::pi*(surface.parameters.emission_two_sided?2:1);
        object.emitters.push_back(emitter);
    }
}

void LightFrame::build_proposals() {
    proposals.assign(local_count,{});if(local_count==0)return;
    scaled_.resize(local_count);actual_.assign(local_count,0);below_.clear();above_.clear();
    double total=0;for(double power:powers_){check(std::isfinite(power)&&power>=0,"Light proposal power is invalid");total+=power;}
    check(std::isfinite(total),"Light proposal power sum is invalid");
    for(std::uint32_t i=0;i<local_count;++i) {
        scaled_[i]=total>0?.95*(powers_[i]/total)*local_count+.05:1;
        proposals[i].accept=1;proposals[i].alias=i;
        (scaled_[i]<1?below_:above_).push_back(i);
    }
    while(!below_.empty()&&!above_.empty()) {
        const auto underfull=below_.back();below_.pop_back();const auto large=above_.back();above_.pop_back();
        proposals[underfull].accept=static_cast<float>(std::clamp(scaled_[underfull],0.,1.));proposals[underfull].alias=large;
        scaled_[large]+=scaled_[underfull]-1;(scaled_[large]<1?below_:above_).push_back(large);
    }
    for(std::uint32_t i=0;i<local_count;++i) {
        const double accept=proposals[i].accept;actual_[i]+=accept/local_count;
        actual_[proposals[i].alias]+=(1-accept)/local_count;
    }
    for(std::uint32_t i=0;i<local_count;++i)proposals[i].probability=static_cast<float>(actual_[i]);
}

void LightFrame::map_previous() {
    current_to_previous.assign(sources.size(),no_light_source);previous_to_current.assign(previous_sources.size(),no_light_source);
    previous_lookup_.clear();previous_lookup_.reserve(previous_identities_.size());
    for(std::size_t i=0;i<previous_identities_.size();++i) {
        const auto& key=previous_identities_[i].key;if(!key.owner)continue;
        const auto [at,inserted]=previous_lookup_.try_emplace(key,static_cast<std::uint32_t>(i));
        if(!inserted)at->second=no_light_source; // Ambiguous producer identity cannot be reused.
    }
    for(std::size_t i=0;i<identities_.size();++i) {
        const auto& identity=identities_[i];if(!identity.key.owner)continue;
        const auto at=previous_lookup_.find(identity.key);if(at==previous_lookup_.end()||at->second==no_light_source)continue;
        const auto previous=at->second;const auto& old=previous_identities_[previous];
        if(identity.key.kind==3 && descriptions_[identity.material]!=previous_descriptions_[old.material])continue;
        check(previous_to_current[previous]==no_light_source,"Duplicate light source identity");
        current_to_previous[i]=previous;previous_to_current[previous]=static_cast<std::uint32_t>(i);
        if(identity.key.kind==3)previous_sources[previous].meta[2]=sources[i].meta[2];
    }
    // Unmapped previous materials are not resident in this frame. Callers must
    // reject their invalid translation before evaluating a previous sample.
    for(std::size_t i=0;i<previous_sources.size();++i)
        if(previous_sources[i].meta[0]==3 && previous_to_current[i]==no_light_source)previous_sources[i].meta[2]=no_light_source;
}

} // namespace atelier::renderer::detail
