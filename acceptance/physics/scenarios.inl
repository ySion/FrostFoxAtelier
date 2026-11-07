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

    w::World world{runtime, world_types};

    pr::Scene shown = take(pr::Scene::create(world, presentation_types));

    std::unique_ptr<p::PhysicsScene> scene = take(p::PhysicsScene::create(world, physics_types, pr::read_mesh_source));

    struct Entity {
        atelier::ObjectRoot entity;

        atelier::ObjectRoot body;

        atelier::ObjectId id() const { return entity.id(); }
    };

    Entity make(Vector3 position, p::BodySettings settings = {}, bool body_enabled = true) {
        Entity result{take(w::create_entity(runtime)),
                      take(w::create_component(runtime, world_types.collision_component))};
        committed(world.attach_components(result.id(), std::array{w::Attachment{result.body.id()}}));
        take(world.set_transform(result.id(), w::Trs{position, {0, 0, 0, 1}, {1, 1, 1}}));
        settings.enabled = body_enabled;
        take(p::configure(runtime, result.body.id(), settings));
        committed(world.attach(result.id()));
        return result;
    }

    Entity ground() {
        p::BodySettings settings;
        settings.half_extents = {100, .5f, 100};
        return make({0, -.5f, 0}, settings);
    }

    void start() { committed(world.start()); }

    void advance(unsigned steps) {
        for (unsigned i = 0; i < steps; ++i)
            step(world, *scene, fixed_delta);
    }

    p::BodyHandle handle(const Entity& entity) { return take(scene->body_handle(entity.id())); }

    p::BodyState state(const Entity& entity) { return take(scene->read_body_state(handle(entity))); }
};

p::BodySettings dynamic_body(p::ShapeKind shape = p::ShapeKind::box) {
    p::BodySettings settings;
    settings.motion = p::MotionKind::dynamic;
    settings.shape = shape;
    return settings;
}

bool near(float actual, float expected, float tolerance = .001f) {
    return std::isfinite(actual) && std::abs(actual - expected) < tolerance;
}

float speed(Vector3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

void rigid_shapes() {
    PhysicsFixture f;
    auto floor = f.ground();
    auto box = f.make({-3, 4, 0}, dynamic_body());
    f.start();
    f.advance(30);
    f.advance(330);
    require(near(f.state(box).position.y, .5f, .06f), "box rests at its support height");
}

void forces() {
    PhysicsFixture f;
    auto impulsed = f.make({10, 0, 0}, dynamic_body());
    f.start();
    take(f.scene->set_gravity({0, 0, 0}));
    take(f.scene->add_impulse(f.handle(impulsed), {10, 0, 0}));
    f.advance(1);
    take(f.scene->teleport(f.handle(impulsed), {20, 4, 0}, {0, 0, 0, 1}));
    f.advance(1);
    require(near(speed(f.state(impulsed).linear_velocity), 0), "default teleport clears momentum");
}

void before_step_exit(bool close) {
    PhysicsFixture f;
    const auto entity = f.make({3, 4, 5}, dynamic_body());
    f.start();
    f.advance(1);
    const auto completed = f.scene->completed_step();
    const auto pose = take(w::read_transform(f.runtime, entity.id()));
    const auto handle = f.handle(entity);
    unsigned exits{};
    unsigned after_calls{};
    f.scene->on_before_step([&](std::uint64_t, float) -> atelier::Result<void> {
        ATELIER_TRY_BIND(stopped, close ? f.world.close() : f.world.stop());
        if (stopped->notification_error)
            return std::unexpected(*stopped->notification_error);
        ++exits;
        return {};
    });
    f.scene->on_after_step([&](std::uint64_t, float) -> atelier::Result<void> {
        ++after_calls;
        return {};
    });
    const auto updated = f.world.update(3 * fixed_delta);
    f.scene->on_before_step({});
    f.scene->on_after_step({});
    require(exits == 1, "before-step did not complete exactly one requested World exit");
    if (!updated)
        throw std::runtime_error(std::string(close ? "successful before-step close made update fail: "
                                                   : "successful before-step stop made update fail: ") +
                                 updated.error().message);
    require(!f.world.running() && f.world.closed() == close && f.scene->body_count() == 0 &&
                !f.scene->read_body_state(handle),
            "successful before-step exit retained World participation or a physical body");
    require(after_calls == 0 && f.scene->completed_step() == completed &&
                take(w::read_transform(f.runtime, entity.id())) == pose,
            "successful before-step exit still solved, wrote a pose or called after-step");
    require(f.scene->last_step().steps == 0 && f.scene->last_step().completed_step == completed &&
                f.scene->last_step().remaining == 0,
            "successful before-step exit retained the previous step report or pending time");
    if (!close) {
        f.start();
        require(f.handle(entity) != handle && step(f.world, *f.scene, 0).steps == 0,
                "restart retained a stopped body's handle or pending substeps");
        require(step(f.world, *f.scene, fixed_delta).steps == 1 && f.scene->completed_step() == completed + 1,
                "World could not resume after a successful before-step stop");
    }
    committed(f.world.close());
}

void callback_exit(bool contact, bool close) {
    PhysicsFixture f;
    p::BodySettings sensor;
    sensor.motion = p::MotionKind::kinematic;
    sensor.sensor = true;
    sensor.half_extents = {10, 1, 1};
    f.make({}, sensor);
    const auto left = f.make({-3, 0, 0}, dynamic_body());
    const auto right = f.make({3, 0, 0}, dynamic_body());
    take(f.scene->set_gravity({}));
    f.start();
    const auto old_left = f.handle(left);
    const auto old_right = f.handle(right);
    unsigned exits{};
    unsigned after_calls{};
    unsigned contacts{};
    const auto exit = [&]() -> atelier::Result<void> {
        ATELIER_TRY_BIND(stopped, close ? f.world.close() : f.world.stop());
        if (stopped->notification_error)
            return std::unexpected(*stopped->notification_error);
        ++exits;
        return {};
    };
    f.scene->on_after_step([&](std::uint64_t, float) -> atelier::Result<void> {
        ++after_calls;
        if (!contact && !exits)
            return exit();
        return {};
    });
    f.scene->on_contact([&](const p::ContactEvent&) -> atelier::Result<void> {
        ++contacts;
        if (contact && !exits)
            return exit();
        return {};
    });
    // Contact dispatch follows all three solves; after-step stops after the first.
    take(f.world.update(3.5f * fixed_delta));
    f.scene->on_after_step({});
    f.scene->on_contact({});
    const unsigned completed = contact ? 3u : 1u;
    require(exits == 1 && !f.world.running() && f.world.closed() == close && f.scene->body_count() == 0 &&
                !f.scene->read_body_state(old_left) && !f.scene->read_body_state(old_right),
            "successful after/contact exit retained World participation or a body handle");
    require(after_calls == completed && contacts == (contact ? 1u : 0u),
            "successful after/contact exit delivered subsequent callbacks");
    require(f.scene->completed_step() == completed && f.scene->last_step().completed_step == completed &&
                f.scene->last_step().steps == completed && f.scene->last_step().remaining == 0 &&
                f.scene->presented_poses().empty(),
            "successful after/contact exit lost completed solves or retained pending time/presentation");
    if (!close) {
        std::vector<p::ContactEvent> restarted;
        f.scene->on_contact([&](const p::ContactEvent& event) -> atelier::Result<void> {
            restarted.push_back(event);
            return {};
        });
        f.start();
        require(f.handle(left) != old_left && f.handle(right) != old_right && f.scene->body_count() == 3,
                "restart after a callback exit reused the old body generation");
        require(step(f.world, *f.scene, 0).steps == 0 && restarted.empty(),
                "restart dispatched contact events or pending substeps from the stopped run");
        require(step(f.world, *f.scene, fixed_delta).steps == 1 && restarted.size() == 2 &&
                    std::ranges::all_of(restarted,
                                        [&](const auto& event) {
                                            return event.phase == p::ContactPhase::began &&
                                                   event.step_id == completed + 1;
                                        }),
                "restart did not produce fresh contacts at the next completed step");
        f.scene->on_contact({});
    }
    committed(f.world.close());
}

void fixed_step() {
    callback_exit(false, false);
    callback_exit(true, false);
    before_step_exit(false);
    PhysicsFixture f;
    auto entity = f.make({0, 10, 0}, dynamic_body());
    f.start();
    f.scene->set_paused(true);
    f.scene->on_before_step([&](std::uint64_t, float) -> atelier::Result<void> {
        return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed, "expected before-step failure"});
    });
    f.scene->step_once();
    const auto pending = f.scene->completed_step();
    require(!f.world.update(0) && f.scene->completed_step() == pending && !f.world.running() &&
                f.scene->body_count() == 0,
            "a failed pre-step simulates nothing and stops the World");
    f.scene->on_before_step({});
    f.start();
    require(step(f.world, *f.scene, 0).steps == 0, "a stopped World discards its pending single step");
    f.scene->on_after_step([](std::uint64_t, float) -> atelier::Result<void> {
        return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed, "expected after-step failure"});
    });
    f.scene->step_once();
    require(!f.world.update(0) && f.scene->completed_step() == pending + 1 && !f.world.running(),
            "a failed post-step keeps the completed solve and stops the World");
    f.scene->on_after_step({});
}

void failed_lifecycle() {
    enum class Exit { tick, task, activation, stop, close };
    struct FailedTask final : atelier::CoroutineFrame {
        atelier::Result<atelier::CoroutineStep> resume() noexcept override {
            return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed, "primary failure"});
        }
    };
    for (const auto exit : {Exit::tick, Exit::task, Exit::activation, Exit::stop, Exit::close}) {
        using Event = std::pair<char, atelier::ObjectId>;
        std::vector<Event> events;
        bool fail_tick{};
        bool fail_activation{};
        bool fail_cleanup{};
        PhysicsFixture f;
        take(f.world.set_failure_policy(w::FailurePolicy::halt));
        auto first = f.make({0, 4, 0}, dynamic_body());
        auto second = f.make({4, 4, 0}, dynamic_body());
        take(f.world.bind_behavior(
            f.world_types.entity,
            {.tick = [&](atelier::Runtime&, atelier::ObjectId, float) -> atelier::Result<void> {
                 if (fail_tick)
                     return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed, "primary failure"});
                 return {};
             },
             .end = [&](atelier::Runtime&, atelier::ObjectId id) -> atelier::Result<void> {
                 events.emplace_back('E', id);
                 if (fail_cleanup)
                     return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed, "end failure"});
                 return {};
             }}));
        take(f.world.observe_lifecycle(
            [&, first_id = first.id(), second_id = second.id()](w::World&, w::Lifecycle event,
                                                                atelier::ObjectId id) -> atelier::Result<void> {
                if (event == w::Lifecycle::activated && fail_activation)
                    return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed, "primary failure"});
                if (event == w::Lifecycle::deactivated && (id == first_id || id == second_id)) {
                    events.emplace_back('D', id);
                    if (fail_cleanup)
                        return std::unexpected(
                            atelier::Error{atelier::ErrorCode::invocation_failed, "notification failure"});
                }
                return {};
            }));
        f.start();
        const auto first_handle = f.handle(first);
        const auto second_handle = f.handle(second);
        take(f.scene->add_impulse(f.handle(second), {100, 0, 0}));
        fail_cleanup = true;
        if (exit == Exit::tick || exit == Exit::task) {
            fail_tick = exit == Exit::tick;
            if (exit == Exit::task)
                take(f.world.start_task(first.id(), [](atelier::Runtime& runtime, atelier::ObjectId) {
                    return atelier::Coroutine::create(runtime, std::make_unique<FailedTask>());
                }));
            (void)f.world.update(fixed_delta);
        } else {
            if (exit == Exit::activation) {
                committed(f.world.set_enabled(first.body.id(), false));
                fail_activation = true;
            }
            (void)take(exit == Exit::activation ? f.world.set_enabled(first.body.id(), true)
                       : exit == Exit::stop     ? f.world.stop()
                                                : f.world.close());
        }
        require(!f.world.running() && w::detail::Access::state(f.world).tasks.size() == 0 && f.scene->body_count() == 0,
                "Failed World exit retained tasks or physical bodies.");
        require(!f.scene->read_body_state(first_handle) && !f.scene->read_body_state(second_handle) &&
                    !f.scene->body_handle(second.id()) && take(f.scene->overlap_sphere({4, 4, 0}, 2)).hits.empty(),
                "Stopped World retained a physics handle or query hit.");
        const std::vector<Event> expected{{'E', second.id()}, {'E', first.id()}, {'D', second.id()}, {'D', first.id()}};
        require(events == expected, "Failed exit did not finish reverse End and deactivation exactly once.");
        fail_tick = fail_activation = fail_cleanup = false;
        if (exit != Exit::close) {
            f.start();
            const auto fresh = f.handle(second);
            require(fresh != second_handle, "Restart reused a body from the failed run.");
            step(f.world, *f.scene, fixed_delta);
            require(near(f.state(second).linear_velocity.x, 0), "Failed run's queued impulse affected the restart.");
            committed(f.world.stop());
            require(events.size() == expected.size() * 2, "Restart did not receive one independent exit.");
        }
        committed(f.world.close());
    }
}

void lifecycle() {
    callback_exit(false, true);
    callback_exit(true, true);
    before_step_exit(true);
    failed_lifecycle();
    PhysicsFixture f;
    auto entity = f.make({0, 4, 0}, dynamic_body());
    f.start();
    PhysicsFixture other;
    auto other_entity = other.make({0, 4, 0}, dynamic_body());
    other.start();
    const auto old = f.handle(entity);
    take(f.scene->add_impulse(f.handle(entity), {100, 0, 0}));
    committed(f.world.set_enabled(entity.body.id(), false));
    require(f.scene->body_count() == 0 && !f.scene->read_body_state(old), "disable releases the old body");
    committed(f.world.set_enabled(entity.body.id(), true));
    require(f.handle(entity).revision != old.revision, "reenable creates a new body generation");
    step(f.world, *f.scene, fixed_delta);
    require(near(f.state(entity).linear_velocity.x, 0), "a queued command cannot affect a replacement body");
    committed(f.world.stop());
    require(f.scene->body_count() == 0, "World stop releases all bodies");
    committed(f.world.start());
    require(f.scene->body_count() == 1 && !f.scene->read_body_state(old), "restart retains no stale body generation");
    f.scene.reset();
    other.advance(1);
    require(other.state(other_entity).linear_velocity.y < 0, "destroying one scene keeps other scenes alive");
    f.scene = take(p::PhysicsScene::create(f.world, f.physics_types, pr::read_mesh_source));
    require(f.scene->body_count() == 1, "new scene cannot reconcile analytic bodies");
    committed(f.world.close());
    require(f.scene->body_count() == 0, "closed World cleans bodies");
}

void character() {
    PhysicsFixture f;
    auto floor = f.ground();
    auto entity = f.make({0, 3, 0}, {}, false);
    f.start();
    const auto handle = take(f.scene->create_character(entity.id()));
    f.advance(120);
    const auto landed = take(f.scene->read_character_state(handle));
    require(landed.grounded && landed.position.y > .8f && landed.position.y < 1.2f, "character lands on ground");
    take(f.scene->destroy_character(handle));
}

void constraint() {
    PhysicsFixture f;
    auto anchor = f.make({0, 5, 0});
    auto pendulum = f.make({0, 3, 0}, dynamic_body());
    f.start();
    const auto handle = take(f.scene->create_distance_constraint(f.handle(anchor), f.handle(pendulum), 2, 2));
    take(f.scene->add_impulse(f.handle(pendulum), {100, 0, 0}));
    f.advance(120);
    const auto position = f.state(pendulum).position;
    require(near(speed({position.x, position.y - 5, position.z}), 2, .1f),
            "constraint holds its distance under gravity");
    take(f.scene->destroy_constraint(handle));
}
