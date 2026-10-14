#pragma once
#include "rendering/scene_lighting.hpp"

#include "runtime/execution.hpp"
#include "runtime/reflection.hpp"
#include "world/math.hpp"
#include "geometry/mesh.hpp"
#include "material/resources.hpp"
#include "foundation/string.hpp"
#include <limits>
#include <utility>
#include "world/world.gen.hpp"

namespace atelier::world {

class World;
namespace detail { struct Access; struct WorldLifetime; }

// Native payloads participate in the same Runtime type hierarchy and GC graph.
// Their relation fields are private and registered as hidden reference fields.
ACLASS(Name="world.Entity")
class Entity {
    AGENERATED_BODY(Entity)
public:
    Entity() = default;
    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    AFIELD(Label="Position (m)", Category="Transform")
    math::Vector3 position;
    AFIELD(Label="Rotation", Category="Transform")
    math::Quaternion rotation;
    AFIELD(Label="Scale", Category="Transform")
    math::Vector3 scale{1.f, 1.f, 1.f};
private:
    friend struct detail::Access;
    AFIELD(Name="components", Hidden, Sequence="world.Component")
    ReferenceSequence components_;
    AFIELD(Name="children", Hidden, Sequence="world.Entity")
    ReferenceSequence children_;
    AFIELD(Name="parent", Hidden, Weak="world.Entity")
    ReferenceSlot parent_;
    AFIELD(Name="origin", Hidden, Weak="world.Entity")
    ReferenceSlot origin_;
    bool spatial_{};
    bool explicit_origin_{};
    bool world_origin_{};
    World* member_world_{};
    bool enabled_{true};
    bool begun_{};
    bool transitioning_{};
};

// A real reflected Entity type. Its complete default composition is built by
// create_static_mesh_entity, while Runtime construction establishes storage only.
ACLASS(Name="world.StaticMeshEntity")
class StaticMeshEntity : public Entity {
    AGENERATED_BODY(StaticMeshEntity)
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
    AFIELD(Name="owner", Hidden, Strong="world.Entity")
    ReferenceSlot owner_;
    bool enabled_{true};
    bool begun_{};
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
class PrimitiveComponent : public Component {
    AGENERATED_BODY(PrimitiveComponent)
public:
    AFIELD(Category="Physics")
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
    AFIELD(Category="Material")
    Material material;
private:
    friend struct detail::Access;
    AFIELD(Name="mesh_source", Hidden)
    String mesh_source_;
    geometry::MeshRef geometry_;
    AFIELD(Name="materials", Hidden, Sequence="material.Material")
    ReferenceSequence materials_;
    std::vector<std::string> material_slots_;
    std::vector<bool> material_overrides_;
    std::shared_ptr<const std::vector<material::SurfaceRef>> surfaces_;
    geometry::MeshRef surface_geometry_;
    Material surface_parameters_;
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
class LightComponent : public Component {
    AGENERATED_BODY(LightComponent)
public:
    AFIELD(Category="Lighting")
    LightSettings settings;
};

[[nodiscard]] Result<void> validate_material(const Material&);
[[nodiscard]] Result<void> validate_light(const LightSettings&);
// Reads authored settings, including detached/disabled objects. Does not root them.
[[nodiscard]] Result<LightSettings> read_light(Runtime&, ObjectId light_component);

using FieldPath = std::vector<FieldId>;
struct Types {
    TypeId entity, component, static_mesh_component, material;
    TypeId static_mesh_entity;
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

enum class EntityState { detached, attached_inactive, active, invalid };
struct EntityStatus {
    EntityState state{EntityState::invalid};
    TypeId type;
    bool enabled{};
    bool can_edit{};
    // True when this member has at least one registered presentation component.
    // UI selection uses membership; an empty Entity can still be selected/edited.
    bool can_present{};
    bool effective_enabled{};
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

[[nodiscard]] Result<ObjectRoot> create_entity(Runtime&, TypeId actual = {});
[[nodiscard]] Result<ObjectRoot> create_component(Runtime&, TypeId actual);
struct StaticMeshEntityDesc {
    geometry::MeshRef mesh;
    Trs transform{};
    Material material{};
    std::string_view mesh_source{};
};
// Returns a complete Detached spatial Entity with one StaticMeshComponent.
// A null mesh is valid and produces no presentation. No World registration,
// lifecycle notification or Begin occurs until the caller invokes World::attach.
// Requires an open World at a host boundary; a running World is supported.
[[nodiscard]] Result<ObjectRoot> create_static_mesh_entity(World&, const StaticMeshEntityDesc&);
[[nodiscard]] Result<EntityStatus> inspect_entity(Runtime&, ObjectId);
[[nodiscard]] Result<ComponentStatus> inspect_component(Runtime&, ObjectId);
// These owning reference snapshots are ordinary roots, not a second relation graph.
[[nodiscard]] Result<std::vector<ObjectRoot>> components(Runtime&, ObjectId entity);
[[nodiscard]] Result<std::vector<ObjectRoot>> children(Runtime&, ObjectId entity);
[[nodiscard]] Result<ObjectId> parent_entity(Runtime&, ObjectId entity);
[[nodiscard]] Result<bool> has_transform(Runtime&, ObjectId entity);
// Evaluates an actual capability, including nonspatial Sky Lights.
[[nodiscard]] Result<bool> component_requires_space(Runtime&, ObjectId component);
[[nodiscard]] Result<bool> effective_enabled(Runtime&, ObjectId entity);
[[nodiscard]] Result<Trs> read_transform(Runtime&, ObjectId entity);
[[nodiscard]] Result<math::Matrix4> world_matrix(Runtime&, ObjectId entity);
// nullopt inherits the nearest spatial composition ancestor; a null ObjectId
// explicitly selects world space. A non-null identity is a weak spatial origin.
[[nodiscard]] Result<std::optional<ObjectId>> space_reference(Runtime&, ObjectId entity);
[[nodiscard]] Result<ObjectId> spatial_parent(Runtime&, ObjectId entity);

struct Commit {
    bool changed{};
    // A completed relation change is never reported as if it had not happened.
    // Consumers record that change, then surface a lifecycle/cleanup error.
    std::optional<Error> notification_error;
};
// The only game-object destruction entry. A detached object does not depend on
// its previous World. Attached objects first use their current World's exit chain.
[[nodiscard]] Result<Commit> destroy_entity(Runtime&, ObjectId);
[[nodiscard]] Result<Commit> destroy_component(Runtime&, ObjectId);

struct Member {
    ObjectRoot root;
    [[nodiscard]] ObjectId id() const noexcept { return root.id(); }
};
struct EntityRemoval {
    ObjectRoot root;
    std::size_t index{};
    Commit commit;
    ObjectId parent{};
    std::size_t sibling_index{};
    std::size_t member_count{1};
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
    Commit commit;
    [[nodiscard]] ObjectId id() const noexcept { return root.id(); }
private:
    friend class World;
    friend struct detail::Access;
    std::weak_ptr<detail::WorldLifetime> origin_;
};
struct Attachment {
    ObjectId component;
};
struct RenderItem {
    ObjectId entity, component;
    math::Matrix4 world;
    geometry::MeshRef geometry;
    Material material;
    std::shared_ptr<const std::vector<material::SurfaceRef>> surfaces{};
};
struct LightRenderItem {
    ObjectId entity, component;
    math::Vector3 position;
    // Orthonormal world axes from the rotation chain, excluding scale/shear.
    // direction is propagation along local +Z; rectangle lies in local XY.
    math::Vector3 direction, right, up;
    // Range and physical dimensions are world metres, not scaled. Directional
    // source_radius is angular (radians); see LightSettings.
    LightSettings settings;
};

enum class ReparentRule { keep_local, keep_world };
struct SimulationPose { ObjectId entity; math::Vector3 position; math::Quaternion rotation; };
// Configure detached objects before attachment, or replace a member's immutable
// mesh resource at a host boundary. No callback/dispatch reentry is permitted.
[[nodiscard]] Result<void> set_geometry(Runtime&, ObjectId static_mesh_component, geometry::MeshRef);
struct MeshBinding { std::string source; geometry::MeshRef resolved; };
struct StaticMeshState { MeshBinding mesh; Material material; };
// Source and resolved immutable content change together. Bare set_geometry
// clears provenance. These snapshots also support detached and null-mesh data.
[[nodiscard]] Result<void> set_mesh_binding(Runtime&, ObjectId, MeshBinding);
[[nodiscard]] Result<StaticMeshState> read_static_mesh(Runtime&, ObjectId);
struct MaterialBinding {
    std::string slot;
    ObjectRoot resource;
    bool overridden{true}; // Explicit instance assignment; false is a mesh default.
};
// Names, not slot positions, are the persistent correspondence. Empty imported
// names use the reserved slot/<index> spelling until assigned a named asset.
[[nodiscard]] std::string material_slot_name(const geometry::Mesh&,std::size_t);
[[nodiscard]] Result<std::vector<MaterialBinding>> read_material_bindings(Runtime&,ObjectId);
[[nodiscard]] Result<void> set_material_bindings(Runtime&,ObjectId,std::span<const MaterialBinding>);
[[nodiscard]] Result<std::shared_ptr<const std::vector<material::SurfaceRef>>> material_surfaces(Runtime&,ObjectId);

// World registration notifications: whole Entity groups enter in forward order
// and leave in reverse order. Detached composition does not emit these events.
enum class Lifecycle { attached, activated, deactivated, detached };
enum class RequestKind {
    attach_entity, detach_entity, attach_component, remove_component,
    destroy_entity, destroy_component, entity_enabled, component_enabled,
    start, stop, close
};
inline constexpr std::size_t append = std::numeric_limits<std::size_t>::max();
struct EntityAttachment {
    ObjectId entity;
    ObjectId parent{};
    // Addresses the pre-batch child sequence, or a World-root boundary/end in
    // the member directory. Equal positions retain the batch's input order.
    std::size_t index{append};
};
struct SpaceReference {
    ObjectId entity;
    // nullopt inherits; a null identity selects world space.
    std::optional<ObjectId> origin;
};
struct Request {
    RequestKind kind;
    ObjectId target{};
    ObjectId entity{};       // attach_component destination
    ObjectId parent{};      // attach_entity composition parent
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
// The World owns only member roots and execution/registration state. Entity's
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
    const rendering::SceneLightingSettings& lighting_settings() const noexcept;
    [[nodiscard]] Result<void> set_lighting_settings(const rendering::SceneLightingSettings&);
    [[nodiscard]] const Types& types() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] Activity activity() const noexcept;
    // Borrow ends at the next structural change. Queries during a transition fail.
    [[nodiscard]] Result<std::span<const Member>> members() const;
    [[nodiscard]] Result<std::vector<ObjectId>> query(TypeId, bool include_derived = true) const;
    [[nodiscard]] Result<Commit> attach(ObjectId entity, std::size_t index = append, ObjectId parent = {});
    [[nodiscard]] Result<Commit> attach_batch(std::span<const ObjectId> roots,
        std::size_t index = append, ObjectId parent = {});
    // Publishes an entire detached forest, including cross-root spatial refs.
    // Each parent is either an existing member of this World or null. Spatial
    // plans target incoming Entities only and publish with the entire forest.
    [[nodiscard]] Result<Commit> attach_batch(std::span<const EntityAttachment>,
        std::span<const SpaceReference> = {});
    // Validates the same final attachment graph without publishing it. Returns
    // the incoming spatial Entities' world matrices for derived resource setup.
    [[nodiscard]] Result<std::vector<std::pair<ObjectId, math::Matrix4>>> attachment_world_matrices(
        std::span<const EntityAttachment>, std::span<const SpaceReference> = {}) const;
    // Composition changes preserve identity. Both objects must be detached or
    // members of this World; adding a detached subtree to a member uses attach.
    // Detached keep-local prepares composition without evaluating unfinished
    // spatial relations; attachment validates the final graph. Keep-world
    // requires valid current/prospective space to preserve the world pose.
    [[nodiscard]] Result<Commit> reparent(ObjectId entity, ObjectId parent,
        ReparentRule = ReparentRule::keep_world, std::size_t index = append);
    // Spatial references may change at a host boundary while running. This
    // preserves composition, lifecycle pairing and tasks; keep-world preserves
    // the world pose. Invalid final space and callback reentry are rejected.
    [[nodiscard]] Result<Commit> set_space_reference(ObjectId entity,
        std::optional<ObjectId> origin, ReparentRule = ReparentRule::keep_world);
    [[nodiscard]] Result<EntityRemoval> detach(ObjectId entity);
    // Removes a forest together. Retained members may not reference departing
    // origins. Departing objects retain outward weak origins, whose Detached
    // matrices fail until a compatible complete attachment is restored.
    // Returned histories retain the input root order and the original positions.
    [[nodiscard]] Result<std::vector<EntityRemoval>> detach_batch(std::span<const ObjectId> roots);
    [[nodiscard]] Result<Commit> restore(const EntityRemoval&);
    // Restores histories from one forest removal through one attach transaction.
    [[nodiscard]] Result<Commit> restore(std::span<const EntityRemoval>);
    // Detached Entities may be composed before World registration. The same
    // validation applies; no member/query/presentation or behavior is registered.
    [[nodiscard]] Result<Commit> attach_components(ObjectId entity,
        std::span<const Attachment>, std::size_t index = append);
    [[nodiscard]] Result<ComponentRemoval> remove_component(ObjectId component);
    [[nodiscard]] Result<Commit> restore(const ComponentRemoval&);
    [[nodiscard]] Result<Commit> set_enabled(ObjectId entity_or_component, bool);
    // Begin/Tick visit each Entity before its components in composition order.
    // End reverses that order and pairs only with completed Begin.
    // Enabled gates Tick, not Begin.
    // Dynamic insertion begins new components immediately; End uses current order.
    [[nodiscard]] Result<Commit> start();
    [[nodiscard]] Result<Commit> stop();
    [[nodiscard]] Result<Commit> close();

    [[nodiscard]] Result<Scalar> read(ObjectId entity_or_component, std::span<const FieldId>) const;
    // Authoring requires an open World at a host boundary. Current running
    // members cannot be edited; same-context Detached objects may be prepared.
    // Objects registered in another World are rejected.
    // Entity and Component properties use the same Runtime field paths. Transform
    // scalar writes preserve TRS validation; material color writes remain [0,1].
    [[nodiscard]] Result<void> write(ObjectId entity_or_component, std::span<const FieldId>, Scalar);
    [[nodiscard]] Result<void> set_transform(ObjectId entity, Trs);
    [[nodiscard]] Result<void> remove_transform(ObjectId entity);
    // Validated whole-value authoring. Shares set_transform's edit eligibility.
    [[nodiscard]] Result<void> set_material(ObjectId static_mesh_component, Material);
    [[nodiscard]] Result<void> set_light(ObjectId light_component, LightSettings);
    // Writes Entity world poses between dispatch phases. The batch is validated
    // and converted parent-first before any TRS is published; scale is retained.
    [[nodiscard]] Result<void> apply_simulation_transform(ObjectId entity, math::Vector3 position,
        math::Quaternion rotation);
    [[nodiscard]] Result<void> apply_simulation_transforms(std::span<const SimulationPose>);
    [[nodiscard]] Result<std::vector<RenderItem>> render_items() const;
    // Enabled members, in Entity/composition order. Stopped Worlds also present.
    [[nodiscard]] Result<std::vector<LightRenderItem>> light_items() const;

    // Stopped-only scheduling configuration, keyed by the Runtime's existing
    // type identity. The nearest ancestor binding is inherited as a whole.
    // Native method and source/JIT adapters run outside every native borrow.
    [[nodiscard]] Result<void> bind_behavior(TypeId entity_or_component_type, Behavior);
    [[nodiscard]] Result<void> observe_lifecycle(LifecycleObserver);
    // Unsubscribing during dispatch immediately prevents later calls, including
    // calls still pending in the current notification.
    [[nodiscard]] Result<std::uint64_t> subscribe_lifecycle(LifecycleObserver);
    void unsubscribe_lifecycle(std::uint64_t) noexcept;
    [[nodiscard]] Result<void> update(float amount);
    // The trusted starter uses the provided Runtime/receiver. World owns the
    // resulting task; deactivate/detach/stop/close cancels it before exit completes.
    [[nodiscard]] Result<void> start_task(ObjectId entity_or_component, const TaskStart&);

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
