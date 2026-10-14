#pragma once

#include "world/world.hpp"
#include <unordered_map>

namespace atelier::world::detail {

struct WorldLifetime { bool open{true}; };
struct EntityData { World* world{}; bool enabled{}; bool transitioning{}; bool begun{}; };
struct ComponentData { ObjectId owner; bool enabled{}, begun{}; };
struct FlagScope {
    bool& flag;
    explicit FlagScope(bool& value) : flag(value) { flag = true; }
    ~FlagScope() { flag = false; }
};
struct DispatchScope {
    std::size_t& depth;
    explicit DispatchScope(std::size_t& value) : depth(value) { ++depth; }
    ~DispatchScope() { --depth; }
};

struct Access {
    using State = World::Impl;
    static State& state(World& world) noexcept { return *world.impl_; }
    static const State& state(const World& world) noexcept { return *world.impl_; }
    static ReferenceSequence& sequence(Entity& value) noexcept { return value.components_; }
    static ReferenceSlot& owner(Component& value) noexcept { return value.owner_; }
    static ReferenceSequence& children(Entity& value) noexcept { return value.children_; }
    static ReferenceSlot& parent(Entity& value) noexcept { return value.parent_; }
    static ReferenceSlot& origin(Entity& value) noexcept { return value.origin_; }
    static bool& spatial(Entity& value) noexcept { return value.spatial_; }
    static bool& explicit_origin(Entity& value) noexcept { return value.explicit_origin_; }
    static bool& world_origin(Entity& value) noexcept { return value.world_origin_; }
    static geometry::MeshRef& geometry(StaticMeshComponent& value) noexcept { return value.geometry_; }
    static const geometry::MeshRef& geometry(const StaticMeshComponent& value) noexcept { return value.geometry_; }
    static String& mesh_source(StaticMeshComponent& value) noexcept { return value.mesh_source_; }
    static const String& mesh_source(const StaticMeshComponent& value) noexcept { return value.mesh_source_; }
    static ReferenceSequence& materials(StaticMeshComponent& value) noexcept {return value.materials_;}
    static std::vector<std::string>& material_slots(StaticMeshComponent& value) noexcept {return value.material_slots_;}
    static std::vector<bool>& material_overrides(StaticMeshComponent& value) noexcept {return value.material_overrides_;}
    static auto& surfaces(StaticMeshComponent& value) noexcept {return value.surfaces_;}
    static auto& surface_geometry(StaticMeshComponent& value) noexcept {return value.surface_geometry_;}
    static auto& surface_parameters(StaticMeshComponent& value) noexcept {return value.surface_parameters_;}
    static bool& begun(Component& value) noexcept { return value.begun_; }
    static bool& begun(Entity& value) noexcept { return value.begun_; }
    static World*& member_world(Entity& value) noexcept { return value.member_world_; }
    static bool& enabled(Entity& value) noexcept { return value.enabled_; }
    static bool& enabled(Component& value) noexcept { return value.enabled_; }
    static bool& transitioning(Entity& value) noexcept { return value.transitioning_; }
};

inline std::unexpected<Error> error(ErrorCode code, std::string message) { return std::unexpected(Error{code, std::move(message)}); }
Result<EntityData> entity_data(Runtime&, ObjectId);
Result<ComponentData> component_data(Runtime&, ObjectId);
Result<void> validate_tree(Runtime&, ObjectId entity, std::span<const ObjectRoot>);
Result<std::vector<ObjectRoot>> subtree(Runtime&, ObjectId entity);
Result<void> clear_component(Runtime&, ObjectId entity, ObjectId component, std::size_t index);
Result<void> unlink_parent(Runtime&, ObjectId entity);
Result<void> requires_space(Runtime&, ObjectId entity, std::span<const ObjectRoot>);
Result<bool> is(Runtime&, ObjectId, TypeId);
Result<void> validate_object(Runtime&, ObjectId, TypeId);
// True means this is a native Material/LightSettings path and was handled.
Result<bool> write_lighting_property(World&, ObjectId, std::span<const FieldId>, Scalar);
Result<void> queue_translation(Runtime&, ObjectId component, math::Vector3);

// A short-lived evaluation snapshot. Direct native/JIT writes remain valid:
// no cache survives its host phase and no cached TRS is an authoring source.
struct SpaceNode {
    ObjectId parent{};
    std::optional<ObjectId> origin;
    bool spatial{};
    Trs local;
    World* world{};
};
struct SpaceGraph {
    Runtime& runtime;
    std::unordered_map<ObjectId, SpaceNode> nodes;
    std::unordered_map<ObjectId, math::Matrix4> matrices;
    explicit SpaceGraph(Runtime& r) : runtime(r) {}
    Result<SpaceNode*> node(ObjectId);
    Result<ObjectId> parent(ObjectId);
    Result<math::Matrix4> matrix(ObjectId);
    void invalidate() { matrices.clear(); }
};

}

namespace atelier::world {

struct World::Impl {
    struct Position { std::size_t index{}, presentations{}; };
    struct Bucket { TypeId type; std::vector<ObjectId> objects; };
    struct Task { ObjectId object, entity; Coroutine coroutine; };
    struct Pending { std::uint64_t serial; Request request; ObjectId owner; bool member_required{}; };

    rendering::SceneLightingSettings lighting;
    Runtime& runtime;
    Types types;
    World& world;
    std::shared_ptr<detail::WorldLifetime> lifetime{std::make_shared<detail::WorldLifetime>()};
    bool running{}, closing{}, closed{}, transitioning{}, flushing{};
    std::size_t dispatch_depth{};
    std::vector<Member> members;
    std::unordered_map<ObjectId, Position> positions;
    std::vector<Bucket> buckets;
    struct Scheduled { ObjectId object, entity; const Behavior* behavior{}; };
    std::vector<Scheduled> ordered, updates;
    std::vector<ObjectId> presentation;
    std::vector<Task> tasks;
    struct Translation { ObjectId component, entity; math::Vector3 delta; };
    std::vector<Translation> translations;
    struct TranslationChange { ObjectId entity; Trs value; };
    std::vector<TranslationChange> translation_changes;
    Result<void> apply_translations();
    std::vector<Pending> pending;
    std::uint64_t next_request{1};
    struct Binding { TypeId type; Behavior behavior; };
    std::vector<Binding> behaviors;
    Result<const Behavior*> behavior(TypeId) const;
    LifecycleObserver observer;
    struct Subscription { std::uint64_t id; std::shared_ptr<LifecycleObserver> observer; };
    std::vector<Subscription> subscriptions;
    std::uint64_t next_subscription{1};

    Impl(Runtime& r, const Types& t, World& w) : runtime(r), types(t), world(w) {}
    Result<void> admit(bool mutation = true) const;
    Result<void> admit_edit(ObjectId) const;
    Result<std::size_t> member(ObjectId) const;
    Result<ObjectId> component_owner(ObjectId) const;
    // Admits same-context detached composition as well as this World's members.
    Result<detail::EntityData> composition_entity(ObjectId) const;
    Result<detail::ComponentData> composition_component(ObjectId) const;
    Result<std::size_t> prepare_bucket(TypeId, std::size_t additional);
    void prepare_activity(std::size_t additional = 0);
    Result<void> refresh();
    // Validate prospective membership/settings before publishing a second sky.
    Result<void> validate_sky_light(std::span<const ObjectRoot> additions = {}, ObjectId edited = {}, const LightSettings* replacement = nullptr) const;
    void cancel_tasks(ObjectId target = {}, bool entity = true) noexcept;
    void halt() noexcept;
    std::optional<Error> begin(ObjectId entity = {}, ObjectId component = {});
    std::optional<Error> end(ObjectId entity = {}, ObjectId component = {});
    std::optional<Error> invoke(const Scheduled&, bool beginning);
    Result<bool> begun(const Scheduled&) const;
    Result<void> mark_begun(const Scheduled&, bool);
    std::optional<Error> shutdown();
    std::optional<Error> notify(Lifecycle, ObjectId);
    Commit finish(bool changed, std::optional<Error> failure = {});
    void erase_indexes(ObjectId component) noexcept;
};

}
