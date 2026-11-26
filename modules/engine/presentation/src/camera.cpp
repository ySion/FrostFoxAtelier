#include "detail/detail.hpp"
#include <numbers>

namespace atelier::presentation {
using detail::error;

Result<CameraMatrices> CameraSnapshot::matrices(float aspect) const {
    if (!std::isfinite(aspect) || aspect <= 0 || !std::isfinite(vertical_fov) || vertical_fov <= 0 ||
        vertical_fov >= 180 || !std::isfinite(near_clip) || !std::isfinite(far_clip) || near_clip <= 0 ||
        far_clip <= near_clip)
        return error(ErrorCode::argument_mismatch, "Invalid camera projection.");
    const auto q = math::try_normalize(math::Quaterniond(rotation));
    if (!q)
        return error(ErrorCode::argument_mismatch, "Invalid camera rotation.");
    auto view = math::rotation_matrix(math::conjugate(*q));
    const auto translation = math::transform_direction(-math::Vector3d(position), view);
    view[12] = translation.x;
    view[13] = translation.y;
    view[14] = translation.z;
    const auto y = 1 / std::tan(double(vertical_fov) * std::numbers::pi_v<double> / 360);
    // Infinite reversed-Z: near maps to one; depth zero unprojects to a direction.
    const math::Matrix4d projection{{y / aspect, 0, 0, 0, 0, y, 0, 0, 0, 0, 0, 1, 0, 0, near_clip, 0}};
    CameraMatrices result;
    result.view = math::Matrix4(view);
    result.projection = math::Matrix4(projection);
    result.far_depth = near_clip / far_clip;
    if (!math::is_finite(result.view) || !math::is_finite(result.projection) || !math::try_inverse(result.projection))
        return error(ErrorCode::argument_mismatch, "Camera matrices are not representable.");
    return result;
}

Result<void> CameraComponent::validate() const {
    // Individual finite bounds are checked by reflected metadata.
    if (!(far_clip > near_clip))
        return error(ErrorCode::argument_mismatch, "Camera Far clip must exceed Near clip.");
    return {};
}

world::World::Admission detail::unique_camera(world::CapabilityId cameras) {
    return [cameras](const world::World& world,
                     std::span<const world::World::AdmissionCandidate> candidates) -> Result<void> {
        ATELIER_TRY_BIND(members, world.members(cameras));
        Map<ObjectId, ObjectId> owners;
        owners.reserve(candidates.size() + members->size());
        const auto visit = [&](ObjectId owner, ObjectId component) -> Result<void> {
            const auto [at, inserted] = owners.try_emplace(owner, component);
            if (!inserted && *at != component)
                return error(
                    ErrorCode::invalid_member,
                    "An Entity can contain only one CameraComponent, including disabled and derived components.");
            return {};
        };
        for (const auto& candidate : candidates)
            ATELIER_TRY(visit(candidate.owner, candidate.component));
        for (const auto& member : *members)
            ATELIER_TRY(visit(member.entity, member.component));
        return {};
    };
}

Result<CameraSnapshot> Scene::camera(ObjectId entity) const {
    auto& runtime = world_->runtime();
    if (!world_->contains(entity))
        return error(ErrorCode::invalid_member, "Camera Entity is not in this World.");
    ATELIER_TRY_BIND(owner, runtime.hold(entity, world_->types().entity));
    ATELIER_TRY_BIND(members, world_->members(cameras_));
    ObjectId component;
    for (const auto& member : *members)
        if (member.entity == entity) {
            if (component != ObjectId{})
                return error(ErrorCode::invalid_member, "Camera Entity has more than one CameraComponent.");
            component = member.component;
        }
    if (component == ObjectId{})
        return error(ErrorCode::invalid_member, "Camera Entity has no CameraComponent.");
    ATELIER_TRY_BIND(held, runtime.hold(component, types_.camera_component));
    ATELIER_TRY_BIND(valid, runtime.validate(component));
    if (*valid)
        return std::unexpected((*valid)->error);
    ATELIER_TRY_BIND(space, world_->spatial_pass(world::DiagnosticPhase::presentation));
    ATELIER_TRY_BIND(matrix, space->matrix(entity));
    if (!*matrix)
        return error(ErrorCode::invalid_member, "Camera has an invalid spatial chain.");
    ATELIER_TRY_BIND(rotation, space->rotation(entity));
    if (!*rotation)
        return error(ErrorCode::invalid_member, "Camera has an invalid rotation chain.");
    return runtime.with_native<const CameraComponent>(
        component, [&](const CameraComponent& value) -> Result<CameraSnapshot> {
            return CameraSnapshot{entity,
                                  component,
                                  {(**matrix)[12], (**matrix)[13], (**matrix)[14]},
                                  math::Quaternion(**rotation),
                                  value.vertical_fov,
                                  value.near_clip,
                                  value.far_clip};
        });
}

Result<CameraSnapshot> Scene::main_camera() const {
    auto& runtime = world_->runtime();
    ATELIER_TRY_BIND(environments, world_->members(environments_));
    if (environments->empty() || !environments->front().active)
        return error(ErrorCode::invalid_member, "No active Environment selects a main camera.");
    ATELIER_TRY_BIND(environment, runtime.hold(environments->front().component, types_.environment_component));
    ATELIER_TRY_BIND(field, runtime.find_field(types_.environment_component, "main_camera"));
    ATELIER_TRY_BIND(selected, runtime.read_reference(environment->id(), *field));
    ATELIER_TRY_BIND(target, runtime.try_acquire(*selected));
    if (!*target)
        return error(ErrorCode::invalid_member, "Main camera is unset or its Entity is no longer alive.");
    const auto entity = (*target)->id();
    if (!world_->contains(entity))
        return error(ErrorCode::invalid_member, "Main camera Entity is not in this World.");
    ATELIER_TRY_BIND(enabled, world::effective_enabled(runtime, entity));
    if (!*enabled)
        return error(ErrorCode::invalid_member, "Main camera Entity or its ancestor is disabled.");
    ATELIER_TRY_BIND(snapshot, camera(entity));
    ATELIER_TRY_BIND(component, world::inspect_component(runtime, snapshot->component));
    if (!component->enabled)
        return error(ErrorCode::invalid_member, "Main CameraComponent is disabled.");
    return snapshot;
}
}
