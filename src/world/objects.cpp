#include "world/detail.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace atelier::world {
using detail::Access;
using detail::error;

Result<Types> register_types(Runtime& runtime) {
    std::vector<TypeDefinition> definitions;
    definitions.push_back(reflection::Definition<Vec3>::build(runtime));
    definitions.push_back(reflection::Definition<Quaternion>::build(runtime));
    definitions.push_back(reflection::Definition<Actor>::build(runtime));
    definitions.push_back(reflection::Definition<Component>::build(runtime));
    definitions.push_back(reflection::Definition<SceneComponent>::build(runtime));
    definitions.push_back(reflection::Definition<Material>::build(runtime));
    definitions.push_back(reflection::Definition<StaticMeshComponent>::build(runtime));
    definitions.push_back(reflection::Definition<StaticMeshActor>::build(runtime));
    auto ids = runtime.register_types(std::move(definitions));
    if (!ids) return std::unexpected(ids.error());
    Types types;
    types.vec3 = (*ids)[0]; types.quaternion = (*ids)[1]; types.actor = (*ids)[2];
    types.component = (*ids)[3]; types.scene_component = (*ids)[4]; types.material = (*ids)[5]; types.static_mesh_component = (*ids)[6];
    types.static_mesh_actor = (*ids)[7];
    const auto make_paths = [&](auto& paths, TypeId record, std::string_view name) -> Result<void> {
        auto outer = runtime.find_field(types.scene_component, name);
        if (!outer) return std::unexpected(outer.error());
        constexpr std::array names{"x", "y", "z", "w"};
        for (std::size_t i = 0; i < paths.size(); ++i) {
            auto field = runtime.find_field(record, names[i]);
            if (!field) return std::unexpected(field.error());
            paths[i] = {*outer, *field};
        }
        return {};
    };
    if (auto result = make_paths(types.position_fields, types.vec3, "position"); !result) return std::unexpected(result.error());
    if (auto result = make_paths(types.rotation_fields, types.quaternion, "rotation"); !result) return std::unexpected(result.error());
    if (auto result = make_paths(types.scale_fields, types.vec3, "scale"); !result) return std::unexpected(result.error());
    return types;
}

namespace detail {
Result<ActorData> actor_data(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Actor>(object, [](Actor& actor) -> Result<ActorData> {
        return ActorData{Access::member_world(actor), Access::enabled(actor), Access::transitioning(actor), Access::begun(actor)};
    });
}
Result<ComponentData> component_data(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Component>(object, [](Component& component) -> Result<ComponentData> {
        return ComponentData{Access::owner(component).id(), Access::enabled(component), Access::begun(component)};
    });
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
Result<void> validate_material(const Material& material) {
    const auto color = material.base_color;
    for (float channel : {color.x, color.y, color.z})
        if (!std::isfinite(channel) || channel < 0 || channel > 1)
            return error(ErrorCode::argument_mismatch, "Material color must be finite and within [0,1].");
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

Result<ObjectRoot> create_static_mesh_actor(World& world, const StaticMeshActorDesc& description) {
    if (auto ready = Access::state(world).admit(); !ready) return std::unexpected(ready.error());
    auto transform = canonical_transform(description.transform);
    if (!transform) return std::unexpected(transform.error());
    if (auto valid = detail::validate_material(description.material); !valid) return std::unexpected(valid.error());
    auto& runtime = world.runtime();
    struct Construction {
        Runtime& runtime;
        ObjectRoot actor, component;
        ~Construction() {
            // The graph is unpublished. Either destruction entry also handles
            // ordinary strong-reference cycles; no partial factory result escapes.
            if (actor.id() != ObjectId{}) (void)destroy_actor(runtime, actor.id());
            if (component.id() != ObjectId{} && runtime.type_of(component.id()))
                (void)destroy_component(runtime, component.id());
        }
    } created{runtime, {}, {}};
    auto actor = create_actor(runtime, world.types().static_mesh_actor);
    if (!actor) return std::unexpected(actor.error());
    created.actor = std::move(*actor);
    auto component = create_component(runtime, world.types().static_mesh_component);
    if (!component) return std::unexpected(component.error());
    created.component = std::move(*component);
    auto configured = runtime.with_native<StaticMeshComponent>(created.component.id(), [&](StaticMeshComponent& value) -> Result<void> {
        value.position = transform->position;
        value.rotation = transform->rotation;
        value.scale = transform->scale;
        value.material = description.material;
        return {};
    });
    if (!configured) return std::unexpected(configured.error());
    auto geometry = set_geometry(runtime, created.component.id(), description.mesh);
    if (!geometry) return std::unexpected(geometry.error());
    const std::array composition{Attachment{created.component.id(), {}, true}};
    auto composed = world.attach_components(created.actor.id(), composition);
    if (!composed) return std::unexpected(composed.error());
    if (composed->notification_error) return std::unexpected(std::move(*composed->notification_error));
    created.component.reset(); // Actor composition now retains its root component.
    return std::move(created.actor);
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
        status.state = state.running && data->begun && data->enabled ? ActorState::active : ActorState::attached_inactive;
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
            status.active = state.running && data->begun && actor->enabled && data->enabled;
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
    return runtime.with_native<SceneComponent>(object, [](SceneComponent& component) -> Result<Trs> {
        return Trs{component.position, component.rotation, component.scale};
    });
}
Result<void> set_geometry(Runtime& runtime, ObjectId object, geometry::MeshRef geometry) {
    if (auto ready = runtime.check_code_publication(); !ready) return ready;
    auto component = detail::component_data(runtime, object);
    if (!component) return std::unexpected(component.error());
    World* world{};
    if (component->owner != ObjectId{}) {
        auto owner = detail::actor_data(runtime, component->owner);
        if (!owner) return std::unexpected(owner.error());
        if (owner->transitioning) return error(ErrorCode::busy, "Actor is transitioning.");
        world = owner->world;
        if (world) {
            auto& state = Access::state(*world);
            if (auto ready = state.admit(); !ready) return ready;
            state.prepare_activity();
        }
    }
    auto changed = runtime.with_native<StaticMeshComponent>(object, [&](StaticMeshComponent& component) -> Result<void> {
        Access::geometry(component) = std::move(geometry); return {};
    });
    if (!changed) return changed;
    return world ? Access::state(*world).refresh() : Result<void>{};
}
Result<ObjectId> scene_parent(Runtime& runtime, ObjectId object) {
    return runtime.with_native<SceneComponent>(object, [](SceneComponent& component) -> Result<ObjectId> {
        return Access::parent(component).id();
    });
}
Result<Matrix4> world_matrix(Runtime& runtime, ObjectId object) {
    if (object == ObjectId{}) return error(ErrorCode::invalid_object, "World matrix requires a SceneComponent identity.");
    Matrix4 result{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    // Private parent links are acyclic by construction. Evaluate all the way to
    // the explicit root; composing matrices preserves shear introduced by TRS.
    for (auto node = object; node != ObjectId{};) {
        auto value = read_transform(runtime, node);
        if (!value) return std::unexpected(value.error());
        auto canonical = canonical_transform(*value);
        if (!canonical) return std::unexpected(canonical.error());
        auto product = multiply(result, transform_matrix(*canonical));
        if (!product) return std::unexpected(product.error());
        result = *product;
        auto parent = scene_parent(runtime, node);
        if (!parent) return std::unexpected(parent.error());
        node = *parent;
    }
    return result;
}
Result<std::optional<ObjectId>> root_component(Runtime& runtime, ObjectId object) {
    return runtime.with_native<Actor>(object, [](Actor& actor) -> Result<std::optional<ObjectId>> {
        const auto root = Access::root(actor).id();
        return root == ObjectId{} ? std::optional<ObjectId>{} : std::optional<ObjectId>{root};
    });
}
namespace detail {
Result<void> validate_tree(Runtime& runtime, ObjectId actor, std::span<const ObjectRoot> group) {
    auto type = runtime.native_type<SceneComponent>();
    if (!type) return std::unexpected(type.error());
    auto root = root_component(runtime, actor);
    if (!root) return std::unexpected(root.error());
    std::unordered_map<ObjectId, ObjectId> parents;
    for (const auto& item : group) {
        auto scene = is(runtime, item.id(), *type);
        if (!scene) return std::unexpected(scene.error());
        if (!*scene) continue;
        auto trs = read_transform(runtime, item.id());
        if (!trs) return std::unexpected(trs.error());
        auto canonical = canonical_transform(*trs);
        if (!canonical) return std::unexpected(canonical.error());
        auto parent = scene_parent(runtime, item.id());
        if (!parent) return std::unexpected(parent.error());
        parents.emplace(item.id(), *parent);
    }
    return validate_parents(root->value_or(ObjectId{}), parents);
}
Result<void> validate_parents(ObjectId root, const std::unordered_map<ObjectId, ObjectId>& parents) {
    if (parents.empty()) {
        if (root != ObjectId{}) return error(ErrorCode::invalid_member, "Actor root is absent from its composition.");
        return {};
    }
    if (root == ObjectId{} || !parents.contains(root) || parents.at(root) != ObjectId{})
        return error(ErrorCode::invalid_member, "Actor requires an explicit SceneComponent root without a parent.");
    for (const auto& [node, parent] : parents) {
        (void)parent;
        auto current = node;
        for (std::size_t depth = 0; current != root; ++depth) {
            auto found = parents.find(current);
            if (depth >= parents.size() || found == parents.end())
                return error(ErrorCode::invalid_member, "Spatial hierarchy must be an acyclic tree within one Actor.");
            current = found->second;
        }
    }
    return {};
}
Result<void> can_remove(Runtime& runtime, ObjectId object, std::span<const ObjectRoot> group) {
    auto type = runtime.native_type<SceneComponent>();
    if (!type) return std::unexpected(type.error());
    for (const auto& item : group) {
        auto scene = is(runtime, item.id(), *type);
        if (!scene) return std::unexpected(scene.error());
        if (!*scene) continue;
        auto parent = scene_parent(runtime, item.id());
        if (!parent) return std::unexpected(parent.error());
        if (*parent == object) return error(ErrorCode::invalid_member, "Reparent or remove the SceneComponent's children first.");
    }
    return {};
}
Result<void> clear_component(Runtime& runtime, ObjectId actor_id, ObjectId object, std::size_t index, bool root) {
    auto actor_type = runtime.native_type<Actor>();
    auto scene_type = runtime.native_type<SceneComponent>();
    if (!actor_type) return std::unexpected(actor_type.error());
    if (!scene_type) return std::unexpected(scene_type.error());
    auto null_owner = runtime.null_reference(*actor_type);
    auto null_scene = runtime.null_reference(*scene_type, ValueForm::weak_reference);
    if (!null_owner) return std::unexpected(null_owner.error());
    if (!null_scene) return std::unexpected(null_scene.error());
    auto scene = is(runtime, object, *scene_type);
    if (!scene) return std::unexpected(scene.error());
    return runtime.with_native<Actor>(actor_id, [&](Actor& actor) -> Result<void> {
        auto sequence = runtime.reference_field(actor_id, Access::sequence(actor));
        auto root_slot = runtime.reference_field(actor_id, Access::root(actor));
        if (!sequence) return std::unexpected(sequence.error());
        if (!root_slot) return std::unexpected(root_slot.error());
        auto owner = runtime.with_native<Component>(object, [&](Component& component) {
            return runtime.reference_field(object, Access::owner(component));
        });
        if (!owner) return std::unexpected(owner.error());
        std::vector<ReferenceChange> changes;
        changes.push_back(SequenceSplice{*sequence, index, 1, {}});
        changes.push_back(ReferenceWrite{*owner, *null_owner});
        if (root) changes.push_back(ReferenceWrite{*root_slot, *null_scene});
        if (*scene) {
            auto parent = runtime.with_native<SceneComponent>(object, [&](SceneComponent& component) {
                return runtime.reference_field(object, Access::parent(component));
            });
            if (!parent) return std::unexpected(parent.error());
            changes.push_back(ReferenceWrite{*parent, *null_scene});
        }
        return runtime.apply_reference_changes(changes);
    });
}
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
    for (const auto& component : *group) {
        auto owned = detail::component_data(runtime, component.id());
        if (!owned) return std::unexpected(owned.error());
        if (owned->owner != object) return error(ErrorCode::invalid_member, "Actor composition has an inconsistent owner.");
    }
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
    for (auto it = group->rbegin(); it != group->rend(); ++it) {
        auto destroyed = runtime.destroy(it->id());
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
            auto removable = detail::can_remove(runtime, object, *group);
            if (!removable) return std::unexpected(removable.error());
            auto actor_root = root_component(runtime, data->owner);
            if (!actor_root) return std::unexpected(actor_root.error());
            auto removed = detail::clear_component(runtime, data->owner, object,
                std::size_t(at - group->begin()), *actor_root == object);
            if (!removed) return std::unexpected(removed.error());
        }
    }
    auto destroyed = runtime.destroy(object);
    if (!destroyed) return std::unexpected(destroyed.error());
    return committed;
}

}
