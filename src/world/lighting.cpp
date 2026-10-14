#include "world/detail.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace atelier::world {
using detail::error;
namespace {
bool nonnegative(math::Vector3 color) {
    return math::is_finite(color) && color.x>=0.f && color.y>=0.f && color.z>=0.f;
}
Result<std::string_view> field_name(Runtime& runtime,TypeId type,FieldId id) {
    auto info=runtime.describe(type); if (!info) return std::unexpected(info.error());
    const auto found=std::ranges::find((*info)->fields,id,&FieldInfo::id);
    if (found==(*info)->fields.end()) return error(ErrorCode::type_mismatch,"Field does not belong to this lighting value.");
    return std::string_view(found->name);
}
Result<float> float_value(const Scalar& value) {
    const auto* number=std::get_if<float>(&value);
    if (!number) return error(ErrorCode::type_mismatch,"Lighting numeric fields require float values.");
    return *number;
}
Result<void> patch_vector(Runtime& runtime,TypeId type,math::Vector3& target,FieldId field,const Scalar& value) {
    auto name=field_name(runtime,type,field); if (!name) return std::unexpected(name.error());
    auto number=float_value(value); if (!number) return std::unexpected(number.error());
    if (*name=="x") target.x=*number;
    else if (*name=="y") target.y=*number;
    else if (*name=="z") target.z=*number;
    else return error(ErrorCode::type_mismatch,"Invalid lighting color channel.");
    return {};
}
Result<void> patch_material(Runtime& runtime,const Types& types,Material& material,std::span<const FieldId> path,const Scalar& value) {
    if (path.empty()) return error(ErrorCode::type_mismatch,"A scalar edit requires a Material leaf field.");
    auto name=field_name(runtime,types.material,path.front()); if (!name) return std::unexpected(name.error());
    if (path.size()==2 && (*name=="base_color" || *name=="emissive"))
        return patch_vector(runtime,types.vector3,*name=="base_color"?material.base_color:material.emissive,path[1],value);
    if (path.size()!=1) return error(ErrorCode::type_mismatch,"Invalid Material field path.");
    auto number=float_value(value); if (!number) return std::unexpected(number.error());
    if (*name=="roughness") material.roughness=*number;
    else if (*name=="metallic") material.metallic=*number;
    else return error(ErrorCode::type_mismatch,"Invalid Material scalar field.");
    return {};
}
Result<void> patch_light(Runtime& runtime,const Types& types,LightSettings& settings,std::span<const FieldId> path,const Scalar& value) {
    if (path.empty()) return error(ErrorCode::type_mismatch,"A scalar edit requires a LightSettings leaf field.");
    auto name=field_name(runtime,types.light_settings,path.front()); if (!name) return std::unexpected(name.error());
    if (path.size()==2 && *name=="color") return patch_vector(runtime,types.vector3,settings.color,path[1],value);
    if (path.size()!=1) return error(ErrorCode::type_mismatch,"Invalid LightSettings field path.");
    if (*name=="kind") {
        const auto* kind=std::get_if<EnumValue>(&value);
        if (!kind || kind->type!=types.light_kind) return error(ErrorCode::type_mismatch,"Light kind requires world.LightKind.");
        settings.kind=static_cast<LightKind>(kind->bits); return {};
    }
    if (*name=="two_sided" || *name=="casts_shadow") {
        const auto* boolean=std::get_if<bool>(&value);
        if (!boolean) return error(ErrorCode::type_mismatch,"Light flags require bool values.");
        (*name=="two_sided"?settings.two_sided:settings.casts_shadow)=*boolean; return {};
    }
    auto number=float_value(value); if (!number) return std::unexpected(number.error());
    if (*name=="intensity") settings.intensity=*number;
    else if (*name=="range") settings.range=*number;
    else if (*name=="source_radius") settings.source_radius=*number;
    else if (*name=="width") settings.width=*number;
    else if (*name=="height") settings.height=*number;
    else return error(ErrorCode::type_mismatch,"Invalid LightSettings scalar field.");
    return {};
}
// Compose the actual local rotations; decomposing the world matrix would make
// emitter orientation depend on nonuniform parent scale and shear.
Result<math::Quaterniond> light_rotation(Runtime& runtime,ObjectId object) {
    math::Quaterniond result;
    for (auto node=object;node!=ObjectId{};) {
        auto value=read_transform(runtime,node); if (!value) return std::unexpected(value.error());
        auto canonical=canonical_transform(*value); if (!canonical) return std::unexpected(canonical.error());
        const auto& q=canonical->rotation;
        const auto composed=math::try_normalize(math::Quaterniond{q.x,q.y,q.z,q.w}*result);
        if (!composed) return error(ErrorCode::argument_mismatch,"Light orientation is not finite.");
        result=*composed;
        auto parent=spatial_parent(runtime,node); if (!parent) return std::unexpected(parent.error());
        node=*parent;
    }
    return result;
}
}

Result<void> validate_material(const Material& material) {
    if (!nonnegative(material.base_color) || material.base_color.x>1.f || material.base_color.y>1.f || material.base_color.z>1.f)
        return error(ErrorCode::argument_mismatch,"Material base color must be finite and within [0,1].");
    if (!std::isfinite(material.roughness) || material.roughness<0.f || material.roughness>1.f ||
        !std::isfinite(material.metallic) || material.metallic<0.f || material.metallic>1.f)
        return error(ErrorCode::argument_mismatch,"Material roughness and metallic must be finite and within [0,1].");
    if (!nonnegative(material.emissive)) return error(ErrorCode::argument_mismatch,"Material emissive must be finite and nonnegative.");
    return {};
}
Result<void> validate_light(const LightSettings& settings) {
    if (settings.kind!=LightKind::point && settings.kind!=LightKind::rectangle && settings.kind!=LightKind::directional && settings.kind!=LightKind::sky)
        return error(ErrorCode::argument_mismatch,"Unknown LightKind.");
    if (!nonnegative(settings.color) || !std::isfinite(settings.intensity) || settings.intensity<0.f)
        return error(ErrorCode::argument_mismatch,"Light color and intensity must be finite and nonnegative.");
    if(settings.kind==LightKind::sky)
        for(std::size_t channel=0;channel<3;++channel)
            if(double(settings.color[channel])*settings.intensity>65504)
                return error(ErrorCode::argument_mismatch,"Sky Light radiance exceeds the HDR scene range.");
    if (!std::isfinite(settings.range) || settings.range<0.f ||
        ((settings.kind==LightKind::point || settings.kind==LightKind::rectangle) && settings.range==0.f))
        return error(ErrorCode::argument_mismatch,"Local light range must be finite and positive; directional range may be zero.");
    if (!std::isfinite(settings.source_radius) || settings.source_radius<0.f)
        return error(ErrorCode::argument_mismatch,"Light source radius must be finite and nonnegative.");
    if (settings.kind==LightKind::directional && double(settings.source_radius)>=std::numbers::pi_v<double>/2)
        return error(ErrorCode::argument_mismatch,"Directional angular radius must be below pi/2 radians.");
    if (!std::isfinite(settings.width) || !std::isfinite(settings.height) || settings.width<0.f || settings.height<0.f ||
        (settings.kind==LightKind::rectangle && (settings.width==0.f || settings.height==0.f)))
        return error(ErrorCode::argument_mismatch,"Rectangle width and height must be finite and positive.");
    return {};
}
Result<LightSettings> read_light(Runtime& runtime,ObjectId object) {
    return runtime.with_native<const LightComponent>(object,[](const LightComponent& light)->Result<LightSettings> { return light.settings; });
}
Result<void> World::Impl::validate_sky_light(std::span<const ObjectRoot> additions,ObjectId edited,const LightSettings* replacement) const {
    ObjectId sky;
    const auto visit=[&](ObjectId object)->Result<void> {
        auto light=detail::is(runtime,object,types.light_component);if(!light) return std::unexpected(light.error());
        if(!*light) return {};
        auto settings=object==edited && replacement?Result<LightSettings>{*replacement}:read_light(runtime,object);
        if(!settings) return std::unexpected(settings.error());
        if(settings->kind!=LightKind::sky) return {};
        if(sky!=ObjectId{} && sky!=object) return error(ErrorCode::invalid_member,"A World can contain only one Sky Light, including disabled components.");
        sky=object;return {};
    };
    for(const auto& object:additions) if(auto valid=visit(object.id());!valid) return valid;
    // Ordinary composition changes cannot introduce another Sky Light.
    // Replacement edits are checked in presentation, preserving detached edits.
    if(sky==ObjectId{} && (!replacement || replacement->kind!=LightKind::sky)) return {};
    for(const auto object:presentation) if(auto valid=visit(object);!valid) return valid;
    return {};
}
Result<void> World::set_material(ObjectId object,Material value) {
    if (auto ready=impl_->admit_edit(object); !ready) return ready;
    auto component=impl_->composition_component(object); if (!component) return std::unexpected(component.error());
    if (auto valid=validate_material(value); !valid) return valid;
    return runtime().with_native<StaticMeshComponent>(object,[&](auto& mesh)->Result<void> {mesh.material=value;return {};});
}
Result<void> World::set_light(ObjectId object,LightSettings value) {
    if (auto ready=impl_->admit_edit(object); !ready) return ready;
    auto component=impl_->composition_component(object); if (!component) return std::unexpected(component.error());
    if (auto valid=validate_light(value); !valid) return valid;
    bool registered{};
    if(component->owner!=ObjectId{}) {
        auto owner=impl_->composition_entity(component->owner);if(!owner) return std::unexpected(owner.error());
        registered=owner->world!=nullptr;
        if(registered && value.kind!=LightKind::sky) {
            auto spatial=has_transform(runtime(),component->owner);if(!spatial) return std::unexpected(spatial.error());
            if(!*spatial) return error(ErrorCode::invalid_member,"Spatial Light requires its owner Entity to have a Transform.");
        }
    }
    if(registered && value.kind==LightKind::sky)
        if(auto valid=impl_->validate_sky_light({},object,&value);!valid) return valid;
    return runtime().with_native<LightComponent>(object,[&](auto& light)->Result<void> {light.settings=value;return {};});
}
namespace detail {
Result<bool> write_lighting_property(World& world,ObjectId object,std::span<const FieldId> path,Scalar value) {
    if (path.empty()) return false;
    auto& runtime=world.runtime(); const auto& types=world.types();
    auto mesh=is(runtime,object,types.static_mesh_component); if (!mesh) return std::unexpected(mesh.error());
    if (*mesh) {
        auto field=runtime.find_field(types.static_mesh_component,"material"); if (!field) return std::unexpected(field.error());
        if (path.front()==*field) {
            auto settings=runtime.with_native<const StaticMeshComponent>(object,[](const auto& component)->Result<Material>{return component.material;});
            if (!settings) return std::unexpected(settings.error());
            auto changed=patch_material(runtime,types,*settings,path.subspan(1),value); if (!changed) return std::unexpected(changed.error());
            auto written=world.set_material(object,*settings); if (!written) return std::unexpected(written.error());
            return true;
        }
    }
    auto light=is(runtime,object,types.light_component); if (!light) return std::unexpected(light.error());
    if (*light) {
        auto field=runtime.find_field(types.light_component,"settings"); if (!field) return std::unexpected(field.error());
        if (path.front()==*field) {
            auto settings=read_light(runtime,object); if (!settings) return std::unexpected(settings.error());
            auto changed=patch_light(runtime,types,*settings,path.subspan(1),value); if (!changed) return std::unexpected(changed.error());
            auto written=world.set_light(object,*settings); if (!written) return std::unexpected(written.error());
            return true;
        }
    }
    return false;
}
}
Result<std::vector<LightRenderItem>> World::light_items() const {
    if (auto ready=impl_->admit(false); !ready) return std::unexpected(ready.error());
    std::vector<LightRenderItem> result;
    detail::SpaceGraph space{runtime()};
    bool has_sky=false;
    for (const auto object:impl_->presentation) {
        auto light=detail::is(runtime(),object,types().light_component); if (!light) return std::unexpected(light.error());
        if (!*light) continue;
        auto owner=impl_->component_owner(object); if (!owner) return std::unexpected(owner.error());
        auto entity=detail::entity_data(runtime(),*owner); if (!entity) return std::unexpected(entity.error());
        auto component=detail::component_data(runtime(),object); if (!component) return std::unexpected(component.error());
        auto settings=read_light(runtime(),object); if (!settings) return std::unexpected(settings.error());
        if(settings->kind==LightKind::sky) {
            if(has_sky) return error(ErrorCode::invalid_member,"A World can contain only one Sky Light, including disabled components.");
            has_sky=true;
        }
        auto enabled=effective_enabled(runtime(),*owner); if (!enabled) return std::unexpected(enabled.error());
        if (!*enabled || !component->enabled) continue;
        if (auto valid=validate_light(*settings); !valid) return std::unexpected(valid.error());
        if(settings->kind==LightKind::sky) {
            result.push_back({*owner,object,{}, {0,0,1},{1,0,0},{0,1,0},*settings});
            continue;
        }
        auto matrix=space.matrix(*owner); if (!matrix) return std::unexpected(matrix.error());
        auto rotation=light_rotation(runtime(),*owner); if (!rotation) return std::unexpected(rotation.error());
        const auto axis=[&](math::Vector3d local) {
            const auto value=math::rotate(*rotation,local);
            return math::Vector3{float(value.x),float(value.y),float(value.z)};
        };
        result.push_back({*owner,object,{(*matrix)[12],(*matrix)[13],(*matrix)[14]},
            axis({0,0,1}),axis({1,0,0}),axis({0,1,0}),*settings});
    }
    return result;
}
}
