#pragma once

#include "runtime/execution.hpp"
#include "runtime/reflection.hpp"
#include "world/math.hpp"
#include "geometry/mesh.hpp"
#include "foundation/string.hpp"
#include <limits>
#include "world/world.gen.hpp"

namespace atelier::world {

class World;
namespace detail { struct Access; struct WorldLifetime; }

// Native payloads participate in the same Runtime type hierarchy and GC graph.
// Their relation fields are private and registered as hidden reference fields.
ACLASS(Name="world.Actor")
class Actor {
    AGENERATED_BODY(Actor)
public:
    Actor() = default;
    Actor(const Actor&) = delete;
    Actor& operator=(const Actor&) = delete;
private:
    friend struct detail::Access;
    AFIELD(Name="components", Hidden, Sequence="world.Component")
    ReferenceSequence components_;
    AFIELD(Name="root", Hidden, Weak="world.SceneComponent")
    ReferenceSlot root_;
    World* member_world_{};
    bool enabled_{true};
    bool begun_{};
    bool transitioning_{};
};

// A real reflected Actor type. Its complete default composition is built by
// create_static_mesh_actor, while Runtime construction establishes storage only.
ACLASS(Name="world.StaticMeshActor")
class StaticMeshActor : public Actor {
    AGENERATED_BODY(StaticMeshActor)
};

ACLASS(Name="world.Component")
class Component {
    AGENERATED_BODY(Component)
public:
    Component() = default;
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
private:
    friend struct detail::Access;
    AFIELD(Name="owner", Hidden, Strong="world.Actor")
    ReferenceSlot owner_;
    bool enabled_{true};
    bool begun_{};
};

ACLASS(Name="world.SceneComponent")
class SceneComponent : public Component {
    AGENERATED_BODY(SceneComponent)
public:
    AFIELD()
    math::Vector3 position;
    AFIELD()
    math::Quaternion rotation;
    AFIELD()
    math::Vector3 scale{1.f, 1.f, 1.f};
private:
    friend struct detail::Access;
    AFIELD(Name="parent", Hidden, Weak="world.SceneComponent")
    ReferenceSlot parent_;
};
AENUM(Name="world.CollisionShape")
enum class ShapeKind : std::uint32_t { box, sphere, triangle_mesh, capsule };
AENUM(Name="world.BodyMotion")
enum class MotionKind : std::uint32_t { stationary, kinematic, dynamic };
AENUM(Name="world.CollisionResponse")
enum class Response : std::uint32_t { ignore, overlap, block };

ASTRUCT(Name="world.PhysicsMaterial")
struct PhysicsMaterial {
    AGENERATED_BODY(PhysicsMaterial)
public:
    AFIELD()
    float friction{.2f};
    AFIELD()
    float restitution{};
    bool operator==(const PhysicsMaterial&) const = default;
};

// Authoring values belong to the World type system. Backend state is scene-owned.
ASTRUCT(Name="world.BodySettings")
struct BodySettings {
    AGENERATED_BODY(BodySettings)
public:
    AFIELD()
    bool enabled{true};
    AFIELD()
    ShapeKind shape{ShapeKind::box};
    AFIELD()
    MotionKind motion{MotionKind::stationary};
    AFIELD()
    math::Vector3 half_extents{.5f, .5f, .5f};
    AFIELD()
    float radius{.5f};
    AFIELD()
    float capsule_half_height{.5f};
    AFIELD()
    std::uint32_t category{};
    // Sixteen categories; disjoint masks encode Ignore/Overlap, otherwise Block.
    AFIELD()
    std::uint32_t ignore_categories{};
    AFIELD()
    std::uint32_t overlap_categories{};
    AFIELD()
    bool sensor{};
    AFIELD()
    PhysicsMaterial material{};
    // Derived cache, rebuilt from the mesh resource; never archived as object state.
    std::shared_ptr<const std::string> cooked_shape{};
    [[nodiscard]] Response response(std::uint32_t category) const noexcept;
    [[nodiscard]] Result<void> set_response(std::uint32_t category, Response);
    bool operator==(const BodySettings&) const = default;
};
[[nodiscard]] Result<void> validate_body_settings(const BodySettings&);

ACLASS(Name="world.PrimitiveComponent")
class PrimitiveComponent : public SceneComponent {
    AGENERATED_BODY(PrimitiveComponent)
public:
    AFIELD()
    BodySettings body{.enabled=false};
};

// A spatial collision primitive without presentation. Choose box/sphere/capsule
// through the same reflected body settings used by StaticMeshComponent.
ACLASS(Name="world.CollisionComponent")
class CollisionComponent : public PrimitiveComponent {
    AGENERATED_BODY(CollisionComponent)
public:
    CollisionComponent() { body.enabled=true; }
};

ASTRUCT(Name="world.Material")
struct Material {
    AGENERATED_BODY(Material)
public:
    AFIELD()
    math::Vector3 base_color{.19f, .42f, .57f};
    AFIELD()
    float roughness{.65f};
    AFIELD()
    float metallic{};
    AFIELD()
    math::Vector3 emissive{};
    bool operator==(const Material&) const = default;
};
ACLASS(Name="world.StaticMeshComponent")
class StaticMeshComponent : public PrimitiveComponent {
    AGENERATED_BODY(StaticMeshComponent)
public:
    AFIELD()
    Material material;
private:
    friend struct detail::Access;
    AFIELD(Name="mesh_source", Hidden)
    String mesh_source_;
    geometry::MeshRef geometry_;
};

AENUM(Name="world.LightKind")
enum class LightKind : std::uint32_t { point, rectangle, directional, sky };

ASTRUCT(Name="world.LightSettings")
struct LightSettings {
    AGENERATED_BODY(LightSettings)
public:
    AFIELD()
    LightKind kind{LightKind::point};
    AFIELD()
    math::Vector3 color{1.f,1.f,1.f};
    // Reference-white intensity: point cd, rectangle/sky cd/m^2, directional lux.
    // Sky is uniform incident radiance; spatial transform and emitter shape do not affect it.
    AFIELD()
    float intensity{1.f};
    AFIELD()
    float range{10.f};
    // Point: metres. Directional: angular radius in radians, below pi/2.
    // Rectangle emission size uses width/height instead.
    AFIELD()
    float source_radius{};
    AFIELD()
    float width{1.f};
    AFIELD()
    float height{1.f};
    AFIELD()
    bool two_sided{};
    AFIELD()
    bool casts_shadow{true};
    bool operator==(const LightSettings&) const = default;
};

ACLASS(Name="world.LightComponent")
class LightComponent : public SceneComponent {
    AGENERATED_BODY(LightComponent)
public:
    AFIELD()
    LightSettings settings;
};

[[nodiscard]] Result<void> validate_material(const Material&);
[[nodiscard]] Result<void> validate_light(const LightSettings&);
// Reads authored settings, including detached/disabled objects. Does not root them.
[[nodiscard]] Result<LightSettings> read_light(Runtime&, ObjectId light_component);

using FieldPath = std::vector<FieldId>;
struct Types {
    TypeId actor, component, scene_component, static_mesh_component, material;
    TypeId static_mesh_actor;
    TypeId vector3, quaternion;
    TypeId light_kind, light_settings, light_component;
    TypeId primitive_component, collision_component;
    std::array<std::array<FieldId, 2>, 3> position_fields;
    std::array<std::array<FieldId, 2>, 4> rotation_fields;
    std::array<std::array<FieldId, 2>, 3> scale_fields;
};
// Call once per Runtime, before object creation; additional derived definitions
// may be registered afterwards, before the Runtime closes registration.
[[nodiscard]] Result<Types> register_types(Runtime&);

enum class ActorState { detached, attached_inactive, active, invalid };
struct ActorStatus {
    ActorState state{ActorState::invalid};
    TypeId type;
    bool enabled{};
    bool can_edit{};
    // True when this member has at least one registered presentation component.
    // UI selection uses membership; an empty Actor can still be selected/edited.
    bool can_present{};
};
struct ComponentStatus {
    bool valid{};
    TypeId type;
    ObjectId owner;
    bool enabled{};
    bool active{};
    // True when this component has an actual World presentation registration.
    bool can_present{};
};

[[nodiscard]] Result<ObjectRoot> create_actor(Runtime&, TypeId actual = {});
[[nodiscard]] Result<ObjectRoot> create_component(Runtime&, TypeId actual);
struct StaticMeshActorDesc {
    geometry::MeshRef mesh;
    Trs transform{};
    Material material{};
    std::string_view mesh_source{};
};
// Returns a complete Detached Actor with one StaticMeshComponent as its root.
// A null mesh is valid and produces no presentation. No World registration,
// lifecycle notification or Begin occurs until the caller invokes World::attach.
// Requires an open World at a host boundary; a running World is supported.
[[nodiscard]] Result<ObjectRoot> create_static_mesh_actor(World&, const StaticMeshActorDesc&);
[[nodiscard]] Result<ActorStatus> inspect_actor(Runtime&, ObjectId);
[[nodiscard]] Result<ComponentStatus> inspect_component(Runtime&, ObjectId);
// These owning reference snapshots are ordinary roots, not a second relation graph.
[[nodiscard]] Result<std::vector<ObjectRoot>> components(Runtime&, ObjectId actor);
[[nodiscard]] Result<Trs> read_transform(Runtime&, ObjectId scene_component);
[[nodiscard]] Result<math::Matrix4> world_matrix(Runtime&, ObjectId scene_component);
[[nodiscard]] Result<std::optional<ObjectId>> root_component(Runtime&, ObjectId actor);

struct Commit {
    bool changed{};
    // A completed relation change is never reported as if it had not happened.
    // Consumers record that change, then surface a lifecycle/cleanup error.
    std::optional<Error> notification_error;
};
// The only game-object destruction entry. A detached object does not depend on
// its previous World. Attached objects first use their current World's exit chain.
[[nodiscard]] Result<Commit> destroy_actor(Runtime&, ObjectId);
[[nodiscard]] Result<Commit> destroy_component(Runtime&, ObjectId);

struct Member {
    ObjectRoot root;
    [[nodiscard]] ObjectId id() const noexcept { return root.id(); }
};
struct ActorRemoval {
    ObjectRoot root;
    std::size_t index{};
    Commit commit;
    [[nodiscard]] ObjectId id() const noexcept { return root.id(); }
private:
    friend class World;
    friend struct detail::Access;
    std::weak_ptr<detail::WorldLifetime> origin_;
};
struct ComponentRemoval {
    ObjectRoot root;
    ObjectId owner;
    std::size_t index{};
    ObjectId parent;
    bool was_root{};
    Commit commit;
    [[nodiscard]] ObjectId id() const noexcept { return root.id(); }
private:
    friend class World;
    friend struct detail::Access;
    std::weak_ptr<detail::WorldLifetime> origin_;
};
struct Attachment {
    ObjectId component;
    ObjectId parent{}; // Explicit same-Actor SceneComponent, possibly in this batch.
    bool root{};    // Explicit root designation; a root cannot have a parent.
};
struct RenderItem {
    ObjectId actor, component;
    math::Matrix4 world;
    geometry::MeshRef geometry;
    Material material;
};
struct LightRenderItem {
    ObjectId actor, component;
    math::Vector3 position;
    // Orthonormal world axes from the rotation chain, excluding scale/shear.
    // direction is propagation along local +Z; rectangle lies in local XY.
    math::Vector3 direction, right, up;
    // Range and physical dimensions are world metres, not scaled. Directional
    // source_radius is angular (radians); see LightSettings.
    LightSettings settings;
};

enum class ReparentRule { keep_local, keep_world };
[[nodiscard]] Result<ObjectId> scene_parent(Runtime&, ObjectId scene_component);
// Configure detached objects before attachment, or replace a member's immutable
// mesh resource at a host boundary. No callback/dispatch reentry is permitted.
[[nodiscard]] Result<void> set_geometry(Runtime&, ObjectId static_mesh_component, geometry::MeshRef);
struct MeshBinding { std::string source; geometry::MeshRef resolved; };
struct StaticMeshState { MeshBinding mesh; Material material; };
// Source and resolved immutable content change together. Bare set_geometry
// clears provenance. These snapshots also support detached and null-mesh data.
[[nodiscard]] Result<void> set_mesh_binding(Runtime&, ObjectId, MeshBinding);
[[nodiscard]] Result<StaticMeshState> read_static_mesh(Runtime&, ObjectId);

// World registration notifications: whole Actor groups enter in forward order
// and leave in reverse order. Detached composition does not emit these events.
enum class Lifecycle { attached, activated, deactivated, detached };
enum class RequestKind {
    attach_actor, detach_actor, attach_component, remove_component,
    destroy_actor, destroy_component, actor_enabled, component_enabled,
    start, stop, close
};
inline constexpr std::size_t append = std::numeric_limits<std::size_t>::max();
struct Request {
    RequestKind kind;
    ObjectId target{};
    ObjectId actor{};       // attach_component destination
    ObjectId parent{};      // attach_component spatial parent
    bool root{};
    std::size_t index{append};
    bool enabled{true};
};
struct RequestCompletion {
    std::uint64_t request;
    Result<Commit> result;
};
struct Activity {
    std::size_t update_registrations{};
    std::size_t presentation_registrations{};
    std::size_t tasks{};
};

// Same Runtime thread. Runtime outlives World, its detached objects and tasks.
// The World owns only member roots and execution/registration state. Actor's
// private member_world relation is authoritative; directories are indexes.
class World final {
public:
    struct Behavior {
        std::function<Result<void>(Runtime&, ObjectId)> begin{};
        std::function<Result<void>(Runtime&, ObjectId, float)> tick{};
        std::function<Result<void>(Runtime&, ObjectId)> end{};
    };
    using LifecycleObserver = std::function<Result<void>(World&, Lifecycle, ObjectId)>;
    using TaskStart = std::function<Result<Coroutine>(Runtime&, ObjectId)>;

    World(Runtime&, const Types&);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    [[nodiscard]] Runtime& runtime() const noexcept;
    [[nodiscard]] const Types& types() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] Activity activity() const noexcept;
    // Borrow ends at the next structural change. Queries during a transition fail.
    [[nodiscard]] Result<std::span<const Member>> members() const;
    [[nodiscard]] Result<std::vector<ObjectId>> query(TypeId, bool include_derived = true) const;
    [[nodiscard]] Result<Commit> attach(ObjectId actor, std::size_t index = append);
    [[nodiscard]] Result<ActorRemoval> detach(ObjectId actor);
    [[nodiscard]] Result<Commit> restore(const ActorRemoval&);
    // Detached Actors may be composed before World registration. The same
    // validation applies; no member/query/presentation or behavior is registered.
    [[nodiscard]] Result<Commit> attach_components(ObjectId actor,
        std::span<const Attachment>, std::size_t index = append);
    [[nodiscard]] Result<ComponentRemoval> remove_component(ObjectId component);
    [[nodiscard]] Result<Commit> restore(const ComponentRemoval&);
    [[nodiscard]] Result<Commit> set_enabled(ObjectId actor_or_component, bool);
    // Begin/Tick visit each Actor before its components in composition order.
    // End reverses that order and pairs only with completed Begin.
    // Enabled gates Tick, not Begin.
    // Dynamic insertion begins new components immediately; End uses current order.
    [[nodiscard]] Result<Commit> start();
    [[nodiscard]] Result<Commit> stop();
    [[nodiscard]] Result<Commit> close();

    // Editing requires an open, stopped World. Same-context detached objects
    // may be prepared; objects registered in another World are rejected.
    // Actor and Component properties use the same Runtime field paths. Transform
    // scalar writes preserve TRS validation; material color writes remain [0,1].
    [[nodiscard]] Result<Scalar> read(ObjectId actor_or_component, std::span<const FieldId>) const;
    [[nodiscard]] Result<void> write(ObjectId actor_or_component, std::span<const FieldId>, Scalar);
    [[nodiscard]] Result<void> set_transform(ObjectId scene_component, Trs);
    // Validated whole-value authoring. Same stopped/ownership policy as set_transform.
    [[nodiscard]] Result<void> set_material(ObjectId static_mesh_component, Material);
    [[nodiscard]] Result<void> set_light(ObjectId light_component, LightSettings);
    // Writes a SceneComponent's world pose between dispatch phases. Converts
    // through its parent and preserves authored scale; unrepresentable TRS fails.
    [[nodiscard]] Result<void> apply_simulation_transform(ObjectId component, math::Vector3 position,
        math::Quaternion rotation);
    // Simulation-owned visual children (ragdoll parts / wheels). Position and
    // rotation are local to their existing parent; authored scale is retained.
    [[nodiscard]] Result<void> apply_simulation_local_transform(ObjectId component, math::Vector3 position,
        math::Quaternion rotation);
    // Root remains explicit. Non-root nodes must stay below that same Actor root.
    // Removing a node with children is rejected; reparent its children first.
    [[nodiscard]] Result<Commit> reparent(ObjectId scene_component, ObjectId parent, ReparentRule);
    [[nodiscard]] Result<std::vector<RenderItem>> render_items() const;
    // Enabled members, in Actor/composition order. Stopped Worlds also present.
    [[nodiscard]] Result<std::vector<LightRenderItem>> light_items() const;

    // Stopped-only scheduling configuration, keyed by the Runtime's existing
    // type identity. The nearest ancestor binding is inherited as a whole.
    // Native method and source/JIT adapters run outside every native borrow.
    [[nodiscard]] Result<void> bind_behavior(TypeId actor_or_component_type, Behavior);
    [[nodiscard]] Result<void> observe_lifecycle(LifecycleObserver);
    // Unsubscribing during dispatch immediately prevents later calls, including
    // calls still pending in the current notification.
    [[nodiscard]] Result<std::uint64_t> subscribe_lifecycle(LifecycleObserver);
    void unsubscribe_lifecycle(std::uint64_t) noexcept;
    [[nodiscard]] Result<void> update(float amount);
    // The trusted starter uses the provided Runtime/receiver. World owns the
    // resulting task; deactivate/detach/stop/close cancels it before exit completes.
    [[nodiscard]] Result<void> start_task(ObjectId actor_or_component, const TaskStart&);

    // Request acceptance is not completion. Replay checks the original World's
    // membership/owner conditions; it never follows a target into another World.
    // Receipts contain Commit only. Queued detach/remove do not return history
    // mementos or retain objects; a consumer needing them must already own roots.
    [[nodiscard]] Result<std::uint64_t> request(Request);
    [[nodiscard]] Result<std::vector<RequestCompletion>> flush();

private:
    friend struct detail::Access;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
