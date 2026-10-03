#include "world/detail.hpp"
#include <algorithm>

namespace atelier::world {
using detail::Access;
using detail::error;

Result<Types> register_types(Runtime& runtime) {
    const auto vector = runtime.native_type<math::Vector3>();
    const auto quaternion = runtime.native_type<math::Quaternion>();
    if (!vector) return std::unexpected(vector.error());
    if (!quaternion) return std::unexpected(quaternion.error());
    std::vector<TypeDefinition> definitions;
    definitions.push_back(reflection::Definition<Actor>::build(runtime));
    definitions.push_back(reflection::Definition<Component>::build(runtime));
    definitions.push_back(reflection::Definition<Transform>::build(runtime));
    definitions.push_back(reflection::Definition<Cube>::build(runtime));
    definitions.push_back(reflection::Definition<Counter>::build(runtime));
    auto ids = runtime.register_types(std::move(definitions));
    if (!ids) return std::unexpected(ids.error());
    Types types;
    types.vector3 = *vector; types.quaternion = *quaternion; types.actor = (*ids)[0];
    types.component = (*ids)[1]; types.transform = (*ids)[2]; types.cube = (*ids)[3]; types.counter = (*ids)[4];
    const auto make_paths = [&](auto& paths, TypeId record, std::string_view name) -> Result<void> {
        auto outer = runtime.find_field(types.transform, name);
        if (!outer) return std::unexpected(outer.error());
        constexpr std::array names{"x", "y", "z", "w"};
        for (std::size_t i = 0; i < paths.size(); ++i) {
            auto field = runtime.find_field(record, names[i]);
            if (!field) return std::unexpected(field.error());
            paths[i] = {*outer, *field};
        }
        return {};
    };
    if (auto result = make_paths(types.position_fields, types.vector3, "position"); !result) return std::unexpected(result.error());
    if (auto result = make_paths(types.rotation_fields, types.quaternion, "rotation"); !result) return std::unexpected(result.error());
    if (auto result = make_paths(types.scale_fields, types.vector3, "scale"); !result) return std::unexpected(result.error());
    auto value = runtime.find_field(types.counter, "value");
    auto steps = runtime.find_field(types.counter, "steps");
    auto advance = runtime.find_method(types.counter, "advance");
    if (!value) return std::unexpected(value.error());
    if (!steps) return std::unexpected(steps.error());
    if (!advance) return std::unexpected(advance.error());
    types.counter_value = *value; types.counter_steps = *steps; types.counter_advance = *advance;
    return types;
}

namespace detail {
Result<ActorData> actor_data(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Actor>(object, [](Actor& actor) -> Result<ActorData> {
        return ActorData{Access::member_world(actor), Access::enabled(actor), Access::transitioning(actor)};
    });
}
Result<ComponentData> component_data(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Component>(object, [](Component& component) -> Result<ComponentData> {
        return ComponentData{Access::owner(component).id(), Access::enabled(component)};
    });
}
Result<ObjectId> cube_transform(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Cube>(object, [](Cube& cube) -> Result<ObjectId> { return Access::transform(cube).id(); });
}
Result<bool> is(Runtime& runtime, ObjectId object, TypeId expected) {
    auto actual = runtime.type_of(object);
    if (!actual) return std::unexpected(actual.error());
    return runtime.is_a(*actual, expected);
}
Result<void> validate_object(Runtime& runtime, ObjectId object, TypeId expected) {
    auto matches = is(runtime, object, expected);
    if (!matches) return std::unexpected(matches.error());
    if (!*matches) return error(ErrorCode::type_mismatch, "Object does not derive from the required World type.");
    return {};
}
}

Result<ObjectRoot> create_actor(Runtime& runtime, TypeId actual) {
    if (auto ready = runtime.check_code_publication(); !ready) return std::unexpected(ready.error());
    auto base = runtime.native_type<Actor>();
    if (!base) return std::unexpected(base.error());
    if (actual == TypeId{}) actual = *base;
    auto matches = runtime.is_a(actual, *base);
    if (!matches) return std::unexpected(matches.error());
    if (!*matches) return error(ErrorCode::type_mismatch, "create_actor requires an Actor type.");
    return runtime.create(actual);
}
Result<ObjectRoot> create_component(Runtime& runtime, TypeId actual) {
    if (auto ready = runtime.check_code_publication(); !ready) return std::unexpected(ready.error());
    auto base = runtime.native_type<Component>();
    if (!base) return std::unexpected(base.error());
    auto matches = runtime.is_a(actual, *base);
    if (!matches) return std::unexpected(matches.error());
    if (!*matches) return error(ErrorCode::type_mismatch, "create_component requires a Component type.");
    return runtime.create(actual);
}

Result<ActorStatus> inspect_actor(Runtime& runtime, ObjectId object) {
    if (object == ObjectId{}) return ActorStatus{};
    if (!runtime.belongs_to_context(object)) return error(ErrorCode::invalid_object, "Actor identity belongs to another Runtime.");
    auto type = runtime.type_of(object);
    if (!type) {
        if (type.error().code == ErrorCode::invalid_object) return ActorStatus{};
        return std::unexpected(type.error());
    }
    auto data = detail::actor_data(runtime, object);
    if (!data) return std::unexpected(data.error());
    if (data->transitioning) return error(ErrorCode::busy, "Actor is transitioning.");
    ActorStatus status{ActorState::detached, *type, data->enabled, false, false};
    if (data->world) {
        const auto& state = Access::state(*data->world);
        if (auto ready = state.admit(false); !ready) return std::unexpected(ready.error());
        status.state = state.running && data->enabled ? ActorState::active : ActorState::attached_inactive;
        status.can_edit = !state.running && !state.dispatch_depth;
        const auto member = state.positions.find(object);
        if (member == state.positions.end()) return error(ErrorCode::invalid_member, "World member index is inconsistent.");
        status.can_present = member->second.presentations != 0;
    }
    return status;
}
Result<ComponentStatus> inspect_component(Runtime& runtime, ObjectId object) {
    if (object == ObjectId{}) return ComponentStatus{};
    if (!runtime.belongs_to_context(object)) return error(ErrorCode::invalid_object, "Component identity belongs to another Runtime.");
    auto type = runtime.type_of(object);
    if (!type) {
        if (type.error().code == ErrorCode::invalid_object) return ComponentStatus{};
        return std::unexpected(type.error());
    }
    auto data = detail::component_data(runtime, object);
    if (!data) return std::unexpected(data.error());
    ComponentStatus status{true, *type, data->owner, data->enabled, false, false};
    if (data->owner != ObjectId{}) {
        auto actor = detail::actor_data(runtime, data->owner);
        if (!actor) return std::unexpected(actor.error());
        if (actor->transitioning) return error(ErrorCode::busy, "Component owner is transitioning.");
        if (actor->world) {
            const auto& state = Access::state(*actor->world);
            if (auto ready = state.admit(false); !ready) return std::unexpected(ready.error());
            // Components without a game updater still have activation eligibility.
            status.active = state.running && actor->enabled && data->enabled;
            status.can_present = std::ranges::find(state.presentation, object) != state.presentation.end();
        }
    }
    return status;
}

Result<std::vector<ObjectRoot>> components(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Actor>(object, [&](Actor& actor) -> Result<std::vector<ObjectRoot>> {
        if (Access::transitioning(actor)) return error(ErrorCode::busy, "Actor is transitioning.");
        if (auto* world = Access::member_world(actor)) {
            if (auto ready = Access::state(*world).admit(false); !ready) return std::unexpected(ready.error());
        }
        auto field = runtime.reference_field(object, Access::sequence(actor));
        if (!field) return std::unexpected(field.error());
        auto count = runtime.sequence_size(*field);
        if (!count) return std::unexpected(count.error());
        std::vector<ObjectRoot> result;
        result.reserve(*count);
        for (std::size_t i = 0; i < *count; ++i) {
            auto reference = runtime.sequence_read(*field, i);
            if (!reference) return std::unexpected(reference.error());
            auto root = runtime.hold(*reference);
            if (!root) return std::unexpected(root.error());
            result.push_back(std::move(*root));
        }
        return result;
    });
}
Result<Trs> read_transform(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Transform>(object, [](Transform& transform) -> Result<Trs> {
        return Trs{transform.position, transform.rotation, transform.scale};
    });
}
Result<math::Matrix4> world_matrix(Runtime& runtime, ObjectId object) {
    auto value = read_transform(runtime, object);
    if (!value) return std::unexpected(value.error());
    auto canonical = canonical_transform(*value);
    if (!canonical) return std::unexpected(canonical.error());
    return transform_matrix(*canonical);
}
Result<std::optional<ObjectId>> root_transform(Runtime& runtime, ObjectId actor) {
    auto type = runtime.native_type<Transform>();
    if (!type) return std::unexpected(type.error());
    auto list = components(runtime, actor);
    if (!list) return std::unexpected(list.error());
    std::optional<ObjectId> result;
    for (const auto& component : *list) {
        auto match = detail::is(runtime, component.id(), *type);
        if (!match) return std::unexpected(match.error());
        if (*match) {
            if (result) return error(ErrorCode::invalid_member, "Actor has multiple root transforms.");
            result = component.id();
        }
    }
    return result;
}

Result<Commit> destroy_actor(Runtime& runtime, ObjectId object) {
    if (auto ready = runtime.check_code_publication(); !ready) return std::unexpected(ready.error());
    auto data = detail::actor_data(runtime, object);
    if (!data) return std::unexpected(data.error());
    if (data->transitioning) return error(ErrorCode::busy, "Actor is transitioning.");
    auto actor_root = runtime.hold(object);
    if (!actor_root) return std::unexpected(actor_root.error());
    auto group = components(runtime, object);
    if (!group) return std::unexpected(group.error());
    auto transform_type = runtime.native_type<Transform>();
    if (!transform_type) return std::unexpected(transform_type.error());
    std::vector<ObjectId> destruction_order, transforms;
    destruction_order.reserve(group->size());
    transforms.reserve(1);
    for (const auto& component : *group) {
        auto owned = detail::component_data(runtime, component.id());
        if (!owned) return std::unexpected(owned.error());
        if (owned->owner != object) return error(ErrorCode::invalid_member, "Actor composition has an inconsistent owner.");
    }
    for (auto it = group->rbegin(); it != group->rend(); ++it) {
        auto transform = detail::is(runtime, it->id(), *transform_type);
        if (!transform) return std::unexpected(transform.error());
        if (*transform) transforms.push_back(it->id());
        else destruction_order.push_back(it->id());
    }
    destruction_order.insert(destruction_order.end(), transforms.begin(), transforms.end());
    Commit committed{true, {}};
    if (data->world) {
        auto removed = data->world->detach(object);
        if (!removed) return std::unexpected(removed.error());
        committed.notification_error = std::move(removed->commit.notification_error);
    }
    auto actor_type = runtime.native_type<Actor>();
    if (!actor_type) return std::unexpected(actor_type.error());
    auto null_owner = runtime.null_reference(*actor_type);
    if (!null_owner) return std::unexpected(null_owner.error());
    // One outer borrow protects all native locators; no native address escapes.
    auto cleared = runtime.with_native<Actor>(object, [&](Actor& actor) -> Result<void> {
        auto sequence = runtime.reference_field(object, Access::sequence(actor));
        if (!sequence) return std::unexpected(sequence.error());
        std::vector<ReferenceChange> changes;
        changes.reserve(group->size() + 1);
        changes.push_back(SequenceSplice{*sequence, 0, group->size(), {}});
        for (const auto& component : *group) {
            auto slot = runtime.with_native<Component>(component.id(), [&](Component& value) {
                return runtime.reference_field(component.id(), Access::owner(value));
            });
            if (!slot) return std::unexpected(slot.error());
            changes.push_back(ReferenceWrite{std::move(*slot), *null_owner});
        }
        auto result = runtime.apply_reference_changes(changes);
        if (!result) return result;
        Access::transitioning(actor) = true;
        return {};
    });
    if (!cleared && !committed.notification_error) committed.notification_error = cleared.error();
    // Once exit has committed, finish termination even if a trusted callback or
    // allocation failed. Do not republish a half-destroyed group as live.
    for (const auto component : destruction_order) {
        auto destroyed = runtime.destroy(component);
        if (!destroyed && !committed.notification_error) committed.notification_error = destroyed.error();
    }
    auto destroyed = runtime.destroy(object);
    if (!destroyed && !committed.notification_error) committed.notification_error = destroyed.error();
    return committed;
}

Result<Commit> destroy_component(Runtime& runtime, ObjectId object) {
    if (auto ready = runtime.check_code_publication(); !ready) return std::unexpected(ready.error());
    auto data = detail::component_data(runtime, object);
    if (!data) return std::unexpected(data.error());
    auto root = runtime.hold(object);
    if (!root) return std::unexpected(root.error());
    Commit committed{true, {}};
    if (data->owner != ObjectId{}) {
        auto actor = detail::actor_data(runtime, data->owner);
        if (!actor) return std::unexpected(actor.error());
        if (actor->world) {
            auto removed = actor->world->remove_component(object);
            if (!removed) return std::unexpected(removed.error());
            committed.notification_error = std::move(removed->commit.notification_error);
        } else {
            auto group = components(runtime, data->owner);
            if (!group) return std::unexpected(group.error());
            const auto at = std::ranges::find_if(*group, [&](const ObjectRoot& item) { return item.id() == object; });
            if (at == group->end()) return error(ErrorCode::invalid_member, "Component is absent from its owner's composition.");
            auto cube_type = runtime.native_type<Cube>();
            if (!cube_type) return std::unexpected(cube_type.error());
            for (const auto& item : *group) {
                auto cube = detail::is(runtime, item.id(), *cube_type);
                if (!cube) return std::unexpected(cube.error());
                if (*cube) {
                    auto target = detail::cube_transform(runtime, item.id());
                    if (!target) return std::unexpected(target.error());
                    if (*target == object) return error(ErrorCode::invalid_member, "Transform is still required by a Cube.");
                }
            }
            auto owner_type = runtime.native_type<Actor>();
            if (!owner_type) return std::unexpected(owner_type.error());
            auto null_owner = runtime.null_reference(*owner_type);
            if (!null_owner) return std::unexpected(null_owner.error());
            const auto index = std::size_t(at - group->begin());
            auto removed = runtime.with_native<Actor>(data->owner, [&](Actor& owner) -> Result<void> {
                auto sequence = runtime.reference_field(data->owner, Access::sequence(owner));
                if (!sequence) return std::unexpected(sequence.error());
                return runtime.with_native<Component>(object, [&](Component& component) -> Result<void> {
                    auto slot = runtime.reference_field(object, Access::owner(component));
                    if (!slot) return std::unexpected(slot.error());
                    const std::array<ReferenceChange, 2> changes{SequenceSplice{*sequence, index, 1, {}}, ReferenceWrite{*slot, *null_owner}};
                    return runtime.apply_reference_changes(changes);
                });
            });
            if (!removed) return std::unexpected(removed.error());
        }
    }
    auto destroyed = runtime.destroy(object);
    if (!destroyed) return std::unexpected(destroyed.error());
    return committed;
}

}
