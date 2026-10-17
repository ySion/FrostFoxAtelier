namespace w = atelier::world;
namespace p = atelier::physics;
using atelier::math::Vector3;
constexpr float fixed_delta = 1.f / 60.f;

// Member order keeps the scene's borrowed World and Runtime alive during teardown.
struct PhysicsFixture {
    atelier::Runtime runtime;
    w::Types world_types = take(w::register_types(runtime));
    p::Types physics_types = take(p::register_types(runtime));
    w::World world{runtime, world_types};
    std::unique_ptr<p::PhysicsScene> scene = take(p::PhysicsScene::create(world, physics_types));

    struct Entity {
        atelier::ObjectRoot entity, body;
        atelier::ObjectId id() const { return entity.id(); }
    };
    Entity make(Vector3 position, p::BodySettings settings = {}, bool body_enabled = true) {
        Entity result{take(w::create_entity(runtime)),take(w::create_component(runtime,world_types.collision_component))};
        committed(world.attach_components(result.id(),std::array{w::Attachment{result.body.id()}}));
        take(world.set_transform(result.id(),w::Trs{position,{0,0,0,1},{1,1,1}}));
        settings.enabled=body_enabled;take(p::configure(runtime,result.body.id(),settings));
        committed(world.attach(result.id()));return result;
    }
    Entity ground() {
        p::BodySettings settings;
        settings.half_extents = {100, .5f, 100};
        return make({0, -.5f, 0}, settings);
    }
    void start() { committed(world.start()); }
    void advance(unsigned steps) { for (unsigned i = 0; i < steps; ++i) take(scene->update(fixed_delta)); }
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
float speed(Vector3 value) { return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); }

void rigid_shapes() {
    PhysicsFixture f;
    auto floor = f.ground();
    auto box = f.make({-3, 4, 0}, dynamic_body());
    auto sphere = f.make({0, 4, 0}, dynamic_body(p::ShapeKind::sphere));
    auto capsule = f.make({3, 4, 0}, dynamic_body(p::ShapeKind::capsule));
    p::BodySettings moving;
    moving.motion = p::MotionKind::kinematic;
    auto platform = f.make({10, 1, 0}, moving);
    auto invalid = dynamic_body();
    invalid.radius = 0;
    require(!p::configure(f.runtime, sphere.body.id(), invalid), "zero radius rejected");
    require(take(p::read_settings(f.runtime, sphere.body.id())) == dynamic_body(p::ShapeKind::sphere),
        "rejected configuration preserves the previous shape");
    f.start();
    f.advance(30);
    for (const auto* entity : {&box, &sphere, &capsule})
        require(f.state(*entity).position.y < 3.5f && f.state(*entity).linear_velocity.y < -1,
            "all primitive shapes fall under gravity");
    f.advance(330);
    require(near(f.state(box).position.y, .5f, .06f) && near(f.state(sphere).position.y, .5f, .06f) &&
        near(f.state(capsule).position.y, 1.f, .08f), "primitives rest at their respective support heights");
    for (const auto* entity : {&box, &sphere, &capsule})
        require(f.state(*entity).sleeping && speed(f.state(*entity).linear_velocity) < .01f,
            "settled primitives sleep with negligible velocity");
    require(near(f.state(floor).position.y, -.5f), "static ground stays fixed");
    require(!f.scene->add_force(floor.id(), {0, 1, 0}), "static body refuses force");
    take(f.scene->set_kinematic_target(f.handle(platform), {11, 2, 0}, {0, 0, 0, 1}));
    f.advance(1);
    require(near(f.state(platform).position.x, 11) && near(f.state(platform).position.y, 2),
        "kinematic target reaches its destination in one step");
}

void forces() {
    PhysicsFixture f;
    auto forced = f.make({-10, 0, 0}, dynamic_body());
    auto impulsed = f.make({10, 0, 0}, dynamic_body());
    auto control = f.make({30, 0, 0}, dynamic_body());
    f.start();
    take(f.scene->set_gravity({0, 0, 0}));
    take(f.scene->add_force(forced.id(), {600, 0, 0}));
    take(f.scene->add_impulse(impulsed.id(), {10, 0, 0}));
    require(near(f.state(forced).linear_velocity.x, 0), "force is queued until a fixed step");
    f.advance(1);
    const auto force_velocity = f.state(forced).linear_velocity.x;
    const auto impulse_velocity = f.state(impulsed).linear_velocity.x;
    require(force_velocity > 0 && near(force_velocity, impulse_velocity, .0001f),
        "force times fixed delta matches an equal impulse on identical masses");
    f.advance(1);
    require(f.state(forced).linear_velocity.x <= force_velocity * 1.001f &&
        f.state(impulsed).linear_velocity.x <= impulse_velocity * 1.001f,
        "one-shot forces and impulses are not consumed twice");
    require(near(speed(f.state(control).linear_velocity), 0), "zero-gravity control body remains still");
    take(f.scene->teleport(f.handle(impulsed), {20, 4, 0}, {0, 0, 0, 1}, true));
    f.advance(1);
    require(f.state(impulsed).position.x >= 20 && f.state(impulsed).linear_velocity.x > 0,
        "teleport can preserve momentum");
    take(f.scene->teleport(f.handle(impulsed), {20, 4, 0}, {0, 0, 0, 1}));
    f.advance(1);
    require(near(speed(f.state(impulsed).linear_velocity), 0), "default teleport clears momentum");
    take(f.scene->set_gravity({0, 3, 0}));
    f.advance(1);
    require(f.state(control).linear_velocity.y > 0, "custom upward gravity affects dynamic bodies");
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    require(!f.scene->set_gravity({nan, 0, 0}) && !f.scene->add_force(forced.id(), {nan, 0, 0}) &&
        !f.scene->add_impulse(forced.id(), {0, nan, 0}), "nonfinite physical inputs are rejected");
}

void queries() {
    PhysicsFixture f;
    p::BodySettings settings;
    settings.category = 2;
    settings.material = {.friction = .6f, .restitution = .25f};
    auto near_entity = f.make({0, 0, 0}, settings);
    auto far_entity = f.make({0, 0, 3}, settings);
    f.start();
    p::QueryFilter filter;
    filter.mode = p::QueryMode::all;
    const auto all = take(f.scene->raycast({0, 0, -3}, {0, 0, 6}, filter));
    require(all.hits.size() == 2 && all.hits[0].body.entity == near_entity.id() &&
        all.hits[1].body.entity == far_entity.id() && all.hits[0].fraction < all.hits[1].fraction,
        "all ray hits are sorted and carry Entity identities");
    require(all.step_id == 0 && all.has_block && all.hits[0].material == settings.material &&
        near(all.hits[0].normal.z, -1), "ray reports completed step, material and outward normal");
    filter.mode = p::QueryMode::closest;
    require(take(f.scene->raycast({0, 0, -3}, {0, 0, 6}, filter)).hits.size() == 1,
        "closest returns one hit");
    filter.mode = p::QueryMode::any;
    require(take(f.scene->raycast({0, 0, -3}, {0, 0, 6}, filter)).hits.size() == 1, "any returns one hit");
    filter.ignore_component = near_entity.body.id();
    const auto excluded = take(f.scene->raycast({0, 0, -3}, {0, 0, 6}, filter));
    require(excluded.hits.size() == 1 && excluded.hits[0].body.entity == far_entity.id(),
        "component exclusion removes only the selected component");
    filter.categories = 1;
    require(take(f.scene->raycast({0, 0, -3}, {0, 0, 6}, filter)).hits.empty(), "category mask excludes bodies");
    const auto overlap = take(f.scene->overlap_sphere({0, 0, 0}, .1f));
    require(overlap.hits.size() == 1 && overlap.hits[0].initial_overlap, "initial overlap is explicit");
    const auto sweep = take(f.scene->sweep_sphere({0, 0, -3}, {0, 0, 6}, .25f));
    require(sweep.hits.size() == 1 && sweep.hits[0].fraction < all.hits[0].fraction,
        "sphere sweep touches earlier than a point ray");
    filter.capacity = 0;
    require(!f.scene->raycast({0, 0, 0}, {0, 0, 1}, filter) &&
        !f.scene->overlap_sphere({}, 0) && !f.scene->sweep_sphere({}, {}, -1), "invalid query bounds rejected");
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    require(!f.scene->raycast({nan, 0, 0}, {}), "nonfinite query rejected");
    f.advance(1);
    require(take(f.scene->overlap_sphere({}, 1)).step_id == 1, "query stamp follows completed simulation");
}

void fixed_step() {
    PhysicsFixture f;
    auto entity = f.make({0, 10, 0}, dynamic_body());
    f.start();
    require(!f.scene->update(-1) && !f.scene->update(1.01f) &&
        !f.scene->update(std::numeric_limits<float>::quiet_NaN()), "invalid elapsed time rejected");
    require(take(f.scene->update(fixed_delta / 2)).steps == 0, "fractional frame waits for a full step");
    require(take(f.scene->update(fixed_delta / 2)).steps == 1, "two half frames form one step");
    const auto bounded = take(f.scene->update(1));
    require(bounded.steps == 8 && bounded.dropped > .86f && bounded.remaining < fixed_delta,
        "long frames cap work and report dropped time");
    require(take(f.scene->update(0)).steps == 0, "zero elapsed does not repeat completed steps");
    f.scene->set_paused(true);
    const auto before = f.state(entity);
    require(take(f.scene->update(.5f)).steps == 0 && f.state(entity).position == before.position,
        "pause freezes simulation without accumulating wall time");
    f.scene->step_once();
    f.scene->step_once();
    require(take(f.scene->update(0)).steps == 1 && take(f.scene->update(0)).steps == 0,
        "repeated step requests coalesce and consume once");
    f.scene->on_before_step([&](std::uint64_t, float) -> atelier::Result<void> {
        require(!f.scene->update(0), "recursive update is rejected");
        return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed, "expected before-step failure"});
    });
    f.scene->step_once();
    const auto pending = f.scene->completed_step();
    require(!f.scene->update(0) && f.scene->completed_step() == pending, "failed pre-step does not simulate");
    f.scene->on_before_step({});
    require(take(f.scene->update(0)).steps == 1, "failed pre-step preserves the pending single step for retry");
    f.scene->on_after_step([](std::uint64_t, float) -> atelier::Result<void> {
        throw std::runtime_error("expected after-step failure");
    });
    f.scene->step_once();
    require(!f.scene->update(0) && f.scene->completed_step() == pending + 2,
        "failed post-step still records the completed solve");
    f.scene->on_after_step({});
    require(take(f.scene->update(0)).steps == 0, "post-step retry does not repeat a completed solve");

    PhysicsFixture slow;
    PhysicsFixture fast;
    auto slow_entity = slow.make({0, 10, 0}, dynamic_body());
    auto fast_entity = fast.make({0, 10, 0}, dynamic_body());
    slow.start();
    fast.start();
    for (unsigned i = 0; i < 30; ++i) take(slow.scene->update(1.f / 30.f));
    for (unsigned i = 0; i < 120; ++i) take(fast.scene->update(1.f / 120.f));
    require(slow.scene->completed_step() == 60 && fast.scene->completed_step() == 60 &&
        near(slow.state(slow_entity).position.y, fast.state(fast_entity).position.y) &&
        near(slow.state(slow_entity).linear_velocity.y, fast.state(fast_entity).linear_velocity.y),
        "30 Hz and 120 Hz hosts produce the same 60 fixed steps");
}

void collision_matrix() {
    constexpr p::Response responses[]{p::Response::ignore, p::Response::overlap, p::Response::block};
    for (const bool sensor : {false, true}) for (const auto left : responses) for (const auto right : responses) {
        PhysicsFixture f;
        p::BodySettings barrier;
        barrier.motion = p::MotionKind::kinematic;
        barrier.sensor = sensor;
        barrier.category = 1;
        take(barrier.set_response(2,left));
        auto wall = f.make({}, barrier);
        auto moving = dynamic_body();
        moving.category = 2;
        take(moving.set_response(1,right));
        auto target = f.make({.75f, 0, 0}, moving);
        take(f.scene->set_gravity({}));
        f.start();
        unsigned began = 0;
        bool trigger = false;
        f.scene->on_contact([&](const p::ContactEvent& event) -> atelier::Result<void> {
            if (event.phase == p::ContactPhase::began) { ++began; trigger = event.trigger; }
            return {};
        });
        f.advance(30);
        const bool ignored = left == p::Response::ignore || right == p::Response::ignore;
        const bool blocked = !sensor && left == p::Response::block && right == p::Response::block;
        require(began == (ignored ? 0u : 1u), "collision matrix sends exactly one enter unless either side ignores");
        if (!ignored) require(trigger == !blocked, "sensor or overlap suppresses blocking in both response directions");
        require(blocked ? f.state(target).position.x > .9f : near(f.state(target).position.x, .75f),
            "only reciprocal Block without Sensor resolves penetration");
    }
}

void world_ownership() {
    PhysicsFixture f;
    auto owner = f.make({}, {}, false);
    f.start();
    w::World other(f.runtime, f.world_types);
    committed(other.start());
    auto other_scene = take(p::PhysicsScene::create(other, f.physics_types));
    require(!other_scene->create_character(owner.id()), "same Runtime does not allow characters from another World");
    const std::array<p::RagdollPart, 1> parts{{{"hip", -1, {}}}};
    require(!other_scene->create_ragdoll(owner.id(), parts), "same Runtime does not allow ragdolls from another World");
    const auto own = take(f.scene->create_character(owner.id()));
    require(!other_scene->read_character_state(own), "special handles are scene-local");
    take(f.scene->destroy_character(own));
    const auto doll = take(f.scene->create_ragdoll(owner.id(), parts));
    require(!other_scene->read_ragdoll_pose(doll), "ragdoll handles are scene-local");
    const auto removed = take(f.world.detach(owner.id()));
    require(!removed.commit.notification_error && !f.scene->read_ragdoll_pose(doll),
        "detaching an Entity invalidates its special system");
    require(!other_scene->create_character(owner.id()) && !other_scene->create_ragdoll(owner.id(), parts),
        "detached Entities cannot own a special system");
    committed(other.attach(owner.id()));
    const auto transferred = take(other_scene->create_character(owner.id()));
    require(!f.scene->create_character(owner.id()) && !f.scene->create_ragdoll(owner.id(), parts),
        "transferred Entity belongs exclusively to its new World");
    take(other_scene->destroy_character(transferred));
}

void failed_lifecycle() {
    enum class Exit { tick, task, activation, stop, close };
    struct FailedTask final : atelier::CoroutineFrame {
        atelier::Result<atelier::CoroutineStep> resume() override {
            return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed,"primary failure"});
        }
    };
    for (const auto exit : {Exit::tick, Exit::task, Exit::activation, Exit::stop, Exit::close}) {
        using Event = std::pair<char, atelier::ObjectId>;
        std::vector<Event> events;
        bool fail_tick{}, fail_activation{}, fail_cleanup{};
        PhysicsFixture f;
        take(f.world.set_failure_policy(w::FailurePolicy::halt));
        auto first = f.make({0, 4, 0}, dynamic_body());
        auto second = f.make({4, 4, 0}, dynamic_body());
        take(f.world.bind_behavior(f.world_types.entity, {
            .tick=[&](atelier::Runtime&, atelier::ObjectId, float)->atelier::Result<void> {
                if (fail_tick) return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed,"primary failure"});
                return {};
            },
            .end=[&](atelier::Runtime&, atelier::ObjectId id)->atelier::Result<void> {
                events.emplace_back('E', id);
                if (fail_cleanup) return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed,"end failure"});
                return {};
            }
        }));
        take(f.world.observe_lifecycle([&, first_id=first.id(), second_id=second.id()](w::World&, w::Lifecycle event, atelier::ObjectId id)->atelier::Result<void> {
            if (event == w::Lifecycle::activated && fail_activation)
                return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed,"primary failure"});
            if (event == w::Lifecycle::deactivated && (id == first_id || id == second_id)) {
                events.emplace_back('D', id);
                if (fail_cleanup) return std::unexpected(atelier::Error{atelier::ErrorCode::invocation_failed,"notification failure"});
            }
            return {};
        }));
        f.start();
        const auto first_handle = f.handle(first), second_handle = f.handle(second);
        take(f.scene->add_impulse(second.id(), {100, 0, 0}));
        fail_cleanup = true;
        if (exit == Exit::tick || exit == Exit::task) {
            fail_tick = exit == Exit::tick;
            if (exit == Exit::task) take(f.world.start_task(first.id(), [](atelier::Runtime& runtime, atelier::ObjectId) {
                return atelier::Coroutine::create(runtime, std::make_unique<FailedTask>());
            }));
            const auto updated = f.scene->update(fixed_delta);
            require(!updated && updated.error().message == "primary failure", "Cleanup replaced the original execution failure.");
        } else {
            if (exit == Exit::activation) {
                committed(f.world.set_enabled(first.body.id(), false));
                fail_activation = true;
            }
            const auto result = take(exit == Exit::activation ? f.world.set_enabled(first.body.id(), true)
                : exit == Exit::stop ? f.world.stop() : f.world.close());
            require(result.changed && result.notification_error && result.notification_error->message ==
                (exit == Exit::activation ? "primary failure" : "end failure"), "Committed exit lost the first failure.");
        }
        require(!f.world.running() && f.world.activity().tasks == 0 && f.scene->body_count() == 0,
            "Failed World exit retained tasks or physical bodies.");
        require(!f.scene->read_body_state(first_handle) && !f.scene->read_body_state(second_handle) &&
            !f.scene->body_handle(second.id()) && take(f.scene->overlap_sphere({4, 4, 0}, 2)).hits.empty(),
            "Stopped World retained a physics handle or query hit.");
        const std::vector<Event> expected{{'E', second.id()}, {'E', first.id()}, {'D', second.id()}, {'D', first.id()}};
        require(events == expected, "Failed exit did not finish reverse End and deactivation exactly once.");
        fail_tick = fail_activation = fail_cleanup = false;
        if (exit != Exit::close) {
            committed(f.world.stop()); committed(f.world.stop());
            require(events == expected, "Repeated stop repeated failed cleanup.");
            f.start();
            const auto fresh = f.handle(second);
            require(fresh != second_handle, "Restart reused a body from the failed run.");
            take(f.scene->update(fixed_delta));
            require(near(f.state(second).linear_velocity.x, 0), "Failed run's queued impulse affected the restart.");
            committed(f.world.stop());
            require(events.size() == expected.size() * 2, "Restart did not receive one independent exit.");
        }
        const auto exits = events.size();
        committed(f.world.close()); committed(f.world.close());
        require(events.size() == exits, "Close repeated completed End or deactivation.");
    }
}

void lifecycle() {
    failed_lifecycle();
    PhysicsFixture f;
    auto entity = f.make({0, 4, 0}, dynamic_body());
    f.start();
    PhysicsFixture other;
    auto other_entity = other.make({0, 4, 0}, dynamic_body());
    other.start();
    const auto old = f.handle(entity);
    require(!other.scene->read_body_state(old) && !other.scene->teleport(old, {}, {0, 0, 0, 1}),
        "another scene rejects foreign body handles");
    take(f.scene->add_impulse(entity.id(), {100, 0, 0}));
    committed(f.world.set_enabled(entity.body.id(), false));
    committed(f.world.set_enabled(entity.body.id(), false));
    require(f.scene->body_count() == 0 && !f.scene->read_body_state(old), "repeated disable is harmless");
    committed(f.world.set_enabled(entity.body.id(), true));
    require(f.handle(entity).revision != old.revision, "reenable creates a new body generation");
    const auto step = take(f.scene->update(fixed_delta));
    require(step.rejected_commands == 1 && near(f.state(entity).linear_velocity.x, 0),
        "a queued command cannot affect a replacement body");
    committed(f.world.stop());
    committed(f.world.stop());
    require(f.scene->body_count() == 0, "repeated World stop releases all bodies");
    committed(f.world.start());
    require(f.scene->body_count() == 1 && !f.scene->read_body_state(old), "restart retains no stale body generation");
    f.scene.reset();
    other.advance(1);
    require(other.state(other_entity).linear_velocity.y < 0, "destroying one scene keeps other scenes alive");
    f.scene = take(p::PhysicsScene::create(f.world, f.physics_types));
    require(f.scene->body_count() == 1, "new scene reconciles an already running World");
    committed(f.world.close());
    committed(f.world.close());
    require(f.scene->body_count() == 0 && !p::PhysicsScene::create(f.world, f.physics_types),
        "closed World cleans bodies and refuses new scenes");
}

void character() {
    PhysicsFixture f;
    auto floor = f.ground();
    auto entity = f.make({0, 3, 0}, {}, false);
    f.start();
    require(!f.scene->create_character(entity.id(), {.radius = 0}), "invalid character shape rejected");
    const auto handle = take(f.scene->create_character(entity.id()));
    require(!f.scene->create_character(entity.id()), "duplicate controller rejected");
    f.advance(120);
    const auto landed = take(f.scene->read_character_state(handle));
    require(landed.grounded && landed.position.y > .8f && landed.position.y < 1.2f, "character lands on ground");
    require(!f.scene->set_character_input(handle, {0, 1, 0}), "vertical movement input rejected");
    take(f.scene->set_character_input(handle, {2, 0, 0}, true));
    f.advance(1);
    const auto jump = take(f.scene->read_character_state(handle));
    require(jump.velocity.y > 0 && jump.position.x > landed.position.x, "jump and planar movement work together");
    take(f.scene->set_character_input(handle, {}));
    f.advance(180);
    require(take(f.scene->read_character_state(handle)).grounded, "jump is consumed once and character lands again");
    take(f.scene->destroy_character(handle));
    require(!f.scene->destroy_character(handle) && !f.scene->set_character_input(handle, {}),
        "stale controller cannot be destroyed or driven again");
    const auto next = take(f.scene->create_character(entity.id()));
    require(next.revision != handle.revision, "new controller has fresh identity");
    committed(f.world.set_enabled(entity.entity.id(), false));
    require(!f.scene->read_character_state(next), "Entity deactivation removes the controller");
}

void constraint() {
    PhysicsFixture f;
    auto anchor = f.make({0, 5, 0});
    auto pendulum = f.make({0, 3, 0}, dynamic_body());
    f.start();
    require(!f.scene->create_distance_constraint(f.handle(anchor), f.handle(anchor), 0, 2),
        "constraint rejects the same body twice");
    require(!f.scene->create_distance_constraint(f.handle(anchor), f.handle(pendulum), 3, 2),
        "constraint rejects inverted bounds");
    const auto handle = take(f.scene->create_distance_constraint(f.handle(anchor), f.handle(pendulum), 2, 2));
    take(f.scene->add_impulse(pendulum.id(), {100, 0, 0}));
    f.advance(120);
    const auto position = f.state(pendulum).position;
    require(near(speed({position.x, position.y - 5, position.z}), 2, .1f), "constraint holds its distance under gravity");
    require(f.state(pendulum).sleeping, "supported pendulum has settled to sleep before release");
    take(f.scene->destroy_constraint(handle));
    require(!f.scene->destroy_constraint(handle), "destroyed constraint rejects repeat destruction");
    const auto before = f.state(pendulum).position.y;
    f.advance(60);
    require(f.state(pendulum).position.y < before - 1, "body falls freely after constraint removal");
    take(f.scene->teleport(f.handle(pendulum), {0, 3, 0}, {0, 0, 0, 1}));
    f.advance(1);
    const auto next = take(f.scene->create_distance_constraint(f.handle(anchor), f.handle(pendulum), 2, 2));
    f.advance(120);
    require(f.state(pendulum).sleeping, "replacement constraint settles the pendulum again");
    const auto supported = f.state(pendulum).position.y;
    committed(f.world.set_enabled(anchor.body.id(), false));
    require(!f.scene->destroy_constraint(next), "removing an endpoint removes its constraint");
    f.advance(60);
    require(f.state(pendulum).position.y < supported - 1,
        "implicit constraint removal wakes the surviving endpoint");
}

void ragdoll() {
    PhysicsFixture f;
    auto floor = f.ground();
    auto entity = f.make({0, 3, 0}, {}, false);
    f.start();
    std::array<p::RagdollPart, 2> parts{{{"hip", -1, {}}, {"head", 0, {0, .6f, 0}, .15f, .15f}}};
    auto invalid = parts;
    invalid[1].parent = 1;
    require(!f.scene->create_ragdoll(entity.id(), invalid), "self-parented ragdoll part rejected");
    require(!f.scene->create_ragdoll(entity.id(), {}), "empty ragdoll rejected");
    const auto handle = take(f.scene->create_ragdoll(entity.id(), parts));
    require(!f.scene->create_ragdoll(entity.id(), parts), "duplicate ragdoll rejected");
    const auto initial = take(f.scene->read_ragdoll_pose(handle));
    f.advance(180);
    const auto pose = take(f.scene->read_ragdoll_pose(handle));
    require(pose.positions.size() == 2 && pose.rotations.size() == 2 && pose.step_id == 180,
        "ragdoll returns every part with a completed step");
    for (std::size_t i = 0; i < pose.positions.size(); ++i)
        require(atelier::math::is_finite(pose.positions[i]) && pose.positions[i].y > 0 &&
            pose.positions[i].y < initial.positions[i].y, "ragdoll parts fall and collide with ground");
    take(f.scene->destroy_ragdoll(handle));
    require(!f.scene->destroy_ragdoll(handle), "stale ragdoll destruction rejected");
    const auto next = take(f.scene->create_ragdoll(entity.id(), parts));
    require(next.revision != handle.revision, "ragdoll recreation uses a new generation");
    committed(f.world.stop());
    require(!f.scene->read_ragdoll_pose(next), "World stop removes the live ragdoll");
}

void vehicle() {
    PhysicsFixture f;
    auto floor = f.ground();
    auto settings = dynamic_body();
    settings.half_extents = {.8f, .25f, 1.4f};
    auto chassis = f.make({0, 1.5f, 0}, settings);
    f.start();
    require(!f.scene->create_vehicle(f.handle(floor)), "vehicle requires a dynamic chassis");
    require(!f.scene->create_vehicle(f.handle(chassis), {.wheel_radius = 0}), "invalid wheel radius rejected");
    const auto handle = take(f.scene->create_vehicle(f.handle(chassis)));
    require(!f.scene->create_vehicle(f.handle(chassis)), "duplicate chassis controller rejected");
    require(!f.scene->set_vehicle_input(handle, 2, 0, 0) && !f.scene->set_vehicle_input(handle, 0, 0, -1),
        "out-of-range driver input rejected");
    f.advance(120);
    require(std::ranges::all_of(take(f.scene->read_vehicle_state(handle)).wheel_contact, [](bool c) { return c; }),
        "all four wheels contact a flat ground");
    take(f.scene->set_vehicle_input(handle, .6f, 0, 0));
    f.advance(180);
    const auto driving = take(f.scene->read_vehicle_state(handle));
    require(std::abs(driving.position.z) > 1 && speed(driving.velocity) > 1, "throttle accelerates the chassis");
    take(f.scene->set_vehicle_input(handle, .3f, .5f, 0));
    f.advance(60);
    require(std::abs(take(f.scene->read_vehicle_state(handle)).position.x - driving.position.x) > .1f,
        "steering changes the vehicle trajectory");
    const auto moving_speed = speed(take(f.scene->read_vehicle_state(handle)).velocity);
    take(f.scene->set_vehicle_input(handle, 0, 0, 1));
    f.advance(180);
    require(speed(take(f.scene->read_vehicle_state(handle)).velocity) < moving_speed * .5f,
        "brake reduces forward speed");
    take(f.scene->destroy_vehicle(handle));
    require(!f.scene->destroy_vehicle(handle) && !f.scene->set_vehicle_input(handle, 0, 0, 0),
        "removed vehicle cannot be driven or destroyed again");
    const auto next = take(f.scene->create_vehicle(f.handle(chassis)));
    require(next.revision != handle.revision, "recreated vehicle uses a new generation");
    committed(f.world.set_enabled(chassis.body.id(), false));
    require(!f.scene->read_vehicle_state(next), "chassis deactivation removes wheel listeners and constraint");
    f.advance(1);
}
