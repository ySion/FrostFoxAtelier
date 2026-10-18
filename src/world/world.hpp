#pragma once
#include "rendering/scene_lighting.hpp"

#include "runtime/execution.hpp"
#include "runtime/reflection.hpp"
#include "world/diagnostic.hpp"
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
ACLASS(Name="world.Entity", DESC="Scene 中的组成对象，可拥有 Component 和子 Entity。空间能力是可选的，组件从所属 Entity 取得空间信息。")
class Entity {
    AGENERATED_BODY(Entity)
public:
    Entity() = default;
    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    AFIELD(Label="Position (m)", Category="Transform", DESC="相对于当前 Space Reference 的位置，单位 m。改变它会移动 Entity 及依赖此空间参考的内容。")
    math::Vector3 position;
    AFIELD(Label="Rotation", Category="Transform", DESC="相对于当前 Space Reference 的姿态，以单位 Quaternion 保存。改变它会旋转 Entity 的局部坐标轴。")
    math::Quaternion rotation;
    AFIELD(Label="Scale", Category="Transform", DESC="沿局部 X、Y、Z 轴的缩放比例，无单位。它会影响网格及依赖此空间参考的子实体；灯光的世界尺寸另由灯参数控制。")
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
    bool transitioning_{};
};

// A real reflected Entity type. Its complete default composition is built by
// create_static_mesh_entity, while Runtime construction establishes storage only.
ACLASS(Name="world.StaticMeshEntity", DESC="用于静态网格的 Entity 类型。编辑器创建时会同时配置空间能力和 StaticMeshComponent；网格与材质保存在组件中。")
class StaticMeshEntity : public Entity {
    AGENERATED_BODY(StaticMeshEntity)
};

ACLASS(Name="world.Component", DESC="附着在 Entity 上的一项能力，拥有独立对象身份。组件不建立另一套空间节点，空间来自所属 Entity。")
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
};

AENUM(Name="world.CollisionShape", DESC="碰撞几何的种类：Box、Sphere、Capsule 或 Triangle Mesh。Triangle Mesh 使用网格资源，只支持 Stationary body。")
enum class ShapeKind : std::uint32_t { box, sphere, triangle_mesh, capsule };
AENUM(Name="world.BodyMotion", DESC="Stationary 固定不动；Kinematic 由程序指定运动；Dynamic 由物理模拟计算运动。")
enum class MotionKind : std::uint32_t { stationary, kinematic, dynamic };
AENUM(Name="world.CollisionResponse", DESC="Ignore 跳过碰撞；Overlap 产生接触信息但不阻挡；双方都选择 Block 时才求解阻挡。")
enum class Response : std::uint32_t { ignore, overlap, block };

ASTRUCT(Name="world.PhysicsMaterial", DESC="控制接触面的摩擦和回弹。它影响物理碰撞，不改变渲染材质。")
struct PhysicsMaterial {
    AGENERATED_BODY(PhysicsMaterial)
public:
    AFIELD(HardMin="0", DESC="接触面的 Friction 系数，无单位。较大值使物体更难沿接触面滑动，必须非负。")
    float friction{.2f};
    AFIELD(Min="0", Max="1", LockMin, LockMax, DESC="接触后的回弹系数，范围 0–1。0 不产生恢复性反弹，较大值保留更多回弹速度。")
    float restitution{};
    bool operator==(const PhysicsMaterial&) const = default;
};

// Authoring values belong to the World type system. Backend state is scene-owned.
ASTRUCT(Name="world.BodySettings", DESC="Primitive 的碰撞形状、运动方式和接触规则。配置随 Scene 保存，实际物理 body 由运行中的物理场景管理。")
struct BodySettings {
    AGENERATED_BODY(BodySettings)
public:
    AFIELD(DESC="是否为此 Component 启用物理 body。关闭它不会删除网格或改变 Entity 的显示开关。")
    bool enabled{true};
    AFIELD(DESC="选择碰撞几何。Box 使用 Half Extents；Sphere 和 Capsule 使用 Radius；Triangle Mesh 使用组件的网格资源。")
    ShapeKind shape{ShapeKind::box};
    AFIELD(DESC="Stationary 固定不动；Kinematic 由程序移动；Dynamic 受物理模拟驱动。Triangle Mesh 只能使用 Stationary。")
    MotionKind motion{MotionKind::stationary};
    AFIELD(HardMin="0", DESC="Box 从中心到各面的局部半尺寸，单位 m。最终碰撞尺寸还会应用 Entity 的世界缩放；每个分量必须大于 0。")
    math::Vector3 half_extents{.5f, .5f, .5f};
    AFIELD(HardMin="0", DESC="Sphere 或 Capsule 的局部半径，单位 m，必须大于 0。这两种形状要求 Entity 的世界缩放均匀。")
    float radius{.5f};
    AFIELD(HardMin="0", DESC="Capsule 中段沿局部 Y 轴的一半长度，不含两端半球，单位 m。最终尺寸会应用均匀世界缩放。")
    float capsule_half_height{.5f};
    AFIELD(Min="0", Max="15", LockMin, LockMax, DESC="此 body 的 Collision Category 编号，范围 0–15。其他 body 用此编号决定 Ignore、Overlap 或 Block。")
    std::uint32_t category{};
    // Sixteen categories; disjoint masks encode Ignore/Overlap, otherwise Block.
    AFIELD(HardMax="65535", DESC="按 bit 指定要忽略的 Collision Category；bit 0 对应类别 0。与 Overlap Categories 不能有共同位。")
    std::uint32_t ignore_categories{};
    AFIELD(HardMax="65535", DESC="按 bit 指定只产生接触、不求解阻挡的 Collision Category。未出现在 Ignore 或 Overlap 中的类别按 Block 处理。")
    std::uint32_t overlap_categories{};
    AFIELD(DESC="启用 Sensor 后只报告接触或重叠，不参与阻挡求解。它仍遵守 Collision Response 过滤。")
    bool sensor{};
    AFIELD(DESC="此 body 接触面的 Friction 和 Restitution。与用于显示的 Material 相互独立。")
    PhysicsMaterial material{};
    // Derived cache, rebuilt from the mesh resource; never archived as object state.
    // The bytes carry their input key (format, Jolt version, mesh source and
    // scaled geometry hash); physics checks it where it uses the shape, so
    // edits never clear it.
    std::shared_ptr<const std::string> cooked_shape{};
    [[nodiscard]] Response response(std::uint32_t category) const noexcept;
    [[nodiscard]] Result<void> set_response(std::uint32_t category, Response);
    bool operator==(const BodySettings&) const = default;
};
// Each checks a value against its reflected type's hard bounds and validator.
[[nodiscard]] Result<void> validate_body_settings(Runtime&, const BodySettings&);

ACLASS(Name="world.PrimitiveComponent", DESC="具有碰撞和刚体配置的基础组件。空间由所属 Entity 提供，具体形状和运动方式在 Body 中设置。")
class PrimitiveComponent : public Component {
    AGENERATED_BODY(PrimitiveComponent)
public:
    AFIELD(Category="Physics", DESC="此组件的碰撞形状、运动方式和接触规则。关闭 Body Enabled 可保留组件而停用其物理能力。")
    BodySettings body{.enabled=false};
};

// A spatial collision primitive without presentation. Choose box/sphere/capsule
// through the same reflected body settings used by StaticMeshComponent.
ACLASS(Name="world.CollisionComponent", DESC="提供 Box、Sphere 或 Capsule 碰撞的组件，不显示网格。使用与 StaticMeshComponent 相同的 Body 设置。")
class CollisionComponent : public PrimitiveComponent {
    AGENERATED_BODY(CollisionComponent)
public:
    CollisionComponent() { body.enabled=true; }
};

ASTRUCT(Name="world.Material", DESC="网格组件的基础表面参数。已绑定材质槽的参数沿 Material 资源及实例覆盖编辑。")
struct Material {
    AGENERATED_BODY(Material)
public:
    AFIELD(Min="0", Max="1", LockMin, LockMax, DESC="表面的线性 RGB 基础色，每个分量在 0–1 之间。它是材质颜色，不是曝光后的屏幕颜色。")
    math::Vector3 base_color{.19f, .42f, .57f};
    AFIELD(Min="0", Max="1", LockMin, LockMax, DESC="表面 Roughness，范围 0–1。较小值使反射更集中，较大值使高光更宽、更柔。")
    float roughness{.65f};
    AFIELD(Min="0", Max="1", LockMin, LockMax, DESC="表面 Metallic，范围 0–1。0 表示非金属，1 表示金属，影响基础色参与漫反射和镜面反射的方式。")
    float metallic{};
    AFIELD(Min="0", Max="10", LockMin, HardMax="65504", DESC="表面自身发出的线性 HDR RGB 辐射。它可以大于 1，不是显示亮度；当前每通道上限为 65504。")
    math::Vector3 emissive{};
    bool operator==(const Material&) const = default;
};
ACLASS(Name="world.StaticMeshComponent", DESC="显示网格资源，并按材质槽绑定 Material。可同时启用 Body 碰撞，空间变换来自所属 Entity。")
class StaticMeshComponent : public PrimitiveComponent {
    AGENERATED_BODY(StaticMeshComponent)
public:
    AFIELD(Category="Material", DESC="组件的基础表面参数。若使用材质槽资源，请编辑对应的共享 Material 或实例覆盖。")
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

AENUM(Name="world.LightKind", DESC="Point 点光、Rectangle 矩形面光、Directional 平行光或 Sky 均匀天光。不同种类使用不同的强度单位和形状参数。")
enum class LightKind : std::uint32_t { point, rectangle, directional, sky };

ASTRUCT(Name="world.LightSettings", DESC="灯光的 Scene 作者参数。Color 与 Intensity 一起决定发光量，局部灯的位置和朝向来自所属 Entity。")
struct LightSettings {
    AGENERATED_BODY(LightSettings)
public:
    AFIELD(DESC="灯光种类，决定 Intensity 单位及哪些尺寸参数生效。一个 World 只能拥有一个 Sky Light，包括已停用的 Sky Light。")
    LightKind kind{LightKind::point};
    AFIELD(HardMin="0", DESC="非负线性 RGB，逐通道乘以 Intensity。可以大于 1；它不是 sRGB 屏幕颜色。")
    math::Vector3 color{1.f,1.f,1.f};
    // Reference-white intensity: point cd, rectangle/sky cd/m^2, directional lux.
    // Sky is uniform incident radiance; spatial transform and emitter shape do not affect it.
    AFIELD(HardMin="0", DESC="参考白色的发光强度，再由 Color 调制。Point 使用 cd；Rectangle 和 Sky 使用 cd/m²；Directional 使用 lux。")
    float intensity{1.f};
    AFIELD(HardMin="0", DESC="Point 和 Rectangle 的作用距离，单位为世界 m，必须大于 0。Directional 和 Sky 忽略此值；Entity 缩放不会改变它。")
    float range{10.f};
    // Point: metres. Directional: angular radius in radians, below pi/2.
    // Rectangle emission size uses width/height instead.
    AFIELD(HardMin="0", DESC="Point 的发光半径使用世界 m；Directional 的角半径使用 rad，必须小于 π/2。它影响软阴影；Rectangle 和 Sky 忽略此值。")
    float source_radius{};
    AFIELD(HardMin="0", DESC="Rectangle 沿局部 X 方向的发光宽度，单位为世界 m，必须大于 0。不乘 Entity 的缩放。")
    float width{1.f};
    AFIELD(HardMin="0", DESC="Rectangle 沿局部 Y 方向的发光高度，单位为世界 m，必须大于 0。不乘 Entity 的缩放。")
    float height{1.f};
    AFIELD(DESC="让 Rectangle 的两面都按同样的 Intensity 发光，不把每面的强度减半。其他灯光种类不使用此项。")
    bool two_sided{};
    AFIELD(DESC="此直接灯是否参与阴影投射，实际效果还受 World 阴影设置控制。Sky 不使用此项。")
    bool casts_shadow{true};
    bool operator==(const LightSettings&) const = default;
};

ACLASS(Name="world.LightComponent", DESC="让 Entity 提供 Point、Rectangle、Directional 或 Sky 光照。Sky 不受 Entity 的位置、旋转和缩放影响。")
class LightComponent : public Component {
    AGENERATED_BODY(LightComponent)
public:
    AFIELD(Category="Lighting", DESC="此灯的种类、颜色、强度和形状。参数随 Scene 保存；全局曝光与效果在 World Settings 中调整。")
    LightSettings settings;
};

[[nodiscard]] Result<void> validate_material(Runtime&, const Material&);
[[nodiscard]] Result<void> validate_light(Runtime&, const LightSettings&);
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
// Call once per Runtime; additional derived definitions may be registered
// afterwards at any type safe point.
[[nodiscard]] Result<Types> register_types(Runtime&);

enum class EntityState { detached, attached_inactive, active, invalid };
struct EntityStatus {
    EntityState state{EntityState::invalid};
    TypeId type;
    bool enabled{};
    bool can_edit{};
    // True when this member has at least one mesh or light the World presents.
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
    // True when the World presents this component as a mesh or light.
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
// isolate confines an object-attributable failure to that Entity or Component:
// it is reported, excluded from its consumer, and a failed behavior stops
// receiving Begin/Tick/tasks. halt keeps first-error shutdown and whole-call
// failure. Both policies report object diagnostics.
enum class FailurePolicy : std::uint8_t { isolate, halt };
// World::update runs the phases in this order, with the registered
// simulations stepping between pre_physics and post_physics. Every phase
// receives the whole frame's dt; late runs before the host extracts
// presentation. Within a phase only explicit tick prerequisites order objects.
enum class TickPhase : std::uint8_t { pre_physics, post_physics, late };
// A system that World::update advances after pre_physics. step runs at a host
// boundary, outside behavior dispatch, so it may call World APIs; a failed step
// is a World-level failure. writes_pose(entity) claims that Entity's pose, so
// translate_owner commands for it are rejected. relocated, when set, runs
// synchronously at the host boundary after a transform edit of a running World
// moved claimed Entities (the edited Entity and its spatial descendants). It may
// read the World but not edit it; an error restores the edit, which fails with it.
struct Simulation {
    std::function<Result<void>(float)> step;
    std::function<bool(ObjectId)> writes_pose;
    std::function<Result<void>(std::span<const ObjectId>)> relocated;
};
// Opaque handle of a component type registered as a capability in one World.
struct CapabilityId {
    std::uint32_t value{};
    bool operator==(const CapabilityId&) const = default;
};
using CapabilityPredicate = std::function<Result<bool>(Runtime&, ObjectId component)>;
struct CapabilityMember {
    ObjectId entity, component;
    bool active{}; // Enabled with an effectively enabled owner.
};
// False for failures of the World or Runtime itself (busy/reentry, code
// publication, aborted execution, capacity/allocation); those stop the World
// and fail the call under every policy, as does any failure after the World
// found its own index inconsistent.
[[nodiscard]] bool isolatable(const Error&) noexcept;
// The invalid_value Diagnostic for a type-rule failure. `record` is the path of
// the checked value inside `object`, empty when the object itself was checked.
[[nodiscard]] Diagnostic validation_diagnostic(const ValidationFailure&, DiagnosticPhase,
    ObjectId object, ObjectId entity, std::vector<std::string> record = {});

// Same Runtime thread. Runtime outlives World, its detached objects and tasks.
// The World owns only member roots and execution/registration state. Entity's
// private member_world relation is authoritative; directories are indexes.
class World final {
    struct Impl;
public:
    struct Behavior {
        std::function<Result<void>(Runtime&, ObjectId)> begin{};
        std::function<Result<void>(Runtime&, ObjectId, float)> tick{}; // pre_physics
        std::function<Result<void>(Runtime&, ObjectId)> end{};
        std::function<Result<void>(Runtime&, ObjectId, float)> post_physics_tick{}, late_tick{};
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
    [[nodiscard]] bool contains(ObjectId entity) const noexcept;
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
    // Begin visits each Entity before its components in composition order.
    // End reverses that order and pairs only with completed Begin.
    // Enabled gates Tick, not Begin.
    // Dynamic insertion begins new objects immediately; they tick from the next
    // phase that runs. End uses current order.
    [[nodiscard]] Result<Commit> start();
    [[nodiscard]] Result<Commit> stop();
    [[nodiscard]] Result<Commit> close();

    // Defaults to isolate. Changeable only while stopped.
    [[nodiscard]] FailurePolicy failure_policy() const noexcept;
    [[nodiscard]] Result<void> set_failure_policy(FailurePolicy);
    // Object diagnostics in serial order; taking empties the log. Repeated
    // consumer failures of one object/field coalesce into `repeats` while they
    // persist across consecutive frames, and a taken entry is not reissued
    // until its failure resolves. The log keeps at most 1024 entries.
    [[nodiscard]] std::vector<Diagnostic> take_diagnostics();
    [[nodiscard]] std::uint64_t dropped_diagnostics() const noexcept;
    // Lets subsystems such as physics write to the same log; fills `serial`
    // and, when empty, the qualified `type` of `object`.
    void report(Diagnostic);
    // Reports `diagnostic` for an object failure with `error`'s message and
    // source. True when the World isolates it; false means fail the caller
    // (halt policy, or a World-level failure, which is not reported).
    [[nodiscard]] bool contain(const Error&, Diagnostic);
    // Host boundary, typically after a compatible code publication. Tick- and
    // task-faulted objects resume Tick without a second Begin; Begin-faulted
    // objects take the running insertion Begin path again.
    [[nodiscard]] Result<Commit> clear_faults();

    [[nodiscard]] Result<Scalar> read(ObjectId entity_or_component, std::span<const FieldId>) const;
    // Authoring requires an open World at a host boundary, outside behavior
    // dispatch. Running members take the new values from the next phase without
    // a second Begin; same-context Detached objects may be prepared. Objects
    // registered in another World are rejected.
    // Entity and Component properties use the same Runtime field paths. Transform
    // scalar writes preserve TRS validation. Other fields take Runtime::write's
    // hard-bound clamping; an edit that makes a valid object fail its type
    // rules (Runtime::validate) or an admission is restored and rejected.
    [[nodiscard]] Result<void> write(ObjectId entity_or_component, std::span<const FieldId>, Scalar);
    // Whole values (String, Array, records) through Runtime::write(ValueInput), with the
    // same type-rule and admission checks; a rejected edit restores the previous whole
    // value. Transform fields use the scalar form or set_transform.
    [[nodiscard]] Result<void> write(ObjectId entity_or_component, std::span<const FieldId>, ValueInput);
    [[nodiscard]] Result<void> set_transform(ObjectId entity, Trs);
    [[nodiscard]] Result<void> remove_transform(ObjectId entity);
    // Whole-value authoring, rejected (not clamped) when the value fails its
    // type rules. Shares set_transform's edit eligibility.
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
    // Domains (presentation, simulation) register the component types they
    // consume. Member components of `type` (or derived) for which `predicate`
    // holds are kept in that registration's bucket, maintained with every
    // composition, membership and enablement change. Registering a type again
    // without a predicate returns its existing predicate-free registration, so
    // a domain host recreated on one World reuses it. At most 32 per World.
    [[nodiscard]] Result<CapabilityId> register_capability(TypeId component_type, CapabilityPredicate = {});
    // Registered member components in composition order, including inactive
    // ones. The borrow ends at the next change of this World.
    [[nodiscard]] Result<std::span<const CapabilityMember>> members(CapabilityId) const;
    // Re-evaluates capability predicates after a member component's data changed.
    [[nodiscard]] Result<void> reevaluate_capabilities(ObjectId component);
    // A rule over the components of one registered type (or derived) that may
    // become or stay members. Attach runs it on the incoming components before
    // publishing. Author edits (write, set_material, set_light) of a member
    // component run it on that component after writing and restore the previous
    // value when it fails. Runtime writes outside World are not admitted.
    using Admission = std::function<Result<void>(const World&, std::span<const ObjectId> candidates)>;
    [[nodiscard]] Result<void> add_admission(TypeId component_type, Admission);

    // Rotation of a space with scale magnitudes dropped and signs kept: each
    // level contributes R*diag(sign(scale)), a zero axis counting as positive.
    // An odd sign total is -1 times a proper rotation, so the frame is
    // `mirrored ? -rotation : rotation`.
    struct Orientation { math::Quaterniond rotation; bool mirrored{}; };
    // One extraction pass over the members' shared space cache. Starting it
    // begins a diagnostic pass of its phase (see take_diagnostics); it ends
    // when the next pass starts.
    class SpatialPass {
    public:
        // World matrix of a member Entity. An unusable space is reported on
        // the failing Entity and yields nullopt; an error means the caller
        // fails (halt policy or a World-level failure, see contain).
        [[nodiscard]] Result<std::optional<math::Matrix4>> matrix(ObjectId entity);
        // True when the chain of `entity` has a zero scale axis or `world`, this
        // pass's matrix of it, is singular; one warning names the topmost
        // zero-scale node. Degenerate spaces are not object failures under any policy.
        [[nodiscard]] bool degenerate(ObjectId entity, const math::Matrix4& world, std::string_view message);
        [[nodiscard]] Result<std::optional<Orientation>> orientation(ObjectId entity);
    private:
        friend class World;
        SpatialPass(Impl& impl, DiagnosticPhase phase) noexcept : impl_(&impl), phase_(phase) {}
        Impl* impl_;
        DiagnosticPhase phase_;
    };
    [[nodiscard]] Result<SpatialPass> spatial_pass(DiagnosticPhase) const;

    // Scheduling configuration keyed by the Runtime's existing type identity.
    // The nearest ancestor binding is inherited as a whole. Allowed at host
    // boundaries while running: begun objects use the new Tick from the next
    // phase without a second Begin, and End uses the binding current at End.
    // Native method and source/JIT adapters run outside every native borrow.
    [[nodiscard]] Result<void> bind_behavior(TypeId entity_or_component_type, Behavior);
    // `prerequisite` ticks before `object` in every phase where both tick. The
    // order follows the transitive closure, also through objects without that
    // phase's Tick. Both must belong to this World; cycles are rejected. Edges
    // persist across stop/start and are dropped when either object leaves.
    [[nodiscard]] Result<void> add_tick_prerequisite(ObjectId object, ObjectId prerequisite);
    [[nodiscard]] Result<void> remove_tick_prerequisite(ObjectId object, ObjectId prerequisite);
    [[nodiscard]] Result<void> observe_lifecycle(LifecycleObserver);
    // Unsubscribing during dispatch immediately prevents later calls, including
    // calls still pending in the current notification.
    [[nodiscard]] Result<std::uint64_t> subscribe_lifecycle(LifecycleObserver);
    void unsubscribe_lifecycle(std::uint64_t) noexcept;
    // The single frame entry: pre_physics Tick, task resumption, then each
    // simulation's step, post_physics Tick and late Tick. Every phase ends by
    // applying its commands and flushing queued requests. Stops early, without
    // an error, when a step or request stops the World.
    [[nodiscard]] Result<void> update(float amount);
    // Steps run in registration order. Removal during update takes effect
    // before the next step call.
    [[nodiscard]] Result<std::uint64_t> add_simulation(Simulation);
    void remove_simulation(std::uint64_t) noexcept;
    // The trusted starter uses the provided Runtime/receiver. World owns the
    // resulting task; deactivate/detach/stop/close cancels it before exit completes.
    [[nodiscard]] Result<void> start_task(ObjectId entity_or_component, const TaskStart&);

    // Request acceptance is not completion. Replay checks the original World's
    // membership/owner conditions; it never follows a target into another World.
    // Each update phase flushes the queue: a failed request is reported as a
    // command_rejected diagnostic on its target and isolated under the failure
    // policy. flush() applies the remaining queue and returns its receipts.
    // Receipts contain Commit only. Queued detach/remove do not return history
    // mementos or retain objects; a consumer needing them must already own roots.
    [[nodiscard]] Result<std::uint64_t> request(Request);
    [[nodiscard]] Result<std::vector<RequestCompletion>> flush();

private:
    friend struct detail::Access;
    std::unique_ptr<Impl> impl_;
};

}
