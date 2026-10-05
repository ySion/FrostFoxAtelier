#pragma once

#include "world/world.hpp"

namespace atelier::physics {

enum class ShapeKind : std::uint8_t { box, sphere, triangle_mesh };
enum class MotionKind : std::uint8_t { stationary, kinematic, dynamic };

struct BodySettings {
    ShapeKind shape{ShapeKind::box};
    MotionKind motion{MotionKind::stationary};
    math::Vector3 half_extents{.5f, .5f, .5f};
    float radius{.5f};
    bool operator==(const BodySettings&) const = default;
};

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

enum class ContactPhase : std::uint8_t { began, ended };
struct ContactEvent {
    ContactPhase phase;
    ObjectId first_actor, second_actor;
    ObjectId first_component, second_component;
};
using ContactHandler = std::function<Result<void>(const ContactEvent&)>;

struct StepResult { std::uint32_t steps{}; float remaining{}; };

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
    void on_contact(ContactHandler handler) { handler_ = std::move(handler); }
    [[nodiscard]] std::size_t body_count() const noexcept;

private:
    struct Impl;
    explicit PhysicsScene(world::World&, Types);
    [[nodiscard]] Result<void> lifecycle(world::Lifecycle, ObjectId);
    [[nodiscard]] Result<void> reconcile(ObjectId actor);
    void remove(ObjectId actor) noexcept;
    world::World& world_;
    Types types_;
    std::unique_ptr<Impl> impl_;
    ContactHandler handler_;
    std::uint64_t subscription_{};
    float remaining_{};
};

}
