namespace w = atelier::world;

namespace pr = atelier::presentation;

namespace p = atelier::physics;

using atelier::math::Vector3;

constexpr float fixed_delta = 1.f / 60.f;

// Member order keeps the scene's borrowed World and Runtime alive during teardown.
struct PhysicsFixture {
    atelier::Runtime runtime;

    w::Types world_types = take(w::register_types(runtime));

    pr::Types presentation_types = take(pr::register_types(runtime));

    p::Types physics_types = take(p::register_types(runtime));

    w::World world;

    pr::Scene shown = take([&]() -> atelier::Result<pr::Scene> {
        ATELIER_TRY(world.initialize(runtime, world_types));
        return pr::Scene::create(world, presentation_types);
    }());

    std::unique_ptr<p::PhysicsScene> scene = take(p::PhysicsScene::create(world, physics_types, pr::read_mesh_source));

    struct Entity {
        atelier::ObjectRoot entity;

        atelier::ObjectRoot body;

        atelier::ObjectId id() const { return entity.id(); }
    };

    Entity make(Vector3 position, p::BodySettings settings = {}) {
        Entity result{take(w::create_entity(runtime)),
                      take(w::create_component(runtime, world_types.collision_component))};
        const std::array added{w::Attachment{result.body.id()}};
        committed(world.attach_components(result.id(), added));
        take(world.set_transform(result.id(), w::Trs{position, {0, 0, 0, 1}, {1, 1, 1}}));
        settings.enabled = true;
        take(p::configure(runtime, result.body.id(), settings));
        committed(world.attach(result.id()));
        return result;
    }
};

p::BodySettings dynamic_body() {
    p::BodySettings settings;
    settings.motion = p::MotionKind::dynamic;
    return settings;
}

void rigid_shapes() {
    PhysicsFixture f;
    p::BodySettings ground;
    ground.half_extents = {100, .5f, 100};
    auto floor = f.make({0, -.5f, 0}, ground);
    auto box = f.make({-3, 4, 0}, dynamic_body());
    committed(f.world.start());
    for (unsigned i = 0; i < 360; ++i)
        take(f.world.update(fixed_delta));
    const auto state = take(f.scene->read_body_state(take(f.scene->body_handle(box.id()))));
    require(std::abs(state.position.y - .5f) < .06f, "Box did not rest at its analytic support height.");
}

void failed_lifecycle() {
    PhysicsFixture f;
    take(f.world.set_failure_policy(w::FailurePolicy::halt));
    auto body = f.make({0, 4, 0}, dynamic_body());
    bool fail = true;
    take(f.world.bind_behavior(
        f.world_types.entity, {.tick = [&](atelier::Runtime&, atelier::ObjectId, float) -> atelier::Result<void> {
            if (fail)
                return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed, "Injected tick failure."});
            return {};
        }}));
    committed(f.world.start());
    take(f.scene->add_impulse(take(f.scene->body_handle(body.id())), {100, 0, 0}));
    (void)f.world.update(fixed_delta);
    require(!f.world.running() && f.scene->body_count() == 0 &&
                take(f.scene->overlap_sphere({0, 4, 0}, 2)).hits.empty(),
            "Failed tick retained a live physics body.");
    fail = false;
    committed(f.world.start());
    take(f.world.update(fixed_delta));
    const auto state = take(f.scene->read_body_state(take(f.scene->body_handle(body.id()))));
    require(std::abs(state.linear_velocity.x) < .001f, "Failed run's queued impulse affected restart.");
    committed(f.world.close());
    require(f.scene->body_count() == 0, "Restarted World leaked its body on close.");
}
