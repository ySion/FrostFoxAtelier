#include "detail/detail.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace atelier::presentation {
using detail::error;

using world::DiagnosticKind;

using world::DiagnosticPhase;

// Single-field bounds are AFIELD hard bounds, checked before this runs.
Result<void> LightSettings::validate() const {
    if (kind != LightKind::point && kind != LightKind::rectangle && kind != LightKind::directional &&
        kind != LightKind::spot)
        return error(ErrorCode::argument_mismatch, "Unknown LightKind.");
    if ((kind == LightKind::point || kind == LightKind::rectangle || kind == LightKind::spot) && range == 0.f)
        return error(ErrorCode::argument_mismatch,
                     "Local light range must be positive; directional range may be zero.");
    if (kind == LightKind::directional && double(source_radius) >= std::numbers::pi_v<double> / 2)
        return error(ErrorCode::argument_mismatch, "Directional angular radius must be below pi/2 radians.");
    if (kind == LightKind::rectangle && (width == 0.f || height == 0.f))
        return error(ErrorCode::argument_mismatch, "Rectangle width and height must be positive.");
    if (kind == LightKind::spot && (!std::isfinite(spot_inner_degrees) || !std::isfinite(spot_outer_degrees) ||
                                    spot_inner_degrees < 0.f || spot_outer_degrees < .1f ||
                                    spot_outer_degrees > 89.9f || spot_inner_degrees > spot_outer_degrees))
        return error(ErrorCode::argument_mismatch,
                     "Spot inner/outer cone half-angles must satisfy 0 <= inner <= outer <= 89.9 degrees.");
    if (const auto* message = rendering::analytic_light_range_error(
            color, intensity, kind == LightKind::rectangle ? double(width) * height : 1))
        return error(ErrorCode::argument_mismatch, message);
    return {};
}

Result<void> validate_light(Runtime& runtime, const LightSettings& settings) {
    return detail::check_value(runtime, settings);
}

namespace {
rendering::SceneLightingSettings environment_settings(const EnvironmentComponent& value) noexcept {
    rendering::SceneLightingSettings result;
    result.shadows = value.shadows;
    result.contact_shadow_length = value.contact_shadow_length;
    result.direct_candidates = value.direct_candidates;
    result.indirect_lighting = value.indirect_lighting;
    result.gi_quality = static_cast<rendering::GiQuality>(value.gi_quality);
    result.indirect_gain = value.indirect_gain;
    result.ssilvb = value.ssilvb;
    result.ao_radius = value.ao_radius;
    result.reflections = value.reflections;
    result.reflection_distance = value.reflection_distance;
    result.reflection_roughness = value.reflection_roughness;
    result.sky_mode = static_cast<rendering::SkyMode>(value.sky_mode);
    result.sky_color = value.sky_color;
    result.sky_intensity = value.sky_intensity;
    result.sky_map = detail::Access::sky_map_resolved(value);
    result.sky_rotation = value.sky_rotation;
    result.fog_enabled = value.fog_enabled;
    result.fog_density = value.fog_density;
    result.fog_height_falloff = value.fog_height_falloff;
    result.fog_anisotropy = value.fog_anisotropy;
    result.exposure_ev = value.exposure_ev;
    result.bloom = value.bloom;
    result.tonemapper = static_cast<rendering::Tonemapper>(value.tonemapper);
    result.antialiasing = static_cast<rendering::Antialiasing>(value.antialiasing);
    return result;
}
}

// Single-field bounds are AFIELD hard bounds, checked before this runs.
Result<void> EnvironmentComponent::validate() const {
    if (const auto* message = rendering::scene_lighting_error(environment_settings(*this)))
        return error(ErrorCode::argument_mismatch, message);
    return {};
}

world::World::Admission detail::unique_environment(world::CapabilityId environments) {
    return [environments](const world::World& world,
                          std::span<const world::World::AdmissionCandidate> candidates) -> Result<void> {
        ATELIER_TRY_BIND(members, world.members(environments));
        ObjectId first;
        const auto visit = [&](ObjectId object) -> Result<void> {
            if (first != ObjectId{} && first != object)
                return error(ErrorCode::invalid_member,
                             "A World can contain only one EnvironmentComponent, including disabled components.");
            first = object;
            return {};
        };
        for (const auto& candidate : candidates)
            ATELIER_TRY(visit(candidate.component));
        for (const auto& member : *members)
            ATELIER_TRY(visit(member.component));
        return {};
    };
}

Result<rendering::SceneLightingSettings> Scene::environment() const {
    ATELIER_TRY_BIND(environments, world_->members(environments_));
    for (const auto& [owner, object, active] : *environments) {
        if (!active)
            continue;
        ATELIER_TRY_BIND(invalid, world_->runtime().validate(object));
        if (*invalid) {
            const auto& failure = **invalid;
            if (!world_->contain(failure.error,
                                 world::validation_diagnostic(failure, DiagnosticPhase::lighting, object, owner)))
                return std::unexpected(failure.error);
            break;
        }
        return world_->runtime().with_native<const EnvironmentComponent>(
            object, [](const EnvironmentComponent& value) -> Result<rendering::SceneLightingSettings> {
                return environment_settings(value);
            });
    }
    return rendering::SceneLightingSettings{};
}

Result<LightSettings> read_light(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const LightComponent>(
        object, [](const LightComponent& light) -> Result<LightSettings> { return light.settings; });
}

namespace {
// The new value's own rules first: World::write would clamp it to its hard bounds, and checks
// the whole object's rules only when the object was valid before the edit.
template <class Owner, class T>
Result<void> set_value(world::World& world, ObjectId object, std::string_view name, const T& value) {
    auto& runtime = world.runtime();
    ATELIER_TRY(detail::check_value(runtime, value));
    ATELIER_TRY_BIND(owner, runtime.native_type<Owner>());
    ATELIER_TRY_BIND(type, runtime.native_type<T>());
    ATELIER_TRY_BIND(field, runtime.find_field(*owner, name));
    return world.write(object, std::span{&*field, 1}, ValueInput{*type, &value});
}
}

Result<void> set_light(world::World& world, ObjectId object, LightSettings value) {
    return set_value<LightComponent>(world, object, "settings", value);
}

Result<void> set_sky_map_binding(Runtime& runtime, ObjectId object, SkyMapBinding binding) {
    if (binding.source.size() > 4096 || binding.source.find('\0') != std::string::npos)
        return error(ErrorCode::invalid_definition, "Invalid sky map key.");
    // The key is what a scene saves; a map without one could not be saved.
    if (binding.resolved && binding.source.empty())
        return error(ErrorCode::invalid_definition, "A sky map needs its asset key.");
    String source;
    if (!source.assign(StringView{binding.source}))
        return error(ErrorCode::invalid_definition, "String requires valid UTF-8.");
    ATELIER_TRY(runtime.check_code_publication());
    ATELIER_TRY(world::admit_component_edit(runtime, object));
    return runtime.with_native<EnvironmentComponent>(object, [&](EnvironmentComponent& component) -> Result<void> {
        detail::Access::sky_map(component) = std::move(source);
        detail::Access::sky_map_resolved(component) = std::move(binding.resolved);
        return {};
    });
}

Result<SkyMapBinding> read_sky_map(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const EnvironmentComponent>(
        object, [](const EnvironmentComponent& component) -> Result<SkyMapBinding> {
            return SkyMapBinding{std::string(detail::Access::sky_map(component).view().std_view()),
                                 detail::Access::sky_map_resolved(component)};
        });
}

Result<std::vector<LightRenderItem>> Scene::light_items() const {
    return extract_lights(nullptr);
}

Result<std::vector<LightRenderItem>> Scene::extract_lights(const detail::Presented* presented, bool* failed,
                                                           bool* moved) const {
    bool unused{};
    if (!failed)
        failed = &unused;
    if (!moved)
        moved = &unused;
    ATELIER_TRY_BIND(pass, world_->spatial_pass(DiagnosticPhase::lighting));
    auto& runtime = world_->runtime();
    std::vector<LightRenderItem> result;
    ATELIER_TRY_BIND(lights, world_->members(lights_));
    valid_lights_.resize(lights->size());
    {
        for (std::size_t slot = 0; slot < lights->size(); ++slot) {
            const auto& [owner, object, active] = (*lights)[slot];
            if (!active)
                continue;
            // Isolated lights are skipped.
            auto settings = read_light(runtime, object);
            if (!settings) {
                if (!world_->contain(settings.error(),
                                     detail::diagnostic(DiagnosticKind::invalid_value, DiagnosticPhase::lighting,
                                                        object, owner, {"settings"})))
                    return std::unexpected(settings.error());
                *failed = true;
                continue;
            }
            if (!valid_lights_.contains(slot, *settings)) {
                ATELIER_TRY_BIND(
                    invalid, runtime.validate_value(types_.light_settings, std::as_bytes(std::span{&*settings, 1})));
                if (*invalid) {
                    const auto& failure = **invalid;
                    if (!world_->contain(failure.error, world::validation_diagnostic(failure, DiagnosticPhase::lighting,
                                                                                     object, owner, {"settings"})))
                        return std::unexpected(failure.error);
                    *failed = true;
                    continue;
                }
                valid_lights_.insert(slot, *settings);
            }
            ATELIER_TRY_BIND(matrix, pass->matrix(owner));
            if (!*matrix) {
                *failed = true;
                continue;
            }
            ATELIER_TRY_BIND(frame, pass->orientation(owner));
            if (!*frame) {
                *failed = true;
                continue;
            }
            // A mirrored frame is -R; negating its right axis keeps it right-handed.
            const double sign = (*frame)->mirrored ? -1 : 1;
            const auto axis = [&](math::Vector3d local) {
                return math::Vector3(math::rotate((*frame)->rotation, local));
            };
            const auto& world = **matrix;
            LightRenderItem item{owner,
                                 object,
                                 {world[12], world[13], world[14]},
                                 axis({0, 0, sign}),
                                 axis({1, 0, 0}),
                                 axis({0, sign, 0}),
                                 *settings};
            if (presented && !presented->empty()) {
                ATELIER_TRY_BIND(offset, presented->find(*pass, owner));
                if (*offset) {
                    *moved = true;
                    const auto& [shift, turn] = **offset;
                    item.position = math::transform_point(item.position, shift);
                    item.direction = math::rotate(turn, item.direction);
                    item.right = math::rotate(turn, item.right);
                    item.up = math::rotate(turn, item.up);
                }
            }
            result.push_back(item);
        }
    }
    return result;
}
}
