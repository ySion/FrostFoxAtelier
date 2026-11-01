#include "world/detail.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_set>

namespace atelier::world {
using detail::Access;

using detail::error;

Result<std::vector<ObjectRoot>> children(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const Entity>(object, [&](const Entity& entity) -> Result<std::vector<ObjectRoot>> {
        if (Access::transitioning(entity))
            return error(ErrorCode::busy, "Entity is transitioning.");
        if (auto* world = Access::member_world(entity))
            if (auto ready = Access::state(*world).admit(false); !ready)
                return std::unexpected(ready.error());
        auto field = runtime.reference_field(object, Access::children(entity));
        if (!field)
            return std::unexpected(field.error());
        auto count = runtime.sequence_size(*field);
        if (!count)
            return std::unexpected(count.error());
        std::vector<ObjectRoot> result;
        result.reserve(*count);
        for (std::size_t i = 0; i < *count; ++i) {
            auto reference = runtime.sequence_read(*field, i);
            if (!reference)
                return std::unexpected(reference.error());
            auto held = runtime.hold(*reference);
            if (!held)
                return std::unexpected(held.error());
            result.push_back(std::move(*held));
        }
        return result;
    });
}

Result<ObjectId> parent_entity(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const Entity>(object, [&](const Entity& entity) -> Result<ObjectId> {
        const auto parent = Access::parent(entity).id();
        if (parent == ObjectId{})
            return parent;
        // A detached leaf's weak parent does not keep an abandoned graph alive.
        auto valid = runtime.type_of(parent);
        if (!valid && valid.error().code == ErrorCode::invalid_object)
            return ObjectId{};
        if (!valid)
            return std::unexpected(valid.error());
        return parent;
    });
}

Result<bool> has_transform(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const Entity>(
        object, [](const Entity& entity) -> Result<bool> { return Access::spatial(entity); });
}

Result<bool> component_requires_space(Runtime& runtime, ObjectId object) {
    auto component = runtime.native_type<Component>();
    auto spatial = runtime.native_type<SpatialComponent>();
    if (!component)
        return std::unexpected(component.error());
    if (!spatial)
        return std::unexpected(spatial.error());
    if (auto valid = detail::validate_object(runtime, object, *component); !valid)
        return std::unexpected(valid.error());
    return detail::is(runtime, object, *spatial);
}

Result<bool> effective_enabled(Runtime& runtime, ObjectId object) {
    auto entity = detail::entity_data(runtime, object);
    if (!entity)
        return std::unexpected(entity.error());
    if (entity->world && !entity->transitioning) {
        // Members use the World's eligibility, maintained with every enablement or composition change.
        const auto& state = Access::state(*entity->world);
        if (!state.transitioning)
            if (const auto* member = state.state_of(object))
                return member->eligible;
    }
    std::unordered_set<ObjectId> seen;
    for (auto node = object; node != ObjectId{};) {
        if (!seen.insert(node).second)
            return error(ErrorCode::invalid_member, "Composition contains a cycle.");
        auto data = detail::entity_data(runtime, node);
        if (!data)
            return std::unexpected(data.error());
        if (!data->enabled)
            return false;
        auto parent = parent_entity(runtime, node);
        if (!parent)
            return std::unexpected(parent.error());
        node = *parent;
    }
    return true;
}

Result<std::optional<ObjectId>> space_reference(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const Entity>(object, [&](const Entity& entity) -> Result<std::optional<ObjectId>> {
        if (!Access::explicit_origin(entity))
            return std::optional<ObjectId>{};
        if (Access::world_origin(entity))
            return std::optional<ObjectId>{ObjectId{}};
        const auto target = Access::origin(entity).id();
        auto valid = runtime.type_of(target);
        if (!valid)
            return error(ErrorCode::invalid_object, "Entity's explicit spatial origin is no longer valid.");
        return std::optional<ObjectId>{target};
    });
}

namespace detail {
Result<SpaceNode*> SpaceGraph::node(ObjectId object) {
    if (auto found = nodes.find(object); found != nodes.end())
        return &found->second;
    auto parent_id = parent_entity(runtime, object);
    if (!parent_id)
        return std::unexpected(parent_id.error());
    auto origin = space_reference(runtime, object);
    if (!origin)
        return std::unexpected(origin.error());
    auto value = runtime.with_native<const Entity>(object, [&](const Entity& entity) -> Result<SpaceNode> {
        return SpaceNode{*parent_id,
                         *origin,
                         Access::spatial(entity),
                         {entity.position, entity.rotation, entity.scale},
                         Access::member_world(entity)};
    });
    if (!value)
        return std::unexpected(value.error());
    return &nodes.emplace(object, *value).first->second;
}

Result<ObjectId> SpaceGraph::parent(ObjectId object) {
    auto current = node(object);
    if (!current)
        return std::unexpected(current.error());
    auto* world = (*current)->world;
    if ((*current)->origin) {
        const auto origin = *(*current)->origin;
        if (origin == ObjectId{})
            return origin;
        auto target = node(origin);
        if (!target)
            return std::unexpected(target.error());
        if (!(*target)->spatial || (*target)->world != world)
            return error(ErrorCode::invalid_member,
                         "Spatial origin requires a spatial Entity with the same World membership.");
        return origin;
    }
    if ((*current)->parent == ObjectId{})
        return ObjectId{};
    parent_seen.clear();
    parent_seen.add(object);
    for (auto id = (*current)->parent; id != ObjectId{};) {
        if (!parent_seen.add(id))
            return error(ErrorCode::invalid_member, "Composition contains a cycle.");
        auto ancestor = node(id);
        if (!ancestor)
            return std::unexpected(ancestor.error());
        if ((*ancestor)->world != world)
            return error(ErrorCode::invalid_member, "Composition crosses World membership.");
        if ((*ancestor)->spatial)
            return id;
        id = (*ancestor)->parent;
    }
    return ObjectId{};
}

Result<math::Matrix4> SpaceGraph::matrix(ObjectId object) {
    if (object == ObjectId{})
        return math::Matrix4::identity();
    if (auto found = matrices.find(object); found != matrices.end())
        return found->second;
    matrix_path.clear();
    matrix_seen.clear();
    auto result = math::Matrix4::identity();
    for (auto id = object; id != ObjectId{};) {
        if (auto found = matrices.find(id); found != matrices.end()) {
            result = found->second;
            break;
        }
        if (!matrix_seen.add(id))
            return error(ErrorCode::invalid_member, "Spatial references contain a cycle.");
        auto value = node(id);
        if (!value)
            return std::unexpected(value.error());
        if (!(*value)->spatial)
            return error(ErrorCode::invalid_member, "Entity has no Transform.");
        matrix_path.push_back(id);
        auto next = parent(id);
        if (!next)
            return std::unexpected(next.error());
        id = *next;
    }
    for (auto it = matrix_path.rbegin(); it != matrix_path.rend(); ++it) {
        auto local = canonical_transform(nodes.at(*it).local);
        if (!local)
            return std::unexpected(local.error());
        auto composed = multiply(transform_matrix(*local), result);
        if (!composed)
            return std::unexpected(composed.error());
        result = *composed;
        matrices.emplace(*it, result);
    }
    return result;
}

namespace {
bool same_bits(const Trs& left, const Trs& right) noexcept {
    const std::array<float, 10> a{left.position.x, left.position.y, left.position.z, left.rotation.x, left.rotation.y,
                                  left.rotation.z, left.rotation.w, left.scale.x,    left.scale.y,    left.scale.z};
    const std::array<float, 10> b{right.position.x, right.position.y, right.position.z, right.rotation.x,
                                  right.rotation.y, right.rotation.z, right.rotation.w, right.scale.x,
                                  right.scale.y,    right.scale.z};
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::bit_cast<std::uint32_t>(a[i]) != std::bit_cast<std::uint32_t>(b[i]))
            return false;
    return true;
}
}

void SpaceCache::clear() noexcept {
    entries.clear();
    failures.clear();
    relations.nodes.clear();
    relations.matrices.clear();
}

Result<SpaceCache::Entry*> SpaceCache::resolve(ObjectId object) {
    if (auto* found = entries.find(object))
        return found;
    auto node = relations.node(object);
    if (!node)
        return std::unexpected(node.error());
    Entry entry;
    entry.spatial = (*node)->spatial;
    auto parent = relations.parent(object);
    if (!parent)
        return std::unexpected(parent.error());
    entry.parent = *parent;
    return entries.try_emplace(object, entry).value;
}

ObjectId SpaceCache::degenerate_root(ObjectId object) const noexcept {
    auto root = object;
    for (const auto* entry = entries.find(object); entry; entry = entries.find(entry->parent)) {
        const auto& scale = entry->local.scale;
        if (scale.x == 0 || scale.y == 0 || scale.z == 0)
            root = object;
        if (entry->parent == ObjectId{})
            break;
        object = entry->parent;
    }
    return root;
}

Result<ObjectId> SpaceCache::parent(ObjectId object) {
    auto entry = resolve(object);
    if (!entry)
        return std::unexpected(entry.error());
    return (*entry)->parent;
}

Result<math::Matrix4> SpaceCache::matrix(ObjectId object) {
    // Collect the chain up to world space or an ancestor already composed in this pass.
    path.clear();
    ObjectId composed{};
    for (auto id = object;;) {
        failed = id;
        auto entry = resolve(id);
        if (!entry)
            return std::unexpected(entry.error());
        if (!(*entry)->spatial)
            return error(ErrorCode::invalid_member, "Entity has no Transform.");
        if ((*entry)->checked == pass && (*entry)->composed) {
            composed = id;
            break;
        }
        if ((*entry)->failed == pass) {
            failed = (*entry)->cause;
            for (const auto below : path) {
                auto* walked = entries.find(below);
                walked->failed = pass;
                walked->cause = failed;
            }
            return std::unexpected(*failures.find(failed));
        }
        path.push_back(id);
        if ((*entry)->parent == ObjectId{})
            break;
        id = (*entry)->parent;
    }
    auto parent_world = math::Matrix4::identity();
    std::uint64_t parent_version{};
    bool parent_flat{};
    if (composed != ObjectId{}) {
        const auto* entry = entries.find(composed);
        parent_world = entry->world;
        parent_version = entry->version;
        parent_flat = entry->flat;
    }
    // A failure is recorded on the failing node and every visited node below it,
    // so later queries under it in this pass stop there instead of re-walking.
    const auto fail = [&](auto at, Error failure) -> Result<math::Matrix4> {
        failures.insert_or_assign(*at, failure);
        for (const auto cause = *at; at != path.rend(); ++at) {
            auto* entry = entries.find(*at);
            entry->failed = pass;
            entry->cause = cause;
        }
        return std::unexpected(std::move(failure));
    };
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        failed = *it;
        auto local = relations.runtime.with_native<const Entity>(*it, [](const Entity& entity) -> Result<Trs> {
            return Trs{entity.position, entity.rotation, entity.scale};
        });
        if (!local)
            return fail(it, std::move(local.error()));
        auto* entry = entries.find(*it);
        if (!entry->composed || entry->parent_version != parent_version || !same_bits(*local, entry->local)) {
            entry->composed = false;
            auto canonical = canonical_transform(*local);
            if (!canonical)
                return fail(it, std::move(canonical.error()));
            auto world = multiply(transform_matrix(*canonical), parent_world);
            if (!world)
                return fail(it, std::move(world.error()));
            entry->local = *local;
            entry->world = *world;
            entry->parent_version = parent_version;
            entry->flat = parent_flat || local->scale.x == 0 || local->scale.y == 0 || local->scale.z == 0;
            ++entry->version;
            entry->composed = true;
        }
        entry->checked = pass;
        parent_world = entry->world;
        parent_version = entry->version;
        parent_flat = entry->flat;
    }
    failed = {};
    return entries.find(object)->world;
}

Result<std::vector<ObjectRoot>> subtree(Runtime& runtime, ObjectId object) {
    std::vector<ObjectRoot> result;
    std::vector<ObjectId> pending{object};
    std::unordered_set<ObjectId> seen;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (!seen.insert(id).second)
            return error(ErrorCode::invalid_member, "Composition contains a cycle or duplicate child.");
        auto type = runtime.native_type<Entity>();
        if (!type)
            return std::unexpected(type.error());
        auto held = runtime.hold(id, *type);
        if (!held)
            return std::unexpected(held.error());
        auto owned = children(runtime, id);
        if (!owned)
            return std::unexpected(owned.error());
        for (auto it = owned->rbegin(); it != owned->rend(); ++it) {
            auto parent = parent_entity(runtime, it->id());
            if (!parent)
                return std::unexpected(parent.error());
            if (*parent != id)
                return error(ErrorCode::invalid_member, "Composition child has a different parent.");
            pending.push_back(it->id());
        }
        result.push_back(std::move(*held));
    }
    return result;
}

Result<void> requires_space(Runtime& runtime, ObjectId entity, std::span<const ObjectRoot> group) {
    auto spatial = has_transform(runtime, entity);
    if (!spatial)
        return std::unexpected(spatial.error());
    if (*spatial)
        return {};
    for (const auto& item : group) {
        auto needs = component_requires_space(runtime, item.id());
        if (!needs)
            return std::unexpected(needs.error());
        if (*needs)
            return error(ErrorCode::invalid_member,
                         "Spatial capability requires its owner Entity to have a Transform.");
    }
    return {};
}

Result<void> validate_tree(Runtime& runtime, ObjectId entity, std::span<const ObjectRoot> group) {
    std::unordered_set<ObjectId> seen;
    for (const auto& item : group) {
        if (!seen.insert(item.id()).second)
            return error(ErrorCode::invalid_member, "Component appears twice in its Entity.");
        auto data = component_data(runtime, item.id());
        if (!data)
            return std::unexpected(data.error());
        if (data->owner != entity)
            return error(ErrorCode::invalid_member, "Component owner differs from its composition.");
    }
    if (auto valid = requires_space(runtime, entity, group); !valid)
        return valid;
    auto spatial = has_transform(runtime, entity);
    if (!spatial)
        return std::unexpected(spatial.error());
    if (*spatial) {
        auto value = read_transform(runtime, entity);
        if (!value)
            return std::unexpected(value.error());
        if (auto canonical = canonical_transform(*value); !canonical)
            return std::unexpected(canonical.error());
    }
    return {};
}

Result<void> clear_component(Runtime& runtime, ObjectId entity, ObjectId object, std::size_t index) {
    return runtime.with_native<Entity>(entity, [&](Entity&) -> Result<void> {
        auto type = runtime.native_type<Entity>();
        if (!type)
            return std::unexpected(type.error());
        auto null_owner = runtime.null_reference(*type);
        if (!null_owner)
            return std::unexpected(null_owner.error());
        auto sequence = runtime.with_native<Entity>(
            entity, [&](Entity& value) { return runtime.reference_field(entity, Access::sequence(value)); });
        auto owner = runtime.with_native<Component>(
            object, [&](Component& value) { return runtime.reference_field(object, Access::owner(value)); });
        if (!sequence)
            return std::unexpected(sequence.error());
        if (!owner)
            return std::unexpected(owner.error());
        const std::array<ReferenceChange, 2> changes{SequenceSplice{*sequence, index, 1, {}},
                                                     ReferenceWrite{*owner, *null_owner}};
        return runtime.apply_reference_changes(changes);
    });
}
}

namespace {
// The member cache serves only an open World outside its own transitions.
detail::SpaceCache* member_space(Runtime& runtime, ObjectId object) {
    auto data = detail::entity_data(runtime, object);
    if (!data || !data->world || data->transitioning)
        return nullptr;
    auto& state = Access::state(*data->world);
    if (!state.admit(false) || !state.positions.contains(object))
        return nullptr;
    return &state.space;
}
}

Result<ObjectId> spatial_parent(Runtime& runtime, ObjectId object) {
    if (auto* space = member_space(runtime, object))
        return space->parent(object);
    detail::SpaceGraph graph{runtime};
    return graph.parent(object);
}

Result<math::Matrix4> world_matrix(Runtime& runtime, ObjectId object) {
    if (object == ObjectId{})
        return error(ErrorCode::invalid_object, "World matrix requires an Entity identity.");
    if (auto* space = member_space(runtime, object)) {
        space->begin_pass();
        return space->matrix(object);
    }
    detail::SpaceGraph graph{runtime};
    return graph.matrix(object);
}

namespace {
Result<void> assign_parent(Runtime& runtime, ObjectId object, ObjectId parent, std::size_t index) {
    return runtime.with_native<Entity>(object, [&](Entity&) -> Result<void> {
        auto old_parent = parent_entity(runtime, object);
        if (!old_parent)
            return std::unexpected(old_parent.error());
        std::vector<ObjectRoot> old_roots;
        std::vector<ObjectRoot> new_roots;
        std::vector<ObjectReference> old_references;
        std::vector<ObjectReference> new_references;
        std::vector<ReferenceChange> changes;
        const auto sequence = [&](ObjectId id) {
            return runtime.with_native<Entity>(
                id, [&](Entity& value) { return runtime.reference_field(id, Access::children(value)); });
        };
        if (*old_parent != ObjectId{}) {
            auto previous = children(runtime, *old_parent);
            if (!previous)
                return std::unexpected(previous.error());
            old_roots = std::move(*previous);
            bool found = false;
            for (const auto& child : old_roots) {
                if (child.id() == object) {
                    found = true;
                    continue;
                }
                old_references.push_back(child.reference());
            }
            if (!found)
                return error(ErrorCode::invalid_member, "Entity is absent from its parent's composition.");
        }
        if (parent != ObjectId{}) {
            auto next = children(runtime, parent);
            if (!next)
                return std::unexpected(next.error());
            new_roots = std::move(*next);
            for (const auto& child : new_roots)
                if (child.id() != object)
                    new_references.push_back(child.reference());
            if (index == append)
                index = new_references.size();
            if (index > new_references.size())
                return error(ErrorCode::argument_mismatch, "Child insertion index is out of range.");
            auto held = runtime.hold(object);
            if (!held)
                return std::unexpected(held.error());
            new_references.insert(new_references.begin() + std::ptrdiff_t(index), held->reference());
            auto field = sequence(parent);
            if (!field)
                return std::unexpected(field.error());
            changes.push_back(SequenceSplice{*field, 0, new_roots.size(), new_references});
        }
        if (*old_parent != ObjectId{} && *old_parent != parent) {
            auto field = sequence(*old_parent);
            if (!field)
                return std::unexpected(field.error());
            changes.push_back(SequenceSplice{*field, 0, old_roots.size(), old_references});
        }
        auto type = runtime.native_type<Entity>();
        if (!type)
            return std::unexpected(type.error());
        auto reference = runtime.null_reference(*type, ValueForm::weak_reference);
        if (!reference)
            return std::unexpected(reference.error());
        if (parent != ObjectId{}) {
            auto held = runtime.hold(parent);
            if (!held)
                return std::unexpected(held.error());
            reference = runtime.observe(held->reference());
            if (!reference)
                return std::unexpected(reference.error());
        }
        auto field = runtime.with_native<Entity>(
            object, [&](Entity& value) { return runtime.reference_field(object, Access::parent(value)); });
        if (!field)
            return std::unexpected(field.error());
        changes.push_back(ReferenceWrite{*field, *reference});
        return runtime.apply_reference_changes(changes);
    });
}

Result<void> publish_transforms(Runtime& runtime, const std::vector<std::pair<ObjectId, Trs>>& values) {
    for (const auto& [id, value] : values) {
        // An unchanged pose keeps the Entity's write stamp, so change consumers skip it.
        auto same = runtime.with_native<const Entity>(id, [&](const Entity& entity) -> Result<bool> {
            const Trs current{entity.position, entity.rotation, entity.scale};
            return std::memcmp(&current, &value, sizeof current) == 0;
        });
        if (!same)
            return std::unexpected(same.error());
        if (*same)
            continue;
        auto written = runtime.with_native<Entity>(id, [&](Entity& entity) -> Result<void> {
            entity.position = value.position;
            entity.rotation = value.rotation;
            entity.scale = value.scale;
            return {};
        });
        if (!written)
            return written;
    }
    return {};
}

// Only frontiers whose resolved origin changes need local compensation. Deeper
// descendants keep their local values, including below nonspatial parents.
Result<std::vector<std::pair<ObjectId, Trs>>> preserve_space(detail::SpaceGraph& before, detail::SpaceGraph& candidate,
                                                             std::span<const ObjectRoot> affected, ReparentRule rule) {
    std::vector<std::pair<ObjectId, Trs>> changes;
    for (const auto& root : affected) {
        auto value = candidate.node(root.id());
        if (!value)
            return std::unexpected(value.error());
        if (!(*value)->spatial)
            continue;
        if (rule == ReparentRule::keep_world) {
            auto old_parent = before.parent(root.id());
            auto next_parent = candidate.parent(root.id());
            if (!old_parent)
                return std::unexpected(old_parent.error());
            if (!next_parent)
                return std::unexpected(next_parent.error());
            if (*old_parent != *next_parent) {
                auto original = before.matrix(root.id());
                auto origin = candidate.matrix(*next_parent);
                if (!original)
                    return std::unexpected(original.error());
                if (!origin)
                    return std::unexpected(origin.error());
                auto local = relative_transform(*original, *origin, (*value)->local.scale);
                if (!local)
                    return std::unexpected(local.error());
                (*value)->local = *local;
                candidate.invalidate();
                changes.emplace_back(root.id(), *local);
            }
        }
        auto valid = candidate.matrix(root.id());
        if (!valid)
            return std::unexpected(valid.error());
    }
    return changes;
}
}

namespace detail {
Result<void> unlink_parent(Runtime& runtime, ObjectId object) {
    return assign_parent(runtime, object, {}, append);
}
}

Result<void> World::set_transform(ObjectId object, Trs value) {
    if (auto ready = impl_->admit_edit(object); !ready)
        return ready;
    auto data = impl_->composition_entity(object);
    if (!data)
        return std::unexpected(data.error());
    auto canonical = canonical_transform(value);
    if (!canonical)
        return std::unexpected(canonical.error());
    detail::SpaceGraph graph{runtime()};
    auto node = graph.node(object);
    if (!node)
        return std::unexpected(node.error());
    (*node)->spatial = true;
    (*node)->local = *canonical;
    if (auto valid = graph.matrix(object); !valid)
        return std::unexpected(valid.error());
    Trs previous;
    bool was_spatial{};
    const auto assign = [&](const Trs& trs, bool spatial) {
        return runtime().with_native<Entity>(object, [&](Entity& entity) -> Result<void> {
            previous = {entity.position, entity.rotation, entity.scale};
            was_spatial = Access::spatial(entity);
            entity.position = trs.position;
            entity.rotation = trs.rotation;
            entity.scale = trs.scale;
            // Gaining or losing space changes resolved parents below it; TRS values alone are revalidated per pass.
            if (Access::spatial(entity) != spatial && data->world)
                impl_->changed();
            Access::spatial(entity) = spatial;
            return {};
        });
    };
    // Spatial chains of members stay within members, so a detached pose moves no simulated body.
    if (auto written = assign(*canonical, true); !written || !impl_->running || !data->world)
        return written;
    const auto before = previous;
    const bool spatial = was_spatial;
    auto relocated = impl_->relocate(object);
    if (!relocated)
        static_cast<void>(assign(before, spatial));
    return relocated;
}

Result<void> World::remove_transform(ObjectId object) {
    if (auto ready = impl_->admit_edit(object); !ready)
        return ready;
    auto data = impl_->composition_entity(object);
    if (!data)
        return std::unexpected(data.error());
    auto spatial = has_transform(runtime(), object);
    if (!spatial)
        return std::unexpected(spatial.error());
    if (!*spatial)
        return {};
    auto group = components(runtime(), object);
    if (!group)
        return std::unexpected(group.error());
    for (const auto& root : *group) {
        auto needs = component_requires_space(runtime(), root.id());
        if (!needs)
            return std::unexpected(needs.error());
        if (*needs)
            return error(ErrorCode::invalid_member,
                         "Remove spatial capabilities before removing the Entity Transform.");
    }
    auto root = object;
    if (!data->world) {
        std::unordered_set<ObjectId> seen{object};
        for (;;) {
            auto parent = parent_entity(runtime(), root);
            if (!parent)
                return std::unexpected(parent.error());
            if (*parent == ObjectId{})
                break;
            if (!seen.insert(*parent).second)
                return error(ErrorCode::invalid_member, "Composition contains a cycle.");
            root = *parent;
        }
    }
    auto all = detail::subtree(runtime(), root);
    if (!all)
        return std::unexpected(all.error());
    std::vector<ObjectId> candidates;
    for (const auto& item : *all)
        candidates.push_back(item.id());
    if (data->world)
        for (const auto& member : impl_->members)
            candidates.push_back(member.id());
    detail::SpaceGraph candidate{runtime()};
    auto parent = parent_entity(runtime(), object);
    if (!parent)
        return std::unexpected(parent.error());
    // Removing space also repairs an expired former origin. Its old matrix is
    // irrelevant; surviving default inheritance is evaluated in the final graph.
    candidate.nodes.emplace(object, detail::SpaceNode{*parent, {}, false, {}, data->world});
    for (const auto id : candidates) {
        auto current = candidate.node(id);
        if (!current)
            return std::unexpected(current.error());
        if (!(*current)->spatial)
            continue;
        if (auto valid = candidate.matrix(id); !valid)
            return std::unexpected(valid.error());
    }
    auto reference = runtime().null_reference(types().entity, ValueForm::weak_reference);
    if (!reference)
        return std::unexpected(reference.error());
    return runtime().with_native<Entity>(object, [&](Entity& entity) -> Result<void> {
        auto field = runtime().reference_field(object, Access::origin(entity));
        if (!field)
            return std::unexpected(field.error());
        const ReferenceChange change = ReferenceWrite{*field, *reference};
        if (auto cleared = runtime().apply_reference_changes({&change, 1}); !cleared)
            return cleared;
        Access::explicit_origin(entity) = false;
        Access::world_origin(entity) = false;
        Access::spatial(entity) = false;
        if (auto* member = data->world ? impl_->state_of(object) : nullptr)
            member->origin = {};
        if (data->world)
            impl_->changed();
        return {};
    });
}

Result<Commit> World::set_space_reference(ObjectId object, std::optional<ObjectId> origin, ReparentRule rule) {
    if (auto ready = impl_->admit(); !ready)
        return std::unexpected(ready.error());
    if (rule != ReparentRule::keep_local && rule != ReparentRule::keep_world)
        return error(ErrorCode::argument_mismatch, "Unknown reparent rule.");
    auto data = impl_->composition_entity(object);
    if (!data)
        return std::unexpected(data.error());
    auto current = space_reference(runtime(), object);
    if (!current && (rule != ReparentRule::keep_local || current.error().code != ErrorCode::invalid_object))
        return std::unexpected(current.error());
    auto spatial = has_transform(runtime(), object);
    if (!spatial)
        return std::unexpected(spatial.error());
    if (!*spatial)
        return error(ErrorCode::invalid_member, "Spatial reference requires an Entity Transform.");
    if (current && *current == origin)
        return Commit{};
    detail::SpaceGraph before{runtime()};
    detail::SpaceGraph candidate{runtime()};
    auto parent = parent_entity(runtime(), object);
    if (!parent)
        return std::unexpected(parent.error());
    // Rebinding can repair an expired weak origin without evaluating its old space.
    auto node = runtime().with_native<const Entity>(object, [&](const Entity& entity) -> Result<detail::SpaceNode> {
        return detail::SpaceNode{*parent,
                                 origin,
                                 Access::spatial(entity),
                                 {entity.position, entity.rotation, entity.scale},
                                 Access::member_world(entity)};
    });
    if (!node)
        return std::unexpected(node.error());
    candidate.nodes.emplace(object, *node);
    auto group = detail::subtree(runtime(), object);
    if (!group)
        return std::unexpected(group.error());
    auto changes = preserve_space(before, candidate, *group, rule);
    if (!changes)
        return std::unexpected(changes.error());
    auto reference = runtime().null_reference(types().entity, ValueForm::weak_reference);
    if (!reference)
        return std::unexpected(reference.error());
    if (origin && *origin != ObjectId{}) {
        auto held = runtime().hold(*origin, types().entity);
        if (!held)
            return std::unexpected(held.error());
        reference = runtime().observe(held->reference());
        if (!reference)
            return std::unexpected(reference.error());
    }
    auto published = runtime().with_native<Entity>(object, [&](Entity& entity) -> Result<void> {
        auto field = runtime().reference_field(object, Access::origin(entity));
        if (!field)
            return std::unexpected(field.error());
        const ReferenceChange change = ReferenceWrite{*field, *reference};
        if (auto changed = runtime().apply_reference_changes({&change, 1}); !changed)
            return changed;
        Access::explicit_origin(entity) = origin.has_value();
        Access::world_origin(entity) = origin && *origin == ObjectId{};
        if (auto* member = data->world ? impl_->state_of(object) : nullptr)
            member->origin = origin.value_or(ObjectId{});
        if (data->world)
            impl_->changed();
        return {};
    });
    if (!published)
        return std::unexpected(published.error());
    if (auto applied = publish_transforms(runtime(), *changes); !applied)
        return impl_->finish(true, applied.error());
    return Commit{true, {}};
}

Result<void> World::apply_simulation_transform(ObjectId entity, math::Vector3 position, math::Quaternion rotation) {
    const SimulationPose pose{entity, position, rotation};
    return apply_simulation_transforms({&pose, 1});
}

Result<void> World::apply_simulation_transforms(std::span<const SimulationPose> poses,
                                                std::vector<std::pair<ObjectId, Trs>>* written) {
    return impl_->apply_poses(poses, DiagnosticPhase::physics, written);
}

Result<void> World::Impl::apply_poses(std::span<const SimulationPose> poses, DiagnosticPhase phase,
                                      std::vector<std::pair<ObjectId, Trs>>* written) {
    if (auto ready = admit(); !ready)
        return ready;
    if (!running)
        return error(ErrorCode::busy, "Simulation writes require a running World.");
    // A rejected pose is reported and excluded; conversion then restarts so no
    // retained pose is expressed relative to a rejected parent's target.
    std::vector<bool> rejected(poses.size());
    const auto reject = [&](std::size_t i, const Error& failure) {
        rejected[i] = true;
        return contain(failure, DiagnosticKind::command_rejected, phase, poses[i].entity, poses[i].entity, {},
                       Severity::warning);
    };
    for (;;) {
        space.begin_pass();
        auto& desired = pose_targets;
        desired.clear();
        for (std::size_t i = 0; i < poses.size(); ++i) {
            if (rejected[i])
                continue;
            const auto& pose = poses[i];
            auto value = [&]() -> Result<Trs> {
                if (auto at = member(pose.entity); !at)
                    return std::unexpected(at.error());
                if (desired.contains(pose.entity))
                    return error(ErrorCode::invalid_member, "An Entity has multiple simulation pose writers.");
                auto matrix = space.matrix(pose.entity);
                if (!matrix)
                    return std::unexpected(matrix.error());
                auto original = relative_transform(*matrix, math::Matrix4::identity());
                if (!original)
                    return std::unexpected(original.error());
                return canonical_transform({pose.position, pose.rotation, original->scale});
            }();
            if (!value) {
                if (!reject(i, value.error()))
                    return std::unexpected(value.error());
                continue;
            }
            desired.try_emplace(pose.entity, *value);
        }
        // Only a pose below another pose of this batch needs that one's target as
        // its origin; the others use the current parent matrix of this pass.
        std::optional<detail::SpaceGraph> candidate;
        auto& locals = pose_locals;
        locals.clear();
        locals.reserve(poses.size());
        bool restart = false;
        for (std::size_t i = 0; i < poses.size() && !restart; ++i) {
            if (rejected[i])
                continue;
            const auto& pose = poses[i];
            auto local = [&]() -> Result<Trs> {
                auto parent = space.parent(pose.entity);
                if (!parent)
                    return std::unexpected(parent.error());
                // matrix() composed this chain in the first loop, so the walk ends.
                bool below_target = false;
                for (auto node = *parent; node != ObjectId{} && !below_target;) {
                    below_target = desired.contains(node);
                    auto next = space.parent(node);
                    if (!next)
                        return std::unexpected(next.error());
                    node = *next;
                }
                if (below_target && !candidate) {
                    // Desired world matrices make conversion independent of hash/container order.
                    candidate.emplace(runtime);
                    for (const auto [id, value] : desired)
                        candidate->matrices.emplace(id, transform_matrix(value));
                }
                auto origin = *parent == ObjectId{} ? Result<math::Matrix4>{math::Matrix4::identity()}
                              : below_target        ? candidate->matrix(*parent)
                                                    : space.matrix(*parent);
                if (!origin)
                    return std::unexpected(origin.error());
                const auto& authored = space.entry(pose.entity)->local;
                auto value = relative_transform(transform_matrix(*desired.find(pose.entity)), *origin, authored.scale);
                if (!value)
                    return std::unexpected(value.error());
                for (std::size_t axis = 0; axis < 3; ++axis)
                    if (std::abs(value->scale[axis] - authored.scale[axis]) >
                        1e-5f * std::max(1.f, std::abs(authored.scale[axis])))
                        return error(ErrorCode::argument_mismatch,
                                     "Simulation pose would change authored local scale.");
                value->scale = authored.scale;
                return value;
            }();
            if (!local) {
                if (!reject(i, local.error()))
                    return std::unexpected(local.error());
                restart = true;
                continue;
            }
            locals.emplace_back(pose.entity, *local);
        }
        if (!restart) {
            if (auto published = publish_transforms(runtime, locals); !published)
                return published;
            if (written)
                written->swap(locals);
            return {};
        }
    }
}

Result<Commit> World::reparent(ObjectId object, ObjectId parent, ReparentRule rule, std::size_t index) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return std::unexpected(ready.error());
    if (rule != ReparentRule::keep_local && rule != ReparentRule::keep_world)
        return error(ErrorCode::argument_mismatch, "Unknown reparent rule.");
    auto data = state.composition_entity(object);
    if (!data)
        return std::unexpected(data.error());
    if (parent != ObjectId{}) {
        auto destination = state.composition_entity(parent);
        if (!destination)
            return std::unexpected(destination.error());
        if (destination->world != data->world)
            return error(ErrorCode::invalid_member,
                         "Reparent requires the same World membership; attach a detached subtree explicitly.");
    }
    auto previous = parent_entity(runtime(), object);
    if (!previous)
        return std::unexpected(previous.error());
    if (*previous == parent && index == append)
        return Commit{};
    ObjectId root_before{};
    if (parent == ObjectId{} && data->world) {
        std::vector<ObjectId> roots;
        std::size_t old_index{};
        for (std::size_t i = 0; i < state.members.size(); i += state.states[i].size) {
            const auto id = state.members[i].id();
            if (id == object) {
                old_index = roots.size();
                continue;
            }
            roots.push_back(id);
        }
        const auto destination = index == append ? roots.size() : index;
        if (destination > roots.size())
            return error(ErrorCode::argument_mismatch, "Root insertion index is out of range.");
        if (*previous == parent && destination == old_index)
            return Commit{};
        if (destination < roots.size())
            root_before = roots[destination];
    } else if (parent != ObjectId{} && parent == *previous && index != append) {
        auto siblings = children(runtime(), parent);
        if (!siblings)
            return std::unexpected(siblings.error());
        auto found = std::ranges::find_if(*siblings, [&](const auto& item) { return item.id() == object; });
        if (found != siblings->end() && std::size_t(found - siblings->begin()) == index)
            return Commit{};
    }
    auto owned = detail::subtree(runtime(), object);
    if (!owned)
        return std::unexpected(owned.error());
    for (const auto& root : *owned)
        if (root.id() == parent)
            return error(ErrorCode::invalid_member, "Composition reparent would create a cycle.");
    std::vector<bool> eligibility;
    eligibility.reserve(owned->size());
    for (const auto& root : *owned) {
        auto active = effective_enabled(runtime(), root.id());
        if (!active)
            return std::unexpected(active.error());
        eligibility.push_back(*active);
    }
    std::vector<std::pair<ObjectId, Trs>> transforms;
    // Detached keep-local composition can precede its final spatial plans.
    // World publication checks that complete graph; keep-world must evaluate
    // the current and prospective matrices now to compute local compensation.
    if (data->world || rule == ReparentRule::keep_world) {
        detail::SpaceGraph before{runtime()};
        detail::SpaceGraph candidate{runtime()};
        auto node = candidate.node(object);
        if (!node)
            return std::unexpected(node.error());
        (*node)->parent = parent;
        auto changes = preserve_space(before, candidate, *owned, rule);
        if (!changes)
            return std::unexpected(changes.error());
        transforms = std::move(*changes);
    }
    // The member range moves before the next sibling, or to its new parent's subtree end.
    ObjectId next_sibling{};
    if (data->world && parent != ObjectId{}) {
        auto siblings = children(runtime(), parent);
        if (!siblings)
            return std::unexpected(siblings.error());
        std::vector<ObjectId> others;
        others.reserve(siblings->size());
        for (const auto& child : *siblings)
            if (child.id() != object)
                others.push_back(child.id());
        const auto at = index == append ? others.size() : index;
        if (at < others.size())
            next_sibling = others[at];
    }
    if (auto changed = assign_parent(runtime(), object, parent, index); !changed)
        return std::unexpected(changed.error());
    if (data->world) {
        detail::FlagScope transition{state.transitioning};
        const auto first = *state.positions.find(object);
        const auto count = state.states[first].size;
        const auto previous_parent = state.states[first].parent;
        auto target = state.members.size();
        if (next_sibling != ObjectId{})
            target = *state.positions.find(next_sibling);
        else if (parent != ObjectId{}) {
            const auto at = *state.positions.find(parent);
            target = at + state.states[at].size;
        } else if (root_before != ObjectId{})
            target = *state.positions.find(root_before);
        const auto move = [&](auto& values) {
            const auto begin = values.begin();
            if (target > first + count)
                std::rotate(begin + std::ptrdiff_t(first), begin + std::ptrdiff_t(first + count),
                            begin + std::ptrdiff_t(target));
            else if (target < first)
                std::rotate(begin + std::ptrdiff_t(target), begin + std::ptrdiff_t(first),
                            begin + std::ptrdiff_t(first + count));
        };
        move(state.members);
        move(state.states);
        state.reindex(std::min(first, target), std::max(first + count, target));
        state.resize_ancestors(previous_parent, -std::ptrdiff_t(count));
        state.resize_ancestors(parent, std::ptrdiff_t(count));
        const auto moved = *state.positions.find(object);
        state.states[moved].parent = parent;
        state.refresh_eligibility(moved, count);
        state.changed();
    }
    if (auto changed = publish_transforms(runtime(), transforms); !changed)
        return state.finish(true, changed.error());
    if (!data->world)
        return Commit{true, {}};
    std::optional<Error> failure;
    if (state.running) {
        for (std::size_t i = owned->size(); i-- > 0;) {
            auto active = effective_enabled(runtime(), (*owned)[i].id());
            if (!active) {
                if (!failure)
                    failure = active.error();
                continue;
            }
            if (eligibility[i] && !*active) {
                state.cancel_tasks((*owned)[i].id());
                auto notified = state.notify(Lifecycle::deactivated, (*owned)[i].id());
                if (!failure)
                    failure = std::move(notified);
            }
        }
        for (std::size_t i = 0; i < owned->size(); ++i) {
            auto active = effective_enabled(runtime(), (*owned)[i].id());
            if (!active) {
                if (!failure)
                    failure = active.error();
                continue;
            }
            if (!eligibility[i] && *active) {
                auto notified = state.notify(Lifecycle::activated, (*owned)[i].id());
                if (!failure)
                    failure = std::move(notified);
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
    for (const auto object : roots)
        attachments.push_back({object, parent, index});
    return attach_batch(std::span<const EntityAttachment>{attachments});
}

namespace {
struct AttachmentGraph {
    detail::SpaceGraph space;

    std::vector<ObjectRoot> incoming;

    std::vector<ObjectRoot> capabilities;

    std::vector<std::pair<std::size_t, std::size_t>> ranges;

    explicit AttachmentGraph(Runtime& runtime) : space(runtime) {}
};

Result<AttachmentGraph> prepare_attachment_graph(World& world, std::span<const EntityAttachment> attachments,
                                                 std::span<const SpaceReference> spaces) {
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
            if (index > state.members.size())
                return error(ErrorCode::argument_mismatch, "Entity insertion index is out of range.");
            if (index < state.members.size() && state.states[index].parent != ObjectId{})
                return error(ErrorCode::argument_mismatch, "Root insertion index must be at a root boundary.");
        } else {
            if (auto member = state.member(attachment.parent); !member)
                return std::unexpected(member.error());
            auto count = parent_sizes.find(attachment.parent);
            if (count == parent_sizes.end()) {
                auto children = world::children(runtime, attachment.parent);
                if (!children)
                    return std::unexpected(children.error());
                count = parent_sizes.emplace(attachment.parent, children->size()).first;
            }
            if (attachment.index != append && attachment.index > count->second)
                return error(ErrorCode::argument_mismatch, "Child insertion index is out of range.");
        }
        auto previous = parent_entity(runtime, attachment.entity);
        if (!previous)
            return std::unexpected(previous.error());
        if (*previous != ObjectId{})
            return error(ErrorCode::invalid_member, "Attach requires detached composition roots.");
        auto tree = detail::subtree(runtime, attachment.entity);
        if (!tree)
            return std::unexpected(tree.error());
        result.ranges.emplace_back(result.incoming.size(), tree->size());
        for (auto& root : *tree) {
            if (!ids.insert(root.id()).second)
                return error(ErrorCode::invalid_member, "Attach contains overlapping subtrees.");
            auto data = state.composition_entity(root.id());
            if (!data)
                return std::unexpected(data.error());
            if (data->world)
                return error(ErrorCode::invalid_member, "Entity is already a World member.");
            auto group = components(runtime, root.id());
            if (!group)
                return std::unexpected(group.error());
            if (auto valid = detail::validate_tree(runtime, root.id(), *group); !valid)
                return std::unexpected(valid.error());
            for (auto& capability : *group)
                result.capabilities.push_back(std::move(capability));
            auto parent = parent_entity(runtime, root.id());
            if (!parent)
                return std::unexpected(parent.error());
            std::optional<ObjectId> origin;
            const auto planned = origins.find(root.id());
            if (planned != origins.end())
                origin = planned->second;
            else {
                auto existing = space_reference(runtime, root.id());
                if (!existing)
                    return std::unexpected(existing.error());
                origin = *existing;
            }
            auto node =
                runtime.with_native<const Entity>(root.id(), [&](const Entity& entity) -> Result<detail::SpaceNode> {
                    return detail::SpaceNode{root.id() == attachment.entity ? attachment.parent : *parent,
                                             origin,
                                             Access::spatial(entity),
                                             {entity.position, entity.rotation, entity.scale},
                                             &world};
                });
            if (!node)
                return std::unexpected(node.error());
            if (planned != origins.end() && !node->spatial)
                return error(ErrorCode::invalid_member, "Spatial reference plan requires an Entity Transform.");
            result.space.nodes.emplace(root.id(), *node);
            result.incoming.push_back(std::move(root));
        }
    }
    for (const auto& [object, origin] : origins) {
        static_cast<void>(origin);
        if (!ids.contains(object))
            return error(ErrorCode::invalid_member,
                         "Spatial reference plan targets an Entity outside the incoming forest.");
    }
    // Every prospective parent, origin and membership is now in one graph.
    for (const auto& root : result.incoming)
        if (result.space.nodes.at(root.id()).spatial)
            if (auto valid = result.space.matrix(root.id()); !valid)
                return std::unexpected(valid.error());
    if (auto admitted = state.check_admissions(result.capabilities); !admitted)
        return std::unexpected(admitted.error());
    return result;
}
}

Result<std::vector<std::pair<ObjectId, math::Matrix4>>>
World::attachment_world_matrices(std::span<const EntityAttachment> attachments,
                                 std::span<const SpaceReference> spaces) const {
    if (auto ready = impl_->admit(false); !ready)
        return std::unexpected(ready.error());
    auto graph = prepare_attachment_graph(impl_->world, attachments, spaces);
    if (!graph)
        return std::unexpected(graph.error());
    std::vector<std::pair<ObjectId, math::Matrix4>> matrices;
    for (const auto& root : graph->incoming) {
        const auto at = graph->space.matrices.find(root.id());
        if (at != graph->space.matrices.end())
            matrices.emplace_back(root.id(), at->second);
    }
    return matrices;
}

Result<Commit> World::attach_batch(std::span<const EntityAttachment> attachments,
                                   std::span<const SpaceReference> spaces) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return std::unexpected(ready.error());
    if (attachments.empty() && spaces.empty())
        return Commit{};
    auto candidate = prepare_attachment_graph(*this, attachments, spaces);
    if (!candidate)
        return std::unexpected(candidate.error());
    auto& incoming = candidate->incoming;
    auto& tree_ranges = candidate->ranges;
    struct ParentPlan {
        ObjectId parent;

        std::vector<ObjectRoot> existing;

        std::vector<std::pair<std::size_t, std::size_t>> insertions;

        std::vector<ObjectReference> references;
    };
    std::vector<ParentPlan> parents;
    std::unordered_map<ObjectId, std::size_t> parent_positions;
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        const auto& attachment = attachments[i];
        const auto parent = attachment.parent;
        if (parent != ObjectId{}) {
            auto at = parent_positions.find(parent);
            if (at == parent_positions.end()) {
                auto existing = children(runtime(), parent);
                if (!existing)
                    return std::unexpected(existing.error());
                const auto position = parents.size();
                parents.push_back({parent, std::move(*existing), {}, {}});
                at = parent_positions.emplace(parent, position).first;
            }
            auto& plan = parents[at->second];
            const auto index = attachment.index == append ? plan.existing.size() : attachment.index;
            plan.insertions.emplace_back(index, i);
        }
    }
    // Derive arriving member state and final preorder positions before any
    // relation commits; the commit itself only moves prepared values.
    std::unordered_map<ObjectId, std::size_t> arriving_index;
    std::vector<Impl::MemberState> arriving;
    arriving.reserve(incoming.size());
    arriving_index.reserve(incoming.size());
    for (std::size_t i = 0; i < incoming.size(); ++i) {
        const auto& node = candidate->space.nodes.at(incoming[i].id());
        auto derived = state.derive(incoming[i].id(), node.parent, node.origin.value_or(ObjectId{}));
        if (!derived)
            return std::unexpected(derived.error());
        arriving.push_back(std::move(*derived));
        arriving_index.emplace(incoming[i].id(), i);
    }
    // Each incoming tree is preorder, so subtree sizes accumulate from its back.
    for (const auto& [first, count] : tree_ranges)
        for (auto i = first + count; i-- > first + 1;)
            arriving[arriving_index.at(arriving[i].parent)].size += arriving[i].size;
    // A tree enters before an existing next sibling, at its parent's subtree
    // end, or at a root boundary. Equal gaps order deeper parents first, then input.
    struct Insertion {
        std::size_t at;

        std::size_t depth;

        std::size_t attachment;
    };
    std::vector<Insertion> insertions;
    insertions.reserve(attachments.size());
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        const auto& attachment = attachments[i];
        if (attachment.parent == ObjectId{}) {
            insertions.push_back({attachment.index == append ? state.members.size() : attachment.index, 0, i});
            continue;
        }
        auto parent = state.member(attachment.parent);
        if (!parent)
            return std::unexpected(parent.error());
        const auto& plan = parents[parent_positions.at(attachment.parent)];
        const auto child = attachment.index == append ? plan.existing.size() : attachment.index;
        auto at = child < plan.existing.size() ? state.member(plan.existing[child].id())
                                               : Result<std::size_t>{*parent + state.states[*parent].size};
        if (!at)
            return std::unexpected(at.error());
        std::size_t depth = 1;
        for (auto id = state.states[*parent].parent; id != ObjectId{};
             id = state.states[*state.positions.find(id)].parent)
            ++depth;
        insertions.push_back({*at, depth, i});
    }
    std::ranges::sort(insertions, [](const Insertion& left, const Insertion& right) {
        if (left.at != right.at)
            return left.at < right.at;
        if (left.depth != right.depth)
            return left.depth > right.depth;
        return left.attachment < right.attachment;
    });
    const auto total = state.members.size() + incoming.size();
    // Capacity is reserved so the in-place insertions inside the commit cannot fail.
    std::vector<std::size_t> starts(attachments.size());
    std::size_t inserted{};
    for (const auto& insertion : insertions) {
        starts[insertion.attachment] = insertion.at + inserted;
        inserted += tree_ranges[insertion.attachment].second;
    }
    state.members.reserve(total);
    state.states.reserve(total);
    state.positions.reserve(total);
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
            if (index < plan.existing.size())
                plan.references.push_back(plan.existing[index].reference());
        }
    }
    std::vector<Member> arriving_members;
    arriving_members.reserve(incoming.size());
    for (auto& root : incoming)
        arriving_members.push_back({std::move(root)});
    // Borrow all native membership locations before the reference transaction.
    // The outer borrow keeps those addresses valid through the nonthrowing
    // membership writes and directory moves; no native pointer escapes it.
    auto published = runtime().with_native<Entity>(arriving_members.front().id(), [&](Entity&) -> Result<void> {
        std::vector<Entity*> entities;
        entities.reserve(arriving_members.size());
        for (const auto& root : arriving_members) {
            auto borrowed = runtime().with_native<Entity>(root.id(), [&](Entity& entity) -> Result<void> {
                entities.push_back(&entity);
                return {};
            });
            if (!borrowed)
                return borrowed;
        }
        std::vector<ReferenceChange> relation_changes;
        relation_changes.reserve(parents.size() + attachments.size() + spaces.size());
        struct OriginFlags {
            Entity* entity;

            bool explicit_origin;

            bool world_origin;
        };
        std::vector<OriginFlags> origin_flags;
        origin_flags.reserve(spaces.size());
        for (const auto& plan : parents) {
            auto sequence = runtime().with_native<Entity>(plan.parent, [&](Entity& entity) {
                return runtime().reference_field(plan.parent, Access::children(entity));
            });
            if (!sequence)
                return std::unexpected(sequence.error());
            relation_changes.push_back(SequenceSplice{*sequence, 0, plan.existing.size(), plan.references});
        }
        for (const auto& attachment : attachments) {
            auto unchanged = runtime().with_native<Entity>(attachment.entity, [&](Entity& entity) -> Result<bool> {
                return Access::parent(entity).id() == attachment.parent;
            });
            if (!unchanged)
                return std::unexpected(unchanged.error());
            if (*unchanged)
                continue;
            auto reference = runtime().null_reference(types().entity, ValueForm::weak_reference);
            if (!reference)
                return std::unexpected(reference.error());
            if (attachment.parent != ObjectId{}) {
                auto held = runtime().hold(attachment.parent);
                if (!held)
                    return std::unexpected(held.error());
                reference = runtime().observe(held->reference());
                if (!reference)
                    return std::unexpected(reference.error());
            }
            auto slot = runtime().with_native<Entity>(attachment.entity, [&](Entity& entity) {
                return runtime().reference_field(attachment.entity, Access::parent(entity));
            });
            if (!slot)
                return std::unexpected(slot.error());
            relation_changes.push_back(ReferenceWrite{*slot, *reference});
        }
        for (const auto& plan : spaces) {
            auto reference = runtime().null_reference(types().entity, ValueForm::weak_reference);
            if (!reference)
                return std::unexpected(reference.error());
            if (plan.origin && *plan.origin != ObjectId{}) {
                auto held = runtime().hold(*plan.origin, types().entity);
                if (!held)
                    return std::unexpected(held.error());
                reference = runtime().observe(held->reference());
                if (!reference)
                    return std::unexpected(reference.error());
            }
            auto slot = runtime().with_native<Entity>(plan.entity, [&](Entity& entity) {
                origin_flags.push_back({&entity, plan.origin.has_value(), plan.origin && *plan.origin == ObjectId{}});
                return runtime().reference_field(plan.entity, Access::origin(entity));
            });
            if (!slot)
                return std::unexpected(slot.error());
            relation_changes.push_back(ReferenceWrite{*slot, *reference});
        }
        detail::FlagScope transition{state.transitioning};
        if (auto changed = runtime().apply_reference_changes(relation_changes); !changed)
            return changed;
        for (const auto& flags : origin_flags) {
            Access::explicit_origin(*flags.entity) = flags.explicit_origin;
            Access::world_origin(*flags.entity) = flags.world_origin;
        }
        for (auto* entity : entities)
            Access::member_world(*entity) = this;
        // Descending gaps leave earlier positions valid; equal gaps insert in reverse.
        for (auto it = insertions.rbegin(); it != insertions.rend(); ++it) {
            const auto [first, count] = tree_ranges[it->attachment];
            const auto at = std::ptrdiff_t(it->at);
            const auto from = std::ptrdiff_t(first);
            const auto to = std::ptrdiff_t(first + count);
            state.members.insert(state.members.begin() + at, std::make_move_iterator(arriving_members.begin() + from),
                                 std::make_move_iterator(arriving_members.begin() + to));
            state.states.insert(state.states.begin() + at, std::make_move_iterator(arriving.begin() + from),
                                std::make_move_iterator(arriving.begin() + to));
        }
        state.reindex(insertions.front().at, total);
        for (std::size_t i = 0; i < attachments.size(); ++i) {
            const auto count = tree_ranges[i].second;
            state.resize_ancestors(attachments[i].parent, std::ptrdiff_t(count));
            state.refresh_eligibility(starts[i], count);
        }
        state.changed();
        return {};
    });
    if (!published)
        return std::unexpected(published.error());
    // Callbacks observe arriving trees in final composition order.
    std::vector<std::size_t> order;
    order.reserve(insertions.size());
    for (const auto& insertion : insertions)
        order.push_back(insertion.attachment);
    std::optional<Error> failure;
    for (const auto i : order)
        for (auto at = starts[i]; at < starts[i] + tree_ranges[i].second; ++at) {
            auto notified = state.notify(Lifecycle::attached, state.members[at].id());
            if (!failure)
                failure = std::move(notified);
            for (const auto& item : state.states[at].components) {
                notified = state.notify(Lifecycle::attached, item.id);
                if (!failure)
                    failure = std::move(notified);
            }
        }
    if (state.running)
        for (const auto i : order)
            if (!failure)
                failure = state.begin(starts[i], starts[i] + tree_ranges[i].second);
    if (state.running)
        for (const auto i : order)
            for (auto at = starts[i]; !failure && at < starts[i] + tree_ranges[i].second; ++at)
                if (state.states[at].eligible)
                    failure = state.notify(Lifecycle::activated, state.members[at].id());
    return state.finish(true, std::move(failure));
}

Result<EntityRemoval> World::detach(ObjectId object) {
    auto removed = detach_batch(std::span<const ObjectId>{&object, 1});
    if (!removed)
        return std::unexpected(removed.error());
    return std::move(removed->front());
}

Result<std::vector<EntityRemoval>> World::detach_batch(std::span<const ObjectId> roots) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return std::unexpected(ready.error());
    if (roots.empty())
        return std::vector<EntityRemoval>{};
    struct ParentPlan {
        ObjectId parent;

        std::vector<ObjectRoot> existing;

        std::vector<ObjectReference> references;
    };
    struct Range {
        std::size_t at;

        std::size_t count;

        ObjectId parent;
    };
    std::vector<ParentPlan> parents;
    std::unordered_map<ObjectId, std::size_t> parent_positions;
    std::vector<EntityRemoval> removals;
    removals.reserve(roots.size());
    std::vector<Range> ranges;
    ranges.reserve(roots.size());
    std::unordered_set<ObjectId> ids;
    std::unordered_set<ObjectId> root_ids;
    for (const auto object : roots) {
        auto at = state.member(object);
        if (!at)
            return std::unexpected(at.error());
        const auto count = state.states[*at].size;
        const auto parent = state.states[*at].parent;
        auto held = runtime().hold(object);
        if (!held)
            return std::unexpected(held.error());
        EntityRemoval removal;
        removal.root = std::move(*held);
        removal.index = *at;
        removal.parent = parent;
        removal.member_count = count;
        removal.origin_ = state.lifetime;
        if (parent != ObjectId{}) {
            auto plan = parent_positions.find(parent);
            if (plan == parent_positions.end()) {
                auto existing = children(runtime(), parent);
                if (!existing)
                    return std::unexpected(existing.error());
                const auto position = parents.size();
                parents.push_back({parent, std::move(*existing), {}});
                plan = parent_positions.emplace(parent, position).first;
            }
            const auto& siblings = parents[plan->second].existing;
            auto found = std::ranges::find_if(siblings, [&](const auto& item) { return item.id() == object; });
            if (found == siblings.end())
                return error(ErrorCode::invalid_member, "Entity is absent from its parent.");
            removal.sibling_index = std::size_t(found - siblings.begin());
        }
        for (auto i = *at; i < *at + count; ++i) {
            const auto id = state.members[i].id();
            if (!ids.insert(id).second)
                return error(ErrorCode::invalid_member, "Detach contains overlapping subtrees.");
            if (auto member = state.composition_entity(id); !member)
                return std::unexpected(member.error());
        }
        root_ids.insert(object);
        ranges.push_back({*at, count, parent});
        removals.push_back(std::move(removal));
    }
    // Retained members must keep valid spatial targets. Departing objects keep
    // their authored weak origins even when Detached space cannot be evaluated;
    // a later attachment validates the complete prospective membership again.
    for (std::size_t i = 0; i < state.members.size(); ++i) {
        const auto origin = state.states[i].origin;
        if (origin != ObjectId{} && ids.contains(origin) && !ids.contains(state.members[i].id()))
            return error(ErrorCode::invalid_member,
                         "Rebind external spatial dependencies before detaching the forest.");
    }
    // Prepare complete parent sequences and the surviving member directory
    // before cancelling tasks or entering lifecycle callbacks.
    for (auto& plan : parents) {
        plan.references.reserve(plan.existing.size());
        for (const auto& child : plan.existing)
            if (!root_ids.contains(child.id()))
                plan.references.push_back(child.reference());
    }
    auto null_parent = runtime().null_reference(types().entity, ValueForm::weak_reference);
    if (!null_parent)
        return std::unexpected(null_parent.error());
    std::ranges::sort(ranges, {}, &Range::at);
    std::vector<ObjectId> exiting;
    for (auto range = ranges.rbegin(); range != ranges.rend(); ++range)
        for (auto i = range->at + range->count; i-- > range->at;) {
            const auto& group = state.states[i].components;
            for (auto it = group.rbegin(); it != group.rend(); ++it)
                exiting.push_back(it->id);
            exiting.push_back(state.members[i].id());
        }
    std::vector<Member> departed;
    departed.reserve(ids.size());
    for (const auto& range : ranges)
        for (auto i = range.at; i < range.at + range.count; ++i)
            state.cancel_tasks(state.members[i].id());
    std::optional<Error> failure;
    for (auto range = ranges.rbegin(); range != ranges.rend(); ++range) {
        auto ended = state.end(range->at, range->at + range->count);
        if (!failure)
            failure = std::move(ended);
    }
    if (state.running)
        for (auto range = ranges.rbegin(); range != ranges.rend(); ++range)
            for (auto i = range->at + range->count; i-- > range->at;) {
                if (!state.states[i].eligible)
                    continue;
                auto notified = state.notify(Lifecycle::deactivated, state.members[i].id());
                if (!failure)
                    failure = std::move(notified);
            }
    // All native locators and membership pointers stay within one outer borrow.
    auto published = runtime().with_native<Entity>(roots.front(), [&](Entity&) -> Result<void> {
        std::vector<Entity*> entities;
        entities.reserve(ids.size());
        for (const auto& range : ranges)
            for (auto i = range.at; i < range.at + range.count; ++i) {
                auto borrowed =
                    runtime().with_native<Entity>(state.members[i].id(), [&](Entity& entity) -> Result<void> {
                        entities.push_back(&entity);
                        return {};
                    });
                if (!borrowed)
                    return borrowed;
            }
        std::vector<ReferenceChange> changes;
        changes.reserve(parents.size() + roots.size());
        for (const auto& plan : parents) {
            auto sequence = runtime().with_native<Entity>(plan.parent, [&](Entity& entity) {
                return runtime().reference_field(plan.parent, Access::children(entity));
            });
            if (!sequence)
                return std::unexpected(sequence.error());
            changes.push_back(SequenceSplice{*sequence, 0, plan.existing.size(), plan.references});
        }
        for (const auto object : roots) {
            auto slot = runtime().with_native<Entity>(
                object, [&](Entity& entity) { return runtime().reference_field(object, Access::parent(entity)); });
            if (!slot)
                return std::unexpected(slot.error());
            changes.push_back(ReferenceWrite{*slot, *null_parent});
        }
        detail::FlagScope transition{state.transitioning};
        if (auto changed = runtime().apply_reference_changes(changes); !changed)
            return changed;
        for (auto* entity : entities)
            Access::member_world(*entity) = nullptr;
        // Departing member roots stay alive until the detached notifications finish.
        // Descending ranges leave earlier positions valid.
        for (auto range = ranges.rbegin(); range != ranges.rend(); ++range) {
            if (!state.prerequisites.empty() || !state.dependents.empty())
                for (auto i = range->at; i < range->at + range->count; ++i) {
                    state.forget_prerequisites(state.members[i].id());
                    for (const auto& item : state.states[i].components)
                        state.forget_prerequisites(item.id);
                }
            const auto first = state.members.begin() + std::ptrdiff_t(range->at);
            const auto last = first + std::ptrdiff_t(range->count);
            for (auto it = first; it != last; ++it) {
                state.positions.remove(it->id());
                departed.push_back(std::move(*it));
            }
            state.members.erase(first, last);
            state.states.erase(state.states.begin() + std::ptrdiff_t(range->at),
                               state.states.begin() + std::ptrdiff_t(range->at + range->count));
        }
        state.reindex(ranges.front().at, state.members.size());
        for (const auto& range : ranges)
            state.resize_ancestors(range.parent, -std::ptrdiff_t(range.count));
        state.changed();
        return {};
    });
    if (!published) {
        if (!failure)
            failure = published.error();
        auto stopped = state.finish(false, std::move(failure));
        return std::unexpected(std::move(*stopped.notification_error));
    }
    for (const auto object : exiting) {
        auto notified = state.notify(Lifecycle::detached, object);
        if (!failure)
            failure = std::move(notified);
    }
    const auto commit = state.finish(true, std::move(failure));
    for (auto& removal : removals)
        removal.commit = commit;
    return removals;
}

Result<Commit> World::restore(const EntityRemoval& removal) {
    return restore(std::span<const EntityRemoval>{&removal, 1});
}

Result<Commit> World::restore(std::span<const EntityRemoval> removals) {
    if (auto ready = impl_->admit(); !ready)
        return std::unexpected(ready.error());
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
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t left, std::size_t right) { return removals[left].index < removals[right].index; });
    std::vector<EntityAttachment> attachments;
    attachments.reserve(removals.size());
    for (const auto i : order) {
        const auto& removal = removals[i];
        const auto original = removal.parent == ObjectId{} ? removal.index : removal.sibling_index;
        std::size_t preceding{};
        for (const auto& other : removals) {
            const bool before = removal.parent == ObjectId{}
                                    ? other.index < removal.index
                                    : other.parent == removal.parent && other.sibling_index < removal.sibling_index;
            if (!before)
                continue;
            const auto count = removal.parent == ObjectId{} ? other.member_count : std::size_t{1};
            if (count > original - preceding)
                return error(ErrorCode::invalid_member, "Removal positions are inconsistent.");
            preceding += count;
        }
        attachments.push_back({removal.id(), removal.parent, original - preceding});
    }
    return attach_batch(std::span<const EntityAttachment>{attachments});
}
}
