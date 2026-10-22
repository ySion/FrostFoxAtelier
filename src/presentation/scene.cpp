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

detail::Presented::Presented(std::span<const PresentedPose> poses) {
    offsets_.reserve(poses.size());
    for (const auto& pose : poses) {
        auto& offset = offsets_[pose.entity];
        offset.reset();
        if (same_bytes(pose.position, pose.world_position) && same_bytes(pose.rotation, pose.world_rotation)) continue;
        // Row vectors: T(-world position) R(presented * world^-1) T(presented position), rigid by construction.
        const auto rotation = math::try_normalize(pose.rotation * math::conjugate(pose.world_rotation));
        if (!rotation) continue;
        Offset value{math::rotation_matrix(*rotation), *rotation};
        const auto moved = pose.position - math::rotate(*rotation, pose.world_position);
        value.matrix[12] = moved.x; value.matrix[13] = moved.y; value.matrix[14] = moved.z;
        offset = value;
    }
}
Result<const detail::Presented::Offset*> detail::Presented::find(world::World::SpatialPass& pass, ObjectId entity) const {
    // The chain was composed by this pass's matrix() of `entity`, so it ends at world space.
    for (auto node = entity; node != ObjectId{};) {
        if (const auto found = offsets_.find(node); found != offsets_.end()) return found->second ? &*found->second : nullptr;
        auto parent = pass.parent(node);
        if (!parent) return std::unexpected(parent.error());
        node = *parent;
    }
    return nullptr;
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

Result<void> Scene::update_meshes(std::uint64_t serial, const detail::Presented* presented) const {
    auto pass = world_->spatial_pass(DiagnosticPhase::presentation);
    if (!pass) return std::unexpected(pass.error());
    auto meshes = world_->members(meshes_);
    if (!meshes) return std::unexpected(meshes.error());
    // The bucket keeps its components and order until World::revision advances.
    if (layout_revision_ != world_->revision()) {
        layout_revision_ = world_->revision();
        mesh_slots_.assign(meshes->size(), {});
        slotted_.assign(meshes->size(), false);
        presented_slots_ = 0;
        mesh_layout_ = version_ = serial;
    }
    valid_materials_.resize(meshes->size());
    // A still scene with nothing presented skips the chain searches.
    const bool presenting = presented && (!presented->empty() || presented_slots_ != 0);
    for (std::size_t slot = 0; slot < meshes->size(); ++slot) {
        auto changed = update_mesh(*pass, slot, (*meshes)[slot]);
        if (!changed) return std::unexpected(changed.error());
        auto& cached = mesh_slots_[slot];
        if (presenting) {
            std::optional<math::Matrix4> drawn;
            if (cached.item && !presented->empty()) {
                auto offset = presented->find(*pass, cached.item->entity);
                if (!offset) return std::unexpected(offset.error());
                if (*offset) drawn = cached.item->world * (*offset)->matrix;
            }
            if (drawn.has_value() != cached.presented.has_value() || (drawn && !same_bytes(*drawn, *cached.presented))) {
                presented_slots_ = presented_slots_ + drawn.has_value() - cached.presented.has_value();
                cached.presented = drawn;
                *changed = true;
            }
        }
        if (*changed) cached.changed = version_ = serial;
    }
    return {};
}

Result<std::uint64_t> Scene::update(std::span<const PresentedPose> presented) const {
    const auto serial = ++serial_;
    // A still frame shares one empty table instead of building its own.
    static const detail::Presented still{{}};
    std::optional<detail::Presented> moving;
    const auto& offsets = presented.empty() ? still : moving.emplace(presented);
    if (auto meshes = update_meshes(serial, &offsets); !meshes) return std::unexpected(meshes.error());
    auto lights = extract_lights(&offsets);
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
    if (auto meshes = update_meshes(++serial_, nullptr); !meshes) return std::unexpected(meshes.error());
    std::vector<RenderItem> result;
    result.reserve(static_cast<std::size_t>(std::ranges::count_if(mesh_slots_, [](const MeshSlot& slot) { return slot.item.has_value(); })));
    for (const auto& slot : mesh_slots_) if (slot.item) result.push_back(*slot.item);
    return result;
}

}
