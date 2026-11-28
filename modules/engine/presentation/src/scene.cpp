#include "detail/detail.hpp"
#include <algorithm>
#include <atomic>

namespace atelier::presentation {
using detail::Access;

using world::DiagnosticKind;

using world::DiagnosticPhase;

world::Diagnostic detail::diagnostic(DiagnosticKind kind, DiagnosticPhase phase, ObjectId object, ObjectId entity,
                                     Array<String> field) {
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
    auto generators =
        world.register_capability(types.procedural_generator_component, [](Runtime& runtime, ObjectId object) {
            return runtime.with_native<const ProceduralGeneratorComponent>(
                object, [](const ProceduralGeneratorComponent& value) -> Result<bool> {
                    return detail::has_renderable_output(Access::output(value));
                });
        });
    if (!generators)
        return std::unexpected(generators.error());
    ATELIER_TRY_BIND(lights, world.register_capability(types.light_component));
    ATELIER_TRY(world.add_admission(types.environment_component, std::move(unique)));
    ATELIER_TRY(world.add_admission(types.camera_component, std::move(camera_unique)));
    scene.meshes_ = *meshes;
    scene.generators_ = *generators;
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

detail::Presented::Presented(Span<const world::PresentedPose> poses) {
    offsets_.reserve(poses.size());
    entities_.reserve(poses.size());
    for (const auto& pose : poses) {
        entities_.add(pose.entity);
        auto& offset = *offsets_.try_emplace(pose.entity).value;
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
        if (const auto* found = offsets_.find(node))
            return *found ? &**found : nullptr;
        ATELIER_TRY_BIND(parent, pass.parent(node));
        node = *parent;
    }
    return nullptr;
}

const detail::Presented::Offset* detail::Presented::find(Span<const ObjectId> chain) const {
    for (const auto node : chain)
        if (const auto* found = offsets_.find(node))
            return *found ? &**found : nullptr;
    return nullptr;
}

void detail::BucketTracker::clear_layout() {
    dependents.clear();
    components.clear();
    chains.clear();
    chain_starts.clear();
}

void detail::BucketTracker::reset(std::size_t count) {
    materials_dirty.assign(count, true);
    material_dependencies.assign(count, {});
    material_dependents.clear();
    presented_list.clear();
    volatile_slots.clear();
    volatile_flags.assign(count, false);
    marks.assign(count, 0);
    presented_marks.assign(count, 0);
}

void detail::BucketTracker::mark(std::uint32_t slot, std::uint64_t serial, bool material) {
    if (material)
        materials_dirty.set(slot);
    if (std::exchange(marks[slot], serial) != serial)
        dirty.add(slot);
}

void detail::BucketTracker::index_materials(std::size_t slot, Set<ObjectId> dependencies) {
    auto& previous = material_dependencies[slot];
    const auto index = static_cast<std::uint32_t>(slot);
    for (const auto object : previous)
        if (!dependencies.contains(object)) {
            auto* found = material_dependents.find(object);
            found->remove(index);
            if (found->empty())
                material_dependents.remove(object);
        }
    for (const auto object : dependencies)
        if (!previous.contains(object))
            material_dependents.try_emplace(object).value->add(index);
    previous = std::move(dependencies);
}

namespace {

template <class VisitChain>
void index_bucket_chains(detail::BucketTracker& tracker, Span<const world::CapabilityMember> members,
                         VisitChain&& chain) {
    tracker.clear_layout();
    tracker.components.reserve(members.size());
    tracker.chain_starts.reserve(members.size() + 1);
    for (std::size_t slot = 0; slot < members.size(); ++slot) {
        const auto index = static_cast<std::uint32_t>(slot);
        tracker.components.try_emplace(members[slot].component, index);
        tracker.chain_starts.add(static_cast<std::uint32_t>(tracker.chains.size()));
        chain(members[slot].entity, [&](ObjectId node) {
            tracker.chains.add(node);
            tracker.dependents.try_emplace(node).value->add(index);
        });
    }
    tracker.chain_starts.add(static_cast<std::uint32_t>(tracker.chains.size()));
}

void mark_written_object(detail::BucketTracker& tracker, ObjectId object, std::uint64_t serial) {
    if (const auto* found = tracker.dependents.find(object))
        for (const auto slot : *found)
            tracker.mark(slot, serial);
    if (const auto* found = tracker.components.find(object))
        tracker.mark(*found, serial, true);
    if (const auto* found = tracker.material_dependents.find(object))
        for (const auto slot : *found)
            tracker.mark(slot, serial, true);
}

void finalize_dirty_slots(Runtime& runtime, std::uint64_t cursor, detail::BucketTracker& tracker,
                          std::uint64_t serial) {
    for (const auto [object, slots] : tracker.material_dependents)
        if (runtime.changed_since(object, cursor))
            for (const auto slot : slots)
                tracker.mark(slot, serial, true);
    tracker.volatile_slots.remove_if([&](std::uint32_t slot) { return !tracker.volatile_flags[slot]; });
    for (const auto slot : tracker.volatile_slots)
        tracker.mark(slot, serial);
    std::ranges::sort(tracker.dirty);
}

template <class SlotT>
void update_bucket_presented(detail::BucketTracker& tracker, Array<SlotT>& slots,
                             const detail::Presented& presented, std::uint64_t serial, std::uint64_t& version) {
    if (presented.empty() && tracker.presented_list.empty())
        return;
    Array<std::uint32_t> candidates;
    const auto mark = [&](std::uint32_t slot) {
        if (std::exchange(tracker.presented_marks[slot], serial) != serial)
            candidates.add(slot);
    };
    for (const auto entity : presented.entities())
        if (const auto* found = tracker.dependents.find(entity))
            for (const auto slot : *found)
                mark(slot);
    for (const auto slot : tracker.presented_list)
        mark(slot);
    tracker.presented_list.clear();
    for (const auto slot : candidates) {
        auto& cached = slots[slot];
        Optional<math::Matrix4> drawn;
        if (cached.item) {
            const Span chain{tracker.chains.data() + tracker.chain_starts[slot],
                             tracker.chains.data() + tracker.chain_starts[slot + 1]};
            if (const auto* offset = presented.find(chain))
                drawn = cached.item->world * offset->matrix;
        }
        if (drawn)
            tracker.presented_list.add(slot);
        if (drawn.has_value() != cached.presented.has_value() || (drawn && !same_bytes(*drawn, *cached.presented))) {
            cached.presented = drawn;
            cached.changed = version = serial;
        }
    }
}

} // namespace

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
    if (!member.active) {
        mesh_tracker_.index_materials(slot, {});
        mesh_tracker_.materials_dirty.set(slot, false);
        return drop();
    }
    ATELIER_TRY_BIND(matrix, pass.matrix(owner));
    if (!*matrix ||
        pass.degenerate(owner, **matrix, "A zero scale axis makes this space degenerate; meshes in it are not drawn."))
        return drop();
    const bool moved = !cached || !same_bytes(cached->world, **matrix);
    if (cached && !mesh_tracker_.materials_dirty[slot]) {
        if (moved)
            cached->world = **matrix;
        return moved;
    }
    Optional<Error> unusable;
    Set<ObjectId> dependencies;
    std::shared_ptr<const Array<material::ProgramSurfaceRef>> uncached;
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
                                                           object, owner, {"materials"_s})))
            return std::unexpected(*unusable);
        return drop();
    }
    // Content identity can stay equal after a reparent or dormant texture edit.
    // Commit the actual live edges independently of the visible change/version.
    mesh_tracker_.index_materials(slot, std::move(dependencies));
    mesh_tracker_.materials_dirty.reset(slot);
    if (*read)
        cached->world = **matrix;
    return *read;
}

Result<bool> Scene::update_generator(world::World::SpatialPass& pass, std::size_t slot,
                                     const world::CapabilityMember& member) const {
    auto& runtime = world_->runtime();
    auto& cached = generator_slots_[slot].item;
    const auto owner = member.entity;
    const auto object = member.component;
    const auto drop = [&] {
        const bool had = cached.has_value();
        cached.reset();
        return had;
    };
    if (!member.active) {
        generator_tracker_.index_materials(slot, {});
        generator_tracker_.materials_dirty.reset(slot);
        return drop();
    }
    ATELIER_TRY_BIND(matrix, pass.matrix(owner));
    if (!*matrix ||
        pass.degenerate(owner, **matrix,
                        "A zero scale axis makes this space degenerate; procedural generators in it are not drawn."))
        return drop();
    const bool moved = !cached || !same_bytes(cached->world, **matrix);
    if (cached && !generator_tracker_.materials_dirty[slot]) {
        if (moved)
            cached->world = **matrix;
        return moved;
    }

    Optional<Error> invalid_component;
    Optional<Error> unusable;
    Set<ObjectId> dependencies;
    detail::GeneratorCapturedSurfaces captured_surfaces;
    GeneratorRenderItem candidate{owner, object, **matrix, 0, {}, {}, {}};

    auto read = runtime.with_native<const ProceduralGeneratorComponent>(
        object, [&](const ProceduralGeneratorComponent& value) -> Result<bool> {
            if (auto checked = Access::validate(value); !checked) {
                invalid_component = std::move(checked.error());
                return false;
            }
            const auto& output = Access::output(value);
            if (!output)
                return false;
            auto surfs = detail::generator_surfaces(runtime, object, value, &dependencies);
            if (!surfs) {
                unusable = std::move(surfs.error());
                return false;
            }
            captured_surfaces = std::move(*surfs);
            candidate.output_generation = output->generation;
            candidate.surface_geometry = output->surface_mesh;
            candidate.surface_materials = captured_surfaces.surface;
            candidate.collections.reserve(output->publication.collections.size());
            for (const auto& col : output->publication.collections) {
                if (!col.transforms || col.transforms->empty())
                    continue;
                const auto key_sv = col.reference.key.view();
                geometry::MeshRef mesh = col.reference.mesh;
                std::shared_ptr<const Array<material::ProgramSurfaceRef>> col_surfs;
                if (!key_sv.empty()) {
                    for (std::size_t a = 0; a < output->assets.size(); ++a) {
                        if (output->assets[a].key == key_sv) {
                            if (output->assets[a].mesh)
                                mesh = output->assets[a].mesh;
                            if (a < captured_surfaces.assets.size())
                                col_surfs = captured_surfaces.assets[a];
                            break;
                        }
                    }
                }
                if (!mesh)
                    continue;
                const std::size_t mat_count = mesh->materials().size();
                if (!col_surfs || col_surfs->size() != mat_count) {
                    auto def = material::default_world_surface();
                    if (!def) {
                        unusable = std::move(def.error());
                        return false;
                    }
                    auto& cached_fallback = *fallback_surfaces_.try_emplace(mat_count).value;
                    if (!cached_fallback || (mat_count > 0 && (*cached_fallback)[0] != *def)) {
                        cached_fallback =
                            std::make_shared<const Array<material::ProgramSurfaceRef>>(mat_count, *def);
                    }
                    col_surfs = cached_fallback;
                }
                candidate.collections.add({std::move(mesh), col.chunk, col.transforms, std::move(col_surfs)});
            }
            if (!candidate.surface_geometry && candidate.collections.empty())
                return false;
            return true;
        });

    if (read && *read && captured_surfaces.uncached)
        if (auto stored = detail::store_generator_surfaces(runtime, object, captured_surfaces); !stored)
            read = std::unexpected(stored.error());
    if (!read) {
        if (!world_->contain(read.error(), detail::diagnostic(DiagnosticKind::invalid_value,
                                                              DiagnosticPhase::presentation, object, owner)))
            return std::unexpected(read.error());
        return drop();
    }
    if (invalid_component) {
        if (!world_->contain(*invalid_component, detail::diagnostic(DiagnosticKind::invalid_value,
                                                                    DiagnosticPhase::presentation, object, owner)))
            return std::unexpected(*invalid_component);
        return drop();
    }
    if (unusable) {
        if (!world_->contain(*unusable, detail::diagnostic(DiagnosticKind::invalid_value, DiagnosticPhase::presentation,
                                                           object, owner, {"materials"_s})))
            return std::unexpected(*unusable);
        return drop();
    }
    if (!*read) {
        generator_tracker_.index_materials(slot, std::move(dependencies));
        generator_tracker_.materials_dirty.reset(slot);
        return drop();
    }

    generator_tracker_.index_materials(slot, std::move(dependencies));
    generator_tracker_.materials_dirty.reset(slot);

    const auto same_collections = [](Span<const GeneratorRenderCollection> a,
                                     Span<const GeneratorRenderCollection> b) {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].geometry != b[i].geometry || a[i].chunk != b[i].chunk || a[i].transforms != b[i].transforms ||
                a[i].surfaces != b[i].surfaces)
                return false;
        return true;
    };
    const bool same_item = cached && cached->output_generation == candidate.output_generation &&
                           cached->surface_geometry == candidate.surface_geometry &&
                           cached->surface_materials == candidate.surface_materials &&
                           same_collections(cached->collections, candidate.collections);
    if (same_item) {
        if (moved)
            cached->world = **matrix;
        return moved;
    }
    cached = std::move(candidate);
    return true;
}

Result<void> Scene::index_layout(world::World::SpatialPass& pass, Span<const world::CapabilityMember> meshes,
                                 Span<const world::CapabilityMember> generators) const {
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
    index_bucket_chains(mesh_tracker_, meshes, chain);
    index_bucket_chains(generator_tracker_, generators, chain);
    ATELIER_TRY_BIND(lights, world_->members(lights_));
    for (const auto& light : *lights) {
        light_sources_.add(light.component);
        chain(light.entity, [&](ObjectId node) { light_sources_.add(node); });
    }
    return {};
}

Result<void> Scene::update_meshes(std::uint64_t serial, const detail::Presented* presented) const {
    auto& runtime = world_->runtime();
    ATELIER_TRY_BIND(pass, world_->spatial_pass(DiagnosticPhase::presentation));
    ATELIER_TRY_BIND(meshes, world_->members(meshes_));
    ATELIER_TRY_BIND(generators, world_->members(generators_));
    const auto count = meshes->size();
    const auto gen_count = generators->size();
    // A failed update leaves unread writes behind; the next one reads every slot.
    const bool tracked = std::exchange(tracked_, false);
    mesh_tracker_.dirty.clear();
    generator_tracker_.dirty.clear();
    // The bucket keeps its components and order until World::revision advances.
    if (!tracked || layout_revision_ != world_->revision()) {
        layout_revision_ = world_->revision();
        mesh_slots_.assign(count, {});
        mesh_tracker_.reset(count);
        generator_slots_.assign(gen_count, {});
        generator_tracker_.reset(gen_count);

        mesh_layout_ = version_ = serial;
        lights_dirty_ = true;
        ATELIER_TRY(index_layout(*pass, *meshes, *generators));
        for (std::size_t slot = 0; slot < count; ++slot)
            mesh_tracker_.dirty.add(static_cast<std::uint32_t>(slot));
        for (std::size_t slot = 0; slot < gen_count; ++slot)
            generator_tracker_.dirty.add(static_cast<std::uint32_t>(slot));
    } else {
        runtime.changed_objects(cursor_, written_);
        for (const auto object : written_) {
            mark_written_object(mesh_tracker_, object, serial);
            if (gen_count > 0)
                mark_written_object(generator_tracker_, object, serial);
            if (!lights_dirty_ && light_sources_.contains(object))
                lights_dirty_ = true;
        }
        // changed_objects lists live writes only. Check each distinct dependency
        // once for death/generation loss; successful fallback replaces its edges.
        finalize_dirty_slots(runtime, cursor_, mesh_tracker_, serial);
        if (gen_count > 0)
            finalize_dirty_slots(runtime, cursor_, generator_tracker_, serial);
    }
    for (const auto slot : mesh_tracker_.dirty) {
        ATELIER_TRY_BIND(changed, update_mesh(*pass, slot, (*meshes)[slot]));
        if (*changed)
            mesh_slots_[slot].changed = version_ = serial;
        const bool always = !mesh_slots_[slot].item && (*meshes)[slot].active;
        if (always && !mesh_tracker_.volatile_flags[slot])
            mesh_tracker_.volatile_slots.add(slot);
        mesh_tracker_.volatile_flags.set(slot, always);
    }
    for (const auto slot : generator_tracker_.dirty) {
        ATELIER_TRY_BIND(changed, update_generator(*pass, slot, (*generators)[slot]));
        if (*changed)
            generator_slots_[slot].changed = version_ = serial;
        const bool always = !generator_slots_[slot].item && (*generators)[slot].active;
        if (always && !generator_tracker_.volatile_flags[slot])
            generator_tracker_.volatile_slots.add(slot);
        generator_tracker_.volatile_flags.set(slot, always);
    }
    // Only slots below a listed Entity, and those presented before, can change their presented matrix.
    if (presented) {
        update_bucket_presented(mesh_tracker_, mesh_slots_, *presented, serial, version_);
        if (gen_count > 0)
            update_bucket_presented(generator_tracker_, generator_slots_, *presented, serial, version_);
    }
    // Nothing but this update writes while it runs: derived surfaces/resources
    // already consumed by the affected slots. Other Scenes have their own cursors.
    // Taking ours after these writes avoids recapturing our own publications.
    ATELIER_TRY_BIND(cursor, runtime.change_cursor());
    cursor_ = *cursor;
    tracked_ = true;
    return {};
}

Result<std::uint64_t> Scene::update(Span<const world::PresentedPose> presented) const {
    const auto serial = ++serial_;
    // A still frame shares one empty table instead of building its own.
    static const detail::Presented still{{}};
    Optional<detail::Presented> moving;
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

Result<Array<RenderItem>> Scene::render_items() const {
    ATELIER_TRY(update_meshes(++serial_, nullptr));
    std::size_t expected_items = static_cast<std::size_t>(
        std::ranges::count_if(mesh_slots_, [](const MeshSlot& slot) { return slot.item.has_value(); }));
    for (const auto& slot : generator_slots_) {
        if (!slot.item)
            continue;
        if (slot.item->surface_geometry)
            ++expected_items;
        for (const auto& col : slot.item->collections)
            if (col.transforms)
                expected_items += col.transforms->size();
    }
    Array<RenderItem> result;
    result.reserve(expected_items);
    for (const auto& slot : mesh_slots_)
        if (slot.item)
            result.add(*slot.item);
    for (const auto& slot : generator_slots_) {
        if (!slot.item)
            continue;
        const auto& gen = *slot.item;
        if (gen.surface_geometry)
            result.add({gen.entity, gen.component, gen.world, gen.surface_geometry, gen.surface_materials});
        for (const auto& col : gen.collections) {
            if (!col.transforms)
                continue;
            for (const auto& affine : *col.transforms) {
                if (const auto inst_world = compose_instance_world(affine, gen.world))
                    result.add({gen.entity, gen.component, *inst_world, col.geometry, col.surfaces});
            }
        }
    }
    return result;
}

}
