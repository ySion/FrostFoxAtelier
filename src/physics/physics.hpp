#pragma once

#include "world/world.hpp"

namespace atelier::physics {

enum class ShapeKind : std::uint8_t { box, sphere, triangle_mesh, capsule };
enum class MotionKind : std::uint8_t { stationary, kinematic, dynamic };
enum class Response : std::uint8_t { ignore, overlap, block };

struct PhysicsMaterial {
    float friction{.2f}, restitution{};
    bool operator==(const PhysicsMaterial&) const = default;
};

struct BodySettings {
    ShapeKind shape{ShapeKind::box};
    MotionKind motion{MotionKind::stationary};
    math::Vector3 half_extents{.5f, .5f, .5f};
    float radius{.5f};
    float capsule_half_height{.5f};
    std::uint8_t category{};
    std::array<Response, 16> responses = [] {
        std::array<Response, 16> values;
        values.fill(Response::block);
        return values;
    }();
    bool sensor{};
    PhysicsMaterial material{};
    // Immutable bytes produced by cook_triangle_mesh; the mesh binding remains
    // the authoritative resource identity and is checked when the body loads.
    std::shared_ptr<const std::string> cooked_shape;
    bool operator==(const BodySettings&) const = default;
};

[[nodiscard]] Result<std::string> cook_triangle_mesh(const geometry::MeshRef& mesh,
    math::Vector3 scale, std::string_view source_key);
[[nodiscard]] Result<void> validate_cooked_triangle_mesh(std::string_view bytes,
    const geometry::MeshRef& mesh, math::Vector3 scale, std::string_view source_key);

// Non-spatial composition. Its body is placed at the owning Actor's explicit root.
// triangle_mesh consumes the immutable mesh of a root StaticMeshComponent and
// supports stationary bodies only. Other shapes work with any SceneComponent root.
class BodyComponent : public world::Component {
public:
    BodySettings settings;
};

struct Types { TypeId body_component; };
[[nodiscard]] Result<Types> register_types(Runtime&);
[[nodiscard]] Result<void> configure(Runtime&, ObjectId component, BodySettings);
[[nodiscard]] Result<BodySettings> read_settings(Runtime&, ObjectId component);

enum class ContactPhase : std::uint8_t { began, persisted, ended };
enum class ContactExitReason : std::uint8_t { separated, body_removed };
struct ContactEvent {
    ContactPhase phase;
    ObjectId first_actor, second_actor;
    ObjectId first_component, second_component;
    std::uint64_t step_id{};
    bool trigger{};
    ContactExitReason exit_reason{ContactExitReason::separated};
};
using ContactHandler = std::function<Result<void>(const ContactEvent&)>;
using StepHandler = std::function<Result<void>(std::uint64_t step_id, float fixed_delta)>;

struct BodyHandle {
    ObjectId actor, component;
    std::uint64_t scene{}, revision{};
    bool operator==(const BodyHandle&) const = default;
};
struct BodyState {
    BodyHandle handle;
    std::uint64_t step_id{};
    math::Vector3 position, linear_velocity, angular_velocity;
    math::Quaternion rotation;
    bool sleeping{};
};
struct SpecialHandle {
    ObjectId actor;
    std::uint64_t scene{}, revision{};
    bool operator==(const SpecialHandle&) const = default;
};
struct CharacterSettings {
    float radius{.35f}, capsule_half_height{.6f};
    float max_slope_radians{.7853982f}, jump_speed{5.f};
    float step_height{.4f};
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
};
struct VehicleState {
    SpecialHandle handle;
    std::uint64_t step_id{};
    math::Vector3 position{}, velocity{};
    std::array<bool, 4> wheel_contact{};
};
struct ConstraintHandle {
    std::uint64_t scene{}, revision{};
    bool operator==(const ConstraintHandle&) const = default;
};
enum class QueryMode : std::uint8_t { any, closest, all };
struct QueryFilter {
    std::uint16_t categories{0xffff};
    ObjectId ignore_actor{};
    ObjectId ignore_component{};
    QueryMode mode{QueryMode::closest};
    std::size_t capacity{64};
    std::uint8_t category{};
    std::array<Response, 16> responses = [] {
        std::array<Response, 16> values;
        values.fill(Response::block);
        return values;
    }();
};
struct QueryHit {
    BodyHandle body;
    float fraction{};
    math::Vector3 point{}, normal{};
    std::uint32_t sub_shape{};
    bool sensor{}, initial_overlap{};
    bool blocking{};
    PhysicsMaterial material{};
};
struct QueryResult {
    std::uint64_t step_id{};
    std::vector<QueryHit> hits;
    bool overflow{};
    bool has_block{};
};
struct StepResult {
    std::uint32_t steps{}, rejected_commands{}, dropped_contact_notifications{};
    float remaining{}, dropped{};
    std::uint64_t completed_step{};
    bool contacts_incomplete{};
};

// The host owns this beside a World and destroys it before that World/Runtime.
// update runs gameplay once, then bounded fixed physics substeps. Contact
// handlers run only after Jolt::Update returns and may enqueue World requests.
class PhysicsScene final {
public:
    [[nodiscard]] static Result<std::unique_ptr<PhysicsScene>> create(world::World&, Types);
    ~PhysicsScene();
    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;

    [[nodiscard]] Result<StepResult> update(float elapsed);
    [[nodiscard]] Result<void> set_gravity(math::Vector3);
    [[nodiscard]] Result<void> add_force(ObjectId actor, math::Vector3);
    [[nodiscard]] Result<void> add_impulse(ObjectId actor, math::Vector3);
    [[nodiscard]] Result<BodyHandle> body_handle(ObjectId actor) const;
    [[nodiscard]] Result<BodyState> read_body_state(BodyHandle) const;
    [[nodiscard]] Result<void> set_kinematic_target(BodyHandle, math::Vector3 position,
        math::Quaternion rotation);
    [[nodiscard]] Result<void> teleport(BodyHandle, math::Vector3 position,
        math::Quaternion rotation, bool preserve_velocity = false);
    [[nodiscard]] Result<ConstraintHandle> create_distance_constraint(BodyHandle first,
        BodyHandle second, float minimum, float maximum);
    [[nodiscard]] Result<void> destroy_constraint(ConstraintHandle);
    [[nodiscard]] Result<SpecialHandle> create_character(ObjectId actor, CharacterSettings = {});
    [[nodiscard]] Result<void> set_character_input(SpecialHandle, math::Vector3 planar_velocity,
        bool jump = false);
    [[nodiscard]] Result<CharacterState> read_character_state(SpecialHandle) const;
    [[nodiscard]] Result<void> destroy_character(SpecialHandle);
    [[nodiscard]] Result<SpecialHandle> create_ragdoll(ObjectId actor,
        std::span<const RagdollPart> parts);
    [[nodiscard]] Result<RagdollPose> read_ragdoll_pose(SpecialHandle) const;
    [[nodiscard]] Result<void> destroy_ragdoll(SpecialHandle);
    [[nodiscard]] Result<SpecialHandle> create_vehicle(BodyHandle chassis,
        VehicleSettings = {});
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
    void set_paused(bool paused) noexcept { paused_ = paused; }
    void step_once() noexcept { single_step_ = true; }
    [[nodiscard]] std::uint64_t completed_step() const noexcept { return completed_step_; }
    void on_contact(ContactHandler handler) { handler_ = std::move(handler); }
    void on_before_step(StepHandler handler) { before_step_ = std::move(handler); }
    void on_after_step(StepHandler handler) { after_step_ = std::move(handler); }
    [[nodiscard]] std::size_t body_count() const noexcept;

private:
    struct Impl;
    explicit PhysicsScene(world::World&, Types);
    [[nodiscard]] Result<void> lifecycle(world::Lifecycle, ObjectId);
    [[nodiscard]] Result<void> reconcile(ObjectId actor);
    void remove(ObjectId actor) noexcept;
    void remove_special(ObjectId actor) noexcept;
    world::World& world_;
    Types types_;
    std::unique_ptr<Impl> impl_;
    ContactHandler handler_;
    StepHandler before_step_, after_step_;
    std::uint64_t subscription_{};
    float remaining_{};
    std::uint64_t completed_step_{}, scene_id_{}, next_revision_{1};
    bool paused_{}, single_step_{}, updating_{};
};

}
