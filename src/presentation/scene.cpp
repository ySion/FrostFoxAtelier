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
    result.kind = kind;
    result.phase = phase;
    result.object = object;
    result.entity = entity;
    result.field = std::move(field);
    return result;
}

Result<Scene> Scene::create(world::World& world, const Types& types) {
    Scene scene{world, types};
    // The admission guards later changes; content already in the World is checked here,
    // before registrations that a rejected creation would leave behind.
    ATELIER_TRY_BIND(environments, world.register_capability(types.environment_component));
    auto unique = detail::unique_environment(*environments);
    ATELIER_TRY(unique(world, {}));
    ATELIER_TRY_BIND(cameras, world.register_capability(types.camera_component));
    auto camera_unique = detail::unique_camera(*cameras);
    ATELIER_TRY(camera_unique(world, {}));
    auto meshes = world.register_capability(types.static_mesh_component, [](Runtime& runtime, ObjectId object) {
        return runtime.with_native<const StaticMeshComponent>(
            object, [](const StaticMeshComponent& value) -> Result<bool> { return bool(Access::geometry(value)); });
    });
    if (!meshes)
        return std::unexpected(meshes.error());
    ATELIER_TRY_BIND(lights, world.register_capability(types.light_component));
    ATELIER_TRY(world.add_admission(types.environment_component, std::move(unique)));
    ATELIER_TRY(world.add_admission(types.camera_component, std::move(camera_unique)));
    scene.meshes_ = *meshes;
    scene.lights_ = *lights;
    scene.environments_ = *environments;
    scene.cameras_ = *cameras;
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
    return a.entity == b.entity && a.component == b.component && a.position == b.position &&
           a.direction == b.direction && a.right == b.right && a.up == b.up && a.settings == b.settings;
}
}

detail::Presented::Presented(std::span<const world::PresentedPose> poses) {
    offsets_.reserve(poses.size());
    entities_.reserve(poses.size());
    for (const auto& pose : poses) {
        entities_.push_back(pose.entity);
        auto& offset = offsets_[pose.entity];
        offset.reset();
        if (same_bytes(pose.position, pose.world_position) && same_bytes(pose.rotation, pose.world_rotation))
            continue;
        // Row vectors: T(-world position) R(presented * world^-1) T(presented position), rigid by construction.
        const auto rotation = math::try_normalize(pose.rotation * math::conjugate(pose.world_rotation));
        if (!rotation)
            continue;
        Offset value{math::rotation_matrix(*rotation), *rotation};
        const auto moved = pose.position - math::rotate(*rotation, pose.world_position);
        value.matrix[12] = moved.x;
        value.matrix[13] = moved.y;
        value.matrix[14] = moved.z;
        offset = value;
    }
}

Result<const detail::Presented::Offset*> detail::Presented::find(world::World::SpatialPass& pass,
                                                                 ObjectId entity) const {
    // The chain was composed by this pass's matrix() of `entity`, so it ends at world space.
    for (auto node = entity; node != ObjectId{};) {
        if (const auto found = offsets_.find(node); found != offsets_.end())
            return found->second ? &*found->second : nullptr;
        ATELIER_TRY_BIND(parent, pass.parent(node));
        node = *parent;
    }
    return nullptr;
}

const detail::Presented::Offset* detail::Presented::find(std::span<const ObjectId> chain) const {
    for (const auto node : chain)
        if (const auto found = offsets_.find(node); found != offsets_.end())
            return found->second ? &*found->second : nullptr;
    return nullptr;
}

Result<bool> Scene::update_mesh(world::World::SpatialPass& pass, std::size_t slot,
                                const world::CapabilityMember& member) const {
    auto& runtime = world_->runtime();
    auto& cached = mesh_slots_[slot].item;
    const auto owner = member.entity;
    const auto object = member.component;
    // Isolated and degenerate items leave the cache: an unusable space is
    // reported once on its Entity, an invalid material on its component.
    const auto drop = [&] {
        const bool had = cached.has_value();
        cached.reset();
        return had;
    };
    ATELIER_TRY_BIND(matrix, pass.matrix(owner));
    if (!*matrix ||
        pass.degenerate(owner, **matrix, "A zero scale axis makes this space degenerate; meshes in it are not drawn."))
        return drop();
    const bool moved = !cached || !same_bytes(cached->world, **matrix);
    if (cached && !materials_dirty_[slot]) {
        if (moved)
            cached->world = **matrix;
        return moved;
    }
    std::optional<Error> unusable;
    std::unordered_set<ObjectId> dependencies;
    std::shared_ptr<const std::vector<material::ProgramSurfaceRef>> uncached;
    auto read =
        runtime.with_native<const StaticMeshComponent>(object, [&](const StaticMeshComponent& value) -> Result<bool> {
            const auto& geometry = Access::geometry(value);
            const bool same_base = cached && cached->geometry == geometry;
            auto surfaces = detail::material_surfaces(runtime, object, value, &dependencies);
            if (!surfaces) {
                unusable = std::move(surfaces.error());
                return false;
            }
            if (*surfaces != Access::surfaces(value))
                uncached = *surfaces;
            if (same_base && cached->surfaces == *surfaces)
                return moved;
            if (!cached)
                cached.emplace(RenderItem{owner, object, **matrix, geometry, std::move(*surfaces)});
            else {
                cached->geometry = geometry;
                cached->surfaces = std::move(*surfaces);
            }
            return true;
        });
    if (read && uncached)
        if (auto stored = detail::store_surfaces(runtime, object, std::move(uncached)); !stored)
            read = std::unexpected(stored.error());
    if (!read) {
        if (!world_->contain(read.error(), detail::diagnostic(DiagnosticKind::invalid_value,
                                                              DiagnosticPhase::presentation, object, owner)))
            return std::unexpected(read.error());
        return drop();
    }
    if (unusable) {
        if (!world_->contain(*unusable, detail::diagnostic(DiagnosticKind::invalid_value, DiagnosticPhase::presentation,
                                                           object, owner, {"materials"})))
            return std::unexpected(*unusable);
        return drop();
    }
    // Content identity can stay equal after a reparent or dormant texture edit.
    // Commit the actual live edges independently of the visible change/version.
    index_materials(slot, std::move(dependencies));
    materials_dirty_[slot] = false;
    if (*read)
        cached->world = **matrix;
    return *read;
}

void Scene::index_materials(std::size_t slot, std::unordered_set<ObjectId> dependencies) const {
    auto& previous = material_dependencies_[slot];
    const auto index = static_cast<std::uint32_t>(slot);
    for (const auto object : previous)
        if (!dependencies.contains(object)) {
            const auto found = material_dependents_.find(object);
            found->second.erase(index);
            if (found->second.empty())
                material_dependents_.erase(found);
        }
    for (const auto object : dependencies)
        if (!previous.contains(object))
            material_dependents_[object].insert(index);
    previous = std::move(dependencies);
}

Result<void> Scene::index_layout(world::World::SpatialPass& pass,
                                 std::span<const world::CapabilityMember> meshes) const {
    mesh_dependents_.clear();
    mesh_components_.clear();
    chains_.clear();
    chain_starts_.clear();
    light_sources_.clear();
    // A chain that cannot be resolved ends where it breaks; reading that slot reports the failure.
    const auto chain = [&](ObjectId entity, auto&& visit) {
        for (auto node = entity; node != ObjectId{};) {
            visit(node);
            auto parent = pass.parent(node);
            if (!parent)
                break;
            node = *parent;
        }
    };
    for (std::size_t slot = 0; slot < meshes.size(); ++slot) {
        const auto index = static_cast<std::uint32_t>(slot);
        mesh_components_.emplace(meshes[slot].component, index);
        chain_starts_.push_back(static_cast<std::uint32_t>(chains_.size()));
        chain(meshes[slot].entity, [&](ObjectId node) {
            chains_.push_back(node);
            mesh_dependents_[node].push_back(index);
        });
    }
    chain_starts_.push_back(static_cast<std::uint32_t>(chains_.size()));
    ATELIER_TRY_BIND(lights, world_->members(lights_));
    for (const auto& light : *lights) {
        light_sources_.insert(light.component);
        chain(light.entity, [&](ObjectId node) { light_sources_.insert(node); });
    }
    return {};
}

Result<void> Scene::update_meshes(std::uint64_t serial, const detail::Presented* presented) const {
    auto& runtime = world_->runtime();
    ATELIER_TRY_BIND(pass, world_->spatial_pass(DiagnosticPhase::presentation));
    ATELIER_TRY_BIND(meshes, world_->members(meshes_));
    const auto count = meshes->size();
    // A failed update leaves unread writes behind; the next one reads every slot.
    const bool tracked = std::exchange(tracked_, false);
    dirty_.clear();
    // The bucket keeps its components and order until World::revision advances.
    if (!tracked || layout_revision_ != world_->revision()) {
        layout_revision_ = world_->revision();
        mesh_slots_.assign(count, {});
        materials_dirty_.assign(count, true);
        material_dependencies_.assign(count, {});
        material_dependents_.clear();
        presented_list_.clear();
        volatile_.clear();
        volatile_flags_.assign(count, false);
        marks_.assign(count, 0);
        presented_marks_.assign(count, 0);
        mesh_layout_ = version_ = serial;
        lights_dirty_ = true;
        ATELIER_TRY(index_layout(*pass, *meshes));
        for (std::size_t slot = 0; slot < count; ++slot)
            dirty_.push_back(static_cast<std::uint32_t>(slot));
    } else {
        const auto mark = [&](std::uint32_t slot, bool material = false) {
            if (material)
                materials_dirty_[slot] = true;
            if (std::exchange(marks_[slot], serial) != serial)
                dirty_.push_back(slot);
        };
        runtime.changed_objects(cursor_, written_);
        for (const auto object : written_) {
            if (const auto found = mesh_dependents_.find(object); found != mesh_dependents_.end())
                for (const auto slot : found->second)
                    mark(slot);
            if (const auto found = mesh_components_.find(object); found != mesh_components_.end())
                mark(found->second, true);
            if (const auto found = material_dependents_.find(object); found != material_dependents_.end())
                for (const auto slot : found->second)
                    mark(slot, true);
            if (!lights_dirty_ && light_sources_.contains(object))
                lights_dirty_ = true;
        }
        // changed_objects lists live writes only. Check each distinct dependency
        // once for death/generation loss; successful fallback replaces its edges.
        for (const auto& [object, slots] : material_dependents_)
            if (runtime.changed_since(object, cursor_))
                for (const auto slot : slots)
                    mark(slot, true);
        std::erase_if(volatile_, [&](std::uint32_t slot) { return !volatile_flags_[slot]; });
        for (const auto slot : volatile_)
            mark(slot);
        std::ranges::sort(dirty_);
    }
    for (const auto slot : dirty_) {
        ATELIER_TRY_BIND(changed, update_mesh(*pass, slot, (*meshes)[slot]));
        if (*changed)
            mesh_slots_[slot].changed = version_ = serial;
        const bool always = !mesh_slots_[slot].item;
        if (always && !volatile_flags_[slot])
            volatile_.push_back(slot);
        volatile_flags_[slot] = always;
    }
    // Only slots below a listed Entity, and those presented before, can change their presented matrix.
    if (presented && (!presented->empty() || !presented_list_.empty())) {
        std::vector<std::uint32_t> candidates;
        const auto mark = [&](std::uint32_t slot) {
            if (std::exchange(presented_marks_[slot], serial) != serial)
                candidates.push_back(slot);
        };
        for (const auto entity : presented->entities())
            if (const auto found = mesh_dependents_.find(entity); found != mesh_dependents_.end())
                for (const auto slot : found->second)
                    mark(slot);
        for (const auto slot : presented_list_)
            mark(slot);
        presented_list_.clear();
        for (const auto slot : candidates) {
            auto& cached = mesh_slots_[slot];
            std::optional<math::Matrix4> drawn;
            if (cached.item) {
                const std::span chain{chains_.data() + chain_starts_[slot], chains_.data() + chain_starts_[slot + 1]};
                if (const auto* offset = presented->find(chain))
                    drawn = cached.item->world * offset->matrix;
            }
            if (drawn)
                presented_list_.push_back(slot);
            if (drawn.has_value() != cached.presented.has_value() ||
                (drawn && !same_bytes(*drawn, *cached.presented))) {
                cached.presented = drawn;
                cached.changed = version_ = serial;
            }
        }
    }
    // Nothing but this update writes while it runs: derived surfaces/resources
    // already consumed by the affected slots. Other Scenes have their own cursors.
    // Taking ours after these writes avoids recapturing our own publications.
    ATELIER_TRY_BIND(cursor, runtime.change_cursor());
    cursor_ = *cursor;
    tracked_ = true;
    return {};
}

Result<std::uint64_t> Scene::update(std::span<const world::PresentedPose> presented) const {
    const auto serial = ++serial_;
    // A still frame shares one empty table instead of building its own.
    static const detail::Presented still{{}};
    std::optional<detail::Presented> moving;
    const auto& offsets = presented.empty() ? still : moving.emplace(presented);
    ATELIER_TRY(update_meshes(serial, &offsets));
    // Lights are extracted again when one was written, one is not drawn, or a presented pose moves or moved one.
    const bool moving_lights =
        std::ranges::any_of(offsets.entities(), [&](ObjectId entity) { return light_sources_.contains(entity); });
    if (lights_dirty_ || lights_failed_ || lights_moved_ || moving_lights) {
        bool failed{};
        bool moved{};
        ATELIER_TRY_BIND(lights, extract_lights(&offsets, &failed, &moved));
        lights_dirty_ = false;
        lights_failed_ = failed;
        lights_moved_ = moved;
        if (!std::ranges::equal(*lights, lights_cache_, same_light)) {
            lights_cache_ = std::move(*lights);
            lights_changed_ = version_ = serial;
        }
    } else {
        // The Environment reports in the lighting pass; a new one keeps counting its persistent failures.
        ATELIER_TRY(world_->spatial_pass(DiagnosticPhase::lighting));
    }
    ATELIER_TRY_BIND(environment, this->environment());
    if (!(*environment == environment_cache_)) {
        environment_cache_ = *environment;
        version_ = serial;
    }
    return version_;
}

Result<std::vector<RenderItem>> Scene::render_items() const {
    ATELIER_TRY(update_meshes(++serial_, nullptr));
    std::vector<RenderItem> result;
    result.reserve(static_cast<std::size_t>(
        std::ranges::count_if(mesh_slots_, [](const MeshSlot& slot) { return slot.item.has_value(); })));
    for (const auto& slot : mesh_slots_)
        if (slot.item)
            result.push_back(*slot.item);
    return result;
}

}
