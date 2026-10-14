#include "world/detail.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace atelier::world {
using detail::Access;
using detail::error;

Result<std::vector<ObjectRoot>> children(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Entity>(object, [&](Entity& entity) -> Result<std::vector<ObjectRoot>> {
        if (Access::transitioning(entity)) return error(ErrorCode::busy, "Entity is transitioning.");
        if (auto* world = Access::member_world(entity))
            if (auto ready = Access::state(*world).admit(false); !ready) return std::unexpected(ready.error());
        auto field = runtime.reference_field(object, Access::children(entity));
        if (!field) return std::unexpected(field.error());
        auto count = runtime.sequence_size(*field);
        if (!count) return std::unexpected(count.error());
        std::vector<ObjectRoot> result;
        result.reserve(*count);
        for (std::size_t i = 0; i < *count; ++i) {
            auto reference = runtime.sequence_read(*field, i);
            if (!reference) return std::unexpected(reference.error());
            auto held = runtime.hold(*reference);
            if (!held) return std::unexpected(held.error());
            result.push_back(std::move(*held));
        }
        return result;
    });
}
Result<ObjectId> parent_entity(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Entity>(object, [&](Entity& entity) -> Result<ObjectId> {
        const auto parent = Access::parent(entity).id();
        if (parent == ObjectId{}) return parent;
        // A detached leaf's weak parent does not keep an abandoned graph alive.
        auto valid = runtime.type_of(parent);
        if (!valid && valid.error().code == ErrorCode::invalid_object) return ObjectId{};
        if (!valid) return std::unexpected(valid.error());
        return parent;
    });
}
Result<bool> has_transform(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Entity>(object, [](Entity& entity) -> Result<bool> { return Access::spatial(entity); });
}
Result<bool> component_requires_space(Runtime& runtime, ObjectId object) {
    auto component = runtime.native_type<Component>();
    auto primitive = runtime.native_type<PrimitiveComponent>();
    auto light = runtime.native_type<LightComponent>();
    if (!component) return std::unexpected(component.error());
    if (!primitive) return std::unexpected(primitive.error());
    if (!light) return std::unexpected(light.error());
    if (auto valid = detail::validate_object(runtime, object, *component); !valid) return std::unexpected(valid.error());
    auto is_primitive = detail::is(runtime, object, *primitive);
    if (!is_primitive) return std::unexpected(is_primitive.error());
    if (*is_primitive) return true;
    auto is_light = detail::is(runtime, object, *light);
    if (!is_light) return std::unexpected(is_light.error());
    if (!*is_light) return false;
    auto settings = read_light(runtime, object);
    if (!settings) return std::unexpected(settings.error());
    return settings->kind != LightKind::sky;
}
Result<bool> effective_enabled(Runtime& runtime, ObjectId object) {
    std::unordered_set<ObjectId> seen;
    for (auto node = object; node != ObjectId{};) {
        if (!seen.insert(node).second) return error(ErrorCode::invalid_member, "Composition contains a cycle.");
        auto data = detail::entity_data(runtime, node);
        if (!data) return std::unexpected(data.error());
        if (!data->enabled) return false;
        auto parent = parent_entity(runtime, node);
        if (!parent) return std::unexpected(parent.error());
        node = *parent;
    }
    return true;
}
Result<std::optional<ObjectId>> space_reference(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Entity>(object, [&](Entity& entity) -> Result<std::optional<ObjectId>> {
        if (!Access::explicit_origin(entity)) return std::optional<ObjectId>{};
        if (Access::world_origin(entity)) return std::optional<ObjectId>{ObjectId{}};
        const auto target = Access::origin(entity).id();
        auto valid = runtime.type_of(target);
        if (!valid) return error(ErrorCode::invalid_object, "Entity's explicit spatial origin is no longer valid.");
        return std::optional<ObjectId>{target};
    });
}
namespace detail {
Result<SpaceNode*> SpaceGraph::node(ObjectId object) {
    if (auto found = nodes.find(object); found != nodes.end()) return &found->second;
    auto parent_id = parent_entity(runtime, object);
    if (!parent_id) return std::unexpected(parent_id.error());
    auto origin = space_reference(runtime, object);
    if (!origin) return std::unexpected(origin.error());
    auto value = runtime.with_native<Entity>(object, [&](Entity& entity) -> Result<SpaceNode> {
        return SpaceNode{*parent_id, *origin, Access::spatial(entity),
            {entity.position, entity.rotation, entity.scale}, Access::member_world(entity)};
    });
    if (!value) return std::unexpected(value.error());
    return &nodes.emplace(object, *value).first->second;
}
Result<ObjectId> SpaceGraph::parent(ObjectId object) {
    auto current = node(object);
    if (!current) return std::unexpected(current.error());
    auto* world = (*current)->world;
    if ((*current)->origin) {
        const auto origin = *(*current)->origin;
        if (origin == ObjectId{}) return origin;
        auto target = node(origin);
        if (!target) return std::unexpected(target.error());
        if (!(*target)->spatial || (*target)->world != world)
            return error(ErrorCode::invalid_member, "Spatial origin requires a spatial Entity with the same World membership.");
        return origin;
    }
    std::unordered_set<ObjectId> seen{object};
    for (auto id = (*current)->parent; id != ObjectId{};) {
        if (!seen.insert(id).second) return error(ErrorCode::invalid_member, "Composition contains a cycle.");
        auto ancestor = node(id);
        if (!ancestor) return std::unexpected(ancestor.error());
        if ((*ancestor)->world != world) return error(ErrorCode::invalid_member, "Composition crosses World membership.");
        if ((*ancestor)->spatial) return id;
        id = (*ancestor)->parent;
    }
    return ObjectId{};
}
Result<math::Matrix4> SpaceGraph::matrix(ObjectId object) {
    if (object == ObjectId{}) return math::Matrix4::identity();
    std::vector<ObjectId> path;
    std::unordered_set<ObjectId> seen;
    auto result = math::Matrix4::identity();
    for (auto id = object; id != ObjectId{};) {
        if (auto found = matrices.find(id); found != matrices.end()) { result = found->second; break; }
        if (!seen.insert(id).second) return error(ErrorCode::invalid_member, "Spatial references contain a cycle.");
        auto value = node(id);
        if (!value) return std::unexpected(value.error());
        if (!(*value)->spatial) return error(ErrorCode::invalid_member, "Entity has no Transform.");
        path.push_back(id);
        auto next = parent(id);
        if (!next) return std::unexpected(next.error());
        id = *next;
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        auto local = canonical_transform(nodes.at(*it).local);
        if (!local) return std::unexpected(local.error());
        auto composed = multiply(transform_matrix(*local), result);
        if (!composed) return std::unexpected(composed.error());
        result = *composed;
        matrices.emplace(*it, result);
    }
    return result;
}
Result<std::vector<ObjectRoot>> subtree(Runtime& runtime, ObjectId object) {
    std::vector<ObjectRoot> result;
    std::vector<ObjectId> pending{object};
    std::unordered_set<ObjectId> seen;
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        if (!seen.insert(id).second) return error(ErrorCode::invalid_member, "Composition contains a cycle or duplicate child.");
        auto type = runtime.native_type<Entity>();
        if (!type) return std::unexpected(type.error());
        auto held = runtime.hold(id, *type);
        if (!held) return std::unexpected(held.error());
        auto owned = children(runtime, id);
        if (!owned) return std::unexpected(owned.error());
        for (auto it = owned->rbegin(); it != owned->rend(); ++it) {
            auto parent = parent_entity(runtime, it->id());
            if (!parent) return std::unexpected(parent.error());
            if (*parent != id) return error(ErrorCode::invalid_member, "Composition child has a different parent.");
            pending.push_back(it->id());
        }
        result.push_back(std::move(*held));
    }
    return result;
}
Result<void> requires_space(Runtime& runtime, ObjectId entity, std::span<const ObjectRoot> group) {
    auto spatial = has_transform(runtime, entity);
    if (!spatial) return std::unexpected(spatial.error());
    if (*spatial) return {};
    for (const auto& item : group) {
        auto needs = component_requires_space(runtime, item.id());
        if (!needs) return std::unexpected(needs.error());
        if (*needs) return error(ErrorCode::invalid_member, "Spatial capability requires its owner Entity to have a Transform.");
    }
    return {};
}
Result<void> validate_tree(Runtime& runtime, ObjectId entity, std::span<const ObjectRoot> group) {
    std::unordered_set<ObjectId> seen;
    for (const auto& item : group) {
        if (!seen.insert(item.id()).second) return error(ErrorCode::invalid_member, "Component appears twice in its Entity.");
        auto data = component_data(runtime, item.id());
        if (!data) return std::unexpected(data.error());
        if (data->owner != entity) return error(ErrorCode::invalid_member, "Component owner differs from its composition.");
    }
    if (auto valid = requires_space(runtime, entity, group); !valid) return valid;
    auto spatial = has_transform(runtime, entity);
    if (!spatial) return std::unexpected(spatial.error());
    if (*spatial) {
        auto value = read_transform(runtime, entity);
        if (!value) return std::unexpected(value.error());
        if (auto canonical = canonical_transform(*value); !canonical) return std::unexpected(canonical.error());
    }
    return {};
}
Result<void> clear_component(Runtime& runtime, ObjectId entity, ObjectId object, std::size_t index) {
    return runtime.with_native<Entity>(entity, [&](Entity&) -> Result<void> {
    auto type = runtime.native_type<Entity>();
    if (!type) return std::unexpected(type.error());
    auto null_owner = runtime.null_reference(*type);
    if (!null_owner) return std::unexpected(null_owner.error());
    auto sequence = runtime.with_native<Entity>(entity, [&](Entity& value) { return runtime.reference_field(entity, Access::sequence(value)); });
    auto owner = runtime.with_native<Component>(object, [&](Component& value) { return runtime.reference_field(object, Access::owner(value)); });
    if (!sequence) return std::unexpected(sequence.error());
    if (!owner) return std::unexpected(owner.error());
    const std::array<ReferenceChange, 2> changes{SequenceSplice{*sequence, index, 1, {}}, ReferenceWrite{*owner, *null_owner}};
    return runtime.apply_reference_changes(changes);
    });
}
}
Result<ObjectId> spatial_parent(Runtime& runtime, ObjectId object) {
    detail::SpaceGraph graph{runtime};
    return graph.parent(object);
}
Result<math::Matrix4> world_matrix(Runtime& runtime, ObjectId object) {
    if (object == ObjectId{}) return error(ErrorCode::invalid_object, "World matrix requires an Entity identity.");
    detail::SpaceGraph graph{runtime};
    return graph.matrix(object);
}

namespace {
Result<void> assign_parent(Runtime& runtime, ObjectId object, ObjectId parent, std::size_t index) {
    return runtime.with_native<Entity>(object, [&](Entity&) -> Result<void> {
    auto old_parent = parent_entity(runtime, object);
    if (!old_parent) return std::unexpected(old_parent.error());
    std::vector<ObjectRoot> old_roots, new_roots;
    std::vector<ObjectReference> old_references, new_references;
    std::vector<ReferenceChange> changes;
    const auto sequence = [&](ObjectId id) {
        return runtime.with_native<Entity>(id, [&](Entity& value) { return runtime.reference_field(id, Access::children(value)); });
    };
    if (*old_parent != ObjectId{}) {
        auto previous = children(runtime, *old_parent);
        if (!previous) return std::unexpected(previous.error());
        old_roots = std::move(*previous);
        bool found = false;
        for (const auto& child : old_roots) {
            if (child.id() == object) { found = true; continue; }
            old_references.push_back(child.reference());
        }
        if (!found) return error(ErrorCode::invalid_member, "Entity is absent from its parent's composition.");
    }
    if (parent != ObjectId{}) {
        auto next = children(runtime, parent);
        if (!next) return std::unexpected(next.error());
        new_roots = std::move(*next);
        for (const auto& child : new_roots) if (child.id() != object) new_references.push_back(child.reference());
        if (index == append) index = new_references.size();
        if (index > new_references.size()) return error(ErrorCode::argument_mismatch, "Child insertion index is out of range.");
        auto held = runtime.hold(object);
        if (!held) return std::unexpected(held.error());
        new_references.insert(new_references.begin() + std::ptrdiff_t(index), held->reference());
        auto field = sequence(parent);
        if (!field) return std::unexpected(field.error());
        changes.push_back(SequenceSplice{*field, 0, new_roots.size(), new_references});
    }
    if (*old_parent != ObjectId{} && *old_parent != parent) {
        auto field = sequence(*old_parent);
        if (!field) return std::unexpected(field.error());
        changes.push_back(SequenceSplice{*field, 0, old_roots.size(), old_references});
    }
    auto type = runtime.native_type<Entity>();
    if (!type) return std::unexpected(type.error());
    auto reference = runtime.null_reference(*type, ValueForm::weak_reference);
    if (!reference) return std::unexpected(reference.error());
    if (parent != ObjectId{}) {
        auto held = runtime.hold(parent);
        if (!held) return std::unexpected(held.error());
        reference = runtime.observe(held->reference());
        if (!reference) return std::unexpected(reference.error());
    }
    auto field = runtime.with_native<Entity>(object, [&](Entity& value) { return runtime.reference_field(object, Access::parent(value)); });
    if (!field) return std::unexpected(field.error());
    changes.push_back(ReferenceWrite{*field, *reference});
    return runtime.apply_reference_changes(changes);
    });
}
Result<void> publish_transforms(Runtime& runtime, const std::vector<std::pair<ObjectId, Trs>>& values) {
    for (const auto& [id, value] : values) {
        auto written = runtime.with_native<Entity>(id, [&](Entity& entity) -> Result<void> {
            entity.position = value.position; entity.rotation = value.rotation; entity.scale = value.scale;
            return {};
        });
        if (!written) return written;
    }
    return {};
}
// Only frontiers whose resolved origin changes need local compensation. Deeper
// descendants keep their local values, including below nonspatial parents.
Result<std::vector<std::pair<ObjectId, Trs>>> preserve_space(detail::SpaceGraph& before,
    detail::SpaceGraph& candidate, std::span<const ObjectRoot> affected, ReparentRule rule) {
    std::vector<std::pair<ObjectId, Trs>> changes;
    for (const auto& root : affected) {
        auto value = candidate.node(root.id());
        if (!value) return std::unexpected(value.error());
        if (!(*value)->spatial) continue;
        if (rule == ReparentRule::keep_world) {
            auto old_parent = before.parent(root.id()), next_parent = candidate.parent(root.id());
            if (!old_parent) return std::unexpected(old_parent.error());
            if (!next_parent) return std::unexpected(next_parent.error());
            if (*old_parent != *next_parent) {
                auto original = before.matrix(root.id()), origin = candidate.matrix(*next_parent);
                if (!original) return std::unexpected(original.error());
                if (!origin) return std::unexpected(origin.error());
                auto local = relative_transform(*original, *origin);
                if (!local) return std::unexpected(local.error());
                (*value)->local = *local;
                candidate.invalidate();
                changes.emplace_back(root.id(), *local);
            }
        }
        auto valid = candidate.matrix(root.id());
        if (!valid) return std::unexpected(valid.error());
    }
    return changes;
}
}
namespace detail {
Result<void> unlink_parent(Runtime& runtime, ObjectId object) { return assign_parent(runtime, object, {}, append); }
}

Result<void> World::set_transform(ObjectId object, Trs value) {
    if (auto ready = impl_->admit_edit(object); !ready) return ready;
    auto data = impl_->composition_entity(object);
    if (!data) return std::unexpected(data.error());
    auto canonical = canonical_transform(value);
    if (!canonical) return std::unexpected(canonical.error());
    detail::SpaceGraph graph{runtime()};
    auto node = graph.node(object);
    if (!node) return std::unexpected(node.error());
    (*node)->spatial = true; (*node)->local = *canonical;
    if (auto valid = graph.matrix(object); !valid) return std::unexpected(valid.error());
    return runtime().with_native<Entity>(object, [&](Entity& entity) -> Result<void> {
        entity.position = canonical->position; entity.rotation = canonical->rotation; entity.scale = canonical->scale;
        Access::spatial(entity) = true;
        return {};
    });
}
Result<void> World::remove_transform(ObjectId object) {
    if (auto ready = impl_->admit_edit(object); !ready) return ready;
    auto data = impl_->composition_entity(object);
    if (!data) return std::unexpected(data.error());
    auto spatial = has_transform(runtime(), object);
    if (!spatial) return std::unexpected(spatial.error());
    if (!*spatial) return {};
    auto group = components(runtime(), object);
    if (!group) return std::unexpected(group.error());
    for (const auto& root : *group) {
        auto needs = component_requires_space(runtime(), root.id());
        if (!needs) return std::unexpected(needs.error());
        if (*needs) return error(ErrorCode::invalid_member, "Remove spatial capabilities before removing the Entity Transform.");
    }
    auto root = object;
    if (!data->world) {
        std::unordered_set<ObjectId> seen{object};
        for (;;) {
            auto parent = parent_entity(runtime(), root);
            if (!parent) return std::unexpected(parent.error());
            if (*parent == ObjectId{}) break;
            if (!seen.insert(*parent).second) return error(ErrorCode::invalid_member, "Composition contains a cycle.");
            root = *parent;
        }
    }
    auto all = detail::subtree(runtime(), root);
    if (!all) return std::unexpected(all.error());
    std::vector<ObjectId> candidates;
    for (const auto& item : *all) candidates.push_back(item.id());
    if (data->world) for (const auto& member : impl_->members) candidates.push_back(member.id());
    detail::SpaceGraph candidate{runtime()};
    auto parent = parent_entity(runtime(), object);
    if (!parent) return std::unexpected(parent.error());
    // Removing space also repairs an expired former origin. Its old matrix is
    // irrelevant; surviving default inheritance is evaluated in the final graph.
    candidate.nodes.emplace(object, detail::SpaceNode{*parent, {}, false, {}, data->world});
    for (const auto id : candidates) {
        auto current = candidate.node(id);
        if (!current) return std::unexpected(current.error());
        if (!(*current)->spatial) continue;
        if (auto valid = candidate.matrix(id); !valid) return std::unexpected(valid.error());
    }
    auto reference = runtime().null_reference(types().entity, ValueForm::weak_reference);
    if (!reference) return std::unexpected(reference.error());
    return runtime().with_native<Entity>(object, [&](Entity& entity) -> Result<void> {
        auto field = runtime().reference_field(object, Access::origin(entity));
        if (!field) return std::unexpected(field.error());
        const ReferenceChange change = ReferenceWrite{*field, *reference};
        if (auto cleared = runtime().apply_reference_changes({&change, 1}); !cleared) return cleared;
        Access::explicit_origin(entity) = false;
        Access::world_origin(entity) = false;
        Access::spatial(entity) = false;
        return {};
    });
}
Result<Commit> World::set_space_reference(ObjectId object, std::optional<ObjectId> origin, ReparentRule rule) {
    if (auto ready = impl_->admit(); !ready) return std::unexpected(ready.error());
    if (rule != ReparentRule::keep_local && rule != ReparentRule::keep_world) return error(ErrorCode::argument_mismatch, "Unknown reparent rule.");
    auto data = impl_->composition_entity(object);
    if (!data) return std::unexpected(data.error());
    auto current = space_reference(runtime(), object);
    if (!current && (rule != ReparentRule::keep_local || current.error().code != ErrorCode::invalid_object))
        return std::unexpected(current.error());
    auto spatial = has_transform(runtime(), object);
    if (!spatial) return std::unexpected(spatial.error());
    if (!*spatial) return error(ErrorCode::invalid_member, "Spatial reference requires an Entity Transform.");
    if (current && *current == origin) return Commit{};
    detail::SpaceGraph before{runtime()}, candidate{runtime()};
    auto parent = parent_entity(runtime(), object);
    if (!parent) return std::unexpected(parent.error());
    // Rebinding can repair an expired weak origin without evaluating its old space.
    auto node = runtime().with_native<Entity>(object, [&](Entity& entity) -> Result<detail::SpaceNode> {
        return detail::SpaceNode{*parent, origin, Access::spatial(entity),
            {entity.position, entity.rotation, entity.scale}, Access::member_world(entity)};
    });
    if (!node) return std::unexpected(node.error());
    candidate.nodes.emplace(object, *node);
    auto group = detail::subtree(runtime(), object);
    if (!group) return std::unexpected(group.error());
    auto changes = preserve_space(before, candidate, *group, rule);
    if (!changes) return std::unexpected(changes.error());
    auto reference = runtime().null_reference(types().entity, ValueForm::weak_reference);
    if (!reference) return std::unexpected(reference.error());
    if (origin && *origin != ObjectId{}) {
        auto held = runtime().hold(*origin, types().entity);
        if (!held) return std::unexpected(held.error());
        reference = runtime().observe(held->reference());
        if (!reference) return std::unexpected(reference.error());
    }
    auto published = runtime().with_native<Entity>(object, [&](Entity& entity) -> Result<void> {
        auto field = runtime().reference_field(object, Access::origin(entity));
        if (!field) return std::unexpected(field.error());
        const ReferenceChange change = ReferenceWrite{*field, *reference};
        if (auto changed = runtime().apply_reference_changes({&change, 1}); !changed) return changed;
        Access::explicit_origin(entity) = origin.has_value();
        Access::world_origin(entity) = origin && *origin == ObjectId{};
        return {};
    });
    if (!published) return std::unexpected(published.error());
    if (auto applied = publish_transforms(runtime(), *changes); !applied) return impl_->finish(true, applied.error());
    return Commit{true, {}};
}

Result<void> World::apply_simulation_transform(ObjectId entity, math::Vector3 position, math::Quaternion rotation) {
    const SimulationPose pose{entity, position, rotation};
    return apply_simulation_transforms({&pose, 1});
}
Result<void> World::apply_simulation_transforms(std::span<const SimulationPose> poses) {
    if (auto ready = impl_->admit(); !ready) return ready;
    if (!running()) return error(ErrorCode::busy, "Simulation writes require a running World.");
    detail::SpaceGraph before{runtime()}, candidate{runtime()};
    std::unordered_map<ObjectId, Trs> desired;
    for (const auto& pose : poses) {
        if (auto member = impl_->member(pose.entity); !member) return std::unexpected(member.error());
        if (desired.contains(pose.entity)) return error(ErrorCode::invalid_member, "An Entity has multiple simulation pose writers.");
        auto matrix = before.matrix(pose.entity);
        if (!matrix) return std::unexpected(matrix.error());
        auto original = relative_transform(*matrix, math::Matrix4::identity());
        if (!original) return std::unexpected(original.error());
        auto value = canonical_transform({pose.position, pose.rotation, original->scale});
        if (!value) return std::unexpected(value.error());
        desired.emplace(pose.entity, *value);
    }
    // Desired world matrices make conversion independent of hash/container order.
    for (const auto& [id, value] : desired) candidate.matrices.emplace(id, transform_matrix(value));
    std::vector<std::pair<ObjectId, Trs>> locals;
    locals.reserve(poses.size());
    for (const auto& pose : poses) {
        auto parent = candidate.parent(pose.entity);
        if (!parent) return std::unexpected(parent.error());
        auto origin = candidate.matrix(*parent);
        if (!origin) return std::unexpected(origin.error());
        auto local = relative_transform(transform_matrix(desired.at(pose.entity)), *origin);
        if (!local) return std::unexpected(local.error());
        auto authored = before.node(pose.entity);
        if (!authored) return std::unexpected(authored.error());
        for (std::size_t i = 0; i < 3; ++i)
            if (std::abs(local->scale[i] - (*authored)->local.scale[i]) > 1e-5f * std::max(1.f, (*authored)->local.scale[i]))
                return error(ErrorCode::argument_mismatch, "Simulation pose would change authored local scale.");
        local->scale = (*authored)->local.scale;
        locals.emplace_back(pose.entity, *local);
    }
    return publish_transforms(runtime(), locals);
}

Result<Commit> World::reparent(ObjectId object, ObjectId parent, ReparentRule rule, std::size_t index) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    if (rule != ReparentRule::keep_local && rule != ReparentRule::keep_world) return error(ErrorCode::argument_mismatch, "Unknown reparent rule.");
    auto data = state.composition_entity(object);
    if (!data) return std::unexpected(data.error());
    if (parent != ObjectId{}) {
        auto destination = state.composition_entity(parent);
        if (!destination) return std::unexpected(destination.error());
        if (destination->world != data->world) return error(ErrorCode::invalid_member, "Reparent requires the same World membership; attach a detached subtree explicitly.");
    }
    auto previous = parent_entity(runtime(), object);
    if (!previous) return std::unexpected(previous.error());
    if (*previous == parent && index == append) return Commit{};
    ObjectId root_before{};
    if (parent == ObjectId{} && data->world) {
        std::vector<ObjectId> roots;
        std::size_t old_index{};
        for (const auto& member : state.members) {
            auto owner = parent_entity(runtime(), member.id());
            if (!owner) return std::unexpected(owner.error());
            if (*owner != ObjectId{}) continue;
            if (member.id() == object) { old_index = roots.size(); continue; }
            roots.push_back(member.id());
        }
        const auto destination = index == append ? roots.size() : index;
        if (destination > roots.size()) return error(ErrorCode::argument_mismatch, "Root insertion index is out of range.");
        if (*previous == parent && destination == old_index) return Commit{};
        if (destination < roots.size()) root_before = roots[destination];
    } else if (parent != ObjectId{} && parent == *previous && index != append) {
        auto siblings = children(runtime(), parent);
        if (!siblings) return std::unexpected(siblings.error());
        auto found = std::ranges::find_if(*siblings, [&](const auto& item) { return item.id() == object; });
        if (found != siblings->end() && std::size_t(found - siblings->begin()) == index) return Commit{};
    }
    auto owned = detail::subtree(runtime(), object);
    if (!owned) return std::unexpected(owned.error());
    for (const auto& root : *owned)
        if (root.id() == parent) return error(ErrorCode::invalid_member, "Composition reparent would create a cycle.");
    std::vector<bool> eligibility;
    eligibility.reserve(owned->size());
    for (const auto& root : *owned) {
        auto active = effective_enabled(runtime(), root.id());
        if (!active) return std::unexpected(active.error());
        eligibility.push_back(*active);
    }
    std::vector<std::pair<ObjectId, Trs>> transforms;
    // Detached keep-local composition can precede its final spatial plans.
    // World publication checks that complete graph; keep-world must evaluate
    // the current and prospective matrices now to compute local compensation.
    if (data->world || rule == ReparentRule::keep_world) {
        detail::SpaceGraph before{runtime()}, candidate{runtime()};
        auto node = candidate.node(object);
        if (!node) return std::unexpected(node.error());
        (*node)->parent = parent;
        auto changes = preserve_space(before, candidate, *owned, rule);
        if (!changes) return std::unexpected(changes.error());
        transforms = std::move(*changes);
    }
    if (auto changed = assign_parent(runtime(), object, parent, index); !changed) return std::unexpected(changed.error());
    if (auto changed = publish_transforms(runtime(), transforms); !changed) return state.finish(true, changed.error());
    if (!data->world) return Commit{true, {}};
    if (parent == ObjectId{}) {
        auto current = std::ranges::find_if(state.members, [&](const Member& item) { return item.id() == object; });
        auto moved = std::move(*current);
        state.members.erase(current);
        auto destination = std::ranges::find_if(state.members, [&](const Member& item) { return item.id() == root_before; });
        state.members.insert(destination, std::move(moved));
    }
    if (auto refreshed = state.refresh(); !refreshed) return state.finish(true, refreshed.error());
    std::optional<Error> failure;
    if (state.running) {
        for (std::size_t i = owned->size(); i-- > 0;) {
            auto active = effective_enabled(runtime(), (*owned)[i].id());
            if (!active) { if (!failure) failure = active.error(); continue; }
            if (eligibility[i] && !*active) {
                state.cancel_tasks((*owned)[i].id());
                auto notified = state.notify(Lifecycle::deactivated, (*owned)[i].id());
                if (!failure) failure = std::move(notified);
            }
        }
        for (std::size_t i = 0; i < owned->size(); ++i) {
            auto active = effective_enabled(runtime(), (*owned)[i].id());
            if (!active) { if (!failure) failure = active.error(); continue; }
            if (!eligibility[i] && *active) {
                auto notified = state.notify(Lifecycle::activated, (*owned)[i].id());
                if (!failure) failure = std::move(notified);
            }
        }
    }
    return state.finish(true, std::move(failure));
}

Result<Commit> World::attach(ObjectId object, std::size_t index, ObjectId parent) {
    const EntityAttachment attachment{object, parent, index};
    return attach_batch(std::span<const EntityAttachment>{&attachment, 1});
}
Result<Commit> World::attach_batch(std::span<const ObjectId> roots, std::size_t index, ObjectId parent) {
    std::vector<EntityAttachment> attachments;
    attachments.reserve(roots.size());
    for (const auto object : roots) attachments.push_back({object, parent, index});
    return attach_batch(std::span<const EntityAttachment>{attachments});
}
namespace {
struct AttachmentGraph {
    detail::SpaceGraph space;
    std::vector<ObjectRoot> incoming, capabilities;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    explicit AttachmentGraph(Runtime& runtime) : space(runtime) {}
};
Result<AttachmentGraph> prepare_attachment_graph(World& world,
    std::span<const EntityAttachment> attachments, std::span<const SpaceReference> spaces) {
    auto& state = Access::state(world);
    auto& runtime = world.runtime();
    AttachmentGraph result{runtime};
    std::unordered_map<ObjectId, std::optional<ObjectId>> origins;
    std::unordered_map<ObjectId, std::size_t> parent_sizes;
    for (const auto& plan : spaces)
        if (!origins.emplace(plan.entity, plan.origin).second)
            return error(ErrorCode::invalid_member, "Attachment has duplicate spatial reference plans.");
    std::unordered_set<ObjectId> ids;
    for (const auto& attachment : attachments) {
        if (attachment.parent == ObjectId{}) {
            const auto index = attachment.index == append ? state.members.size() : attachment.index;
            if (index > state.members.size()) return error(ErrorCode::argument_mismatch, "Entity insertion index is out of range.");
            if (index < state.members.size()) {
                auto parent = parent_entity(runtime, state.members[index].id());
                if (!parent) return std::unexpected(parent.error());
                if (*parent != ObjectId{}) return error(ErrorCode::argument_mismatch, "Root insertion index must be at a root boundary.");
            }
        } else {
            if (auto member = state.member(attachment.parent); !member) return std::unexpected(member.error());
            auto count = parent_sizes.find(attachment.parent);
            if (count == parent_sizes.end()) {
                auto children = world::children(runtime, attachment.parent);
                if (!children) return std::unexpected(children.error());
                count = parent_sizes.emplace(attachment.parent, children->size()).first;
            }
            if (attachment.index != append && attachment.index > count->second)
                return error(ErrorCode::argument_mismatch, "Child insertion index is out of range.");
        }
        auto previous = parent_entity(runtime, attachment.entity);
        if (!previous) return std::unexpected(previous.error());
        if (*previous != ObjectId{}) return error(ErrorCode::invalid_member, "Attach requires detached composition roots.");
        auto tree = detail::subtree(runtime, attachment.entity);
        if (!tree) return std::unexpected(tree.error());
        result.ranges.emplace_back(result.incoming.size(), tree->size());
        for (auto& root : *tree) {
            if (!ids.insert(root.id()).second) return error(ErrorCode::invalid_member, "Attach contains overlapping subtrees.");
            auto data = state.composition_entity(root.id());
            if (!data) return std::unexpected(data.error());
            if (data->world) return error(ErrorCode::invalid_member, "Entity is already a World member.");
            auto group = components(runtime, root.id());
            if (!group) return std::unexpected(group.error());
            if (auto valid = detail::validate_tree(runtime, root.id(), *group); !valid) return std::unexpected(valid.error());
            for (auto& capability : *group) result.capabilities.push_back(std::move(capability));
            auto parent = parent_entity(runtime, root.id());
            if (!parent) return std::unexpected(parent.error());
            std::optional<ObjectId> origin;
            const auto planned = origins.find(root.id());
            if (planned != origins.end()) origin = planned->second;
            else {
                auto existing = space_reference(runtime, root.id());
                if (!existing) return std::unexpected(existing.error());
                origin = *existing;
            }
            auto node = runtime.with_native<Entity>(root.id(), [&](Entity& entity) -> Result<detail::SpaceNode> {
                return detail::SpaceNode{root.id() == attachment.entity ? attachment.parent : *parent,
                    origin, Access::spatial(entity), {entity.position, entity.rotation, entity.scale}, &world};
            });
            if (!node) return std::unexpected(node.error());
            if (planned != origins.end() && !node->spatial)
                return error(ErrorCode::invalid_member, "Spatial reference plan requires an Entity Transform.");
            result.space.nodes.emplace(root.id(), *node);
            result.incoming.push_back(std::move(root));
        }
    }
    for (const auto& [object, origin] : origins) {
        static_cast<void>(origin);
        if (!ids.contains(object)) return error(ErrorCode::invalid_member, "Spatial reference plan targets an Entity outside the incoming forest.");
    }
    // Every prospective parent, origin and membership is now in one graph.
    for (const auto& root : result.incoming)
        if (result.space.nodes.at(root.id()).spatial)
            if (auto valid = result.space.matrix(root.id()); !valid) return std::unexpected(valid.error());
    if (auto sky = state.validate_sky_light(result.capabilities); !sky) return std::unexpected(sky.error());
    return result;
}
}
Result<std::vector<std::pair<ObjectId, math::Matrix4>>> World::attachment_world_matrices(
    std::span<const EntityAttachment> attachments, std::span<const SpaceReference> spaces) const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    auto graph = prepare_attachment_graph(impl_->world, attachments, spaces);
    if (!graph) return std::unexpected(graph.error());
    std::vector<std::pair<ObjectId, math::Matrix4>> matrices;
    for (const auto& root : graph->incoming) {
        const auto at = graph->space.matrices.find(root.id());
        if (at != graph->space.matrices.end()) matrices.emplace_back(root.id(), at->second);
    }
    return matrices;
}
Result<Commit> World::attach_batch(std::span<const EntityAttachment> attachments, std::span<const SpaceReference> spaces) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    if (attachments.empty() && spaces.empty()) return Commit{};
    auto candidate = prepare_attachment_graph(*this, attachments, spaces);
    if (!candidate) return std::unexpected(candidate.error());
    auto& incoming = candidate->incoming;
    auto& capabilities = candidate->capabilities;
    auto& tree_ranges = candidate->ranges;
    struct ParentPlan {
        ObjectId parent;
        std::vector<ObjectRoot> existing;
        std::vector<std::pair<std::size_t, std::size_t>> insertions;
        std::vector<ObjectReference> references;
    };
    std::vector<ParentPlan> parents;
    std::unordered_map<ObjectId, std::size_t> parent_positions;
    std::vector<std::pair<std::size_t, std::size_t>> root_insertions;
    std::unordered_set<ObjectId> ids;
    for (const auto& root : incoming) ids.insert(root.id());
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        const auto& attachment = attachments[i];
        const auto parent = attachment.parent;
        if (parent == ObjectId{}) {
            const auto index = attachment.index == append ? state.members.size() : attachment.index;
            root_insertions.emplace_back(index, i);
        } else {
            auto at = parent_positions.find(parent);
            if (at == parent_positions.end()) {
                auto existing = children(runtime(), parent);
                if (!existing) return std::unexpected(existing.error());
                const auto position = parents.size();
                parents.push_back({parent, std::move(*existing), {}, {}});
                at = parent_positions.emplace(parent, position).first;
            }
            auto& plan = parents[at->second];
            const auto index = attachment.index == append ? plan.existing.size() : attachment.index;
            plan.insertions.emplace_back(index, i);
        }
    }
    state.members.reserve(state.members.size() + incoming.size());
    state.positions.reserve(state.positions.size() + incoming.size());
    state.prepare_activity(incoming.size() + capabilities.size());
    // Build one complete sequence per existing parent. Indices refer to the
    // original sequence, so same-position roots preserve input order.
    const auto by_index = [](const auto& left, const auto& right) { return left.first < right.first; };
    for (auto& plan : parents) {
        std::stable_sort(plan.insertions.begin(), plan.insertions.end(), by_index);
        plan.references.reserve(plan.existing.size() + plan.insertions.size());
        std::size_t insertion{};
        for (std::size_t index = 0; index <= plan.existing.size(); ++index) {
            while (insertion < plan.insertions.size() && plan.insertions[insertion].first == index) {
                const auto attachment_index = plan.insertions[insertion++].second;
                plan.references.push_back(incoming[tree_ranges[attachment_index].first].reference());
            }
            if (index < plan.existing.size()) plan.references.push_back(plan.existing[index].reference());
        }
    }
    // Prepare the whole member directory before committing any parent relation.
    std::vector<Member> publication;
    publication.reserve(state.members.size() + incoming.size());
    const auto add_member = [&](ObjectId id) -> Result<void> {
        auto held = runtime().hold(id);
        if (!held) return std::unexpected(held.error());
        publication.push_back({std::move(*held)});
        return {};
    };
    const auto add_tree = [&](std::size_t attachment_index) -> Result<void> {
        const auto [first, count] = tree_ranges[attachment_index];
        for (std::size_t i = first; i < first + count; ++i) {
            if (auto added = add_member(incoming[i].id()); !added) return added;
        }
        return {};
    };
    std::stable_sort(root_insertions.begin(), root_insertions.end(), by_index);
    std::size_t insertion{};
    for (std::size_t index = 0; index <= state.members.size(); ++index) {
        while (insertion < root_insertions.size() && root_insertions[insertion].first == index) {
            if (auto added = add_tree(root_insertions[insertion++].second); !added) return std::unexpected(added.error());
        }
        if (index < state.members.size()) {
            if (auto added = add_member(state.members[index].id()); !added) return std::unexpected(added.error());
        }
    }
    for (std::size_t i = 0; i < attachments.size(); ++i) if (attachments[i].parent != ObjectId{})
        if (auto added = add_tree(i); !added) return std::unexpected(added.error());
    // Borrow all native membership locations before the reference transaction.
    // The outer borrow keeps those addresses valid through the nonthrowing
    // membership writes and directory swap; no native pointer escapes it.
    auto published = runtime().with_native<Entity>(incoming.front().id(), [&](Entity&) -> Result<void> {
        std::vector<Entity*> entities;
        entities.reserve(incoming.size());
        for (const auto& root : incoming) {
            auto borrowed = runtime().with_native<Entity>(root.id(), [&](Entity& entity) -> Result<void> {
                entities.push_back(&entity); return {};
            });
            if (!borrowed) return borrowed;
        }
        std::vector<ReferenceChange> relation_changes;
        relation_changes.reserve(parents.size() + attachments.size() + spaces.size());
        struct OriginFlags { Entity* entity; bool explicit_origin, world_origin; };
        std::vector<OriginFlags> origin_flags;
        origin_flags.reserve(spaces.size());
        for (const auto& plan : parents) {
            auto sequence = runtime().with_native<Entity>(plan.parent, [&](Entity& entity) {
                return runtime().reference_field(plan.parent, Access::children(entity));
            });
            if (!sequence) return std::unexpected(sequence.error());
            relation_changes.push_back(SequenceSplice{*sequence, 0, plan.existing.size(), plan.references});
        }
        for (const auto& attachment : attachments) {
            auto unchanged = runtime().with_native<Entity>(attachment.entity, [&](Entity& entity) -> Result<bool> {
                return Access::parent(entity).id() == attachment.parent;
            });
            if (!unchanged) return std::unexpected(unchanged.error());
            if (*unchanged) continue;
            auto reference = runtime().null_reference(types().entity, ValueForm::weak_reference);
            if (!reference) return std::unexpected(reference.error());
            if (attachment.parent != ObjectId{}) {
                auto held = runtime().hold(attachment.parent);
                if (!held) return std::unexpected(held.error());
                reference = runtime().observe(held->reference());
                if (!reference) return std::unexpected(reference.error());
            }
            auto slot = runtime().with_native<Entity>(attachment.entity, [&](Entity& entity) {
                return runtime().reference_field(attachment.entity, Access::parent(entity));
            });
            if (!slot) return std::unexpected(slot.error());
            relation_changes.push_back(ReferenceWrite{*slot, *reference});
        }
        for (const auto& plan : spaces) {
            auto reference = runtime().null_reference(types().entity, ValueForm::weak_reference);
            if (!reference) return std::unexpected(reference.error());
            if (plan.origin && *plan.origin != ObjectId{}) {
                auto held = runtime().hold(*plan.origin, types().entity);
                if (!held) return std::unexpected(held.error());
                reference = runtime().observe(held->reference());
                if (!reference) return std::unexpected(reference.error());
            }
            auto slot = runtime().with_native<Entity>(plan.entity, [&](Entity& entity) {
                origin_flags.push_back({&entity, plan.origin.has_value(), plan.origin && *plan.origin == ObjectId{}});
                return runtime().reference_field(plan.entity, Access::origin(entity));
            });
            if (!slot) return std::unexpected(slot.error());
            relation_changes.push_back(ReferenceWrite{*slot, *reference});
        }
        detail::FlagScope transition{state.transitioning};
        if (auto changed = runtime().apply_reference_changes(relation_changes); !changed) return changed;
        for (const auto& flags : origin_flags) {
            Access::explicit_origin(*flags.entity) = flags.explicit_origin;
            Access::world_origin(*flags.entity) = flags.world_origin;
        }
        for (auto* entity : entities) Access::member_world(*entity) = this;
        state.members.swap(publication);
        return {};
    });
    if (!published) return std::unexpected(published.error());
    auto refreshed = state.refresh();
    if (!refreshed) return state.finish(true, refreshed.error());
    std::optional<Error> failure;
    for (const auto& item : state.ordered) if (ids.contains(item.entity)) {
        auto notified = state.notify(Lifecycle::attached, item.object);
        if (!failure) failure = std::move(notified);
    }
    if (!failure && state.running) failure = state.begin();
    if (!failure) if (auto active = state.refresh(); !active) failure = active.error();
    if (!failure && state.running) for (const auto& item : state.ordered) if (item.object == item.entity && ids.contains(item.entity)) {
        auto eligible = effective_enabled(runtime(), item.entity);
        if (!eligible) { failure = eligible.error(); break; }
        if (*eligible) if (auto notified = state.notify(Lifecycle::activated, item.entity)) { failure = std::move(notified); break; }
    }
    return state.finish(true, std::move(failure));
}

Result<EntityRemoval> World::detach(ObjectId object) {
    auto removed = detach_batch(std::span<const ObjectId>{&object, 1});
    if (!removed) return std::unexpected(removed.error());
    return std::move(removed->front());
}
Result<std::vector<EntityRemoval>> World::detach_batch(std::span<const ObjectId> roots) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    if (roots.empty()) return std::vector<EntityRemoval>{};
    struct ParentPlan {
        ObjectId parent;
        std::vector<ObjectRoot> existing;
        std::vector<ObjectReference> references;
    };
    std::vector<ParentPlan> parents;
    std::unordered_map<ObjectId, std::size_t> parent_positions;
    std::vector<EntityRemoval> removals;
    removals.reserve(roots.size());
    std::vector<ObjectRoot> owned;
    std::unordered_set<ObjectId> ids, root_ids;
    for (const auto object : roots) {
        auto at = state.member(object);
        if (!at) return std::unexpected(at.error());
        auto tree = detail::subtree(runtime(), object);
        if (!tree) return std::unexpected(tree.error());
        auto held = runtime().hold(object);
        if (!held) return std::unexpected(held.error());
        auto parent = parent_entity(runtime(), object);
        if (!parent) return std::unexpected(parent.error());
        EntityRemoval removal;
        removal.root = std::move(*held); removal.index = *at; removal.parent = *parent;
        removal.member_count = tree->size(); removal.origin_ = state.lifetime;
        if (*parent != ObjectId{}) {
            if (auto member = state.member(*parent); !member) return std::unexpected(member.error());
            auto plan = parent_positions.find(*parent);
            if (plan == parent_positions.end()) {
                auto existing = children(runtime(), *parent);
                if (!existing) return std::unexpected(existing.error());
                const auto position = parents.size();
                parents.push_back({*parent, std::move(*existing), {}});
                plan = parent_positions.emplace(*parent, position).first;
            }
            const auto& siblings = parents[plan->second].existing;
            auto found = std::ranges::find_if(siblings, [&](const auto& item) { return item.id() == object; });
            if (found == siblings.end()) return error(ErrorCode::invalid_member, "Entity is absent from its parent.");
            removal.sibling_index = std::size_t(found - siblings.begin());
        }
        for (auto& root : *tree) {
            if (!ids.insert(root.id()).second) return error(ErrorCode::invalid_member, "Detach contains overlapping subtrees.");
            auto member = state.composition_entity(root.id());
            if (!member) return std::unexpected(member.error());
            if (member->world != this) return error(ErrorCode::invalid_member, "Subtree has inconsistent World membership.");
            if (auto indexed = state.member(root.id()); !indexed) return std::unexpected(indexed.error());
            owned.push_back(std::move(root));
        }
        root_ids.insert(object);
        removals.push_back(std::move(removal));
    }
    // Retained members must keep valid spatial targets. Departing objects keep
    // their authored weak origins even when Detached space cannot be evaluated;
    // a later attachment validates the complete prospective membership again.
    for (const auto& member : state.members) {
        auto origin = space_reference(runtime(), member.id());
        if (!origin) return std::unexpected(origin.error());
        if (*origin && **origin != ObjectId{} && !ids.contains(member.id()) && ids.contains(**origin))
            return error(ErrorCode::invalid_member, "Rebind external spatial dependencies before detaching the forest.");
    }
    // Prepare complete parent sequences and the surviving member directory
    // before cancelling tasks or entering lifecycle callbacks.
    for (auto& plan : parents) {
        plan.references.reserve(plan.existing.size());
        for (const auto& child : plan.existing) if (!root_ids.contains(child.id()))
            plan.references.push_back(child.reference());
    }
    auto null_parent = runtime().null_reference(types().entity, ValueForm::weak_reference);
    if (!null_parent) return std::unexpected(null_parent.error());
    std::vector<Member> publication;
    publication.reserve(state.members.size() - owned.size());
    for (const auto& member : state.members) if (!ids.contains(member.id())) {
        auto held = runtime().hold(member.id());
        if (!held) return std::unexpected(held.error());
        publication.push_back({std::move(*held)});
    }
    std::vector<Impl::Scheduled> exiting;
    exiting.reserve(state.ordered.size());
    for (const auto& item : state.ordered) if (ids.contains(item.entity)) exiting.push_back(item);
    std::vector<ObjectId> eligible;
    eligible.reserve(owned.size());
    for (const auto& item : exiting) if (item.object == item.entity) {
        auto active = effective_enabled(runtime(), item.entity);
        if (!active) return std::unexpected(active.error());
        if (*active) eligible.push_back(item.entity);
    }
    for (const auto& root : owned) state.cancel_tasks(root.id());
    std::optional<Error> failure;
    for (auto it = exiting.rbegin(); it != exiting.rend(); ++it) {
        auto started = state.begun(*it);
        if (!started) { if (!failure) failure = started.error(); continue; }
        if (!*started) continue;
        auto marked = state.mark_begun(*it, false);
        if (!marked) { if (!failure) failure = marked.error(); continue; }
        auto ended = state.invoke(*it, false);
        if (!failure) failure = std::move(ended);
    }
    if (state.running) for (auto it = eligible.rbegin(); it != eligible.rend(); ++it) {
        auto notified = state.notify(Lifecycle::deactivated, *it);
        if (!failure) failure = std::move(notified);
    }
    // All native locators and membership pointers stay within one outer borrow.
    auto published = runtime().with_native<Entity>(owned.front().id(), [&](Entity&) -> Result<void> {
        std::vector<Entity*> entities;
        entities.reserve(owned.size());
        for (const auto& root : owned) {
            auto borrowed = runtime().with_native<Entity>(root.id(), [&](Entity& entity) -> Result<void> {
                entities.push_back(&entity); return {};
            });
            if (!borrowed) return borrowed;
        }
        std::vector<ReferenceChange> changes;
        changes.reserve(parents.size() + roots.size());
        for (const auto& plan : parents) {
            auto sequence = runtime().with_native<Entity>(plan.parent, [&](Entity& entity) {
                return runtime().reference_field(plan.parent, Access::children(entity));
            });
            if (!sequence) return std::unexpected(sequence.error());
            changes.push_back(SequenceSplice{*sequence, 0, plan.existing.size(), plan.references});
        }
        for (const auto object : roots) {
            auto slot = runtime().with_native<Entity>(object, [&](Entity& entity) {
                return runtime().reference_field(object, Access::parent(entity));
            });
            if (!slot) return std::unexpected(slot.error());
            changes.push_back(ReferenceWrite{*slot, *null_parent});
        }
        detail::FlagScope transition{state.transitioning};
        if (auto changed = runtime().apply_reference_changes(changes); !changed) return changed;
        for (auto* entity : entities) Access::member_world(*entity) = nullptr;
        state.members.swap(publication);
        return {};
    });
    if (!published) {
        if (!failure) failure = published.error();
        auto stopped = state.finish(false, std::move(failure));
        return std::unexpected(std::move(*stopped.notification_error));
    }
    if (auto refreshed = state.refresh(); !refreshed && !failure) failure = refreshed.error();
    for (auto it = exiting.rbegin(); it != exiting.rend(); ++it) {
        auto notified = state.notify(Lifecycle::detached, it->object);
        if (!failure) failure = std::move(notified);
    }
    const auto commit = state.finish(true, std::move(failure));
    for (auto& removal : removals) removal.commit = commit;
    return removals;
}
Result<Commit> World::restore(const EntityRemoval& removal) {
    return restore(std::span<const EntityRemoval>{&removal, 1});
}
Result<Commit> World::restore(std::span<const EntityRemoval> removals) {
    if (auto ready = impl_->admit(); !ready) return std::unexpected(ready.error());
    std::vector<std::size_t> order;
    order.reserve(removals.size());
    for (std::size_t i = 0; i < removals.size(); ++i) {
        auto origin = removals[i].origin_.lock();
        if (!origin || !origin->open || origin != impl_->lifetime)
            return error(ErrorCode::invalid_member, "Removal does not belong to this open World's history.");
        order.push_back(i);
    }
    // Restore against the surviving pre-batch sequences. Earlier removals
    // collapsed their original positions; equal new anchors retain old order.
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return removals[left].index < removals[right].index;
    });
    std::vector<EntityAttachment> attachments;
    attachments.reserve(removals.size());
    for (const auto i : order) {
        const auto& removal = removals[i];
        const auto original = removal.parent == ObjectId{} ? removal.index : removal.sibling_index;
        std::size_t preceding{};
        for (const auto& other : removals) {
            const bool before = removal.parent == ObjectId{} ? other.index < removal.index
                : other.parent == removal.parent && other.sibling_index < removal.sibling_index;
            if (!before) continue;
            const auto count = removal.parent == ObjectId{} ? other.member_count : std::size_t{1};
            if (count > original - preceding) return error(ErrorCode::invalid_member, "Removal positions are inconsistent.");
            preceding += count;
        }
        attachments.push_back({removal.id(), removal.parent, original - preceding});
    }
    return attach_batch(std::span<const EntityAttachment>{attachments});
}
}
