#pragma once
#include "physics/physics.hpp"
#include <unordered_map>
#include <optional>

namespace atelier::physics::detail {
// Reports a physics failure of one object to the World log. True when the
// World's policy isolates it; false means the physics update must fail.
bool contain(world::World&, const Error&, ObjectId object, ObjectId entity);
// Orchestrates component ownership through the same public backend API used by
// native clients. It never owns another Jolt world or another object registry.
class ComponentSimulation final {
public:
    ComponentSimulation(world::World& world, PhysicsScene& scene) : world_(world), scene_(scene) {}
    ~ComponentSimulation();
    Result<void> reconcile();
    Result<void> before_step(float fixed);
    Result<void> append_poses(std::vector<world::SimulationPose>&);
    Result<void> after_step();
    Result<void> removed(ObjectId,bool detached);
    ObjectId bound_pose_writer(ObjectId) const;
    void clear() noexcept;
    Result<QueryResult> execute_query(ObjectId,QueryIgnore = {});
    Result<QueryResult> read_query_result(ObjectId) const;
private:
    enum class Kind { character, ragdoll, vehicle, distance, force, kinematic, query };
    struct Entry {
        Kind kind;
        ObjectId entity, component, target{};
        SpecialHandle special{};
        ConstraintHandle constraint{};
        BodyHandle body{}, other_body{};
        std::vector<ObjectId> targets{}, metadata{}, ancestors{};
        math::Vector3 origin{};
        math::Quaternion rotation{};
        double elapsed{};
        bool impulse_pending{true};
        std::optional<QueryResult> query{};
    };
    world::World& world_;
    PhysicsScene& scene_;
    std::unordered_map<ObjectId, Entry> entries_;
    void release(const Entry&) noexcept;
    void clear_query(ObjectId,bool clear_request) noexcept;
    Result<void> sample_query(ObjectId,QueryIgnore = {});
};
}
