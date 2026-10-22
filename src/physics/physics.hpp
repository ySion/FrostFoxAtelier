#pragma once

#include "presentation/presentation.hpp"
#include "world/world.hpp"
#include "geometry/mesh.hpp"
#include <unordered_set>

namespace atelier::physics {

using world::ShapeKind;
using world::MotionKind;
using world::Response;
using world::PhysicsMaterial;
using world::BodySettings;

[[nodiscard]] Result<std::string> cook_triangle_mesh(const geometry::MeshRef& mesh,
    math::Vector3 scale, std::string_view source_key);
[[nodiscard]] Result<void> validate_cooked_triangle_mesh(std::string_view bytes,
    const geometry::MeshRef& mesh, math::Vector3 scale, std::string_view source_key);

struct Types {
    TypeId character_component{}, ragdoll_component{}, ragdoll_part_component{},
        vehicle_component{}, vehicle_wheel_component{}, distance_constraint_component{},
        force_component{}, kinematic_motion_component{}, scene_query_component{};
};
[[nodiscard]] Result<Types> register_types(Runtime&);
[[nodiscard]] Result<void> configure(Runtime&, ObjectId component, BodySettings);
[[nodiscard]] Result<BodySettings> read_settings(Runtime&, ObjectId component);

struct BodyHandle {
    ObjectId entity, component;
    std::uint64_t scene{}, revision{};
    bool operator==(const BodyHandle&) const = default;
};

enum class ContactPhase : std::uint8_t { began, persisted, ended };
enum class ContactExitReason : std::uint8_t { separated, body_removed };
struct ContactEvent {
    ContactPhase phase;
    // Historical instances at contact time. An ended event remains meaningful
    // after removal; using either handle for a command still checks its revision.
    BodyHandle first, second;
    std::uint64_t step_id{};
    bool trigger{};
    ContactExitReason exit_reason{ContactExitReason::separated};
};
using ContactHandler = std::function<Result<void>(const ContactEvent&)>;
using StepHandler = std::function<Result<void>(std::uint64_t step_id, float fixed_delta)>;

struct BodyState {
    BodyHandle handle;
    std::uint64_t step_id{};
    math::Vector3 position, linear_velocity, angular_velocity;
    math::Quaternion rotation;
    bool sleeping{};
};
struct SpecialHandle {
    ObjectId entity;
    std::uint64_t scene{}, revision{};
    bool operator==(const SpecialHandle&) const = default;
};
struct CharacterSettings {
    float radius{.35f}, capsule_half_height{.6f};
    float max_slope_radians{.7853982f}, jump_speed{5.f};
    float step_height{.4f};
    bool operator==(const CharacterSettings&) const = default;
};
struct CharacterState {
    SpecialHandle handle;
    std::uint64_t step_id{};
    math::Vector3 position{}, velocity{};
    bool grounded{};
};
struct RagdollPart {
    std::string name;
    int parent{-1};
    math::Vector3 local_position{};
    float radius{.2f}, capsule_half_height{.25f};
};
struct RagdollPose {
    SpecialHandle handle;
    std::uint64_t step_id{};
    std::vector<math::Vector3> positions;
    std::vector<math::Quaternion> rotations;
};
struct VehicleSettings {
    float half_width{.8f}, half_length{1.4f}, wheel_radius{.35f};
    float suspension_length{.5f};
    bool operator==(const VehicleSettings&) const = default;
};
struct VehicleState {
    SpecialHandle handle;
    std::uint64_t step_id{};
    math::Vector3 position{}, velocity{};
    std::array<bool, 4> wheel_contact{};
    std::array<math::Vector3, 4> wheel_positions{};
    std::array<math::Quaternion, 4> wheel_rotations{};
};

// Authoring components use the existing Runtime graph and scene properties.
// Inputs are sampled per fixed step. Backend handles remain scene-owned.
// Shape/constraint configuration is sampled on activation; disable/re-enable to
// rebuild it. Parts/wheels explicitly bind spatial Entity children.
class CharacterComponent : public world::Component {
public:
    CharacterSettings settings;
    math::Vector3 velocity{};
    bool jump{};
};
class RagdollComponent : public world::Component {
public:
    ReferenceSequence parts;
};
// Binding order defines backend part indices; parent_index must precede the part.
// rest_position is relative to the controller Entity, independent of simulated poses.
class RagdollPartComponent : public world::Component {
public:
    std::int32_t parent_index{-1};
    math::Vector3 rest_position{};
    float radius{.2f}, capsule_half_height{.25f};
};
class VehicleComponent : public world::Component {
public:
    ReferenceSequence wheels;
    VehicleSettings settings;
    float throttle{}, steering{}, brake{};
};
class VehicleWheelComponent : public world::Component {
public:
    std::uint32_t wheel_index{};
};
class DistanceConstraintComponent : public world::Component {
public:
    ReferenceSlot target; // Registered weak Entity reference, persisted as a scene-local identity.
    float minimum{1}, maximum{1};
};
class ForceComponent : public world::Component {
public:
    math::Vector3 force{}, initial_impulse{};
};
class KinematicMotionComponent : public world::Component {
public:
    math::Vector3 velocity{}, amplitude{};
    float period{4};
};
[[nodiscard]] Result<void> set_constraint_target(Runtime&, ObjectId component, ObjectId entity);
[[nodiscard]] Result<ObjectId> constraint_target(Runtime&, ObjectId component);
[[nodiscard]] Result<void> set_ragdoll_parts(Runtime&, ObjectId component, std::span<const ObjectId> entities);
[[nodiscard]] Result<std::vector<ObjectId>> ragdoll_parts(Runtime&, ObjectId component);
[[nodiscard]] Result<void> set_vehicle_wheels(Runtime&, ObjectId component, std::span<const ObjectId> entities);
[[nodiscard]] Result<std::vector<ObjectId>> vehicle_wheels(Runtime&, ObjectId component);
// Persistent query exclusions may change while active and affect the next sample.
[[nodiscard]] Result<void> set_query_ignored_entities(Runtime&, ObjectId component, std::span<const ObjectId> entities);
[[nodiscard]] Result<std::vector<ObjectId>> query_ignored_entities(Runtime&, ObjectId component);
namespace detail { class ComponentSimulation; }
struct ConstraintHandle {
    std::uint64_t scene{}, revision{};
    bool operator==(const ConstraintHandle&) const = default;
};
enum class QueryMode : std::uint8_t { any, closest, all };
// interaction preserves the original two-sided query response contract.
enum class QueryPolicy : std::uint8_t { interaction, channel, object_type };
struct QueryFilter {
    std::uint16_t categories{0xffff};
    ObjectId ignore_entity{};
    ObjectId ignore_component{};
    QueryMode mode{QueryMode::closest};
    std::size_t capacity{64};
    std::uint8_t category{};
    std::array<Response, 16> responses = [] {
        std::array<Response, 16> values;
        values.fill(Response::block);
        return values;
    }();
    QueryPolicy policy{QueryPolicy::interaction};
    // Borrowed only for the duration of the synchronous call.
    std::span<const ObjectId> ignored_entities{}, ignored_components{};
    bool include_sensors{true}, ignore_touches{}, stop_at_block{};
};
struct QueryHit {
    BodyHandle body;
    float fraction{};
    math::Vector3 point{}, normal{};
    std::uint32_t sub_shape{};
    bool sensor{}, initial_overlap{};
    bool blocking{};
    PhysicsMaterial material{};
    float distance{}, penetration_depth{};
    math::Vector3 location{}; // Ray point or swept shape center; point is the surface contact.
};
struct QueryResult {
    std::uint64_t step_id{};
    std::vector<QueryHit> hits;
    bool overflow{};
    bool has_block{};
    bool has_hit{}; // Before capacity truncation; test mode does not retain hits.
    math::Vector3 start{}, end{};
};
enum class TraceShape : std::uint32_t { line, sphere_sweep, sphere_overlap };
enum class TraceSelection : std::uint32_t { channel, object_type };
enum class TraceMode : std::uint32_t { test, single, multi };
struct TraceSettings {
    TraceShape shape{TraceShape::line};
    TraceSelection selection{TraceSelection::channel};
    TraceMode mode{TraceMode::single};
    std::uint32_t channel{}, object_types{0xffff}, capacity{64};
    float radius{.25f}; // Sphere radius in world units, independent of component scale.
    bool include_sensors{true};
};
struct QueryIgnore {
    std::span<const ObjectId> entities{}, components{};
};
// Configuration is persistent; results/requests are transient. Script Tick reads
// the previous completed physics step, then may request the next fixed-step query.
class SceneQueryComponent : public world::Component {
public:
    ReferenceSequence ignored_entities;
    TraceSettings settings;
    math::Vector3 start{}, end{0,0,10}; // Owner Entity-local endpoints; overlap uses start.
    bool automatic{true}, ignore_self{true};
    void request_query() noexcept { pending_=true; }
    bool query_ready() const noexcept { return ready_; }
    bool has_hit() const noexcept { return hit_; }
    bool has_blocking_hit() const noexcept { return blocking_; }
    bool result_overflow() const noexcept { return overflow_; }
    std::uint32_t hit_count() const noexcept { return count_; }
    float hit_distance() const noexcept { return distance_; }
    math::Vector3 hit_point() const noexcept { return point_; }
    math::Vector3 hit_normal() const noexcept { return normal_; }
private:
    friend class detail::ComponentSimulation;
    bool pending_{}, ready_{}, hit_{}, blocking_{}, overflow_{};
    std::uint32_t count_{};
    float distance_{};
    math::Vector3 point_{}, normal_{};
};
struct StepResult {
    std::uint32_t steps{}, rejected_commands{}, dropped_contact_notifications{};
    // Triangle shapes cooked at runtime since the previous step because a cooked shape no longer matched its scale or mesh.
    std::uint32_t runtime_cooks{};
    // Fixed substeps whose solve ignored some contacts because a Capacity buffer
    // was full; the simulation continues and each one reports a physics warning.
    std::uint32_t contact_overflows{};
    float remaining{}, dropped{};
    std::uint64_t completed_step{};
    bool contacts_incomplete{};
};

// Jolt's solver buffers, fixed when a PhysicsScene is created. A zero field is
// derived from the Primitives in the World at creation.
struct Capacity {
    std::uint32_t bodies{}, body_pairs{}, contact_constraints{};
    bool operator==(const Capacity&) const = default;
};

// The host owns this beside a World and destroys it before that World/Runtime.
// It registers as the World's simulation: World::update steps it between the
// pre_physics and post_physics phases with bounded fixed substeps. Step,
// contact and before/after-step handlers run at a World host boundary and may
// call World APIs; a failure there stops the World.
class PhysicsScene final {
public:
    [[nodiscard]] static Result<std::unique_ptr<PhysicsScene>> create(world::World&, Types, Capacity = {});
    ~PhysicsScene();
    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;

    // The buffers this scene was created with, derived fields filled in.
    [[nodiscard]] Capacity capacity() const noexcept { return capacity_; }
    // The latest step World::update ran; completed_step is cumulative.
    [[nodiscard]] const StepResult& last_step() const noexcept { return last_step_; }
    // Poses to draw this frame for the Entities the latest fixed step published, for
    // presentation::Scene::update: each between its previous and its current simulated
    // pose by the fraction of a step left over, so the image trails the simulation by
    // up to one step. Empty while paused. Simulated Entities below a moved one are listed
    // with equal poses. A created, teleported or relocated Entity starts again from its new pose.
    [[nodiscard]] std::span<const presentation::PresentedPose> presented_poses() const noexcept { return presented_; }
    [[nodiscard]] Result<void> set_gravity(math::Vector3);
    [[nodiscard]] Result<void> add_force(ObjectId entity, math::Vector3);
    [[nodiscard]] Result<void> add_impulse(ObjectId entity, math::Vector3);
    [[nodiscard]] Result<void> add_force(BodyHandle, math::Vector3);
    [[nodiscard]] Result<void> add_impulse(BodyHandle, math::Vector3);
    // A Primitive selects its own body; an Entity selects its unique active body.
    [[nodiscard]] Result<BodyHandle> body_handle(ObjectId entity_or_primitive) const;
    [[nodiscard]] Result<BodyState> read_body_state(BodyHandle) const;
    [[nodiscard]] Result<void> set_kinematic_target(BodyHandle, math::Vector3 position,
        math::Quaternion rotation);
    [[nodiscard]] Result<void> teleport(BodyHandle, math::Vector3 position,
        math::Quaternion rotation, bool preserve_velocity = false);
    [[nodiscard]] Result<ConstraintHandle> create_distance_constraint(BodyHandle first,
        BodyHandle second, float minimum, float maximum);
    // Explicit removal or endpoint removal wakes the surviving endpoints.
    [[nodiscard]] Result<void> destroy_constraint(ConstraintHandle);
    // Character and ragdoll Entities must be active spatial members of this World.
    [[nodiscard]] Result<SpecialHandle> create_character(ObjectId entity, CharacterSettings = {});
    [[nodiscard]] Result<SpecialHandle> character_handle(ObjectId entity) const;
    [[nodiscard]] Result<void> set_character_input(SpecialHandle, math::Vector3 planar_velocity,
        bool jump = false);
    [[nodiscard]] Result<CharacterState> read_character_state(SpecialHandle) const;
    [[nodiscard]] Result<void> destroy_character(SpecialHandle);
    [[nodiscard]] Result<SpecialHandle> create_ragdoll(ObjectId entity,
        std::span<const RagdollPart> parts);
    [[nodiscard]] Result<SpecialHandle> ragdoll_handle(ObjectId entity) const;
    [[nodiscard]] Result<RagdollPose> read_ragdoll_pose(SpecialHandle) const;
    [[nodiscard]] Result<void> destroy_ragdoll(SpecialHandle);
    [[nodiscard]] Result<SpecialHandle> create_vehicle(BodyHandle chassis,
        VehicleSettings = {});
    [[nodiscard]] Result<SpecialHandle> vehicle_handle(ObjectId entity) const;
    [[nodiscard]] Result<void> set_vehicle_input(SpecialHandle, float throttle,
        float steering, float brake);
    [[nodiscard]] Result<VehicleState> read_vehicle_state(SpecialHandle) const;
    [[nodiscard]] Result<void> destroy_vehicle(SpecialHandle);
    [[nodiscard]] Result<QueryResult> raycast(math::Vector3 start, math::Vector3 end,
        QueryFilter filter = {}) const;
    [[nodiscard]] Result<QueryResult> sweep_sphere(math::Vector3 start, math::Vector3 end,
        float radius, QueryFilter filter = {}) const;
    [[nodiscard]] Result<QueryResult> overlap_sphere(math::Vector3 center,
        float radius, QueryFilter filter = {}) const;
    // Channel multi returns touches through the first block; object-type multi
    // returns all matching bodies. Test/single channel modes ignore touches.
    [[nodiscard]] Result<QueryResult> trace(math::Vector3 start, math::Vector3 end,
        const TraceSettings& = {}, QueryIgnore = {}) const;
    // Component must be active and reconciled by World::update(0). Manual sampling works
    // while paused. Returned snapshots own values, never the hit Entities/bodies.
    [[nodiscard]] Result<QueryResult> execute_query(ObjectId component, QueryIgnore = {});
    [[nodiscard]] Result<QueryResult> read_query_result(ObjectId component) const;
    void set_paused(bool paused) noexcept;
    // Paused requests coalesce and a completed solve consumes them. A failed
    // step stops the World, which discards a pending request.
    void step_once() noexcept { single_step_ = true; }
    [[nodiscard]] std::uint64_t completed_step() const noexcept { return completed_step_; }
    void on_contact(ContactHandler handler) { handler_ = std::move(handler); }
    void on_before_step(StepHandler handler) { before_step_ = std::move(handler); }
    void on_after_step(StepHandler handler) { after_step_ = std::move(handler); }
    [[nodiscard]] std::size_t body_count() const noexcept;
    // Rigid bodies Jolt is simulating now; sleeping and static bodies are not counted.
    [[nodiscard]] std::size_t active_body_count() const noexcept;

private:
    friend class detail::ComponentSimulation;
    struct Impl;
    explicit PhysicsScene(world::World&, Types, Capacity);
    [[nodiscard]] Result<void> step(float elapsed);
    // Lookup failures answer true, so an unknown Entity is never translated.
    [[nodiscard]] bool writes_pose(ObjectId entity) const;
    [[nodiscard]] Result<void> relocate(std::span<const ObjectId> entities);
    [[nodiscard]] Result<void> lifecycle(world::Lifecycle, ObjectId);
    [[nodiscard]] Result<void> reconcile(ObjectId entity);
    [[nodiscard]] Result<void> reconcile_component(ObjectId entity, ObjectId component);
    [[nodiscard]] ObjectId pose_writer(ObjectId entity) const noexcept;
    [[nodiscard]] Result<bool> controller_transform_matches(ObjectId,math::Vector3,math::Quaternion) const;
    void remove(ObjectId component) noexcept;
    void remove_special(ObjectId entity) noexcept;
    // Drops the interpolation of an Entity that physics no longer drives.
    void forget_pose(ObjectId entity) noexcept;
    // Draws created or relocated Entities at these World poses until a step publishes them;
    // that step moves from here.
    void hold_poses(std::span<const world::SimulationPose>);
    // Rebuilds presented_ from the latest published step and the fraction of a step left over.
    void present();
    world::World& world_;
    Types types_;
    Capacity capacity_;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<detail::ComponentSimulation> components_;
    ContactHandler handler_;
    StepHandler before_step_, after_step_;
    std::uint64_t subscription_{}, simulation_{};
    // World buckets of the components that can claim an Entity's physics pose.
    world::CapabilityId primitives_, characters_, ragdolls_;
    StepResult last_step_{};
    std::vector<presentation::PresentedPose> presented_;
    float remaining_{};
    std::uint64_t completed_step_{}, scene_id_{}, next_revision_{1};
    std::uint32_t runtime_cooks_{};
    bool paused_{}, single_step_{};
};

}
