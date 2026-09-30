#include "world/detail.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

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
Result<std::size_t> World::Impl::prepare_bucket(TypeId type, std::size_t additional) {
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        if (buckets[i].type == type) { buckets[i].objects.reserve(buckets[i].objects.size() + additional); return i; }
    }
    auto cube = runtime.is_a(type, types.cube), counter = runtime.is_a(type, types.counter), transform = runtime.is_a(type, types.transform);
    if (!cube) return std::unexpected(cube.error());
    if (!counter) return std::unexpected(counter.error());
    if (!transform) return std::unexpected(transform.error());
    Bucket bucket{type, *cube, *counter, *transform, {}};
    bucket.objects.reserve(additional);
    buckets.push_back(std::move(bucket));
    return buckets.size() - 1;
}
void World::Impl::prepare_activity(std::size_t additional) {
    std::size_t cubes = additional, counters = additional;
    for (const auto& bucket : buckets) {
        if (bucket.cube) cubes += bucket.objects.size();
        if (bucket.counter) counters += bucket.objects.size();
    }
    presentation.reserve(cubes); updates.reserve(counters);
}
Result<void> World::Impl::refresh() {
    updates.clear(); presentation.clear();
    for (auto& [id, position] : positions) { (void)id; position.presentations = 0; }
    for (const auto& bucket : buckets) {
        if (bucket.cube) {
            for (const auto object : bucket.objects) {
                auto component = detail::component_data(runtime, object);
                if (!component) return std::unexpected(component.error());
                const auto owner = positions.find(component->owner);
                if (owner == positions.end()) return error(ErrorCode::invalid_member, "Presentation index contains a foreign component.");
                ++owner->second.presentations;
                presentation.push_back(object);
            }
        }
        if (!running || !bucket.counter) continue;
        for (auto id : bucket.objects) {
            auto component = detail::component_data(runtime, id);
            if (!component) return std::unexpected(component.error());
            auto actor = detail::actor_data(runtime, component->owner);
            if (!actor) return std::unexpected(actor.error());
            if (actor->world != &world) return error(ErrorCode::invalid_member, "Execution index contains a foreign component.");
            if (component->enabled && actor->enabled) updates.push_back(id);
        }
    }
    return {};
}
void World::Impl::cancel_tasks(ObjectId target, bool actor) noexcept {
    std::erase_if(tasks, [&](Task& task) {
        if (target != ObjectId{} && (actor ? task.actor : task.component) != target) return false;
        task.coroutine.cancel();
        return true;
    });
}
void World::Impl::halt() noexcept { running = false; updates.clear(); cancel_tasks(); }
std::optional<Error> World::Impl::notify(Lifecycle event, ObjectId object) {
    if (!observer) return {};
    auto scope = ExecutionScope::enter(runtime);
    if (!scope) return scope.error();
    detail::DispatchScope dispatch{dispatch_depth};
    try {
        auto result = observer(world, event, object);
        if (!result) return result.error();
    } catch (const std::exception& exception) {
        return Error{ErrorCode::invocation_failed, exception.what()};
    } catch (...) {
        return Error{ErrorCode::invocation_failed, "World lifecycle observer threw an unknown exception."};
    }
    return {};
}
Commit World::Impl::finish(bool changed, std::optional<Error> failure) {
    if (failure) halt();
    return {changed, std::move(failure)};
}
void World::Impl::erase_indexes(ObjectId object) noexcept {
    for (auto& bucket : buckets) std::erase(bucket.objects, object);
    std::erase(presentation, object); std::erase(updates, object);
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
    std::optional<ObjectId> transform;
    std::unordered_set<ObjectId> seen;
    std::vector<std::size_t> buckets;
    buckets.reserve(group->size());
    for (const auto& component : *group) {
        if (!seen.insert(component.id()).second) return error(ErrorCode::invalid_member, "Actor contains duplicate component identities.");
        auto owned = detail::component_data(runtime(), component.id());
        if (!owned) return std::unexpected(owned.error());
        if (owned->owner != object) return error(ErrorCode::invalid_member, "Component has a different owner.");
        auto type = runtime().type_of(component.id());
        if (!type) return std::unexpected(type.error());
        auto bucket = state.prepare_bucket(*type, group->size());
        if (!bucket) return std::unexpected(bucket.error());
        buckets.push_back(*bucket);
        if (state.buckets[*bucket].transform) {
            if (transform) return error(ErrorCode::invalid_member, "Only one root Transform is supported.");
            transform = component.id();
            auto trs = read_transform(runtime(), component.id());
            if (!trs) return std::unexpected(trs.error());
            auto canonical = canonical_transform(*trs);
            if (!canonical) return std::unexpected(canonical.error());
        }
    }
    for (std::size_t i = 0; i < group->size(); ++i) {
        if (!state.buckets[buckets[i]].cube) continue;
        auto target = detail::cube_transform(runtime(), (*group)[i].id());
        if (!target) return std::unexpected(target.error());
        if (!transform || *target != *transform) return error(ErrorCode::invalid_member, "Cube requires its Actor's explicit Transform.");
    }
    state.members.reserve(state.members.size() + 1);
    state.positions.reserve(state.positions.size() + 1);
    state.prepare_activity(group->size());
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
    auto failure = state.notify(Lifecycle::attached, object);
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
    std::optional<Error> failure;
    {
        detail::FlagScope transition{state.transitioning};
        state.cancel_tasks(object);
        for (const auto& component : *group) state.erase_indexes(component.id());
        if (state.running && data->enabled) failure = state.notify(Lifecycle::deactivated, object);
        auto published = runtime().with_native<Actor>(object, [&](Actor& actor) -> Result<void> {
            Access::member_world(actor) = nullptr;
            return {};
        });
        if (!published) { state.halt(); return std::unexpected(published.error()); }
        state.members.erase(state.members.begin() + std::ptrdiff_t(*at));
        state.positions.erase(object);
        for (std::size_t i = *at; i < state.members.size(); ++i) state.positions.find(state.members[i].id())->second.index = i;
        auto detached = state.notify(Lifecycle::detached, object);
        if (!failure) failure = std::move(detached);
    }
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
    auto actor_at = state.member(actor_id);
    if (!actor_at) return std::unexpected(actor_at.error());
    auto existing = components(runtime(), actor_id);
    if (!existing) return std::unexpected(existing.error());
    if (index == append) index = existing->size();
    if (index > existing->size()) return error(ErrorCode::argument_mismatch, "Component insertion index is out of range.");
    if (incoming.empty()) return Commit{};
    std::vector<ObjectRoot> roots;
    std::vector<ObjectReference> references, transform_references;
    std::vector<std::size_t> buckets;
    roots.reserve(incoming.size()); references.reserve(incoming.size());
    buckets.reserve(incoming.size()); transform_references.resize(incoming.size());
    std::unordered_set<ObjectId> seen;
    std::optional<ObjectId> transform;
    for (const auto& component : *existing) {
        seen.insert(component.id());
        auto matches = detail::is(runtime(), component.id(), types().transform);
        if (!matches) return std::unexpected(matches.error());
        if (*matches) transform = component.id();
    }
    for (const auto& item : incoming) {
        if (!seen.insert(item.component).second) return error(ErrorCode::invalid_member, "Component is already present or duplicated in the batch.");
        auto data = detail::component_data(runtime(), item.component);
        if (!data) return std::unexpected(data.error());
        if (data->owner != ObjectId{}) return error(ErrorCode::invalid_member, "Component already has an owner.");
        auto type = runtime().type_of(item.component);
        if (!type) return std::unexpected(type.error());
        auto bucket = state.prepare_bucket(*type, incoming.size());
        if (!bucket) return std::unexpected(bucket.error());
        buckets.push_back(*bucket);
        if (state.buckets[*bucket].transform) {
            if (transform) return error(ErrorCode::invalid_member, "Only one root Transform is supported.");
            transform = item.component;
            auto value = read_transform(runtime(), item.component);
            if (!value) return std::unexpected(value.error());
            auto canonical = canonical_transform(*value);
            if (!canonical) return std::unexpected(canonical.error());
        }
        if (!state.buckets[*bucket].cube && item.transform != ObjectId{})
            return error(ErrorCode::argument_mismatch, "Only Cube accepts a Transform binding.");
        auto root = runtime().hold(item.component, types().component);
        if (!root) return std::unexpected(root.error());
        references.push_back(root->reference()); roots.push_back(std::move(*root));
    }
    for (std::size_t i = 0; i < incoming.size(); ++i) {
        if (!state.buckets[buckets[i]].cube) continue;
        auto target = incoming[i].transform;
        if (target == ObjectId{}) {
            auto previous = detail::cube_transform(runtime(), incoming[i].component);
            if (!previous) return std::unexpected(previous.error());
            target = *previous;
        }
        if (!transform || target != *transform)
            return error(ErrorCode::invalid_member, "Cube must explicitly bind the same Actor's Transform.");
        auto held = runtime().hold(target, types().transform);
        if (!held) return std::unexpected(held.error());
        auto observed = runtime().observe(held->reference());
        if (!observed) return std::unexpected(observed.error());
        transform_references[i] = *observed;
    }
    auto actor_root = runtime().hold(actor_id, types().actor);
    if (!actor_root) return std::unexpected(actor_root.error());
    state.prepare_activity(incoming.size());
    {
        detail::FlagScope transition{state.transitioning};
        auto changed = runtime().with_native<Actor>(actor_id, [&](Actor& actor) -> Result<void> {
            auto sequence = runtime().reference_field(actor_id, Access::sequence(actor));
            if (!sequence) return std::unexpected(sequence.error());
            std::vector<ReferenceChange> changes;
            changes.reserve(1 + incoming.size() * 2);
            changes.push_back(SequenceSplice{*sequence, index, 0, references});
            for (std::size_t i = 0; i < incoming.size(); ++i) {
                auto owner = runtime().with_native<Component>(incoming[i].component, [&](Component& component) {
                    return runtime().reference_field(incoming[i].component, Access::owner(component));
                });
                if (!owner) return std::unexpected(owner.error());
                changes.push_back(ReferenceWrite{std::move(*owner), actor_root->reference()});
                if (state.buckets[buckets[i]].cube) {
                    auto target = runtime().with_native<Cube>(incoming[i].component, [&](Cube& cube) {
                        return runtime().reference_field(incoming[i].component, Access::transform(cube));
                    });
                    if (!target) return std::unexpected(target.error());
                    changes.push_back(ReferenceWrite{std::move(*target), transform_references[i]});
                }
            }
            return runtime().apply_reference_changes(changes);
        });
        if (!changed) return std::unexpected(changed.error());
        for (std::size_t i = 0; i < incoming.size(); ++i) state.buckets[buckets[i]].objects.push_back(incoming[i].component);
    }
    auto refreshed = state.refresh();
    if (!refreshed) return state.finish(true, refreshed.error());
    std::optional<Error> failure;
    for (const auto& item : incoming) {
        auto notification = state.notify(Lifecycle::attached, item.component);
        if (!failure) failure = std::move(notification);
        if (!failure) {
            auto status = inspect_component(runtime(), item.component);
            if (!status) failure = status.error();
            else if (status->active) failure = state.notify(Lifecycle::activated, item.component);
        }
    }
    return state.finish(true, std::move(failure));
}

Result<ComponentRemoval> World::remove_component(ObjectId object) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto owner = state.component_owner(object);
    if (!owner) return std::unexpected(owner.error());
    auto group = components(runtime(), *owner);
    if (!group) return std::unexpected(group.error());
    const auto at = std::ranges::find_if(*group, [&](const ObjectRoot& item) { return item.id() == object; });
    if (at == group->end()) return error(ErrorCode::invalid_member, "Component is absent from its owner's composition.");
    for (const auto& item : *group) {
        auto cube = detail::is(runtime(), item.id(), types().cube);
        if (!cube) return std::unexpected(cube.error());
        if (*cube) {
            auto target = detail::cube_transform(runtime(), item.id());
            if (!target) return std::unexpected(target.error());
            if (*target == object) return error(ErrorCode::invalid_member, "Transform is required by an attached Cube.");
        }
    }
    auto root = runtime().hold(object, types().component);
    if (!root) return std::unexpected(root.error());
    auto null_owner = runtime().null_reference(types().actor);
    if (!null_owner) return std::unexpected(null_owner.error());
    auto status = inspect_component(runtime(), object);
    if (!status) return std::unexpected(status.error());
    ComponentRemoval result;
    result.root = std::move(*root); result.owner = *owner;
    result.index = std::size_t(at - group->begin()); result.origin_ = state.lifetime;
    std::optional<Error> failure;
    {
        detail::FlagScope transition{state.transitioning};
        state.cancel_tasks(object, false);
        std::erase(state.updates, object);
        if (status->active) failure = state.notify(Lifecycle::deactivated, object);
        auto removed = runtime().with_native<Actor>(*owner, [&](Actor& actor) -> Result<void> {
            auto sequence = runtime().reference_field(*owner, Access::sequence(actor));
            if (!sequence) return std::unexpected(sequence.error());
            return runtime().with_native<Component>(object, [&](Component& component) -> Result<void> {
                auto field = runtime().reference_field(object, Access::owner(component));
                if (!field) return std::unexpected(field.error());
                const std::array<ReferenceChange, 2> changes{
                    SequenceSplice{*sequence, result.index, 1, {}}, ReferenceWrite{*field, *null_owner}};
                return runtime().apply_reference_changes(changes);
            });
        });
        if (!removed) { state.halt(); return std::unexpected(removed.error()); }
        state.erase_indexes(object);
        auto detached = state.notify(Lifecycle::detached, object);
        if (!failure) failure = std::move(detached);
    }
    auto refreshed = state.refresh();
    if (!refreshed && !failure) failure = refreshed.error();
    result.commit = state.finish(true, std::move(failure));
    return result;
}
Result<Commit> World::restore(const ComponentRemoval& removal) {
    auto origin = removal.origin_.lock();
    if (!origin || !origin->open || origin != impl_->lifetime)
        return error(ErrorCode::invalid_member, "Component removal does not belong to this open World's history.");
    const std::array incoming{Attachment{removal.id(), {}}};
    return attach_components(removal.owner, incoming, removal.index);
}

Result<Scalar> World::read(ObjectId component, std::span<const FieldId> path) const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    auto owner = impl_->component_owner(component);
    if (!owner) return std::unexpected(owner.error());
    return runtime().read(component, path);
}
Result<void> World::set_transform(ObjectId object, Trs value) {
    if (auto ready = impl_->admit(); !ready) return std::unexpected(ready.error());
    if (impl_->running) return error(ErrorCode::busy, "Editing is disabled while the World runs.");
    auto owner = impl_->component_owner(object);
    if (!owner) return std::unexpected(owner.error());
    auto canonical = canonical_transform(value);
    if (!canonical) return std::unexpected(canonical.error());
    return runtime().with_native<Transform>(object, [&](Transform& transform) -> Result<void> {
        transform.position = canonical->position; transform.rotation = canonical->rotation; transform.scale = canonical->scale;
        return {};
    });
}
Result<void> World::write(ObjectId object, std::span<const FieldId> path, Scalar value) {
    if (auto ready = impl_->admit(); !ready) return std::unexpected(ready.error());
    if (impl_->running) return error(ErrorCode::busy, "Editing is disabled while the World runs.");
    auto owner = impl_->component_owner(object);
    if (!owner) return std::unexpected(owner.error());
    auto transform = detail::is(runtime(), object, types().transform);
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
    result.reserve(impl_->presentation.size());
    for (const auto object : impl_->presentation) {
        auto owner = impl_->component_owner(object);
        if (!owner) return std::unexpected(owner.error());
        auto transform = detail::cube_transform(runtime(), object);
        if (!transform) return std::unexpected(transform.error());
        auto transform_owner = impl_->component_owner(*transform);
        if (!transform_owner) return std::unexpected(transform_owner.error());
        if (*owner != *transform_owner) return error(ErrorCode::invalid_member, "Cube's Transform belongs to another Actor.");
        auto matrix = world_matrix(runtime(), *transform);
        if (!matrix) return std::unexpected(matrix.error());
        result.push_back({*owner, object, *transform, *matrix});
    }
    return result;
}

}
