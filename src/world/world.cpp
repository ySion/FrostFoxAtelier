#include "world/detail.hpp"
#include "foundation/set.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace atelier::world {
using detail::Access;
using detail::error;

Result<void> World::Impl::admit(bool mutation) const {
    if (closed || (closing && mutation)) return error(ErrorCode::unsupported_operation, closing ? "World is closing." : "World is closed.");
    if (transitioning || (mutation && dispatch_depth)) return error(ErrorCode::busy, "World operation cannot reenter this phase.");
    if (mutation) return runtime.check_code_publication();
    return {};
}
Result<std::size_t> World::Impl::member(ObjectId object) const {
    auto entity = detail::entity_data(runtime, object);
    if (!entity) return std::unexpected(entity.error());
    if (entity->world != &world)
        return error(ErrorCode::invalid_member, entity->world ? "Entity belongs to another World." : "Entity is not a World member.");
    auto found = positions.find(object);
    if (found == positions.end()) return error(ErrorCode::invalid_member, "World member index is inconsistent.");
    return found->second.index;
}
Result<ObjectId> World::Impl::component_owner(ObjectId object) const {
    auto component = detail::component_data(runtime, object);
    if (!component) return std::unexpected(component.error());
    if (component->owner == ObjectId{}) return error(ErrorCode::invalid_member, "Component is not attached.");
    auto found = member(component->owner);
    if (!found) return std::unexpected(found.error());
    return component->owner;
}
Result<detail::EntityData> World::Impl::composition_entity(ObjectId object) const {
    auto entity = detail::entity_data(runtime, object);
    if (!entity) return std::unexpected(entity.error());
    if (entity->transitioning) return error(ErrorCode::busy, "Entity is transitioning.");
    if (entity->world && entity->world != &world) return error(ErrorCode::invalid_member, "Entity belongs to another World.");
    return entity;
}
Result<detail::ComponentData> World::Impl::composition_component(ObjectId object) const {
    auto component = detail::component_data(runtime, object);
    if (!component) return std::unexpected(component.error());
    if (component->owner != ObjectId{}) {
        auto entity = composition_entity(component->owner);
        if (!entity) return std::unexpected(entity.error());
    }
    return component;
}
Result<void> World::Impl::admit_edit(ObjectId object) const {
    if (auto ready = admit(); !ready) return ready;
    auto entity = detail::is(runtime, object, types.entity);
    if (!entity) return std::unexpected(entity.error());
    World* owner{};
    if (*entity) {
        auto data = composition_entity(object);
        if (!data) return std::unexpected(data.error());
        owner = data->world;
    } else {
        auto data = composition_component(object);
        if (!data) return std::unexpected(data.error());
        if (data->owner != ObjectId{}) {
            auto parent = composition_entity(data->owner);
            if (!parent) return std::unexpected(parent.error());
            owner = parent->world;
        }
    }
    if (running && owner) return error(ErrorCode::busy, "Stop the World before editing its members.");
    return {};
}
Result<std::size_t> World::Impl::prepare_bucket(TypeId type, std::size_t additional) {
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        if (buckets[i].type == type) {
            auto& objects = buckets[i].objects;
            if (objects.size() + additional > objects.capacity())
                objects.reserve(std::max(objects.size() + additional, std::max(std::size_t{8}, objects.capacity() * 2)));
            return i;
        }
    }
    Bucket bucket{type, {}};
    bucket.objects.reserve(additional);
    buckets.push_back(std::move(bucket));
    return buckets.size() - 1;
}
void World::Impl::prepare_activity(std::size_t additional) {
    std::size_t count = additional + members.size();
    for (const auto& bucket : buckets) count += bucket.objects.size();
    presentation.reserve(count); updates.reserve(count); ordered.reserve(count);
}
Result<void> World::Impl::refresh() {
    // A single member directory, ordered by the owned forest. Children never
    // create a second registration path or a second Tick traversal.
    std::unordered_set<ObjectId> admitted;
    for (const auto& member : members) admitted.insert(member.id());
    std::vector<ObjectId> roots, pending;
    for (const auto& member : members) {
        auto parent = parent_entity(runtime, member.id());
        if (!parent) return std::unexpected(parent.error());
        if (*parent == ObjectId{}) roots.push_back(member.id());
        else if (!admitted.contains(*parent)) return error(ErrorCode::invalid_member, "Member's composition parent is absent from its World.");
    }
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) pending.push_back(*it);
    std::vector<Member> ordered_members;
    ordered_members.reserve(members.size());
    std::unordered_set<ObjectId> seen;
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        if (!admitted.contains(id) || !seen.insert(id).second)
            return error(ErrorCode::invalid_member, "World composition contains an absent or repeated Entity.");
        auto owned = children(runtime, id);
        if (!owned) return std::unexpected(owned.error());
        for (auto it = owned->rbegin(); it != owned->rend(); ++it) pending.push_back(it->id());
        auto held = runtime.hold(id);
        if (!held) return std::unexpected(held.error());
        ordered_members.push_back({std::move(*held)});
    }
    if (seen.size() != members.size()) return error(ErrorCode::invalid_member, "World composition contains a cycle.");
    members = std::move(ordered_members);
    updates.clear(); presentation.clear(); ordered.clear(); positions.clear();
    for (auto& bucket : buckets) bucket.objects.clear();
    std::unordered_map<ObjectId, bool> eligible;
    const auto index_type = [&](ObjectId object, TypeId type) -> Result<void> {
        auto bucket = prepare_bucket(type, 1);
        if (!bucket) return std::unexpected(bucket.error());
        buckets[*bucket].objects.push_back(object);
        return {};
    };
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto id = members[i].id();
        positions.emplace(id, Position{i, 0});
        auto entity = detail::entity_data(runtime, id);
        auto parent = parent_entity(runtime, id);
        auto type = runtime.type_of(id);
        if (!entity) return std::unexpected(entity.error());
        if (!parent) return std::unexpected(parent.error());
        if (!type) return std::unexpected(type.error());
        const bool active = entity->enabled && (*parent == ObjectId{} || eligible.at(*parent));
        eligible.emplace(id, active);
        if (auto indexed = index_type(id, *type); !indexed) return indexed;
        auto binding = behavior(*type);
        if (!binding) return std::unexpected(binding.error());
        Scheduled item{id, id, *binding};
        ordered.push_back(item);
        if (running && entity->begun && active && item.behavior && item.behavior->tick) updates.push_back(item);
        auto group = components(runtime, id);
        if (!group) return std::unexpected(group.error());
        for (const auto& root : *group) {
            const auto object = root.id();
            auto component_type = runtime.type_of(object);
            if (!component_type) return std::unexpected(component_type.error());
            if (auto indexed = index_type(object, *component_type); !indexed) return indexed;
            auto component_binding = behavior(*component_type);
            if (!component_binding) return std::unexpected(component_binding.error());
            Scheduled component_item{object, id, *component_binding};
            ordered.push_back(component_item);
            auto data = detail::component_data(runtime, object);
            if (!data) return std::unexpected(data.error());
            if (running && data->begun && active && data->enabled && component_item.behavior && component_item.behavior->tick)
                updates.push_back(component_item);
            auto mesh = runtime.is_a(*component_type, types.static_mesh_component);
            if (!mesh) return std::unexpected(mesh.error());
            if (*mesh) {
                auto present = runtime.with_native<StaticMeshComponent>(object, [](StaticMeshComponent& value) -> Result<bool> { return bool(Access::geometry(value)); });
                if (!present) return std::unexpected(present.error());
                if (*present) { ++positions.at(id).presentations; presentation.push_back(object); }
            } else {
                auto light = runtime.is_a(*component_type, types.light_component);
                if (!light) return std::unexpected(light.error());
                if (*light) { ++positions.at(id).presentations; presentation.push_back(object); }
            }
        }
    }
    return {};
}
void World::Impl::cancel_tasks(ObjectId target, bool entity) noexcept {
    std::erase_if(tasks, [&](Task& task) {
        if (target != ObjectId{} && (entity ? task.entity : task.object) != target) return false;
        task.coroutine.cancel();
        return true;
    });
}
void World::Impl::halt() noexcept { running = false; updates.clear(); translations.clear(); cancel_tasks(); }
std::optional<Error> World::Impl::notify(Lifecycle event, ObjectId object) {
    if (!observer && subscriptions.empty()) return {};
    auto scope = ExecutionScope::enter(runtime);
    if (!scope) return scope.error();
    detail::DispatchScope dispatch{dispatch_depth};
    std::optional<Error> failure;
    const auto invoke = [&](const LifecycleObserver& callback) {
        if (!callback) return;
        try {
            auto result = callback(world, event, object);
            if (!result && !failure) failure = result.error();
        } catch (const std::exception& exception) {
            if (!failure) failure = Error{ErrorCode::invocation_failed, exception.what()};
        } catch (...) {
            if (!failure) failure = Error{ErrorCode::invocation_failed, "World lifecycle observer threw an unknown exception."};
        }
    };
    invoke(observer);
    for (const auto& entry : subscriptions) {
        // A callback may unsubscribe itself; this reference keeps it alive until it returns.
        const auto callback = entry.observer;
        if (callback) invoke(*callback);
    }
    return failure;
}
Commit World::Impl::finish(bool changed, std::optional<Error> failure) {
    if (!failure && running) {
        auto applied=apply_translations();
        if (!applied) failure=applied.error();
    }
    if (failure) (void)shutdown();
    return {changed, std::move(failure)};
}
void World::Impl::erase_indexes(ObjectId object) noexcept {
    for (auto& bucket : buckets) std::erase(bucket.objects, object);
    std::erase(presentation, object);
    std::erase_if(updates, [object](const Scheduled& item) { return item.object == object; });
    std::erase_if(ordered, [object](const Scheduled& item) { return item.object == object; });
}

World::World(Runtime& runtime, const Types& types) : impl_(std::make_unique<Impl>(runtime, types, *this)) {
    const auto registered = runtime.native_type<Entity>();
    if (!registered || *registered != types.entity) throw std::invalid_argument("World types must be registered in its Runtime.");
}
World::~World() {
    if (!impl_->closed) {
        const auto result = close();
        // Destruction during a dispatch would leave dangling member_world links.
        if (!result) std::terminate();
    }
}
Runtime& World::runtime() const noexcept { return impl_->runtime; }
const rendering::SceneLightingSettings& World::lighting_settings() const noexcept { return impl_->lighting; }
Result<void> World::set_lighting_settings(const rendering::SceneLightingSettings& candidate) {
    if(auto ready=impl_->admit();!ready) return ready;
    if(impl_->running) return error(ErrorCode::unsupported_operation,"World lighting is editable only while stopped.");
    if(const auto* message=rendering::scene_lighting_error(candidate)) return error(ErrorCode::invalid_definition,message);
    impl_->lighting=candidate;return {};
}
const Types& World::types() const noexcept { return impl_->types; }
bool World::running() const noexcept { return impl_->running; }
bool World::closed() const noexcept { return impl_->closed; }
Activity World::activity() const noexcept { return {impl_->updates.size(), impl_->presentation.size(), impl_->tasks.size()}; }
Result<std::span<const Member>> World::members() const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    return std::span<const Member>{impl_->members};
}
Result<std::vector<ObjectId>> World::query(TypeId type, bool include_derived) const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    auto info = runtime().describe(type);
    if (!info) return std::unexpected(info.error());
    std::vector<ObjectId> result;
    for (const auto& bucket : impl_->buckets) {
        auto matches = include_derived ? runtime().is_a(bucket.type, type) : Result<bool>{bucket.type == type};
        if (!matches) return std::unexpected(matches.error());
        if (*matches) result.insert(result.end(), bucket.objects.begin(), bucket.objects.end());
    }
    return result;
}

Result<Commit> World::attach_components(ObjectId entity_id, std::span<const Attachment> incoming, std::size_t index) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto entity = state.composition_entity(entity_id);
    if (!entity) return std::unexpected(entity.error());
    auto existing = components(runtime(), entity_id);
    if (!existing) return std::unexpected(existing.error());
    if (index == append) index = existing->size();
    if (index > existing->size()) return error(ErrorCode::argument_mismatch, "Component insertion index is out of range.");
    if (incoming.empty()) return Commit{};
    std::vector<ObjectRoot> roots;
    std::vector<ObjectReference> references;
    Set<ObjectId> seen;
    roots.reserve(incoming.size()); references.reserve(incoming.size());
    for (const auto& item : *existing) seen.add(item.id());
    for (const auto& item : incoming) {
        if (!seen.add(item.component)) return error(ErrorCode::invalid_member, "Component is already present or duplicated.");
        auto data = detail::component_data(runtime(), item.component);
        if (!data) return std::unexpected(data.error());
        if (data->owner != ObjectId{}) return error(ErrorCode::invalid_member, "Component already has an owner.");
        auto held = runtime().hold(item.component, types().component);
        if (!held) return std::unexpected(held.error());
        references.push_back(held->reference()); roots.push_back(std::move(*held));
    }
    // Detached composition is the preparation phase: capabilities may be added
    // before their owner's Transform. Complete spatial requirements are checked
    // by attach_batch before any member, observer or behavior is published.
    if (entity->world)
        if (auto valid = detail::requires_space(runtime(), entity_id, roots); !valid) return std::unexpected(valid.error());
    if (entity->world) if (auto sky = state.validate_sky_light(roots); !sky) return std::unexpected(sky.error());
    auto held = runtime().hold(entity_id, types().entity);
    if (!held) return std::unexpected(held.error());
    state.prepare_activity(incoming.size());
    auto changed = runtime().with_native<Entity>(entity_id, [&](Entity& value) -> Result<void> {
    auto sequence = runtime().reference_field(entity_id, Access::sequence(value));
    if (!sequence) return std::unexpected(sequence.error());
    std::vector<ReferenceChange> changes;
    changes.reserve(incoming.size() + 1);
    changes.push_back(SequenceSplice{*sequence, index, 0, references});
    for (const auto& item : incoming) {
        auto owner = runtime().with_native<Component>(item.component, [&](Component& value) { return runtime().reference_field(item.component, Access::owner(value)); });
        if (!owner) return std::unexpected(owner.error());
        changes.push_back(ReferenceWrite{*owner, held->reference()});
    }
    return runtime().apply_reference_changes(changes);
    });
    if (!changed) return std::unexpected(changed.error());
    if (!entity->world) return Commit{true, {}};
    if (auto refreshed = state.refresh(); !refreshed) return state.finish(true, refreshed.error());
    std::optional<Error> failure;
    for (const auto& item : incoming) {
        auto notification = state.notify(Lifecycle::attached, item.component);
        if (!failure) failure = std::move(notification);
    }
    if (!failure && state.running) failure = state.begin(entity_id);
    if (!failure) if (auto active = state.refresh(); !active) failure = active.error();
    return state.finish(true, std::move(failure));
}
Result<ComponentRemoval> World::remove_component(ObjectId object) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto data = state.composition_component(object);
    if (!data) return std::unexpected(data.error());
    const auto owner = data->owner;
    if (owner == ObjectId{}) return error(ErrorCode::invalid_member, "Component is not attached.");
    auto entity = state.composition_entity(owner);
    if (!entity) return std::unexpected(entity.error());
    const bool registered = entity->world != nullptr;
    auto group = components(runtime(), owner);
    if (!group) return std::unexpected(group.error());
    const auto at = std::ranges::find_if(*group, [&](const ObjectRoot& item) { return item.id() == object; });
    if (at == group->end()) return error(ErrorCode::invalid_member, "Component is absent from its owner's composition.");
    auto root = runtime().hold(object, types().component);
    if (!root) return std::unexpected(root.error());
    auto status = inspect_component(runtime(), object);
    if (!status) return std::unexpected(status.error());
    ComponentRemoval result;
    result.root = std::move(*root); result.owner = owner;
    result.index = std::size_t(at - group->begin()); result.origin_ = state.lifetime;
    if (registered) state.cancel_tasks(object, false);
    std::optional<Error> failure = registered ? state.end({}, object) : std::nullopt;
    if (status->active) { auto notification = state.notify(Lifecycle::deactivated, object); if (!failure) failure = std::move(notification); }
    {
        detail::FlagScope transition{state.transitioning};
        auto removed = detail::clear_component(runtime(), owner, object, result.index);
        if (!removed) { if (registered) state.halt(); return std::unexpected(removed.error()); }
        if (registered) state.erase_indexes(object);
    }
    if (!registered) { result.commit = Commit{true, {}}; return result; }
    auto refreshed = state.refresh();
    if (!refreshed && !failure) failure = refreshed.error();
    auto detached = state.notify(Lifecycle::detached, object);
    if (!failure) failure = std::move(detached);
    result.commit = state.finish(true, std::move(failure));
    return result;
}
Result<Commit> World::restore(const ComponentRemoval& removal) {
    auto origin = removal.origin_.lock();
    if (!origin || !origin->open || origin != impl_->lifetime)
        return error(ErrorCode::invalid_member, "Component removal does not belong to this open World's history.");
    const std::array incoming{Attachment{removal.id()}};
    return attach_components(removal.owner, incoming, removal.index);
}

namespace detail {
Result<void> queue_translation(Runtime& runtime,ObjectId object,math::Vector3 delta) {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z))
        return error(ErrorCode::argument_mismatch,"Translation must be finite.");
    auto component=component_data(runtime,object); if (!component) return std::unexpected(component.error());
    if (component->owner==ObjectId{}) return error(ErrorCode::invalid_member,"Translation requires an owned Component.");
    auto entity=entity_data(runtime,component->owner); if (!entity) return std::unexpected(entity.error());
    if (!entity->world) return error(ErrorCode::invalid_member,"Translation requires World membership.");
    auto& state=Access::state(*entity->world);
    if (!state.running || state.closing || state.transitioning || !state.dispatch_depth)
        return error(ErrorCode::busy,"translate_owner requires a running World behavior phase.");
    auto spatial=has_transform(runtime,component->owner); if (!spatial) return std::unexpected(spatial.error());
    if (!*spatial) return error(ErrorCode::invalid_member,"Translation requires an Entity Transform.");
    state.translations.push_back({object,component->owner,delta});
    return {};
}
}
Result<void> World::Impl::apply_translations() {
    if (translations.empty()) return {};
    // No callbacks execute while applying. Retain buffers between frames, and
    // discard the batch on every exit so a rejected action cannot replay later.
    struct Cleanup { Impl& state; ~Cleanup() { state.translations.clear(); state.translation_changes.clear(); } } cleanup{*this};
    auto& changes=translation_changes; changes.reserve(translations.size());
    const std::array physical_types{runtime.find_type("physics.CharacterComponent"),runtime.find_type("physics.RagdollComponent")};
    for (const auto& command:translations) {
        auto owner=component_owner(command.component);
        if (!owner) return std::unexpected(owner.error());
        if (*owner!=command.entity) return error(ErrorCode::invalid_member,"Translation receiver changed owner.");
        {
            auto group=components(runtime,command.entity); if (!group) return std::unexpected(group.error());
            for (const auto& component:*group) {
                auto type=runtime.type_of(component.id()); if (!type) return std::unexpected(type.error());
                auto primitive=runtime.is_a(*type,types.primitive_component);
                if (!primitive) return std::unexpected(primitive.error());
                if (*primitive) {
                    auto physical=runtime.with_native<const PrimitiveComponent>(component.id(),[](const auto& p)->Result<bool>{return p.body.enabled;});
                    if (!physical) return std::unexpected(physical.error());
                    if (*physical) return error(ErrorCode::unsupported_operation,"translate_owner cannot move a physics-controlled Entity; use physics actions.");
                }
                for(const auto& physical_type:physical_types) if(physical_type) {
                    auto physical=runtime.is_a(*type,*physical_type); if (!physical) return std::unexpected(physical.error());
                    if (*physical) return error(ErrorCode::unsupported_operation,"translate_owner cannot move a physics-controlled Entity; use physics actions.");
                }
            }
        }
        auto found=std::ranges::find(changes,command.entity,&TranslationChange::entity);
        if (found==changes.end()) {
            auto matrix=world_matrix(runtime,command.entity); if (!matrix) return std::unexpected(matrix.error());
            auto value=relative_transform(*matrix,math::Matrix4::identity()); if (!value) return std::unexpected(value.error());
            changes.push_back({command.entity,*value}); found=std::prev(changes.end());
        }
        auto& value=found->value;
        value.position.x+=command.delta.x; value.position.y+=command.delta.y; value.position.z+=command.delta.z;
        auto canonical=canonical_transform(value); if (!canonical) return std::unexpected(canonical.error());
        value=*canonical;
    }
    std::vector<SimulationPose> poses;
    poses.reserve(changes.size());
    for (const auto& change:changes) poses.push_back({change.entity,change.value.position,change.value.rotation});
    return world.apply_simulation_transforms(poses);
}

Result<Scalar> World::read(ObjectId component, std::span<const FieldId> path) const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    auto entity = detail::is(runtime(),component,types().entity);
    if (!entity) return std::unexpected(entity.error());
    if (*entity) { auto data=impl_->composition_entity(component); if (!data) return std::unexpected(data.error()); }
    else { auto data=impl_->composition_component(component); if (!data) return std::unexpected(data.error()); }
    return runtime().read(component, path);
}
Result<void> World::write(ObjectId object, std::span<const FieldId> path, Scalar value) {
    if (auto ready = impl_->admit_edit(object); !ready) return ready;
    auto lighting=detail::write_lighting_property(*this,object,path,value);
    if (!lighting) return std::unexpected(lighting.error());
    if (*lighting) return {};
    auto primitive=detail::is(runtime(),object,types().primitive_component);
    if (!primitive) return std::unexpected(primitive.error());
    if (*primitive && !path.empty()) {
        auto body_field=runtime().find_field(types().primitive_component,"body");
        if (!body_field) return std::unexpected(body_field.error());
        if (path.front()==*body_field) {
            auto previous=runtime().with_native<const PrimitiveComponent>(object,[](const auto& p)->Result<BodySettings>{return p.body;});
            if (!previous) return std::unexpected(previous.error());
            if (auto written=runtime().write(object,path,value);!written) return written;
            return runtime().with_native<PrimitiveComponent>(object,[&](auto& p)->Result<void>{
                if (p.body.shape!=previous->shape) p.body.cooked_shape.reset();
                auto valid=validate_body_settings(p.body);
                if (!valid) p.body=*previous;
                return valid;
            });
        }
    }
    auto transform = detail::is(runtime(), object, types().entity);
    if (!transform) return std::unexpected(transform.error());
    if (*transform) {
        const auto match = [&](const auto& paths) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < paths.size(); ++i)
                if (std::ranges::equal(path, paths[i])) return i;
            return {};
        };
        const auto position = match(types().position_fields), rotation = match(types().rotation_fields), scale = match(types().scale_fields);
        if (!position && !rotation && !scale) return runtime().write(object, path, value);
        auto current = read_transform(runtime(), object);
        if (!current) return std::unexpected(current.error());
        if (!std::holds_alternative<float>(value)) return error(ErrorCode::type_mismatch, "Transform fields require float values.");
        if (auto i = match(types().position_fields)) {
            std::array fields{&current->position.x, &current->position.y, &current->position.z}; *fields[*i] = std::get<float>(value);
        } else if (auto i = match(types().rotation_fields)) {
            std::array fields{&current->rotation.x, &current->rotation.y, &current->rotation.z, &current->rotation.w}; *fields[*i] = std::get<float>(value);
        } else if (auto i = match(types().scale_fields)) {
            std::array fields{&current->scale.x, &current->scale.y, &current->scale.z}; *fields[*i] = std::get<float>(value);
        }
        return set_transform(object, *current);
    }
    return runtime().write(object, path, value);
}
Result<std::vector<RenderItem>> World::render_items() const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    std::vector<RenderItem> result;
    detail::SpaceGraph space{runtime()};
    for (const auto object : impl_->presentation) {
        auto mesh=detail::is(runtime(),object,types().static_mesh_component);
        if (!mesh) return std::unexpected(mesh.error());
        if (!*mesh) continue;
        auto owner = impl_->component_owner(object);
        if (!owner) return std::unexpected(owner.error());
        auto matrix = space.matrix(*owner);
        if (!matrix) return std::unexpected(matrix.error());
        auto item = runtime().with_native<StaticMeshComponent>(object, [&](StaticMeshComponent& component) -> Result<RenderItem> {
            if (auto valid = validate_material(component.material); !valid) return std::unexpected(valid.error());
            return RenderItem{*owner, object, *matrix, Access::geometry(component), component.material};
        });
        if (!item) return std::unexpected(item.error());
        auto surfaces=material_surfaces(runtime(),object);if(!surfaces) return std::unexpected(surfaces.error());
        item->surfaces=std::move(*surfaces);
        result.push_back(std::move(*item));
    }
    return result;
}

}
