#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Physics acceptance: FAIL (%s)\n", message);
        std::exit(EXIT_FAILURE);
    }
}

void simulate() {
    constexpr JPH::ObjectLayer stationary = 0;
    constexpr JPH::ObjectLayer moving = 1;
    JPH::BroadPhaseLayerInterfaceTable broad_phase(2, 2);
    broad_phase.MapObjectToBroadPhaseLayer(stationary, JPH::BroadPhaseLayer(0));
    broad_phase.MapObjectToBroadPhaseLayer(moving, JPH::BroadPhaseLayer(1));
    JPH::ObjectLayerPairFilterTable pairs(2);
    pairs.EnableCollision(stationary, moving);
    pairs.EnableCollision(moving, moving);
    JPH::ObjectVsBroadPhaseLayerFilterTable broad_phase_filter(broad_phase, 2, pairs, 2);
    JPH::TempAllocatorImpl temporary(1024 * 1024);
    JPH::JobSystemSingleThreaded jobs(JPH::cMaxPhysicsJobs);

    // Filters, allocator and scheduler outlive the simulation that borrows them.
    JPH::PhysicsSystem physics;
    physics.Init(16, 0, 16, 16, broad_phase, broad_phase_filter, pairs);
    auto& bodies = physics.GetBodyInterface();
    const auto floor = bodies.CreateAndAddBody(
        JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(10.0f, 1.0f, 10.0f)),
            JPH::RVec3(0.0f, -1.0f, 0.0f), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, stationary), JPH::EActivation::DontActivate);
    const auto sphere = bodies.CreateAndAddBody(
        JPH::BodyCreationSettings(new JPH::SphereShape(0.5f),
            JPH::RVec3(0.0f, 3.0f, 0.0f), JPH::Quat::sIdentity(),
            JPH::EMotionType::Dynamic, moving), JPH::EActivation::Activate);
    require(!floor.IsInvalid() && !sphere.IsInvalid(), "body creation");
    require(physics.GetNumBodies() == 2, "two bodies registered");

    for (int step = 0; step < 300; ++step) {
        require(physics.Update(1.0f / 60.0f, 1, &temporary, &jobs)
            == JPH::EPhysicsUpdateError::None, "physics update");
        if (step == 29) {
            const auto height = bodies.GetPosition(sphere).GetY();
            require(height > 0.6f && height < 2.5f, "sphere falls under gravity");
        }
    }
    const auto height = bodies.GetPosition(sphere).GetY();
    require(std::isfinite(height) && std::abs(height - 0.5f) < 0.05f,
        "sphere rests on the floor");
    require(!bodies.IsActive(sphere), "sphere sleeps after settling");
    require(bodies.GetLinearVelocity(sphere).LengthSq() < 0.0001f, "resting velocity");
    bodies.RemoveBody(sphere);
    bodies.DestroyBody(sphere);
    bodies.RemoveBody(floor);
    bodies.DestroyBody(floor);
    require(physics.GetNumBodies() == 0, "all bodies destroyed");
}

} // namespace

int main() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    // All physics resources are gone before unregistering process-wide types.
    simulate();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    std::puts("Physics acceptance: PASS (gravity, collision, sleep, cleanup)");
    return EXIT_SUCCESS;
}
