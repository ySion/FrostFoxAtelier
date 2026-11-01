#include "world/detail.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <unordered_map>
#include <unordered_set>

namespace atelier::world {
using detail::Access;

using detail::error;

Result<Types> register_types(Runtime& runtime) {
    const auto vector = runtime.native_type<math::Vector3>();
    const auto quaternion = runtime.native_type<math::Quaternion>();
    if (!vector)
        return std::unexpected(vector.error());
    if (!quaternion)
        return std::unexpected(quaternion.error());
    std::vector<TypeDefinition> definitions;
    definitions.push_back(reflection::Definition<Entity>::build(runtime));
    definitions.push_back(reflection::Definition<Component>::build(runtime));
    SignatureDefinition translation;
    translation.parameters.resize(3, ParameterDefinition{{TypeReference::native<float>()}});
    // The native method collects values only: safe for a generated caller with
    // borrowed fields. World applies them after every generated call has returned.
    definitions.back().methods.push_back({"translate_owner", std::move(translation),
                                          [](CallContext& call) -> Result<void> {
                                              return detail::queue_translation(call.runtime, call.self,
                                                                               {call.arguments[0].get<float>(),
                                                                                call.arguments[1].get<float>(),
                                                                                call.arguments[2].get<float>()});
                                          },
                                          true});
    // Requests only enqueue; the World applies them when the current phase ends.
    SignatureDefinition toggle;
    toggle.parameters.push_back(ParameterDefinition{{TypeReference::native<bool>()}});
    const auto requests = [&](TypeDefinition& definition, Invoker destroy, Invoker enable) {
        definition.methods.push_back({"destroy", {}, destroy, true});
        definition.methods.push_back({"set_enabled", toggle, enable, true});
    };
    requests(
        definitions[0],
        [](CallContext& call) -> Result<void> {
            return detail::queue_request(call.runtime, call.self, RequestKind::destroy_entity, false);
        },
        [](CallContext& call) -> Result<void> {
            return detail::queue_request(call.runtime, call.self, RequestKind::entity_enabled,
                                         call.arguments[0].get<bool>());
        });
    requests(
        definitions[1],
        [](CallContext& call) -> Result<void> {
            return detail::queue_request(call.runtime, call.self, RequestKind::destroy_component, false);
        },
        [](CallContext& call) -> Result<void> {
            return detail::queue_request(call.runtime, call.self, RequestKind::component_enabled,
                                         call.arguments[0].get<bool>());
        });
    definitions.push_back(reflection::Definition<ShapeKind>::build(runtime));
    definitions.push_back(reflection::Definition<MotionKind>::build(runtime));
    definitions.push_back(reflection::Definition<Response>::build(runtime));
    definitions.push_back(reflection::Definition<PhysicsMaterial>::build(runtime));
    definitions.push_back(reflection::Definition<BodySettings>::build(runtime));
    definitions.back().validate = [](const void* value) {
        return detail::body_rules(*static_cast<const BodySettings*>(value));
    };
    definitions.push_back(reflection::Definition<PrimitiveComponent>::build(runtime));
    definitions.push_back(reflection::Definition<CollisionComponent>::build(runtime));
    definitions.push_back(reflection::Definition<SpatialComponent>::build(runtime));
    auto ids = runtime.register_types(std::move(definitions));
    if (!ids)
        return std::unexpected(ids.error());
    Types types;
    types.vector3 = *vector;
    types.quaternion = *quaternion;
    types.entity = (*ids)[0];
    types.component = (*ids)[1];
    types.primitive_component = (*ids)[7];
    types.collision_component = (*ids)[8];
    types.spatial_component = (*ids)[9];
    const auto make_paths = [&](auto& paths, TypeId record, std::string_view name) -> Result<void> {
        auto outer = runtime.find_field(types.entity, name);
        if (!outer)
            return std::unexpected(outer.error());
        constexpr std::array names{"x", "y", "z", "w"};
        for (std::size_t i = 0; i < paths.size(); ++i) {
            auto field = runtime.find_field(record, names[i]);
            if (!field)
                return std::unexpected(field.error());
            paths[i] = {*outer, *field};
        }
        return {};
    };
    if (auto result = make_paths(types.position_fields, types.vector3, "position"); !result)
        return std::unexpected(result.error());
    if (auto result = make_paths(types.rotation_fields, types.quaternion, "rotation"); !result)
        return std::unexpected(result.error());
    if (auto result = make_paths(types.scale_fields, types.vector3, "scale"); !result)
        return std::unexpected(result.error());
    return types;
}

Response BodySettings::response(std::uint32_t channel) const noexcept {
    if (channel >= 16)
        return Response::ignore;
    const auto bit = std::uint32_t{1} << channel;
    return ignore_categories & bit ? Response::ignore : overlap_categories & bit ? Response::overlap : Response::block;
}

Result<void> BodySettings::set_response(std::uint32_t channel, Response value) {
    if (channel >= 16 || (value != Response::ignore && value != Response::overlap && value != Response::block))
        return error(ErrorCode::argument_mismatch, "Invalid collision channel or response.");
    const auto bit = std::uint32_t{1} << channel;
    ignore_categories = (ignore_categories & ~bit) | (value == Response::ignore ? bit : 0);
    overlap_categories = (overlap_categories & ~bit) | (value == Response::overlap ? bit : 0);
    return {};
}

// Single-field bounds are AFIELD hard bounds, checked before this runs.
Result<void> detail::body_rules(const BodySettings& value) {
    if (value.shape != ShapeKind::box && value.shape != ShapeKind::sphere && value.shape != ShapeKind::triangle_mesh &&
        value.shape != ShapeKind::capsule)
        return error(ErrorCode::argument_mismatch, "Unknown collision shape.");
    if (value.motion != MotionKind::stationary && value.motion != MotionKind::kinematic &&
        value.motion != MotionKind::dynamic)
        return error(ErrorCode::argument_mismatch, "Unknown body motion type.");
    if (value.half_extents.x == 0 || value.half_extents.y == 0 || value.half_extents.z == 0 || value.radius == 0 ||
        value.capsule_half_height == 0)
        return error(ErrorCode::argument_mismatch, "Collision dimensions must be positive.");
    if ((value.ignore_categories & value.overlap_categories) != 0)
        return error(ErrorCode::argument_mismatch, "A collision category cannot be both ignored and overlapping.");
    if (value.shape == ShapeKind::triangle_mesh && value.motion != MotionKind::stationary)
        return error(ErrorCode::argument_mismatch, "Triangle mesh collision requires a stationary body.");
    return {};
}

Result<void> validate_body_settings(Runtime& runtime, const BodySettings& value) {
    return detail::check_value(runtime, value);
}

namespace detail {
Result<EntityData> entity_data(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const Entity>(object, [](const Entity& entity) -> Result<EntityData> {
        return EntityData{Access::member_world(entity), Access::enabled(entity), Access::transitioning(entity)};
    });
}

Result<ComponentData> component_data(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const Component>(object, [](const Component& component) -> Result<ComponentData> {
        return ComponentData{Access::owner(component).id(), Access::enabled(component)};
    });
}

Result<bool> is(Runtime& runtime, ObjectId object, TypeId expected) {
    auto actual = runtime.type_of(object);
    if (!actual)
        return std::unexpected(actual.error());
    return runtime.is_a(*actual, expected);
}

Result<void> validate_object(Runtime& runtime, ObjectId object, TypeId expected) {
    auto matches = is(runtime, object, expected);
    if (!matches)
        return std::unexpected(matches.error());
    if (!*matches)
        return error(ErrorCode::type_mismatch, "Object does not derive from the required World type.");
    return {};
}

}

Result<ObjectRoot> create_entity(Runtime& runtime, TypeId actual) {
    if (auto ready = runtime.check_code_publication(); !ready)
        return std::unexpected(ready.error());
    auto base = runtime.native_type<Entity>();
    if (!base)
        return std::unexpected(base.error());
    if (actual == TypeId{})
        actual = *base;
    auto matches = runtime.is_a(actual, *base);
    if (!matches)
        return std::unexpected(matches.error());
    if (!*matches)
        return error(ErrorCode::type_mismatch, "create_entity requires an Entity type.");
    return runtime.create(actual);
}

Result<ObjectRoot> create_component(Runtime& runtime, TypeId actual) {
    if (auto ready = runtime.check_code_publication(); !ready)
        return std::unexpected(ready.error());
    auto base = runtime.native_type<Component>();
    if (!base)
        return std::unexpected(base.error());
    auto matches = runtime.is_a(actual, *base);
    if (!matches)
        return std::unexpected(matches.error());
    if (!*matches)
        return error(ErrorCode::type_mismatch, "create_component requires a Component type.");
    return runtime.create(actual);
}

Result<EntityStatus> inspect_entity(Runtime& runtime, ObjectId object) {
    if (object == ObjectId{})
        return EntityStatus{};
    if (!runtime.belongs_to_context(object))
        return error(ErrorCode::invalid_object, "Entity identity belongs to another Runtime.");
    auto type = runtime.type_of(object);
    if (!type) {
        if (type.error().code == ErrorCode::invalid_object)
            return EntityStatus{};
        return std::unexpected(type.error());
    }
    auto data = detail::entity_data(runtime, object);
    if (!data)
        return std::unexpected(data.error());
    if (data->transitioning)
        return error(ErrorCode::busy, "Entity is transitioning.");
    auto eligible = effective_enabled(runtime, object);
    if (!eligible)
        return std::unexpected(eligible.error());
    EntityStatus status{EntityState::detached, *type, data->enabled, false, *eligible};
    if (data->world) {
        const auto& state = Access::state(*data->world);
        if (auto ready = state.admit(false); !ready)
            return std::unexpected(ready.error());
        const auto* member = state.state_of(object);
        if (!member)
            return std::unexpected(state.inconsistent("World member index is inconsistent."));
        status.state =
            state.running && member->begun && *eligible ? EntityState::active : EntityState::attached_inactive;
        status.can_edit = !state.dispatch_depth;
    }
    return status;
}

Result<ComponentStatus> inspect_component(Runtime& runtime, ObjectId object) {
    if (object == ObjectId{})
        return ComponentStatus{};
    if (!runtime.belongs_to_context(object))
        return error(ErrorCode::invalid_object, "Component identity belongs to another Runtime.");
    auto type = runtime.type_of(object);
    if (!type) {
        if (type.error().code == ErrorCode::invalid_object)
            return ComponentStatus{};
        return std::unexpected(type.error());
    }
    auto data = detail::component_data(runtime, object);
    if (!data)
        return std::unexpected(data.error());
    ComponentStatus status{true, *type, data->owner, data->enabled, false};
    if (data->owner != ObjectId{}) {
        auto entity = detail::entity_data(runtime, data->owner);
        if (!entity)
            return std::unexpected(entity.error());
        if (entity->transitioning)
            return error(ErrorCode::busy, "Component owner is transitioning.");
        if (entity->world) {
            const auto& state = Access::state(*entity->world);
            if (auto ready = state.admit(false); !ready)
                return std::unexpected(ready.error());
            // Components without a game updater still have activation eligibility.
            const auto* member = state.state_of(data->owner);
            if (!member)
                return std::unexpected(state.inconsistent("World member index is inconsistent."));
            const auto item = std::ranges::find(member->components, object, &detail::Access::State::ComponentState::id);
            if (item == member->components.end())
                return std::unexpected(state.inconsistent("World component index is inconsistent."));
            status.active = state.running && item->begun && member->eligible && data->enabled;
        }
    }
    return status;
}

Result<std::vector<ObjectRoot>> components(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const Entity>(object, [&](const Entity& entity) -> Result<std::vector<ObjectRoot>> {
        if (Access::transitioning(entity))
            return error(ErrorCode::busy, "Entity is transitioning.");
        if (auto* world = Access::member_world(entity)) {
            if (auto ready = Access::state(*world).admit(false); !ready)
                return std::unexpected(ready.error());
        }
        auto field = runtime.reference_field(object, Access::sequence(entity));
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
            auto root = runtime.hold(*reference);
            if (!root)
                return std::unexpected(root.error());
            result.push_back(std::move(*root));
        }
        return result;
    });
}

Result<Trs> read_transform(Runtime& runtime, ObjectId object) {
    return runtime.with_native<const Entity>(object, [](const Entity& entity) -> Result<Trs> {
        if (!Access::spatial(entity))
            return error(ErrorCode::invalid_member, "Entity has no Transform.");
        return Trs{entity.position, entity.rotation, entity.scale};
    });
}

Result<World*> admit_component_edit(Runtime& runtime, ObjectId object) {
    auto component = detail::component_data(runtime, object);
    if (!component)
        return std::unexpected(component.error());
    if (component->owner == ObjectId{})
        return nullptr;
    auto owner = detail::entity_data(runtime, component->owner);
    if (!owner)
        return std::unexpected(owner.error());
    if (owner->transitioning)
        return error(ErrorCode::busy, "Entity is transitioning.");
    if (owner->world)
        if (auto ready = Access::state(*owner->world).admit(); !ready)
            return std::unexpected(ready.error());
    return owner->world;
}

Result<Commit> destroy_entity(Runtime& runtime, ObjectId object) {
    if (auto ready = runtime.check_code_publication(); !ready)
        return std::unexpected(ready.error());
    auto data = detail::entity_data(runtime, object);
    if (!data)
        return std::unexpected(data.error());
    if (data->transitioning)
        return error(ErrorCode::busy, "Entity is transitioning.");
    if (data->world) {
        if (auto ready = Access::state(*data->world).admit(); !ready)
            return std::unexpected(ready.error());
    }
    // Hold and validate the complete composition before any exit or unlink.
    auto owned = detail::subtree(runtime, object);
    if (!owned)
        return std::unexpected(owned.error());
    std::vector<std::vector<ObjectRoot>> groups;
    groups.reserve(owned->size());
    std::unordered_map<ObjectId, bool> component_ids;
    for (const auto& root : *owned) {
        auto entity = detail::entity_data(runtime, root.id());
        if (!entity)
            return std::unexpected(entity.error());
        if (entity->transitioning)
            return error(ErrorCode::busy, "Subtree Entity is transitioning.");
        if (entity->world != data->world)
            return error(ErrorCode::invalid_member, "Subtree has inconsistent World membership.");
        auto group = components(runtime, root.id());
        if (!group)
            return std::unexpected(group.error());
        for (const auto& component : *group) {
            auto capability = detail::component_data(runtime, component.id());
            if (!capability)
                return std::unexpected(capability.error());
            if (capability->owner != root.id() || !component_ids.emplace(component.id(), true).second)
                return error(ErrorCode::invalid_member, "Subtree has an inconsistent or duplicate component owner.");
        }
        groups.push_back(std::move(*group));
    }
    auto entity_type = runtime.native_type<Entity>();
    if (!entity_type)
        return std::unexpected(entity_type.error());
    auto null_owner = runtime.null_reference(*entity_type);
    auto null_weak = runtime.null_reference(*entity_type, ValueForm::weak_reference);
    if (!null_owner)
        return std::unexpected(null_owner.error());
    if (!null_weak)
        return std::unexpected(null_weak.error());
    Commit committed{true, {}};
    if (data->world) {
        auto removed = data->world->detach(object);
        if (!removed)
            return std::unexpected(removed.error());
        committed.notification_error = std::move(removed->commit.notification_error);
    } else {
        // A detached child remains owned until explicitly removed from its parent.
        auto unlinked = detail::unlink_parent(runtime, object);
        if (!unlinked)
            return std::unexpected(unlinked.error());
    }
    const auto cleanup = [&](auto&& operation) {
        try {
            auto result = operation();
            if (!result && !committed.notification_error)
                committed.notification_error = result.error();
        } catch (const std::exception& exception) {
            if (!committed.notification_error)
                committed.notification_error = Error{ErrorCode::invocation_failed, exception.what()};
        } catch (...) {
            if (!committed.notification_error)
                committed.notification_error =
                    Error{ErrorCode::invocation_failed, "Entity termination cleanup threw an unknown exception."};
        }
    };
    // Clear all owning and observing edges while every target is still held.
    // An outer native borrow protects the reference locators for each batch.
    for (std::size_t i = 0; i < owned->size(); ++i) {
        const auto id = (*owned)[i].id();
        cleanup([&] {
            return runtime.with_native<Entity>(id, [&](Entity& entity) -> Result<void> {
                Access::transitioning(entity) = true;
                auto sequence = runtime.reference_field(id, Access::sequence(entity));
                auto child_sequence = runtime.reference_field(id, Access::children(entity));
                auto parent = runtime.reference_field(id, Access::parent(entity));
                auto origin = runtime.reference_field(id, Access::origin(entity));
                if (!sequence)
                    return std::unexpected(sequence.error());
                if (!child_sequence)
                    return std::unexpected(child_sequence.error());
                if (!parent)
                    return std::unexpected(parent.error());
                if (!origin)
                    return std::unexpected(origin.error());
                auto count = runtime.sequence_size(*child_sequence);
                if (!count)
                    return std::unexpected(count.error());
                std::vector<ReferenceChange> changes;
                changes.reserve(groups[i].size() + 4);
                changes.push_back(SequenceSplice{*sequence, 0, groups[i].size(), {}});
                changes.push_back(SequenceSplice{*child_sequence, 0, *count, {}});
                changes.push_back(ReferenceWrite{*parent, *null_weak});
                changes.push_back(ReferenceWrite{*origin, *null_weak});
                for (const auto& component : groups[i]) {
                    auto owner = runtime.with_native<Component>(component.id(), [&](Component& value) {
                        return runtime.reference_field(component.id(), Access::owner(value));
                    });
                    if (!owner)
                        return std::unexpected(owner.error());
                    changes.push_back(ReferenceWrite{*owner, *null_owner});
                }
                auto cleared = runtime.apply_reference_changes(changes);
                if (!cleared)
                    return cleared;
                Access::explicit_origin(entity) = false;
                Access::world_origin(entity) = false;
                return {};
            });
        });
    }
    // Reverse Entity/components/children preorder. Once exit has committed,
    // cleanup failures must not republish any part of the subtree as live.
    for (std::size_t i = owned->size(); i-- > 0;) {
        for (auto it = groups[i].rbegin(); it != groups[i].rend(); ++it)
            cleanup([&] { return runtime.destroy(it->id()); });
        cleanup([&] { return runtime.destroy((*owned)[i].id()); });
    }
    return committed;
}

Result<Commit> destroy_component(Runtime& runtime, ObjectId object) {
    if (auto ready = runtime.check_code_publication(); !ready)
        return std::unexpected(ready.error());
    auto data = detail::component_data(runtime, object);
    if (!data)
        return std::unexpected(data.error());
    auto root = runtime.hold(object);
    if (!root)
        return std::unexpected(root.error());
    ObjectRoot owner_root;
    Commit committed{true, {}};
    bool removed{};
    if (data->owner != ObjectId{}) {
        auto entity = detail::entity_data(runtime, data->owner);
        if (!entity)
            return std::unexpected(entity.error());
        if (entity->transitioning)
            return error(ErrorCode::busy, "Component owner is transitioning.");
        auto held = runtime.hold(data->owner);
        if (!held)
            return std::unexpected(held.error());
        owner_root = std::move(*held);
        if (entity->world) {
            auto removal = entity->world->remove_component(object);
            if (!removal)
                return std::unexpected(removal.error());
            committed.notification_error = std::move(removal->commit.notification_error);
        } else {
            auto group = components(runtime, data->owner);
            if (!group)
                return std::unexpected(group.error());
            const auto at = std::ranges::find_if(*group, [&](const ObjectRoot& item) { return item.id() == object; });
            if (at == group->end())
                return error(ErrorCode::invalid_member, "Component is absent from its owner's composition.");
            auto cleared = detail::clear_component(runtime, data->owner, object, std::size_t(at - group->begin()));
            if (!cleared)
                return std::unexpected(cleared.error());
        }
        removed = true;
    }
    auto destroyed = runtime.destroy(object);
    if (!destroyed) {
        if (!removed)
            return std::unexpected(destroyed.error());
        if (!committed.notification_error)
            committed.notification_error = destroyed.error();
    }
    return committed;
}

}
