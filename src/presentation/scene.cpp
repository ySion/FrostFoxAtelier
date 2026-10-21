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
    // A failed call leaves the flag set, so version() keeps advancing.
    isolated_meshes_ = true;
    bool isolated = false;
    for (std::size_t slot = 0; slot < meshes->size(); ++slot) {
        const auto& [owner, object, active] = (*meshes)[slot];
        (void)active;
        // Isolated items are skipped: an unusable space is reported once on
        // its Entity, an invalid material on its component.
        auto matrix = pass->matrix(owner);
        if (!matrix) return std::unexpected(matrix.error());
        if (!*matrix) { isolated = true; continue; }
        if (pass->degenerate(owner, **matrix, "A zero scale axis makes this space degenerate; meshes in it are not drawn."))
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
            isolated = true;
            continue;
        }
        if (!invalid) return std::unexpected(invalid.error());
        if (*invalid) {
            const auto& failure = **invalid;
            if (!world_->contain(failure.error, world::validation_diagnostic(failure, DiagnosticPhase::presentation, object, owner, {"material"})))
                return std::unexpected(failure.error);
            isolated = true;
            continue;
        }
        if (!surfaces) {
            if (!world_->contain(surfaces.error(), detail::diagnostic(DiagnosticKind::invalid_value, DiagnosticPhase::presentation, object, owner, {"materials"})))
                return std::unexpected(surfaces.error());
            isolated = true;
            continue;
        }
        item.surfaces = std::move(*surfaces);
        result.push_back(std::move(item));
    }
    isolated_meshes_ = isolated;
    return result;
}

namespace {
bool same_bytes(const auto& a, const auto& b) noexcept {
    static_assert(std::is_trivially_copyable_v<std::remove_cvref_t<decltype(a)>>);
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}
// An isolatable object failure only means the item is not presented now.
Result<bool> failed(const Error& failure) {
    if (!world::isolatable(failure)) return std::unexpected(failure);
    return true;
}
}

Result<std::uint64_t> Scene::version() const {
    auto pass = world_->spatial_probe();
    if (!pass) return std::unexpected(pass.error());
    auto& runtime = world_->runtime();
    bool changed = isolated_meshes_ || isolated_lights_ || isolated_environment_ || seen_.world != world_->revision();
    seen_.world = world_->revision();

    auto meshes = world_->members(meshes_);
    if (!meshes) return std::unexpected(meshes.error());
    changed |= seen_.meshes.size() != meshes->size();
    seen_.meshes.resize(meshes->size());
    for (std::size_t slot = 0; slot < meshes->size(); ++slot) {
        const auto& member = (*meshes)[slot];
        auto& seen = seen_.meshes[slot];
        auto matrix = pass->matrix(member.entity);
        if (!matrix) return std::unexpected(matrix.error());
        if (!*matrix) { changed = true; seen.placed = false; continue; }
        const bool flat = pass->degenerate(member.entity, **matrix, {});
        changed |= !seen.placed || seen.flat != flat || !same_bytes(seen.world, **matrix);
        seen.placed = true; seen.flat = flat; seen.world = **matrix;
        if (flat) continue;
        auto read = runtime.with_native<StaticMeshComponent>(member.component, [&](StaticMeshComponent& value) -> Result<bool> {
            // Shared pointers are copied only when they change: a copy per mesh and frame costs atomic updates.
            bool differs = false;
            if (seen.geometry != Access::geometry(value)) { differs = true; seen.geometry = Access::geometry(value); }
            if (!same_bytes(seen.material, value.material)) { differs = true; seen.material = value.material; }
            // Without material slots the surfaces follow from geometry and Material.
            const bool slotted = !Access::material_slots(value).empty();
            if (seen.slotted != slotted) { differs = true; seen.slotted = slotted; }
            if (!slotted) return differs;
            auto surfaces = detail::material_surfaces(runtime, member.component, value);
            if (!surfaces) return failed(surfaces.error());
            if (seen.surfaces != *surfaces) { differs = true; seen.surfaces = std::move(*surfaces); }
            return differs;
        });
        if (!read) { auto isolated = failed(read.error()); if (!isolated) return std::unexpected(isolated.error()); }
        changed |= !read || *read;
    }

    auto lights = world_->members(lights_);
    if (!lights) return std::unexpected(lights.error());
    changed |= seen_.lights.size() != lights->size();
    seen_.lights.resize(lights->size());
    for (std::size_t slot = 0; slot < lights->size(); ++slot) {
        const auto& member = (*lights)[slot];
        if (!member.active) continue;
        auto& seen = seen_.lights[slot];
        auto settings = read_light(runtime, member.component);
        if (!settings) { if (auto isolated = failed(settings.error()); !isolated) return std::unexpected(isolated.error()); changed = true; continue; }
        changed |= !same_bytes(seen.settings, *settings);
        seen.settings = *settings;
        auto matrix = pass->matrix(member.entity);
        if (!matrix) return std::unexpected(matrix.error());
        auto frame = *matrix ? pass->orientation(member.entity) : Result<std::optional<world::World::Orientation>>{};
        if (!frame) return std::unexpected(frame.error());
        if (!*frame) { changed = true; seen.placed = false; continue; }
        changed |= !seen.placed || !same_bytes(seen.world, **matrix) || !same_bytes(seen.frame.rotation, (*frame)->rotation) ||
            seen.frame.mirrored != (*frame)->mirrored;
        seen.placed = true; seen.world = **matrix; seen.frame = **frame;
    }

    auto environments = world_->members(environments_);
    if (!environments) return std::unexpected(environments.error());
    ObjectId active;
    for (const auto& member : *environments) {
        if (!member.active) continue;
        active = member.component;
        auto settings = runtime.with_native<const EnvironmentComponent>(member.component,
            [](const EnvironmentComponent& value) -> Result<rendering::SceneLightingSettings> { return environment_settings(value); });
        if (!settings) { if (auto isolated = failed(settings.error()); !isolated) return std::unexpected(isolated.error()); changed = true; break; }
        // Not-a-number values compare unequal and keep advancing; extraction rejects them.
        changed |= !(seen_.settings == *settings);
        seen_.settings = *settings;
        break;
    }
    changed |= seen_.environment != active;
    seen_.environment = active;
    if (changed) ++version_;
    return version_;
}

}
