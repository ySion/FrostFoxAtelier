#include "world/detail.hpp"
#include "foundation/set.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atelier::world {
using detail::Access;
using detail::error;

Result<void> World::Impl::admit(bool mutation) const {
    if (closing || closed) return error(ErrorCode::unsupported_operation, closing ? "World is closing." : "World is closed.");
    if (transitioning || (mutation && dispatch_depth)) return error(ErrorCode::busy, "World operation cannot reenter this phase.");
    if (mutation) return runtime.check_code_publication();
    return {};
}
Result<std::size_t> World::Impl::member(ObjectId object) const {
    auto actor = detail::actor_data(runtime, object);
    if (!actor) return std::unexpected(actor.error());
    if (actor->world != &world)
        return error(ErrorCode::invalid_member, actor->world ? "Actor belongs to another World." : "Actor is not a World member.");
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
Result<detail::ActorData> World::Impl::composition_actor(ObjectId object) const {
    auto actor = detail::actor_data(runtime, object);
    if (!actor) return std::unexpected(actor.error());
    if (actor->transitioning) return error(ErrorCode::busy, "Actor is transitioning.");
    if (actor->world && actor->world != &world) return error(ErrorCode::invalid_member, "Actor belongs to another World.");
    return actor;
}
Result<detail::ComponentData> World::Impl::composition_component(ObjectId object) const {
    auto component = detail::component_data(runtime, object);
    if (!component) return std::unexpected(component.error());
    if (component->owner != ObjectId{}) {
        auto actor = composition_actor(component->owner);
        if (!actor) return std::unexpected(actor.error());
    }
    return component;
}
Result<std::size_t> World::Impl::prepare_bucket(TypeId type, std::size_t additional) {
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        if (buckets[i].type == type) { buckets[i].objects.reserve(buckets[i].objects.size() + additional); return i; }
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
    updates.clear(); presentation.clear(); ordered.clear();
    for (auto& [id, position] : positions) { (void)id; position.presentations = 0; }
    // Composition order is authoritative. Type buckets remain query indexes only.
    for (const auto& member : members) {
        auto actor = detail::actor_data(runtime, member.id());
        if (!actor) return std::unexpected(actor.error());
        auto actor_type = runtime.type_of(member.id());
        if (!actor_type) return std::unexpected(actor_type.error());
        auto actor_binding = behavior(*actor_type);
        if (!actor_binding) return std::unexpected(actor_binding.error());
        Scheduled actor_item{member.id(), member.id(), *actor_binding};
        ordered.push_back(actor_item);
        if (running && actor->begun && actor->enabled && actor_item.behavior && actor_item.behavior->tick)
            updates.push_back(actor_item);
        auto group = components(runtime, member.id());
        if (!group) return std::unexpected(group.error());
        for (const auto& root : *group) {
            const auto object = root.id();
            auto type = runtime.type_of(object);
            if (!type) return std::unexpected(type.error());
            auto binding = behavior(*type);
            if (!binding) return std::unexpected(binding.error());
            Scheduled item{object, member.id(), *binding};
            ordered.push_back(item);
            auto data = detail::component_data(runtime, object);
            if (!data) return std::unexpected(data.error());
            if (running && data->begun && actor->enabled && data->enabled && item.behavior && item.behavior->tick)
                updates.push_back(item);
            auto mesh = runtime.is_a(*type, types.static_mesh_component);
            if (!mesh) return std::unexpected(mesh.error());
            if (*mesh) {
                auto present = runtime.with_native<StaticMeshComponent>(object, [](StaticMeshComponent& value) -> Result<bool> {
                    return bool(Access::geometry(value));
                });
                if (!present) return std::unexpected(present.error());
                if (*present) { ++positions.at(member.id()).presentations; presentation.push_back(object); }
            } else {
                auto light=runtime.is_a(*type,types.light_component);
                if (!light) return std::unexpected(light.error());
                if (*light) { ++positions.at(member.id()).presentations; presentation.push_back(object); }
            }
        }
    }
    return {};
}
void World::Impl::cancel_tasks(ObjectId target, bool actor) noexcept {
    std::erase_if(tasks, [&](Task& task) {
        if (target != ObjectId{} && (actor ? task.actor : task.object) != target) return false;
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
std::optional<Error> World::Impl::notify_group(Lifecycle event, ObjectId actor, std::span<const ObjectRoot> group) {
    std::optional<Error> failure;
    const auto emit = [&](ObjectId object) {
        auto result = notify(event, object);
        if (!failure) failure = std::move(result);
    };
    if (event == Lifecycle::attached) {
        emit(actor);
        for (const auto& component : group) emit(component.id());
    } else {
        for (auto it = group.rbegin(); it != group.rend(); ++it) emit(it->id());
        emit(actor);
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
    const auto registered = runtime.native_type<Actor>();
    if (!registered || *registered != types.actor) throw std::invalid_argument("World types must be registered in its Runtime.");
}
World::~World() {
    if (!impl_->closed) {
        const auto result = close();
        // Destruction during a dispatch would leave dangling member_world links.
        if (!result) std::terminate();
    }
}
Runtime& World::runtime() const noexcept { return impl_->runtime; }
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

Result<Commit> World::attach(ObjectId object, std::size_t index) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto data = detail::actor_data(runtime(), object);
    if (!data) return std::unexpected(data.error());
    if (data->transitioning) return error(ErrorCode::busy, "Actor is transitioning.");
    if (data->world) return error(ErrorCode::invalid_member, data->world == this ? "Actor is already a member." : "Actor belongs to another World.");
    if (index == append) index = state.members.size();
    if (index > state.members.size()) return error(ErrorCode::argument_mismatch, "Actor insertion index is out of range.");
    auto root = runtime().hold(object);
    if (!root) return std::unexpected(root.error());
    auto group = components(runtime(), object);
    if (!group) return std::unexpected(group.error());
    Set<ObjectId> seen;
    std::vector<std::size_t> buckets;
    buckets.reserve(group->size());
    for (const auto& component : *group) {
        if (!seen.add(component.id())) return error(ErrorCode::invalid_member, "Actor contains duplicate component identities.");
        auto owned = detail::component_data(runtime(), component.id());
        if (!owned) return std::unexpected(owned.error());
        if (owned->owner != object) return error(ErrorCode::invalid_member, "Component has a different owner.");
        auto type = runtime().type_of(component.id());
        if (!type) return std::unexpected(type.error());
        auto bucket = state.prepare_bucket(*type, group->size());
        if (!bucket) return std::unexpected(bucket.error());
        buckets.push_back(*bucket);
    }
    if (auto tree = detail::validate_tree(runtime(), object, *group); !tree) return std::unexpected(tree.error());
    if (auto sky = state.validate_sky_light(*group); !sky) return std::unexpected(sky.error());
    state.members.reserve(state.members.size() + 1);
    state.positions.reserve(state.positions.size() + 1);
    state.prepare_activity(group->size() + 1);
    // Prepare the hash node before publishing membership; it is not observable
    // while the structural guard is held, and is removed if the borrow rejects.
    {
        detail::FlagScope transition{state.transitioning};
        state.positions.emplace(object, Impl::Position{index, 0});
        auto published = runtime().with_native<Actor>(object, [&](Actor& actor) -> Result<void> {
            Access::member_world(actor) = this;
            return {};
        });
        if (!published) { state.positions.erase(object); return std::unexpected(published.error()); }
        state.members.insert(state.members.begin() + std::ptrdiff_t(index), Member{std::move(*root)});
        for (std::size_t i = index + 1; i < state.members.size(); ++i) state.positions.find(state.members[i].id())->second.index = i;
        for (std::size_t i = 0; i < group->size(); ++i) state.buckets[buckets[i]].objects.push_back((*group)[i].id());
    }
    auto refreshed = state.refresh();
    if (!refreshed) return state.finish(true, refreshed.error());
    auto failure = state.notify_group(Lifecycle::attached, object, *group);
    if (!failure && state.running) failure = state.begin(object);
    if (!failure) { auto active = state.refresh(); if (!active) failure = active.error(); }
    if (!failure && state.running && data->enabled) failure = state.notify(Lifecycle::activated, object);
    return state.finish(true, std::move(failure));
}

Result<ActorRemoval> World::detach(ObjectId object) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto at = state.member(object);
    if (!at) return std::unexpected(at.error());
    auto root = runtime().hold(object);
    if (!root) return std::unexpected(root.error());
    auto group = components(runtime(), object);
    if (!group) return std::unexpected(group.error());
    auto data = detail::actor_data(runtime(), object);
    if (!data) return std::unexpected(data.error());
    ActorRemoval result;
    result.root = std::move(*root); result.index = *at; result.origin_ = state.lifetime;
    state.cancel_tasks(object);
    std::optional<Error> failure = state.end(object);
    {
        detail::FlagScope transition{state.transitioning};
        for (const auto& component : *group) state.erase_indexes(component.id());
        state.erase_indexes(object);
        if (state.running && data->enabled) { auto notification = state.notify(Lifecycle::deactivated, object); if (!failure) failure = std::move(notification); }
        auto published = runtime().with_native<Actor>(object, [&](Actor& actor) -> Result<void> {
            Access::member_world(actor) = nullptr;
            return {};
        });
        if (!published) { state.halt(); return std::unexpected(published.error()); }
        state.members.erase(state.members.begin() + std::ptrdiff_t(*at));
        state.positions.erase(object);
        for (std::size_t i = *at; i < state.members.size(); ++i) state.positions.find(state.members[i].id())->second.index = i;
    }
    auto detached = state.notify_group(Lifecycle::detached, object, *group);
    if (!failure) failure = std::move(detached);
    result.commit = state.finish(true, std::move(failure));
    return result;
}
Result<Commit> World::restore(const ActorRemoval& removal) {
    auto origin = removal.origin_.lock();
    if (!origin || !origin->open || origin != impl_->lifetime)
        return error(ErrorCode::invalid_member, "Removal does not belong to this open World's history.");
    return attach(removal.id(), removal.index);
}

Result<Commit> World::attach_components(ObjectId actor_id, std::span<const Attachment> incoming, std::size_t index) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto actor = state.composition_actor(actor_id);
    if (!actor) return std::unexpected(actor.error());
    const bool registered = actor->world != nullptr;
    auto existing = components(runtime(), actor_id);
    if (!existing) return std::unexpected(existing.error());
    if (index == append) index = existing->size();
    if (index > existing->size()) return error(ErrorCode::argument_mismatch, "Component insertion index is out of range.");
    if (incoming.empty()) return Commit{};
    auto root = root_component(runtime(), actor_id);
    if (!root) return std::unexpected(root.error());
    ObjectId proposed_root = root->value_or(ObjectId{});
    std::vector<ObjectRoot> roots;
    std::vector<ObjectReference> references;
    std::vector<std::size_t> buckets;
    Set<ObjectId> seen;
    std::unordered_map<ObjectId, ObjectId> parents;
    roots.reserve(incoming.size()); references.reserve(incoming.size()); buckets.reserve(incoming.size());
    for (const auto& item : *existing) {
        seen.add(item.id());
        auto scene = detail::is(runtime(), item.id(), types().scene_component);
        if (!scene) return std::unexpected(scene.error());
        if (*scene) {
            auto parent = scene_parent(runtime(), item.id());
            if (!parent) return std::unexpected(parent.error());
            parents.emplace(item.id(), *parent);
        }
    }
    for (const auto& item : incoming) {
        if (!seen.add(item.component)) return error(ErrorCode::invalid_member, "Component is already present or duplicated.");
        auto data = detail::component_data(runtime(), item.component);
        if (!data) return std::unexpected(data.error());
        if (data->owner != ObjectId{}) return error(ErrorCode::invalid_member, "Component already has an owner.");
        auto type = runtime().type_of(item.component);
        if (!type) return std::unexpected(type.error());
        if (registered) {
            auto bucket = state.prepare_bucket(*type, incoming.size());
            if (!bucket) return std::unexpected(bucket.error());
            buckets.push_back(*bucket);
        }
        auto scene = runtime().is_a(*type, types().scene_component);
        if (!scene) return std::unexpected(scene.error());
        if (*scene) {
            auto value = read_transform(runtime(), item.component);
            if (!value) return std::unexpected(value.error());
            auto canonical = canonical_transform(*value);
            if (!canonical) return std::unexpected(canonical.error());
            if (item.root) {
                if (proposed_root != ObjectId{}) return error(ErrorCode::invalid_member, "Actor already has a root.");
                proposed_root = item.component;
            }
            parents.emplace(item.component, item.parent);
        } else if (item.root || item.parent != ObjectId{})
            return error(ErrorCode::type_mismatch, "Only SceneComponent can participate in a spatial tree.");
        auto held = runtime().hold(item.component, types().component);
        if (!held) return std::unexpected(held.error());
        references.push_back(held->reference()); roots.push_back(std::move(*held));
    }
    if (auto valid = detail::validate_parents(proposed_root, parents); !valid) return std::unexpected(valid.error());
    if (registered) if (auto sky=state.validate_sky_light(roots); !sky) return std::unexpected(sky.error());
    auto actor_root = runtime().hold(actor_id, types().actor);
    if (!actor_root) return std::unexpected(actor_root.error());
    const auto weak = [&](ObjectId id) -> Result<ObjectReference> {
        if (id == ObjectId{}) return runtime().null_reference(types().scene_component, ValueForm::weak_reference);
        auto held = runtime().hold(id, types().scene_component);
        if (!held) return std::unexpected(held.error());
        return runtime().observe(held->reference());
    };
    if (registered) state.prepare_activity(incoming.size());
    {
        detail::FlagScope transition{state.transitioning};
        auto changed = runtime().with_native<Actor>(actor_id, [&](Actor& actor) -> Result<void> {
            auto sequence = runtime().reference_field(actor_id, Access::sequence(actor));
            auto root_slot = runtime().reference_field(actor_id, Access::root(actor));
            if (!sequence) return std::unexpected(sequence.error());
            if (!root_slot) return std::unexpected(root_slot.error());
            auto root_reference = weak(proposed_root);
            if (!root_reference) return std::unexpected(root_reference.error());
            std::vector<ReferenceChange> changes;
            changes.reserve(2 + incoming.size() * 2);
            changes.push_back(SequenceSplice{*sequence, index, 0, references});
            changes.push_back(ReferenceWrite{*root_slot, *root_reference});
            for (std::size_t i = 0; i < incoming.size(); ++i) {
                auto owner = runtime().with_native<Component>(incoming[i].component, [&](Component& component) {
                    return runtime().reference_field(incoming[i].component, Access::owner(component));
                });
                if (!owner) return std::unexpected(owner.error());
                changes.push_back(ReferenceWrite{*owner, actor_root->reference()});
                if (parents.contains(incoming[i].component)) {
                    auto parent = runtime().with_native<SceneComponent>(incoming[i].component, [&](SceneComponent& component) {
                        return runtime().reference_field(incoming[i].component, Access::parent(component));
                    });
                    if (!parent) return std::unexpected(parent.error());
                    auto reference = weak(incoming[i].parent);
                    if (!reference) return std::unexpected(reference.error());
                    changes.push_back(ReferenceWrite{*parent, *reference});
                }
            }
            return runtime().apply_reference_changes(changes);
        });
        if (!changed) return std::unexpected(changed.error());
        if (registered) for (std::size_t i = 0; i < incoming.size(); ++i)
            state.buckets[buckets[i]].objects.push_back(incoming[i].component);
    }
    if (!registered) return Commit{true, {}};
    auto refreshed = state.refresh();
    if (!refreshed) return state.finish(true, refreshed.error());
    std::optional<Error> failure;
    for (const auto& item : incoming) {
        auto notification = state.notify(Lifecycle::attached, item.component);
        if (!failure) failure = std::move(notification);
    }
    if (!failure && state.running) failure = state.begin(actor_id);
    if (!failure) { auto active = state.refresh(); if (!active) failure = active.error(); }
    return state.finish(true, std::move(failure));
}

Result<ComponentRemoval> World::remove_component(ObjectId object) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto data = state.composition_component(object);
    if (!data) return std::unexpected(data.error());
    const auto owner = data->owner;
    if (owner == ObjectId{}) return error(ErrorCode::invalid_member, "Component is not attached.");
    auto actor = state.composition_actor(owner);
    if (!actor) return std::unexpected(actor.error());
    const bool registered = actor->world != nullptr;
    auto group = components(runtime(), owner);
    if (!group) return std::unexpected(group.error());
    const auto at = std::ranges::find_if(*group, [&](const ObjectRoot& item) { return item.id() == object; });
    if (at == group->end()) return error(ErrorCode::invalid_member, "Component is absent from its owner's composition.");
    auto removable = detail::can_remove(runtime(), object, *group);
    if (!removable) return std::unexpected(removable.error());
    auto root = runtime().hold(object, types().component);
    if (!root) return std::unexpected(root.error());
    auto status = inspect_component(runtime(), object);
    if (!status) return std::unexpected(status.error());
    ComponentRemoval result;
    result.root = std::move(*root); result.owner = owner;
    result.index = std::size_t(at - group->begin()); result.origin_ = state.lifetime;
    auto actor_root = root_component(runtime(), owner);
    if (!actor_root) return std::unexpected(actor_root.error());
    result.was_root = *actor_root == object;
    auto scene = detail::is(runtime(), object, types().scene_component);
    if (!scene) return std::unexpected(scene.error());
    if (*scene) {
        auto parent = scene_parent(runtime(), object);
        if (!parent) return std::unexpected(parent.error());
        result.parent = *parent;
    }
    if (registered) state.cancel_tasks(object, false);
    std::optional<Error> failure = registered ? state.end({}, object) : std::nullopt;
    {
        detail::FlagScope transition{state.transitioning};
        if (status->active) { auto notification = state.notify(Lifecycle::deactivated, object); if (!failure) failure = std::move(notification); }
        auto removed = detail::clear_component(runtime(), owner, object, result.index, result.was_root);
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
    const std::array incoming{Attachment{removal.id(), removal.parent, removal.was_root}};
    return attach_components(removal.owner, incoming, removal.index);
}

namespace detail {
Result<void> queue_translation(Runtime& runtime,ObjectId object,math::Vector3 delta) {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z))
        return error(ErrorCode::argument_mismatch,"Translation must be finite.");
    auto component=component_data(runtime,object); if (!component) return std::unexpected(component.error());
    if (component->owner==ObjectId{}) return error(ErrorCode::invalid_member,"Translation requires an owned Component.");
    auto actor=actor_data(runtime,component->owner); if (!actor) return std::unexpected(actor.error());
    if (!actor->world) return error(ErrorCode::invalid_member,"Translation requires World membership.");
    auto& state=Access::state(*actor->world);
    if (!state.running || state.closing || state.transitioning || !state.dispatch_depth)
        return error(ErrorCode::busy,"translate_owner requires a running World behavior phase.");
    auto root=root_component(runtime,component->owner); if (!root) return std::unexpected(root.error());
    if (!*root) return error(ErrorCode::invalid_member,"Translation requires an explicit Actor root.");
    state.translations.push_back({object,component->owner,**root,delta});
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
        if (*owner!=command.actor) return error(ErrorCode::invalid_member,"Translation receiver changed owner.");
        auto root=root_component(runtime,command.actor); if (!root) return std::unexpected(root.error());
        if (*root!=command.root) return error(ErrorCode::invalid_member,"Translation target root changed.");
        {
            auto group=components(runtime,command.actor); if (!group) return std::unexpected(group.error());
            for (const auto& component:*group) {
                auto type=runtime.type_of(component.id()); if (!type) return std::unexpected(type.error());
                auto primitive=runtime.is_a(*type,types.primitive_component);
                if (!primitive) return std::unexpected(primitive.error());
                if (*primitive) {
                    auto physical=runtime.with_native<const PrimitiveComponent>(component.id(),[](const auto& p)->Result<bool>{return p.body.enabled;});
                    if (!physical) return std::unexpected(physical.error());
                    if (*physical) return error(ErrorCode::unsupported_operation,"translate_owner cannot move a physics-controlled Actor; use physics actions.");
                }
                for(const auto& physical_type:physical_types) if(physical_type) {
                    auto physical=runtime.is_a(*type,*physical_type); if (!physical) return std::unexpected(physical.error());
                    if (*physical) return error(ErrorCode::unsupported_operation,"translate_owner cannot move a physics-controlled Actor; use physics actions.");
                }
            }
        }
        auto found=std::ranges::find(changes,command.root,&TranslationChange::root);
        if (found==changes.end()) {
            auto value=read_transform(runtime,command.root); if (!value) return std::unexpected(value.error());
            changes.push_back({command.root,*value}); found=std::prev(changes.end());
        }
        auto& value=found->value;
        value.position.x+=command.delta.x; value.position.y+=command.delta.y; value.position.z+=command.delta.z;
        auto canonical=canonical_transform(value); if (!canonical) return std::unexpected(canonical.error());
        value=*canonical;
    }
    for (const auto& change:changes) {
        auto applied=world.apply_simulation_transform(change.root,change.value.position,change.value.rotation);
        if (!applied) return applied;
    }
    return {};
}

Result<Scalar> World::read(ObjectId component, std::span<const FieldId> path) const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    auto actor = detail::is(runtime(),component,types().actor);
    if (!actor) return std::unexpected(actor.error());
    if (*actor) { auto data=impl_->composition_actor(component); if (!data) return std::unexpected(data.error()); }
    else { auto data=impl_->composition_component(component); if (!data) return std::unexpected(data.error()); }
    return runtime().read(component, path);
}
Result<void> World::set_transform(ObjectId object, Trs value) {
    if (auto ready = impl_->admit(); !ready) return std::unexpected(ready.error());
    if (impl_->running) return error(ErrorCode::busy, "Editing is disabled while the World runs.");
    auto component = impl_->composition_component(object);
    if (!component) return std::unexpected(component.error());
    auto canonical = canonical_transform(value);
    if (!canonical) return std::unexpected(canonical.error());
    return runtime().with_native<SceneComponent>(object, [&](SceneComponent& transform) -> Result<void> {
        transform.position = canonical->position; transform.rotation = canonical->rotation; transform.scale = canonical->scale;
        return {};
    });
}
Result<void> World::apply_simulation_transform(ObjectId object, math::Vector3 position,
    math::Quaternion rotation) {
    if (auto ready = impl_->admit(); !ready) return std::unexpected(ready.error());
    if (!impl_->running) return error(ErrorCode::busy, "Simulation writes require a running World.");
    auto owner = impl_->component_owner(object);
    if (!owner) return std::unexpected(owner.error());
    auto parent=scene_parent(runtime(),object);
    if (!parent) return std::unexpected(parent.error());
    if (*parent==ObjectId{}) return apply_simulation_local_transform(object,position,rotation);
    auto matrix=world_matrix(runtime(),object);
    if (!matrix) return std::unexpected(matrix.error());
    auto previous=relative_transform(*matrix,transform_matrix(Trs{}));
    if (!previous) return std::unexpected(previous.error());
    auto desired=canonical_transform({position,rotation,previous->scale});
    if (!desired) return std::unexpected(desired.error());
    auto parent_matrix=world_matrix(runtime(),*parent);
    if (!parent_matrix) return std::unexpected(parent_matrix.error());
    auto local=relative_transform(transform_matrix(*desired),*parent_matrix);
    if (!local) return std::unexpected(local.error());
    auto authored=read_transform(runtime(),object);
    if (!authored) return std::unexpected(authored.error());
    for (std::size_t i=0;i<3;++i)
        if (std::abs(local->scale[i]-authored->scale[i])>1e-5f*std::max(1.f,authored->scale[i]))
            return error(ErrorCode::argument_mismatch,"Simulation pose would change authored local scale.");
    return apply_simulation_local_transform(object,local->position,local->rotation);
}
Result<void> World::apply_simulation_local_transform(ObjectId object, math::Vector3 position,
    math::Quaternion rotation) {
    if (auto ready=impl_->admit(); !ready) return std::unexpected(ready.error());
    if (!impl_->running) return error(ErrorCode::busy,"Simulation writes require a running World.");
    auto owner=impl_->component_owner(object);
    if (!owner) return std::unexpected(owner.error());
    auto current = read_transform(runtime(), object);
    if (!current) return std::unexpected(current.error());
    auto canonical = canonical_transform({position, rotation, current->scale});
    if (!canonical) return std::unexpected(canonical.error());
    return runtime().with_native<SceneComponent>(object, [&](SceneComponent& transform) -> Result<void> {
        transform.position = canonical->position;
        transform.rotation = canonical->rotation;
        return {};
    });
}
Result<void> World::write(ObjectId object, std::span<const FieldId> path, Scalar value) {
    if (auto ready = impl_->admit(); !ready) return std::unexpected(ready.error());
    if (impl_->running) return error(ErrorCode::busy, "Editing is disabled while the World runs.");
    auto actor = detail::is(runtime(),object,types().actor);
    if (!actor) return std::unexpected(actor.error());
    if (*actor) { auto data=impl_->composition_actor(object); if (!data) return std::unexpected(data.error()); }
    else { auto data=impl_->composition_component(object); if (!data) return std::unexpected(data.error()); }
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
    auto transform = detail::is(runtime(), object, types().scene_component);
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
Result<Commit> World::reparent(ObjectId object, ObjectId parent, ReparentRule rule) {
    if (auto ready = impl_->admit(); !ready) return std::unexpected(ready.error());
    if (impl_->running) return error(ErrorCode::busy, "Stop the World before editing the spatial hierarchy.");
    if (rule != ReparentRule::keep_local && rule != ReparentRule::keep_world)
        return error(ErrorCode::argument_mismatch, "Unknown reparent rule.");
    auto component = impl_->composition_component(object);
    auto parent_component = impl_->composition_component(parent);
    if (!component) return std::unexpected(component.error());
    if (!parent_component) return std::unexpected(parent_component.error());
    const auto owner = component->owner, parent_owner = parent_component->owner;
    if (owner == ObjectId{} || owner != parent_owner) return error(ErrorCode::invalid_member, "Spatial parent must belong to the same Actor.");
    auto root = root_component(runtime(), owner);
    if (!root) return std::unexpected(root.error());
    if (*root == object) return error(ErrorCode::invalid_member, "The explicit Actor root cannot be parented.");
    auto old_parent = scene_parent(runtime(), object);
    if (!old_parent) return std::unexpected(old_parent.error());
    if (*old_parent == parent) return Commit{};
    for (auto node = parent; node != ObjectId{};) {
        if (node == object) return error(ErrorCode::invalid_member, "Spatial attachment would create a cycle.");
        auto next = scene_parent(runtime(), node);
        if (!next) return std::unexpected(next.error());
        node = *next;
    }
    auto value = read_transform(runtime(), object);
    if (!value) return std::unexpected(value.error());
    if (rule == ReparentRule::keep_world) {
        auto before = world_matrix(runtime(), object), parent_matrix = world_matrix(runtime(), parent);
        if (!before) return std::unexpected(before.error());
        if (!parent_matrix) return std::unexpected(parent_matrix.error());
        value = relative_transform(*before, *parent_matrix);
        if (!value) return std::unexpected(value.error());
    }
    auto held = runtime().hold(parent, types().scene_component);
    if (!held) return std::unexpected(held.error());
    auto reference = runtime().observe(held->reference());
    if (!reference) return std::unexpected(reference.error());
    auto changed = runtime().with_native<SceneComponent>(object, [&](SceneComponent& component) -> Result<void> {
        auto slot = runtime().reference_field(object, Access::parent(component));
        if (!slot) return std::unexpected(slot.error());
        const std::array<ReferenceChange, 1> changes{ReferenceWrite{*slot, *reference}};
        auto written = runtime().apply_reference_changes(changes);
        if (!written) return written;
        component.position = value->position; component.rotation = value->rotation; component.scale = value->scale;
        return {};
    });
    if (!changed) return std::unexpected(changed.error());
    return Commit{true, {}};
}
Result<std::vector<RenderItem>> World::render_items() const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    std::vector<RenderItem> result;
    result.reserve(impl_->presentation.size());
    for (const auto object : impl_->presentation) {
        auto mesh=detail::is(runtime(),object,types().static_mesh_component);
        if (!mesh) return std::unexpected(mesh.error());
        if (!*mesh) continue;
        auto owner = impl_->component_owner(object);
        if (!owner) return std::unexpected(owner.error());
        auto matrix = world_matrix(runtime(), object);
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
