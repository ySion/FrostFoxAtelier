void simulate_world() {
    namespace w = atelier::world;
    namespace pr = atelier::presentation;
    namespace p = atelier::physics;
    atelier::Runtime runtime;
    const auto world_types = take(w::register_types(runtime));
    (void)take(pr::register_types(runtime));
    const auto physics_types = take(p::register_types(runtime));
    w::World world(runtime, world_types);
    auto scene = take(p::PhysicsScene::create(world, physics_types, pr::read_mesh_source));

    pr::StaticMeshEntityDesc floor_desc;
    floor_desc.mesh = atelier::geometry::cube_mesh();
    floor_desc.transform.position = {0, -.5f, 0};
    floor_desc.transform.scale = {8, 1, 8};
    floor_desc.mesh_source = "acceptance/floor";
    auto floor = take(pr::create_static_mesh_entity(world, floor_desc));
    auto floor_body = take(runtime.hold(mesh_component(runtime, floor.id())));
    auto floor_sibling = take(w::create_component(runtime, world_types.collision_component));
    committed(world.attach_components(floor.id(), std::array{w::Attachment{floor_sibling.id()}}));
    committed(world.set_enabled(floor_sibling.id(), false));
    const auto captured = take(pr::read_mesh_source(runtime, floor_body.id()));
    require(captured.source == floor_desc.mesh_source && captured.resolved == floor_desc.mesh,
            "geometry reading loses its exact source or immutable content");
    p::BodySettings floor_settings;
    floor_settings.shape = p::ShapeKind::triangle_mesh;
    auto invalid_mesh = floor_settings;
    invalid_mesh.motion = p::MotionKind::dynamic;
    require(!p::configure(runtime, floor_body.id(), invalid_mesh), "dynamic triangle mesh rejected");
    take(p::configure(runtime, floor_body.id(), floor_settings));

    auto entity = take(w::create_entity(runtime));
    auto body = take(w::create_component(runtime, world_types.collision_component));
    auto disabled_body = take(w::create_component(runtime, world_types.collision_component));
    const std::array entity_attachments{w::Attachment{body.id()}, w::Attachment{disabled_body.id()}};
    committed(world.attach_components(entity.id(), entity_attachments));
    committed(world.set_enabled(disabled_body.id(), false));
    take(world.set_transform(entity.id(), w::Trs{{0, 2, 0}, {0, 0, 0, 1}, {1, 1, 1}}));
    p::BodySettings body_settings;
    body_settings.shape = p::ShapeKind::sphere;
    body_settings.motion = p::MotionKind::dynamic;
    take(p::configure(runtime, body.id(), body_settings));

    auto box_entity = take(w::create_entity(runtime));
    auto box_collision = take(w::create_component(runtime, world_types.collision_component));
    const std::array box_attachments{w::Attachment{box_collision.id()}};
    committed(world.attach_components(box_entity.id(), box_attachments));
    take(world.set_transform(box_entity.id(), w::Trs{{20, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}));

    committed(world.attach(floor.id()));
    committed(world.attach(entity.id()));
    committed(world.attach(box_entity.id()));
    committed(world.start());
    require(scene->body_count() == 3, "Mesh, sphere and box bodies registered through Entity lifecycle");
    const auto first_handle = take(scene->body_handle(floor.id()));
    const auto entity_handle = take(scene->body_handle(entity.id()));
    require(take(scene->read_body_state(entity_handle)).step_id == 0, "initial body state has step zero");
    const auto ray = take(scene->raycast({0, 4, 0}, {0, -2, 0}));
    require(!ray.hits.empty() && ray.hits.front().body.entity == entity.id(), "ray returns nearest Entity");
    const auto ground_ray = take(scene->raycast({0, 4, 0}, {0, -2, 0}, p::QueryFilter{.ignore_entity = entity.id()}));
    require(!ground_ray.hits.empty() && ground_ray.hits.front().body.entity == floor.id(),
            "ray ignore Entity finds mesh ground");
    const auto sweep =
        take(scene->sweep_sphere({0, 4, 0}, {0, -2, 0}, .2f, p::QueryFilter{.ignore_entity = entity.id()}));
    require(!sweep.hits.empty() && sweep.hits.front().body.entity == floor.id(), "sphere sweep finds ground");
    const auto overlap = take(scene->overlap_sphere({0, 2, 0}, 1.f));
    require(!overlap.hits.empty() && overlap.hits.front().body.entity == entity.id(),
            "sphere overlap returns Entity identity");
    const auto floor_mesh = mesh_component(runtime, floor.id());
    require(!pr::read_mesh_source(runtime, floor.id()) && !pr::read_mesh_source(runtime, floor_sibling.id()),
            "geometry reading borrowed a sibling mesh for an Entity or a non-mesh Primitive");
    require(!take(scene->raycast({3, 4, 0}, {3, -2, 0})).hits.empty(), "original floor must reach x = 3");
    const auto old_vertices = captured.resolved->vertices();
    std::vector<atelier::geometry::Vertex> vertices(old_vertices.begin(), old_vertices.end());
    for (auto& vertex : vertices)
        vertex.position[0] *= .5f;
    auto narrower = std::make_shared<const atelier::geometry::Mesh>(std::move(vertices));
    take(pr::set_mesh_binding(runtime, floor_mesh, {captured.source, narrower}));
    step(world, *scene, 0);
    require(scene->body_count() == 3, "active static mesh replacement rebuilds its body");
    require(!scene->read_body_state(first_handle), "mesh replacement invalidates old body handle");
    require(take(scene->raycast({3, 4, 0}, {3, -2, 0})).hits.empty() &&
                take(pr::read_mesh_source(runtime, floor_mesh)).source == captured.source &&
                take(pr::read_mesh_source(runtime, floor_mesh)).resolved == narrower &&
                captured.resolved == floor_desc.mesh,
            "same-key mesh replacement reused old collision content or rewrote an earlier snapshot");
    std::vector<p::ContactPhase> contacts;
    scene->on_contact([&](const p::ContactEvent& event) -> atelier::Result<void> {
        require((event.first.entity == floor.id() && event.second.entity == entity.id()) ||
                    (event.second.entity == floor.id() && event.first.entity == entity.id()),
                "contact Entity identity");
        require((event.first.component == floor_body.id() && event.second.component == body.id()) ||
                    (event.second.component == floor_body.id() && event.first.component == body.id()),
                "contact component identity");
        contacts.push_back(event.phase);
        return {};
    });
    for (int i = 0; i < 120; ++i)
        step(world, *scene, 1.f / 60.f);
    const auto resting = take(w::read_transform(runtime, entity.id())).position.y;
    require(std::abs(resting - .5f) < .06f, "dynamic Entity rests on static mesh");
    require(std::find(contacts.begin(), contacts.end(), p::ContactPhase::began) != contacts.end(),
            "World contact began event");
    take(scene->set_gravity({0, 0, 0}));
    scene->set_paused(true);
    const auto paused = step(world, *scene, 1.f / 60.f);
    require(paused.steps == 0, "paused scene does not advance physics");
    scene->step_once();
    const auto stepped = step(world, *scene, 0);
    require(stepped.steps == 1 && stepped.completed_step > paused.completed_step,
            "single step advances exactly one physics step");
    scene->set_paused(false);
    std::uint32_t before_steps = 0;
    std::uint32_t after_steps = 0;
    scene->on_before_step([&](std::uint64_t step, float delta) -> atelier::Result<void> {
        require(step == scene->completed_step() + 1 && std::abs(delta - 1.f / 60.f) < 1e-6f,
                "before-step receives the pending fixed step");
        ++before_steps;
        return scene->add_force(entity.id(), {0, 60, 0});
    });
    scene->on_after_step([&](std::uint64_t step, float) -> atelier::Result<void> {
        const auto state = take(scene->read_body_state(entity_handle));
        require(state.step_id == step && state.linear_velocity.y > 0,
                "after-step reads completed state from the same step");
        ++after_steps;
        return {};
    });
    const auto twice = step(world, *scene, 1.f / 30.f);
    require(twice.steps == 2 && before_steps == 2 && after_steps == 2,
            "fixed-step gameplay callbacks run for every substep");
    scene->on_before_step({});
    scene->on_after_step({});
    take(scene->add_force(entity.id(), {0, 100, 0}));
    step(world, *scene, 1.f / 60.f);
    const auto after_force = take(w::read_transform(runtime, entity.id())).position.y;
    require(after_force > resting, "force moves dynamic Entity");
    take(scene->add_impulse(entity.id(), {0, 2, 0}));
    step(world, *scene, 1.f / 60.f);
    const auto after_impulse = take(w::read_transform(runtime, entity.id())).position.y;
    require(after_impulse > after_force, "impulse moves dynamic Entity");
    require(std::find(contacts.begin(), contacts.end(), p::ContactPhase::ended) != contacts.end(),
            "World contact ended event");
    committed(world.set_enabled(body.id(), false));
    require(scene->body_count() == 2, "disabled body is removed");
    committed(world.stop());
    require(scene->body_count() == 0, "stopped World releases physics bodies");
}

void simulate_filter_and_trigger() {
    namespace w = atelier::world;
    namespace pr = atelier::presentation;
    namespace p = atelier::physics;
    atelier::Runtime runtime;
    const auto world_types = take(w::register_types(runtime));
    (void)take(pr::register_types(runtime));
    const auto physics_types = take(p::register_types(runtime));
    w::World world(runtime, world_types);
    auto scene = take(p::PhysicsScene::create(world, physics_types, pr::read_mesh_source));
    auto create = [&](atelier::math::Vector3 position, p::BodySettings settings) {
        auto entity = take(w::create_entity(runtime));
        auto body = take(w::create_component(runtime, world_types.collision_component));
        const std::array attachments{w::Attachment{body.id()}};
        committed(world.attach_components(entity.id(), attachments));
        take(world.set_transform(entity.id(), w::Trs{position, {0, 0, 0, 1}, {1, 1, 1}}));
        take(p::configure(runtime, body.id(), settings));
        committed(world.attach(entity.id()));
        return std::pair{std::move(entity), std::move(body)};
    };
    p::BodySettings sensor;
    sensor.motion = p::MotionKind::kinematic;
    sensor.shape = p::ShapeKind::sphere;
    sensor.radius = 2;
    sensor.category = 1;
    sensor.sensor = true;
    take(sensor.set_response(2, p::Response::overlap));
    auto [area, area_body] = create({0, 0, 0}, sensor);
    p::BodySettings dynamic;
    dynamic.motion = p::MotionKind::dynamic;
    dynamic.shape = p::ShapeKind::sphere;
    dynamic.category = 2;
    take(dynamic.set_response(1, p::Response::block));
    take(dynamic.set_response(3, p::Response::ignore));
    auto [target, target_body] = create({0, 0, 0}, dynamic);
    take(scene->set_gravity({0, 0, 0}));
    committed(world.start());
    std::vector<p::ContactEvent> events;
    scene->on_contact([&](const p::ContactEvent& event) -> atelier::Result<void> {
        events.push_back(event);
        return {};
    });
    step(world, *scene, 1.f / 60.f);
    require(events.size() == 1 && events[0].phase == p::ContactPhase::began && events[0].trigger &&
                events[0].step_id == 1,
            "trigger enters once per body pair");
    const auto contact_area = events[0].first.entity == area.id() ? events[0].first : events[0].second;
    require(contact_area == take(scene->body_handle(area.id())),
            "contact carries the same scene and body generation as queries and commands");
    const auto position_before = take(scene->read_body_state(take(scene->body_handle(target.id())))).position;
    step(world, *scene, 1.f / 60.f);
    require(events.size() == 2 && events[1].phase == p::ContactPhase::persisted, "trigger remains once per fixed step");
    const auto position_after = take(scene->read_body_state(take(scene->body_handle(target.id())))).position;
    require(position_before == position_after, "trigger does not solve as a blocking body");
    auto all = take(scene->overlap_sphere({0, 0, 0}, 3, p::QueryFilter{.mode = p::QueryMode::all, .capacity = 1}));
    require(all.hits.size() == 1 && all.overflow, "query capacity reports truncation");
    p::QueryFilter filter;
    filter.category = 3;
    filter.mode = p::QueryMode::all;
    auto filtered = take(scene->overlap_sphere({0, 0, 0}, 3, filter));
    require(filtered.hits.size() == 1 && filtered.hits[0].body.entity == area.id() && !filtered.has_block,
            "query checks reciprocal response and sensor overlap");
    committed(world.set_enabled(area_body.id(), false));
    step(world, *scene, 0);
    require(events.size() == 3 && events.back().phase == p::ContactPhase::ended &&
                events.back().exit_reason == p::ContactExitReason::body_removed,
            "deactivation emits one explicit trigger exit");
    const auto ended_area = events.back().first.entity == area.id() ? events.back().first : events.back().second;
    require(ended_area == contact_area && !scene->read_body_state(ended_area),
            "removed contact keeps its historical instance without granting current body access");
    committed(world.set_enabled(area_body.id(), true));
    const auto rebuilt_area = take(scene->body_handle(area.id()));
    require(rebuilt_area.component == ended_area.component && rebuilt_area.revision != ended_area.revision &&
                !scene->set_kinematic_target(ended_area, {10, 0, 0}, {0, 0, 0, 1}),
            "a historical event cannot move a replacement body with the same component identity");
    committed(world.stop());
    require(scene->body_count() == 0, "trigger World closes cleanly");
}

void simulate_cooked_assets() {
    namespace w = atelier::world;
    namespace pr = atelier::presentation;
    namespace p = atelier::physics;
    const auto mesh = atelier::geometry::cube_mesh();
    const auto cooked = take(p::cook_triangle_mesh(mesh, {8, 1, 8}, "builtin/cube"));
    const auto path =
        std::filesystem::temp_directory_path() /
        ("atelier-collision-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    {
        std::ofstream file(path, std::ios::binary);
        require(bool(file.write(cooked.data(), static_cast<std::streamsize>(cooked.size()))),
                "offline collision asset is written");
    }
    std::ifstream file(path, std::ios::binary);
    const std::string loaded(std::istreambuf_iterator<char>{file}, {});
    file.close();
    std::filesystem::remove(path);
    require(loaded == cooked, "offline collision asset round trips through disk");
    // Returns the runtime cooks the first step reports, or nothing when the body cannot be built.
    const auto load = [&](std::string bytes, std::string_view source,
                          atelier::math::Vector3 scale) -> std::optional<std::uint32_t> {
        atelier::Runtime runtime;
        const auto world_types = take(w::register_types(runtime));
        (void)take(pr::register_types(runtime));
        const auto physics_types = take(p::register_types(runtime));
        w::World world(runtime, world_types);
        pr::StaticMeshEntityDesc desc;
        desc.mesh = mesh;
        desc.mesh_source = source;
        desc.transform.scale = scale;
        auto ground = take(pr::create_static_mesh_entity(world, desc));
        auto body = take(runtime.hold(mesh_component(runtime, ground.id())));
        p::BodySettings settings;
        settings.shape = p::ShapeKind::triangle_mesh;
        settings.cooked_shape = std::make_shared<const std::string>(std::move(bytes));
        take(p::configure(runtime, body.id(), settings));
        committed(world.attach(ground.id()));
        committed(world.start());
        auto scene = p::PhysicsScene::create(world, physics_types, pr::read_mesh_source);
        if (!scene)
            return std::nullopt;
        take(world.update(1.f / 60));
        const auto cooks = (*scene)->last_step().runtime_cooks;
        const auto hit = take((*scene)->raycast({0, 4, 0}, {0, -4, 0}));
        require(!hit.hits.empty() && hit.hits.front().body.entity == ground.id(),
                "restored Jolt shape participates in World query");
        // The cube is 1 m wide, so only an 8 m x scale reaches x = 3.
        require(take((*scene)->raycast({3, 4, 0}, {3, -4, 0})).hits.empty() == (scale.x < 6),
                "triangle shape does not follow the Entity's current scale");
        scene->reset();
        committed(world.stop());
        return cooks;
    };
    require(load(loaded, "builtin/cube", {8, 1, 8}) == 0u, "cooked shape loads into World");
    require(load(loaded, "builtin/cube", {8, 1, 8}) == 0u, "cooked shape reloads into a new World");
    auto corrupt = loaded;
    corrupt.back() ^= 1;
    require(!load(corrupt, "builtin/cube", {8, 1, 8}), "corrupt asset rejected");
    auto version = loaded;
    version[8] = 2;
    require(!load(version, "builtin/cube", {8, 1, 8}), "unsupported asset version rejected");
    require(!p::validate_cooked_triangle_mesh(loaded, mesh, {4, 1, 8}, "builtin/cube"),
            "scale mismatch passes cache validation");
    require(load(loaded, "other/cube", {8, 1, 8}) == 1u, "source mismatch is not cooked at runtime");
    require(load(loaded, "builtin/cube", {4, 1, 8}) == 1u, "scale mismatch is not cooked at runtime");
}

void simulate_special_systems() {
    namespace w = atelier::world;
    namespace pr = atelier::presentation;
    namespace p = atelier::physics;
    atelier::Runtime runtime;
    const auto world_types = take(w::register_types(runtime));
    (void)take(pr::register_types(runtime));
    const auto physics_types = take(p::register_types(runtime));
    w::World world(runtime, world_types);
    auto scene = take(p::PhysicsScene::create(world, physics_types, pr::read_mesh_source));
    auto make_entity = [&](atelier::math::Vector3 position, bool body_enabled, p::BodySettings settings = {}) {
        auto entity = take(w::create_entity(runtime));
        auto body = take(w::create_component(runtime, world_types.collision_component));
        const std::array attachments{w::Attachment{body.id()}};
        committed(world.attach_components(entity.id(), attachments));
        take(world.set_transform(entity.id(), w::Trs{position, {0, 0, 0, 1}, {1, 1, 1}}));
        take(p::configure(runtime, body.id(), settings));
        settings.enabled = body_enabled;
        take(p::configure(runtime, body.id(), settings));
        committed(world.attach(entity.id()));
        return std::pair{std::move(entity), std::move(body)};
    };
    p::BodySettings ground_settings;
    ground_settings.half_extents = {50, .5f, 50};
    auto [ground, ground_body] = make_entity({0, -.5f, 0}, true, ground_settings);
    auto [character_entity, character_body] = make_entity({0, 2, 0}, false);
    auto [ragdoll_entity, ragdoll_body] = make_entity({-10, 2, 0}, false);
    constexpr float half_yaw = .78539816f;
    take(world.set_transform(ragdoll_entity.id(),
                             w::Trs{{-10, 2, 0}, {0, std::sin(half_yaw), 0, std::cos(half_yaw)}, {1, 1, 1}}));
    p::BodySettings step_settings;
    step_settings.half_extents = {.5f, .125f, 2};
    auto [step_entity, step_body] = make_entity({1.5f, .125f, 0}, true, step_settings);
    p::BodySettings ramp_settings;
    ramp_settings.half_extents = {3, .1f, 2};
    auto [ramp_entity, ramp_body] = make_entity({-15, .8f, 0}, true, ramp_settings);
    constexpr float slope = .34906585f;
    take(world.set_transform(ramp_entity.id(),
                             w::Trs{{-15, .8f, 0}, {0, 0, std::sin(slope / 2), std::cos(slope / 2)}, {1, 1, 1}}));
    auto [slope_entity, slope_body] = make_entity({-17.7f, 1.1f, 0}, false);
    p::BodySettings platform_settings;
    platform_settings.motion = p::MotionKind::kinematic;
    platform_settings.half_extents = {1, .25f, 1};
    auto [platform_entity, platform_body] = make_entity({20, .25f, 0}, true, platform_settings);
    auto [rider_entity, rider_body] = make_entity({20, 1.45f, 0}, false);
    committed(world.start());

    const auto character = take(scene->create_character(character_entity.id()));
    const auto slope_character = take(scene->create_character(slope_entity.id()));
    const auto rider = take(scene->create_character(rider_entity.id()));
    const auto ragdoll = take(scene->create_ragdoll(
        ragdoll_entity.id(),
        std::array<p::RagdollPart, 3>{
            {{"hip", -1, {0, 0, 0}}, {"head", 0, {0, .6f, 0}, .15f, .15f}, {"arm", 0, {.6f, 0, 0}, .15f, .15f}}}));
    const auto first_pose = take(scene->read_ragdoll_pose(ragdoll));
    require(first_pose.positions.size() == 3 && std::abs(first_pose.positions[2].x + 10.f) < .02f &&
                std::abs(first_pose.positions[2].z + .6f) < .02f,
            "ragdoll local joint offset follows the Entity Entity rotation");
    for (int i = 0; i < 150; ++i)
        step(world, *scene, 1.f / 60.f);
    const auto character_state = take(scene->read_character_state(character));
    require(character_state.grounded && character_state.position.y < 1.3f, "virtual character lands on World ground");
    const auto ragdoll_pose = take(scene->read_ragdoll_pose(ragdoll));
    require(ragdoll_pose.positions[0].y < first_pose.positions[0].y, "ragdoll parts simulate gravity");
    take(scene->set_character_input(character, {2, 0, 0}, true));
    step(world, *scene, 1.f / 60.f);
    const auto jumped = take(scene->read_character_state(character));
    require(jumped.velocity.y > 0 && jumped.position.x > character_state.position.x,
            "character jump and horizontal movement");
    take(scene->set_character_input(character, {0, 0, 0}));
    for (int i = 0; i < 100; ++i)
        step(world, *scene, 1.f / 60.f);
    take(scene->set_character_input(character, {3, 0, 0}));
    float maximum_step_height = 0;
    for (int i = 0; i < 90; ++i) {
        step(world, *scene, 1.f / 60.f);
        maximum_step_height = std::max(maximum_step_height, take(scene->read_character_state(character)).position.y);
    }
    require(maximum_step_height > 1.15f && take(scene->read_character_state(character)).position.x > 2.5f,
            "virtual character climbs a World step below its configured step height");
    take(scene->set_character_input(slope_character, {3, 0, 0}));
    float maximum_slope_height = 0;
    for (int i = 0; i < 120; ++i) {
        step(world, *scene, 1.f / 60.f);
        maximum_slope_height =
            std::max(maximum_slope_height, take(scene->read_character_state(slope_character)).position.y);
    }
    require(maximum_slope_height > 1.4f && take(scene->read_character_state(slope_character)).position.x > -14.f,
            "virtual character climbs a walkable World slope");
    const auto rider_before = take(scene->read_character_state(rider));
    require(rider_before.grounded, "character starts supported by kinematic platform");
    const auto platform_handle = take(scene->body_handle(platform_entity.id()));
    for (int i = 1; i <= 60; ++i) {
        take(scene->set_kinematic_target(platform_handle, {20, .25f + .01f * i, 0}, {0, 0, 0, 1}));
        step(world, *scene, 1.f / 60.f);
    }
    const auto rider_after = take(scene->read_character_state(rider));
    require(rider_after.grounded && rider_after.position.y > rider_before.position.y + .3f,
            "virtual character follows a moving World platform");
    take(scene->destroy_ragdoll(ragdoll));
    const auto next_ragdoll =
        take(scene->create_ragdoll(ragdoll_entity.id(), std::array<p::RagdollPart, 1>{{{"hip", -1, {0, 0, 0}}}}));
    committed(world.set_enabled(ragdoll_entity.id(), false));
    require(!scene->read_ragdoll_pose(next_ragdoll), "disabled ragdoll Entity invalidates the special handle");
    require(!scene->create_ragdoll(ragdoll_entity.id(), std::array<p::RagdollPart, 1>{{{"hip", -1, {0, 0, 0}}}}),
            "disabled ragdoll Entity cannot create a new special body");
    take(scene->destroy_character(character));
    take(scene->destroy_character(slope_character));
    take(scene->destroy_character(rider));
    require(!scene->read_character_state(character), "destroyed character handle is rejected");
    const auto again = take(scene->create_character(character_entity.id()));
    require(again.revision != character.revision, "recreated character has a new generation");
    committed(world.set_enabled(character_entity.id(), false));
    auto replacement_settings = take(p::read_settings(runtime, character_body.id()));
    replacement_settings.enabled = true;
    take(p::configure(runtime, character_body.id(), replacement_settings));
    committed(world.set_enabled(character_entity.id(), true));
    require(!scene->read_character_state(again) && bool(scene->body_handle(character_entity.id())),
            "enabling a Body replaces the virtual character on the same Entity");
    committed(world.set_enabled(character_entity.id(), false));
    replacement_settings.enabled = false;
    take(p::configure(runtime, character_body.id(), replacement_settings));
    committed(world.set_enabled(character_entity.id(), true));
    const auto replaced = take(scene->create_character(character_entity.id()));
    require(replaced.revision != again.revision, "returning from a Body creates a new character generation");
    committed(world.set_enabled(character_entity.id(), false));
    require(!scene->read_character_state(replaced), "disabled character Entity invalidates special handle");
    require(!scene->create_character(character_entity.id()),
            "disabled character Entity cannot create a new controller");
    committed(world.stop());
    require(!scene->read_character_state(again) && scene->body_count() == 0,
            "World stop releases all special physics systems");
}
