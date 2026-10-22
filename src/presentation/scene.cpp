#include "presentation/detail.hpp"
#include <algorithm>
#include <atomic>

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
    // The admission guards later changes; content already in the World is checked here,
    // before registrations that a rejected creation would leave behind.
    auto environments = world.register_capability(types.environment_component);
    if (!environments) return std::unexpected(environments.error());
    auto unique = detail::unique_environment(*environments);
    if (auto existing = unique(world, {}); !existing) return std::unexpected(existing.error());
    auto meshes = world.register_capability(types.static_mesh_component, [](Runtime& runtime, ObjectId object) {
        return runtime.with_native<StaticMeshComponent>(object, [](StaticMeshComponent& value) -> Result<bool> { return bool(Access::geometry(value)); });
    });
    if (!meshes) return std::unexpected(meshes.error());
    auto lights = world.register_capability(types.light_component);
    if (!lights) return std::unexpected(lights.error());
    if (auto admitted = world.add_admission(types.environment_component, std::move(unique)); !admitted)
        return std::unexpected(admitted.error());
    scene.meshes_ = *meshes; scene.lights_ = *lights; scene.environments_ = *environments;
    static std::atomic<std::uint64_t> created;
    scene.identity_ = ++created;
    return scene;
}

namespace {
bool same_bytes(const auto& a, const auto& b) noexcept {
    static_assert(std::is_trivially_copyable_v<std::remove_cvref_t<decltype(a)>>);
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}
bool same_light(const LightRenderItem& a, const LightRenderItem& b) noexcept {
    return a.entity == b.entity && a.component == b.component && a.position == b.position && a.direction == b.direction &&
        a.right == b.right && a.up == b.up && a.settings == b.settings;
}
}

Result<bool> Scene::update_mesh(world::World::SpatialPass& pass, std::size_t slot, const world::CapabilityMember& member) const {
    auto& runtime = world_->runtime();
    auto& cached = mesh_slots_[slot].item;
    const auto owner = member.entity, object = member.component;
    // Isolated and degenerate items leave the cache: an unusable space is
    // reported once on its Entity, an invalid material on its component.
    const auto drop = [&] { const bool had = cached.has_value(); cached.reset(); return had; };
    auto matrix = pass.matrix(owner);
    if (!matrix) return std::unexpected(matrix.error());
    if (!*matrix || pass.degenerate(owner, **matrix, "A zero scale axis makes this space degenerate; meshes in it are not drawn."))
        return drop();
    const bool moved = !cached || !same_bytes(cached->world, **matrix);
    std::optional<ValidationFailure> rejected;
    std::optional<Error> unusable;
    auto read = runtime.with_native<StaticMeshComponent>(object, [&](StaticMeshComponent& value) -> Result<bool> {
        const auto& geometry = Access::geometry(value);
        const bool slotted = !Access::material_slots(value).empty();
        const bool same_base = cached && cached->geometry == geometry && same_bytes(cached->material, value.material);
        if (!same_base && !valid_materials_.contains(slot, value.material)) {
            auto invalid = runtime.validate_value(types_.material, std::as_bytes(std::span{&value.material, 1}));
            if (!invalid) return std::unexpected(invalid.error());
            if (*invalid) { rejected = std::move(**invalid); return false; }
            valid_materials_.insert(slot, value.material);
        }
        // Unslotted surfaces follow from geometry and Material; slotted ones
        // capture their resources on every update.
        if (same_base && !slotted && !slotted_[slot]) return moved;
        auto surfaces = detail::material_surfaces(runtime, object, value);
        if (!surfaces) { unusable = std::move(surfaces.error()); return false; }
        slotted_[slot] = slotted;
        if (same_base && cached->surfaces == *surfaces) return moved;
        if (!cached) cached.emplace(RenderItem{owner, object, **matrix, geometry, value.material, std::move(*surfaces)});
        else { cached->geometry = geometry; cached->material = value.material; cached->surfaces = std::move(*surfaces); }
        return true;
    });
    if (!read) {
        if (!world_->contain(read.error(), detail::diagnostic(DiagnosticKind::invalid_value, DiagnosticPhase::presentation, object, owner)))
            return std::unexpected(read.error());
        return drop();
    }
    if (rejected) {
        if (!world_->contain(rejected->error, world::validation_diagnostic(*rejected, DiagnosticPhase::presentation, object, owner, {"material"})))
            return std::unexpected(rejected->error);
        return drop();
    }
    if (unusable) {
        if (!world_->contain(*unusable, detail::diagnostic(DiagnosticKind::invalid_value, DiagnosticPhase::presentation, object, owner, {"materials"})))
            return std::unexpected(*unusable);
        return drop();
    }
    if (*read) cached->world = **matrix;
    return *read;
}

Result<void> Scene::update_meshes(std::uint64_t serial) const {
    auto pass = world_->spatial_pass(DiagnosticPhase::presentation);
    if (!pass) return std::unexpected(pass.error());
    auto meshes = world_->members(meshes_);
    if (!meshes) return std::unexpected(meshes.error());
    // The bucket keeps its components and order until World::revision advances.
    if (layout_revision_ != world_->revision()) {
        layout_revision_ = world_->revision();
        mesh_slots_.assign(meshes->size(), {});
        slotted_.assign(meshes->size(), false);
        mesh_layout_ = version_ = serial;
    }
    valid_materials_.resize(meshes->size());
    for (std::size_t slot = 0; slot < meshes->size(); ++slot) {
        auto changed = update_mesh(*pass, slot, (*meshes)[slot]);
        if (!changed) return std::unexpected(changed.error());
        if (*changed) mesh_slots_[slot].changed = version_ = serial;
    }
    return {};
}

Result<std::uint64_t> Scene::update() const {
    const auto serial = ++serial_;
    if (auto meshes = update_meshes(serial); !meshes) return std::unexpected(meshes.error());
    auto lights = light_items();
    if (!lights) return std::unexpected(lights.error());
    if (!std::ranges::equal(*lights, lights_cache_, same_light)) {
        lights_cache_ = std::move(*lights);
        lights_changed_ = version_ = serial;
    }
    auto environment = this->environment();
    if (!environment) return std::unexpected(environment.error());
    if (!(*environment == environment_cache_)) {
        environment_cache_ = *environment;
        version_ = serial;
    }
    return version_;
}

Result<std::vector<RenderItem>> Scene::render_items() const {
    if (auto meshes = update_meshes(++serial_); !meshes) return std::unexpected(meshes.error());
    std::vector<RenderItem> result;
    result.reserve(static_cast<std::size_t>(std::ranges::count_if(mesh_slots_, [](const MeshSlot& slot) { return slot.item.has_value(); })));
    for (const auto& slot : mesh_slots_) if (slot.item) result.push_back(*slot.item);
    return result;
}

}
