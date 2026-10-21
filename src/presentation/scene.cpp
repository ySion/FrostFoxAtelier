#include "presentation/detail.hpp"

namespace atelier::presentation {
using detail::Access;
using world::DiagnosticKind;
using world::DiagnosticPhase;

world::Diagnostic detail::diagnostic(DiagnosticKind kind, DiagnosticPhase phase, ObjectId object, ObjectId entity,
    std::vector<std::string> field) {
    world::Diagnostic result;
    result.kind = kind; result.phase = phase;
    result.object = object; result.entity = entity; result.field = std::move(field);
    return result;
}

Result<Scene> Scene::create(world::World& world, const Types& types) {
    Scene scene{world, types};
    auto meshes = world.register_capability(types.static_mesh_component, [](Runtime& runtime, ObjectId object) {
        return runtime.with_native<StaticMeshComponent>(object, [](StaticMeshComponent& value) -> Result<bool> { return bool(Access::geometry(value)); });
    });
    if (!meshes) return std::unexpected(meshes.error());
    auto lights = world.register_capability(types.light_component);
    if (!lights) return std::unexpected(lights.error());
    auto environments = world.register_capability(types.environment_component);
    if (!environments) return std::unexpected(environments.error());
    if (auto admitted = world.add_admission(types.environment_component, detail::unique_environment(*environments)); !admitted)
        return std::unexpected(admitted.error());
    scene.meshes_ = *meshes; scene.lights_ = *lights; scene.environments_ = *environments;
    return scene;
}

Result<std::vector<RenderItem>> Scene::render_items() const {
    auto pass = world_->spatial_pass(DiagnosticPhase::presentation);
    if (!pass) return std::unexpected(pass.error());
    auto& runtime = world_->runtime();
    std::vector<RenderItem> result;
    auto meshes = world_->members(meshes_);
    if (!meshes) return std::unexpected(meshes.error());
    result.reserve(meshes->size());
    valid_materials_.resize(meshes->size());
    for (std::size_t slot = 0; slot < meshes->size(); ++slot) {
        const auto& [owner, object, active] = (*meshes)[slot];
        (void)active;
        // Isolated items are skipped: an unusable space is reported once on
        // its Entity, an invalid material on its component.
        auto matrix = pass->matrix(owner);
        if (!matrix) return std::unexpected(matrix.error());
        if (!*matrix || pass->degenerate(owner, **matrix, "A zero scale axis makes this space degenerate; meshes in it are not drawn."))
            continue;
        RenderItem item{owner, object, **matrix, {}, {}};
        Result<std::optional<ValidationFailure>> invalid = std::nullopt;
        Result<std::shared_ptr<const std::vector<material::SurfaceRef>>> surfaces;
        auto read = runtime.with_native<StaticMeshComponent>(object, [&](StaticMeshComponent& value) -> Result<void> {
            item.geometry = Access::geometry(value);
            item.material = value.material;
            if (!valid_materials_.contains(slot, value.material)) {
                invalid = runtime.validate_value(types_.material, std::as_bytes(std::span{&value.material, 1}));
                if (!invalid || *invalid) return {};
                valid_materials_.insert(slot, value.material);
            }
            surfaces = detail::material_surfaces(runtime, object, value);
            return {};
        });
        if (!read) {
            if (!world_->contain(read.error(), detail::diagnostic(DiagnosticKind::invalid_value, DiagnosticPhase::presentation, object, owner)))
                return std::unexpected(read.error());
            continue;
        }
        if (!invalid) return std::unexpected(invalid.error());
        if (*invalid) {
            const auto& failure = **invalid;
            if (!world_->contain(failure.error, world::validation_diagnostic(failure, DiagnosticPhase::presentation, object, owner, {"material"})))
                return std::unexpected(failure.error);
            continue;
        }
        if (!surfaces) {
            if (!world_->contain(surfaces.error(), detail::diagnostic(DiagnosticKind::invalid_value, DiagnosticPhase::presentation, object, owner, {"materials"})))
                return std::unexpected(surfaces.error());
            continue;
        }
        item.surfaces = std::move(*surfaces);
        result.push_back(std::move(item));
    }
    return result;
}

}
