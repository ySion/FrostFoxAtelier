#include "presentation/detail.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace atelier::presentation {
using detail::error;
using world::DiagnosticKind;
using world::DiagnosticPhase;

// Single-field bounds are AFIELD hard bounds, checked before this runs.
Result<void> detail::light_rules(const LightSettings& settings) {
    if (settings.kind != LightKind::point && settings.kind != LightKind::rectangle &&
        settings.kind != LightKind::directional)
        return error(ErrorCode::argument_mismatch, "Unknown LightKind.");
    if ((settings.kind == LightKind::point || settings.kind == LightKind::rectangle) && settings.range == 0.f)
        return error(ErrorCode::argument_mismatch,
                     "Local light range must be positive; directional range may be zero.");
    if (settings.kind == LightKind::directional && double(settings.source_radius) >= std::numbers::pi_v<double> / 2)
        return error(ErrorCode::argument_mismatch, "Directional angular radius must be below pi/2 radians.");
    if (settings.kind == LightKind::rectangle && (settings.width == 0.f || settings.height == 0.f))
        return error(ErrorCode::argument_mismatch, "Rectangle width and height must be positive.");
    if (const auto* message = rendering::analytic_light_range_error(
            {settings.color.x, settings.color.y, settings.color.z}, settings.intensity,
            settings.kind == LightKind::rectangle ? double(settings.width) * settings.height : 1))
        return error(ErrorCode::argument_mismatch, message);
    return {};
}
Result<void> validate_light(Runtime& runtime, const LightSettings& settings) {
    return detail::check_value(runtime, settings);
}
rendering::SceneLightingSettings environment_settings(const EnvironmentComponent& value) noexcept {
    rendering::SceneLightingSettings result;
    result.shadows = value.shadows;
    result.ddgi = value.ddgi;
    result.gtao = value.gtao;
    result.sssr = value.sssr;
    result.bloom = value.bloom;
    result.antialiasing = static_cast<rendering::Antialiasing>(value.antialiasing);
    result.exposure_ev = value.exposure_ev;
    result.direct_candidates = value.direct_candidates;
    result.ao_radius = value.ao_radius;
    result.reflection_distance = value.reflection_distance;
    result.reflection_roughness = value.reflection_roughness;
    const std::array volumes{&value.volume0, &value.volume1, &value.volume2, &value.volume3};
    for (std::size_t slot = 0; slot < volumes.size(); ++slot) {
        const auto& from = *volumes[slot];
        auto& to = result.probe_volumes[slot];
        to.enabled = from.enabled;
        to.placement = static_cast<rendering::ProbePlacement>(from.placement);
        to.minimum = {from.minimum.x, from.minimum.y, from.minimum.z};
        to.maximum = {from.maximum.x, from.maximum.y, from.maximum.z};
        to.counts = {from.count_x, from.count_y, from.count_z};
        to.blend_cells = from.blend_cells;
    }
    result.probe_update_budget = value.probe_update_budget;
    result.probe_rays = value.probe_rays;
    result.probe_bounce = value.probe_bounce;
    result.indirect_gain = value.indirect_gain;
    result.sky_enabled = value.sky_enabled;
    result.sky_color = {value.sky_color.x, value.sky_color.y, value.sky_color.z};
    result.sky_intensity = value.sky_intensity;
    result.sky_map = detail::Access::sky_map_resolved(value);
    result.sky_rotation = value.sky_rotation;
    return result;
}
// Single-field bounds are AFIELD hard bounds, checked before this runs.
Result<void> detail::environment_rules(const EnvironmentComponent& value) {
    if (const auto* message = rendering::scene_lighting_error(environment_settings(value)))
        return error(ErrorCode::argument_mismatch, message);
    return {};
}
world::World::Admission detail::unique_environment(world::CapabilityId environments) {
    return [environments](const world::World& world,
                          std::span<const world::World::AdmissionCandidate> candidates) -> Result<void> {
        auto members = world.members(environments);
        if (!members)
            return std::unexpected(members.error());
        ObjectId first;
        const auto visit = [&](ObjectId object) -> Result<void> {
            if (first != ObjectId{} && first != object)
                return error(ErrorCode::invalid_member,
                             "A World can contain only one EnvironmentComponent, including disabled components.");
            first = object;
            return {};
        };
        for (const auto& candidate : candidates)
            if (auto valid = visit(candidate.component); !valid)
                return valid;
        for (const auto& member : *members)
            if (auto valid = visit(member.component); !valid)
                return valid;
        return {};
    };
}
Result<rendering::SceneLightingSettings> Scene::environment() const {
    auto environments = world_->members(environments_);
    if (!environments)
        return std::unexpected(environments.error());
    for (const auto& [owner, object, active] : *environments) {
        if (!active)
            continue;
        auto invalid = world_->runtime().validate(object);
        if (!invalid)
            return std::unexpected(invalid.error());
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
    if (auto valid = detail::check_value(runtime, value); !valid)
        return valid;
    auto owner = runtime.native_type<Owner>();
    if (!owner)
        return std::unexpected(owner.error());
    auto type = runtime.native_type<T>();
    if (!type)
        return std::unexpected(type.error());
    auto field = runtime.find_field(*owner, name);
    if (!field)
        return std::unexpected(field.error());
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
    try {
        source = String(StringView(binding.source));
    } catch (const std::exception& failure) {
        return error(ErrorCode::invalid_definition, failure.what());
    }
    if (auto ready = runtime.check_code_publication(); !ready)
        return ready;
    if (auto owner = world::admit_component_edit(runtime, object); !owner)
        return std::unexpected(owner.error());
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
    auto pass = world_->spatial_pass(DiagnosticPhase::lighting);
    if (!pass)
        return std::unexpected(pass.error());
    auto& runtime = world_->runtime();
    std::vector<LightRenderItem> result;
    auto lights = world_->members(lights_);
    if (!lights)
        return std::unexpected(lights.error());
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
                auto invalid = runtime.validate_value(types_.light_settings, std::as_bytes(std::span{&*settings, 1}));
                if (!invalid)
                    return std::unexpected(invalid.error());
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
            auto matrix = pass->matrix(owner);
            if (!matrix)
                return std::unexpected(matrix.error());
            if (!*matrix) {
                *failed = true;
                continue;
            }
            auto frame = pass->orientation(owner);
            if (!frame)
                return std::unexpected(frame.error());
            if (!*frame) {
                *failed = true;
                continue;
            }
            // A mirrored frame is -R; negating its right axis keeps it right-handed.
            const double sign = (*frame)->mirrored ? -1 : 1;
            const auto axis = [&](math::Vector3d local) {
                const auto value = math::rotate((*frame)->rotation, local);
                return math::Vector3{float(value.x), float(value.y), float(value.z)};
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
                auto offset = presented->find(*pass, owner);
                if (!offset)
                    return std::unexpected(offset.error());
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
