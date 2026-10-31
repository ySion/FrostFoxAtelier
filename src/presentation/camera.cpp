#include "presentation/detail.hpp"
#include <numbers>

namespace atelier::presentation {
using detail::error;

Result<CameraMatrices> CameraSnapshot::matrices(float aspect) const {
    if (!std::isfinite(aspect) || aspect <= 0 || !std::isfinite(vertical_fov) || vertical_fov <= 0 ||
        vertical_fov >= 180 || !std::isfinite(near_clip) || !std::isfinite(far_clip) || near_clip <= 0 ||
        far_clip <= near_clip)
        return error(ErrorCode::argument_mismatch, "Invalid camera projection.");
    const auto q = math::try_normalize(math::Quaterniond{rotation.x, rotation.y, rotation.z, rotation.w});
    if (!q)
        return error(ErrorCode::argument_mismatch, "Invalid camera rotation.");
    auto view = math::rotation_matrix(math::Quaterniond{-q->x, -q->y, -q->z, q->w});
    const auto translation = math::transform_direction(math::Vector3d{-position.x, -position.y, -position.z}, view);
    view[12] = translation.x;
    view[13] = translation.y;
    view[14] = translation.z;
    const auto y = 1 / std::tan(double(vertical_fov) * std::numbers::pi_v<double> / 360);
    // Infinite reversed-Z: near maps to one; depth zero unprojects to a direction.
    const math::Matrix4d projection{{y / aspect, 0, 0, 0, 0, y, 0, 0, 0, 0, 0, 1, 0, 0, near_clip, 0}};
    CameraMatrices result;
    for (std::size_t i = 0; i < 16; ++i) {
        result.view[i] = static_cast<float>(view[i]);
        result.projection[i] = static_cast<float>(projection[i]);
    }
    result.far_depth = near_clip / far_clip;
    if (!math::is_finite(result.view) || !math::is_finite(result.projection) || !math::try_inverse(result.projection))
        return error(ErrorCode::argument_mismatch, "Camera matrices are not representable.");
    return result;
}

Result<void> detail::camera_rules(const CameraComponent& value) {
    // Individual finite bounds are checked by reflected metadata.
    if (!(value.far_clip > value.near_clip))
        return error(ErrorCode::argument_mismatch, "Camera Far clip must exceed Near clip.");
    return {};
}

world::World::Admission detail::unique_camera(world::CapabilityId cameras) {
    return [cameras](const world::World& world,
                     std::span<const world::World::AdmissionCandidate> candidates) -> Result<void> {
        auto members = world.members(cameras);
        if (!members)
            return std::unexpected(members.error());
        std::unordered_map<ObjectId, ObjectId> owners;
        const auto visit = [&](ObjectId owner, ObjectId component) -> Result<void> {
            const auto [at, inserted] = owners.emplace(owner, component);
            if (!inserted && at->second != component)
                return error(
                    ErrorCode::invalid_member,
                    "An Entity can contain only one CameraComponent, including disabled and derived components.");
            return {};
        };
        for (const auto& candidate : candidates)
            if (auto valid = visit(candidate.owner, candidate.component); !valid)
                return valid;
        for (const auto& member : *members)
            if (auto valid = visit(member.entity, member.component); !valid)
                return valid;
        return {};
    };
}

Result<CameraSnapshot> Scene::camera(ObjectId entity) const {
    auto& runtime = world_->runtime();
    if (!world_->contains(entity))
        return error(ErrorCode::invalid_member, "Camera Entity is not in this World.");
    auto owner = runtime.hold(entity, world_->types().entity);
    if (!owner)
        return std::unexpected(owner.error());
    auto members = world_->members(cameras_);
    if (!members)
        return std::unexpected(members.error());
    ObjectId component;
    for (const auto& member : *members)
        if (member.entity == entity) {
            if (component != ObjectId{})
                return error(ErrorCode::invalid_member, "Camera Entity has more than one CameraComponent.");
            component = member.component;
        }
    if (component == ObjectId{})
        return error(ErrorCode::invalid_member, "Camera Entity has no CameraComponent.");
    auto held = runtime.hold(component, types_.camera_component);
    if (!held)
        return std::unexpected(held.error());
    auto valid = runtime.validate(component);
    if (!valid)
        return std::unexpected(valid.error());
    if (*valid)
        return std::unexpected((*valid)->error);
    auto space = world_->spatial_pass(world::DiagnosticPhase::presentation);
    if (!space)
        return std::unexpected(space.error());
    auto matrix = space->matrix(entity);
    if (!matrix)
        return std::unexpected(matrix.error());
    if (!*matrix)
        return error(ErrorCode::invalid_member, "Camera has an invalid spatial chain.");
    auto rotation = space->rotation(entity);
    if (!rotation)
        return std::unexpected(rotation.error());
    if (!*rotation)
        return error(ErrorCode::invalid_member, "Camera has an invalid rotation chain.");
    const auto& q = **rotation;
    return runtime.with_native<const CameraComponent>(
        component, [&](const CameraComponent& value) -> Result<CameraSnapshot> {
            return CameraSnapshot{entity,
                                  component,
                                  {(**matrix)[12], (**matrix)[13], (**matrix)[14]},
                                  {float(q.x), float(q.y), float(q.z), float(q.w)},
                                  value.vertical_fov,
                                  value.near_clip,
                                  value.far_clip};
        });
}

Result<CameraSnapshot> Scene::main_camera() const {
    auto& runtime = world_->runtime();
    auto environments = world_->members(environments_);
    if (!environments)
        return std::unexpected(environments.error());
    if (environments->empty() || !environments->front().active)
        return error(ErrorCode::invalid_member, "No active Environment selects a main camera.");
    auto environment = runtime.hold(environments->front().component, types_.environment_component);
    if (!environment)
        return std::unexpected(environment.error());
    auto field = runtime.find_field(types_.environment_component, "main_camera");
    if (!field)
        return std::unexpected(field.error());
    auto selected = runtime.read_reference(environment->id(), *field);
    if (!selected)
        return std::unexpected(selected.error());
    auto target = runtime.try_acquire(*selected);
    if (!target)
        return std::unexpected(target.error());
    if (!*target)
        return error(ErrorCode::invalid_member, "Main camera is unset or its Entity is no longer alive.");
    const auto entity = (*target)->id();
    if (!world_->contains(entity))
        return error(ErrorCode::invalid_member, "Main camera Entity is not in this World.");
    auto enabled = world::effective_enabled(runtime, entity);
    if (!enabled)
        return std::unexpected(enabled.error());
    if (!*enabled)
        return error(ErrorCode::invalid_member, "Main camera Entity or its ancestor is disabled.");
    auto snapshot = camera(entity);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    auto component = world::inspect_component(runtime, snapshot->component);
    if (!component)
        return std::unexpected(component.error());
    if (!component->enabled)
        return error(ErrorCode::invalid_member, "Main CameraComponent is disabled.");
    return snapshot;
}
}
