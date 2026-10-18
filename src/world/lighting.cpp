#include "world/detail.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace atelier::world {
using detail::error;

// Single-field bounds are AFIELD hard bounds, checked before this runs.
Result<void> detail::light_rules(const LightSettings& settings) {
    if (settings.kind!=LightKind::point && settings.kind!=LightKind::rectangle && settings.kind!=LightKind::directional && settings.kind!=LightKind::sky)
        return error(ErrorCode::argument_mismatch,"Unknown LightKind.");
    if(settings.kind==LightKind::sky)
        for(std::size_t channel=0;channel<3;++channel)
            if(double(settings.color[channel])*settings.intensity>65504)
                return error(ErrorCode::argument_mismatch,"Sky Light radiance exceeds the HDR scene range.");
    if ((settings.kind==LightKind::point || settings.kind==LightKind::rectangle) && settings.range==0.f)
        return error(ErrorCode::argument_mismatch,"Local light range must be positive; directional range may be zero.");
    if (settings.kind==LightKind::directional && double(settings.source_radius)>=std::numbers::pi_v<double>/2)
        return error(ErrorCode::argument_mismatch,"Directional angular radius must be below pi/2 radians.");
    if (settings.kind==LightKind::rectangle && (settings.width==0.f || settings.height==0.f))
        return error(ErrorCode::argument_mismatch,"Rectangle width and height must be positive.");
    if(settings.kind!=LightKind::sky)
        if(const auto* message=rendering::analytic_light_range_error(
            {settings.color.x,settings.color.y,settings.color.z},settings.intensity,
            settings.kind==LightKind::rectangle?double(settings.width)*settings.height:1))
            return error(ErrorCode::argument_mismatch,message);
    return {};
}
Result<void> validate_material(Runtime& runtime,const Material& material) { return detail::check_value(runtime,material); }
Result<void> validate_light(Runtime& runtime,const LightSettings& settings) { return detail::check_value(runtime,settings); }
Result<LightSettings> read_light(Runtime& runtime,ObjectId object) {
    return runtime.with_native<const LightComponent>(object,[](const LightComponent& light)->Result<LightSettings> { return light.settings; });
}
World::Admission detail::unique_sky_light(CapabilityId lights) {
    return [lights](const World& world,std::span<const ObjectId> candidates)->Result<void> {
        ObjectId sky;
        const auto visit=[&](ObjectId object)->Result<void> {
            auto settings=read_light(world.runtime(),object);if(!settings) return std::unexpected(settings.error());
            if(settings->kind!=LightKind::sky) return {};
            if(sky!=ObjectId{} && sky!=object) return error(ErrorCode::invalid_member,"A World can contain only one Sky Light, including disabled components.");
            sky=object;return {};
        };
        for(const auto object:candidates) if(auto valid=visit(object);!valid) return valid;
        if(sky==ObjectId{}) return {};
        auto members=world.members(lights);if(!members) return std::unexpected(members.error());
        for(const auto& member:*members) if(auto valid=visit(member.component);!valid) return valid;
        return {};
    };
}
World::Admission detail::spatial_lights() {
    return [](const World& world,std::span<const ObjectId> candidates)->Result<void> {
        auto& runtime=world.runtime();
        for(const auto object:candidates) {
            auto settings=read_light(runtime,object);if(!settings) return std::unexpected(settings.error());
            if(settings->kind==LightKind::sky) continue;
            auto component=component_data(runtime,object);if(!component) return std::unexpected(component.error());
            // Components entering an Entity are checked by its spatial requirements.
            if(component->owner==ObjectId{}) continue;
            auto spatial=has_transform(runtime,component->owner);if(!spatial) return std::unexpected(spatial.error());
            if(!*spatial) return error(ErrorCode::invalid_member,"Spatial Light requires its owner Entity to have a Transform.");
        }
        return {};
    };
}
Result<void> World::set_material(ObjectId object,Material value) {
    if (auto ready=impl_->admit_edit(object); !ready) return ready;
    auto component=impl_->composition_component(object); if (!component) return std::unexpected(component.error());
    if (auto valid=validate_material(runtime(),value); !valid) return valid;
    const auto assign=[&](const Material& material) {
        return runtime().with_native<StaticMeshComponent>(object,[&](auto& mesh)->Result<void> {mesh.material=material;return {};});
    };
    auto previous=runtime().with_native<const StaticMeshComponent>(object,[](const auto& mesh)->Result<Material> {return mesh.material;});
    if (!previous) return std::unexpected(previous.error());
    if (auto written=assign(value); !written) return written;
    if (auto admitted=impl_->check_edit(object); !admitted) { (void)assign(*previous); return admitted; }
    return {};
}
Result<void> World::set_light(ObjectId object,LightSettings value) {
    if (auto ready=impl_->admit_edit(object); !ready) return ready;
    auto component=impl_->composition_component(object); if (!component) return std::unexpected(component.error());
    if (auto valid=validate_light(runtime(),value); !valid) return valid;
    const auto assign=[&](const LightSettings& settings) {
        return runtime().with_native<LightComponent>(object,[&](auto& light)->Result<void> {light.settings=settings;return {};});
    };
    auto previous=read_light(runtime(),object); if (!previous) return std::unexpected(previous.error());
    if (auto written=assign(value); !written) return written;
    if (auto admitted=impl_->check_edit(object); !admitted) { (void)assign(*previous); return admitted; }
    return {};
}
Result<std::vector<LightRenderItem>> World::light_items() const {
    auto pass=spatial_pass(DiagnosticPhase::lighting);
    if (!pass) return std::unexpected(pass.error());
    auto& state=*impl_;
    std::vector<LightRenderItem> result;
    bool has_sky=false;
    auto lights=members(state.lights);
    if (!lights) return std::unexpected(lights.error());
    {
        for (const auto& [owner,object,active]:*lights) {
            // Isolated lights are skipped. The first Sky Light in composition
            // preorder, enabled or not, owns the World's single sky slot.
            const auto skip=[&](const Error& failure,DiagnosticKind kind,ObjectId at,ObjectId entity,std::vector<std::string> field) {
                return state.contain(failure,kind,DiagnosticPhase::lighting,at,entity,std::move(field));
            };
            auto settings=read_light(runtime(),object);
            if (!settings) { if (!skip(settings.error(),DiagnosticKind::invalid_value,object,owner,{"settings"})) return std::unexpected(settings.error()); continue; }
            if(settings->kind==LightKind::sky) {
                if(has_sky) {
                    const Error conflict{ErrorCode::invalid_member,"A World can contain only one Sky Light, including disabled components."};
                    if (!skip(conflict,DiagnosticKind::conflict,object,owner,{"settings","kind"})) return std::unexpected(conflict);
                    continue;
                }
                has_sky=true;
            }
            if (!active) continue;
            auto invalid=runtime().validate_value(types().light_settings,std::as_bytes(std::span{&*settings,1}));
            if (!invalid) return std::unexpected(invalid.error());
            if (*invalid) {
                const auto& failure=**invalid;
                if (!state.contain(failure.error,validation_diagnostic(failure,DiagnosticPhase::lighting,object,owner,{"settings"}))) return std::unexpected(failure.error);
                continue;
            }
            if(settings->kind==LightKind::sky) {
                result.push_back({owner,object,{}, {0,0,1},{1,0,0},{0,1,0},*settings});
                continue;
            }
            auto matrix=pass->matrix(owner);
            if (!matrix) return std::unexpected(matrix.error());
            if (!*matrix) continue;
            auto frame=pass->orientation(owner);
            if (!frame) return std::unexpected(frame.error());
            if (!*frame) continue;
            // A mirrored frame is -R; negating its right axis keeps it right-handed.
            const double sign=(*frame)->mirrored?-1:1;
            const auto axis=[&](math::Vector3d local) {
                const auto value=math::rotate((*frame)->rotation,local);
                return math::Vector3{float(value.x),float(value.y),float(value.z)};
            };
            const auto& world=**matrix;
            result.push_back({owner,object,{world[12],world[13],world[14]},
                axis({0,0,sign}),axis({1,0,0}),axis({0,sign,0}),*settings});
        }
    }
    return result;
}
}
