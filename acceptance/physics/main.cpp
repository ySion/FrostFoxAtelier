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
#include "physics/physics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Physics acceptance: FAIL (%s)\n", message);
        std::exit(EXIT_FAILURE);
    }
}

template<class T> T take(atelier::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(atelier::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
void committed(atelier::Result<atelier::world::Commit> result) {
    const auto value = take(std::move(result));
    if (value.notification_error) throw std::runtime_error(value.notification_error->message);
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

void simulate_world() {
    namespace w = atelier::world;
    namespace p = atelier::physics;
    atelier::Runtime runtime;
    const auto world_types = take(w::register_types(runtime));
    const auto physics_types = take(p::register_types(runtime));
    w::World world(runtime, world_types);
    auto scene = take(p::PhysicsScene::create(world, physics_types));

    w::StaticMeshActorDesc floor_desc;
    floor_desc.mesh = atelier::geometry::cube_mesh();
    floor_desc.transform.position = {0, -.5f, 0};
    floor_desc.transform.scale = {8, 1, 8};
    auto floor = take(w::create_static_mesh_actor(world, floor_desc));
    auto floor_body = take(w::create_component(runtime, physics_types.body_component));
    p::BodySettings floor_settings;
    floor_settings.shape = p::ShapeKind::triangle_mesh;
    auto invalid_mesh = floor_settings;
    invalid_mesh.motion = p::MotionKind::dynamic;
    require(!p::configure(runtime, floor_body.id(), invalid_mesh), "dynamic triangle mesh rejected");
    take(p::configure(runtime, floor_body.id(), floor_settings));
    const std::array floor_attachment{w::Attachment{floor_body.id()}};
    committed(world.attach_components(floor.id(), floor_attachment));

    auto actor = take(w::create_actor(runtime));
    auto root = take(w::create_component(runtime, world_types.scene_component));
    auto body = take(w::create_component(runtime, physics_types.body_component));
    auto disabled_body = take(w::create_component(runtime, physics_types.body_component));
    const std::array actor_attachments{w::Attachment{root.id(), {}, true}, w::Attachment{body.id()},
        w::Attachment{disabled_body.id()}};
    committed(world.attach_components(actor.id(), actor_attachments));
    committed(world.set_enabled(disabled_body.id(), false));
    take(world.set_transform(root.id(), w::Trs{{0, 2, 0}, {0, 0, 0, 1}, {1, 1, 1}}));
    p::BodySettings body_settings;
    body_settings.shape = p::ShapeKind::sphere;
    body_settings.motion = p::MotionKind::dynamic;
    take(p::configure(runtime, body.id(), body_settings));

    auto box_actor = take(w::create_actor(runtime));
    auto box_root = take(w::create_component(runtime, world_types.scene_component));
    auto box_body = take(w::create_component(runtime, physics_types.body_component));
    const std::array box_attachments{w::Attachment{box_root.id(), {}, true}, w::Attachment{box_body.id()}};
    committed(world.attach_components(box_actor.id(), box_attachments));
    take(world.set_transform(box_root.id(), w::Trs{{20, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}));

    committed(world.attach(floor.id()));
    committed(world.attach(actor.id()));
    committed(world.attach(box_actor.id()));
    committed(world.start());
    require(scene->body_count() == 3, "Mesh, sphere and box bodies registered through Actor lifecycle");
    const auto floor_root = *take(w::root_component(runtime, floor.id()));
    auto vertices = floor_desc.mesh->vertices();
    take(w::set_geometry(runtime, floor_root,
        std::make_shared<const atelier::geometry::Mesh>(
            std::vector<atelier::geometry::Vertex>(vertices.begin(), vertices.end()))));
    take(scene->update(0));
    require(scene->body_count() == 3, "active static mesh replacement rebuilds its body");
    std::vector<p::ContactPhase> contacts;
    scene->on_contact([&](const p::ContactEvent& event) -> atelier::Result<void> {
        require((event.first_actor == floor.id() && event.second_actor == actor.id()) ||
            (event.second_actor == floor.id() && event.first_actor == actor.id()), "contact Actor identity");
        require((event.first_component == floor_body.id() && event.second_component == body.id()) ||
            (event.second_component == floor_body.id() && event.first_component == body.id()), "contact component identity");
        contacts.push_back(event.phase);
        return {};
    });
    for (int i = 0; i < 120; ++i) take(scene->update(1.f / 60.f));
    const auto resting = take(w::read_transform(runtime, root.id())).position.y;
    require(std::abs(resting - .5f) < .06f, "dynamic Actor rests on static mesh");
    require(std::find(contacts.begin(), contacts.end(), p::ContactPhase::began) != contacts.end(),
        "World contact began event");
    take(scene->set_gravity({0, 0, 0}));
    take(scene->add_force(actor.id(), {0, 100, 0}));
    take(scene->update(1.f / 60.f));
    const auto after_force = take(w::read_transform(runtime, root.id())).position.y;
    require(after_force > resting, "force moves dynamic Actor");
    take(scene->add_impulse(actor.id(), {0, 2, 0}));
    take(scene->update(1.f / 60.f));
    const auto after_impulse = take(w::read_transform(runtime, root.id())).position.y;
    require(after_impulse > after_force, "impulse moves dynamic Actor");
    require(std::find(contacts.begin(), contacts.end(), p::ContactPhase::ended) != contacts.end(),
        "World contact ended event");
    committed(world.set_enabled(body.id(), false));
    require(scene->body_count() == 2, "disabled body is removed");
    committed(world.stop());
    require(scene->body_count() == 0, "stopped World releases physics bodies");
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
    simulate_world();
    std::puts("Physics acceptance: PASS (Jolt, World mesh/Actor bodies, gravity, force, impulse, contact, cleanup)");
    return EXIT_SUCCESS;
}
