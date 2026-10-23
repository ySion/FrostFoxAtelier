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
        return runtime.with_native<const StaticMeshComponent>(object, [](const StaticMeshComponent& value) -> Result<bool> { return bool(Access::geometry(value)); });
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
    entities_.reserve(poses.size());
    for (const auto& pose : poses) {
        entities_.push_back(pose.entity);
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
const detail::Presented::Offset* detail::Presented::find(std::span<const ObjectId> chain) const {
    for (const auto node : chain)
        if (const auto found = offsets_.find(node); found != offsets_.end()) return found->second ? &*found->second : nullptr;
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
    std::shared_ptr<const std::vector<material::SurfaceRef>> uncached;
    auto read = runtime.with_native<const StaticMeshComponent>(object, [&](const StaticMeshComponent& value) -> Result<bool> {
        const auto& geometry = Access::geometry(value);
        const bool slotted = !Access::material_slots(value).empty() || !Access::default_slots(value).empty();
        const bool same_base = cached && cached->geometry == geometry && same_bytes(cached->material, value.material);
        if (!same_base && !valid_materials_.contains(slot, value.material)) {
            auto invalid = runtime.validate_value(types_.material, std::as_bytes(std::span{&value.material, 1}));
            if (!invalid) return std::unexpected(invalid.error());
            if (*invalid) { rejected = std::move(**invalid); return false; }
            valid_materials_.insert(slot, value.material);
        }
        // Unslotted surfaces follow from geometry and Material; slots with a bound
        // or default material capture their resources on every update.
        if (same_base && !slotted && !slotted_[slot]) return moved;
        auto surfaces = detail::material_surfaces(runtime, object, value);
        if (!surfaces) { unusable = std::move(surfaces.error()); return false; }
        if (*surfaces != Access::surfaces(value)) uncached = *surfaces;
        slotted_[slot] = slotted;
        if (same_base && cached->surfaces == *surfaces) return moved;
        if (!cached) cached.emplace(RenderItem{owner, object, **matrix, geometry, value.material, std::move(*surfaces)});
        else { cached->geometry = geometry; cached->material = value.material; cached->surfaces = std::move(*surfaces); }
        return true;
    });
    if (read && uncached)
        if (auto stored = detail::store_surfaces(runtime, object, std::move(uncached)); !stored) read = std::unexpected(stored.error());
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

Result<void> Scene::index_layout(world::World::SpatialPass& pass, std::span<const world::CapabilityMember> meshes) const {
    mesh_dependents_.clear(); mesh_components_.clear(); chains_.clear(); chain_starts_.clear(); light_sources_.clear();
    // A chain that cannot be resolved ends where it breaks; reading that slot reports the failure.
    const auto chain = [&](ObjectId entity, auto&& visit) {
        for (auto node = entity; node != ObjectId{};) {
            visit(node);
            auto parent = pass.parent(node);
            if (!parent) break;
            node = *parent;
        }
    };
    for (std::size_t slot = 0; slot < meshes.size(); ++slot) {
        const auto index = static_cast<std::uint32_t>(slot);
        mesh_components_.emplace(meshes[slot].component, index);
        chain_starts_.push_back(static_cast<std::uint32_t>(chains_.size()));
        chain(meshes[slot].entity, [&](ObjectId node) { chains_.push_back(node); mesh_dependents_[node].push_back(index); });
    }
    chain_starts_.push_back(static_cast<std::uint32_t>(chains_.size()));
    auto lights = world_->members(lights_);
    if (!lights) return std::unexpected(lights.error());
    for (const auto& light : *lights) {
        light_sources_.insert(light.component);
        chain(light.entity, [&](ObjectId node) { light_sources_.insert(node); });
    }
    return {};
}

Result<void> Scene::update_meshes(std::uint64_t serial, const detail::Presented* presented) const {
    auto& runtime = world_->runtime();
    auto pass = world_->spatial_pass(DiagnosticPhase::presentation);
    if (!pass) return std::unexpected(pass.error());
    auto meshes = world_->members(meshes_);
    if (!meshes) return std::unexpected(meshes.error());
    const auto count = meshes->size();
    // A failed update leaves unread writes behind; the next one reads every slot.
    const bool tracked = std::exchange(tracked_, false);
    dirty_.clear();
    // The bucket keeps its components and order until World::revision advances.
    if (!tracked || layout_revision_ != world_->revision()) {
        layout_revision_ = world_->revision();
        mesh_slots_.assign(count, {});
        slotted_.assign(count, false);
        presented_slots_ = 0;
        presented_list_.clear();
        volatile_.clear();
        volatile_flags_.assign(count, false);
        marks_.assign(count, 0);
        presented_marks_.assign(count, 0);
        mesh_layout_ = version_ = serial;
        lights_dirty_ = true;
        if (auto indexed = index_layout(*pass, *meshes); !indexed) return indexed;
        for (std::size_t slot = 0; slot < count; ++slot) dirty_.push_back(static_cast<std::uint32_t>(slot));
    } else {
        const auto mark = [&](std::uint32_t slot) { if (std::exchange(marks_[slot], serial) != serial) dirty_.push_back(slot); };
        runtime.changed_objects(cursor_, written_);
        for (const auto object : written_) {
            if (const auto found = mesh_dependents_.find(object); found != mesh_dependents_.end()) for (const auto slot : found->second) mark(slot);
            if (const auto found = mesh_components_.find(object); found != mesh_components_.end()) mark(found->second);
            if (!lights_dirty_ && light_sources_.contains(object)) lights_dirty_ = true;
        }
        std::erase_if(volatile_, [&](std::uint32_t slot) { return !volatile_flags_[slot]; });
        for (const auto slot : volatile_) mark(slot);
        std::ranges::sort(dirty_);
    }
    valid_materials_.resize(count);
    for (const auto slot : dirty_) {
        auto changed = update_mesh(*pass, slot, (*meshes)[slot]);
        if (!changed) return std::unexpected(changed.error());
        if (*changed) mesh_slots_[slot].changed = version_ = serial;
        const bool always = slotted_[slot] || !mesh_slots_[slot].item;
        if (always && !volatile_flags_[slot]) volatile_.push_back(slot);
        volatile_flags_[slot] = always;
    }
    // Only slots below a listed Entity, and those presented before, can change their presented matrix.
    if (presented && (!presented->empty() || !presented_list_.empty())) {
        std::vector<std::uint32_t> candidates;
        const auto mark = [&](std::uint32_t slot) { if (std::exchange(presented_marks_[slot], serial) != serial) candidates.push_back(slot); };
        for (const auto entity : presented->entities())
            if (const auto found = mesh_dependents_.find(entity); found != mesh_dependents_.end()) for (const auto slot : found->second) mark(slot);
        for (const auto slot : presented_list_) mark(slot);
        presented_list_.clear();
        for (const auto slot : candidates) {
            auto& cached = mesh_slots_[slot];
            std::optional<math::Matrix4> drawn;
            if (cached.item) {
                const std::span chain{chains_.data() + chain_starts_[slot], chains_.data() + chain_starts_[slot + 1]};
                if (const auto* offset = presented->find(chain)) drawn = cached.item->world * offset->matrix;
            }
            if (drawn) presented_list_.push_back(slot);
            if (drawn.has_value() != cached.presented.has_value() || (drawn && !same_bytes(*drawn, *cached.presented))) {
                cached.presented = drawn;
                cached.changed = version_ = serial;
            }
        }
        presented_slots_ = presented_list_.size();
    }
    // Nothing but this update writes while it runs: surface caches and captured material
    // resources, which slotted meshes re-read on every update anyway. Taking the cursor
    // after them keeps the next update from reading those slots again.
    auto cursor = runtime.change_cursor();
    if (!cursor) return std::unexpected(cursor.error());
    cursor_ = *cursor;
    tracked_ = true;
    return {};
}

Result<std::uint64_t> Scene::update(std::span<const PresentedPose> presented) const {
    const auto serial = ++serial_;
    // A still frame shares one empty table instead of building its own.
    static const detail::Presented still{{}};
    std::optional<detail::Presented> moving;
    const auto& offsets = presented.empty() ? still : moving.emplace(presented);
    if (auto meshes = update_meshes(serial, &offsets); !meshes) return std::unexpected(meshes.error());
    // Lights are extracted again when one was written, one is not drawn, or a presented pose moves or moved one.
    const bool moving_lights = std::ranges::any_of(offsets.entities(), [&](ObjectId entity) { return light_sources_.contains(entity); });
    if (lights_dirty_ || lights_failed_ || lights_moved_ || moving_lights) {
        bool failed{}, moved{};
        auto lights = extract_lights(&offsets, &failed, &moved);
        if (!lights) return std::unexpected(lights.error());
        lights_dirty_ = false; lights_failed_ = failed; lights_moved_ = moved;
        if (!std::ranges::equal(*lights, lights_cache_, same_light)) {
            lights_cache_ = std::move(*lights);
            lights_changed_ = version_ = serial;
        }
    } else if (auto pass = world_->spatial_pass(DiagnosticPhase::lighting); !pass) {
        // The Environment reports in the lighting pass; a new one keeps counting its persistent failures.
        return std::unexpected(pass.error());
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
