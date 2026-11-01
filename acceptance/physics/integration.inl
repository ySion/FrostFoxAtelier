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
    const auto captured = take(pr::read_mesh_source(runtime, floor_body.id()));
    require(captured.source == floor_desc.mesh_source && captured.resolved == floor_desc.mesh,
            "geometry reading loses its exact source or immutable content");
    p::BodySettings floor_settings;
    floor_settings.shape = p::ShapeKind::triangle_mesh;
    take(p::configure(runtime, floor_body.id(), floor_settings));
    committed(world.attach(floor.id()));
    committed(world.start());
    const auto floor_mesh = mesh_component(runtime, floor.id());
    require(!take(scene->raycast({3, 4, 0}, {3, -2, 0})).hits.empty(), "original floor must reach x = 3");
    const auto old_vertices = captured.resolved->vertices();
    std::vector<atelier::geometry::Vertex> vertices(old_vertices.begin(), old_vertices.end());
    for (auto& vertex : vertices)
        vertex.position[0] *= .5f;
    auto narrower = std::make_shared<const atelier::geometry::Mesh>(std::move(vertices));
    take(pr::set_mesh_binding(runtime, floor_mesh, {captured.source, narrower}));
    step(world, *scene, 0);
    require(take(scene->raycast({3, 4, 0}, {3, -2, 0})).hits.empty() &&
                take(pr::read_mesh_source(runtime, floor_mesh)).source == captured.source &&
                take(pr::read_mesh_source(runtime, floor_mesh)).resolved == narrower &&
                captured.resolved == floor_desc.mesh,
            "same-key mesh replacement reused old collision content or rewrote an earlier snapshot");
    committed(world.stop());
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
    const auto load = [&](std::string bytes, std::string_view source, atelier::math::Vector3 scale) {
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
        auto scene = take(p::PhysicsScene::create(world, physics_types, pr::read_mesh_source));
        take(world.update(1.f / 60));
        const auto hit = take(scene->raycast({0, 4, 0}, {0, -4, 0}));
        require(!hit.hits.empty() && hit.hits.front().body.entity == ground.id(),
                "restored Jolt shape participates in World query");
        // The cube is 1 m wide, so only an 8 m x scale reaches x = 3.
        require(take(scene->raycast({3, 4, 0}, {3, -4, 0})).hits.empty() == (scale.x < 6),
                "triangle shape does not follow the Entity's current scale");
        scene.reset();
        committed(world.stop());
    };
    load(loaded, "builtin/cube", {8, 1, 8});
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
    auto [ragdoll_entity, ragdoll_body] = make_entity({-10, 2, 0}, false);
    constexpr float half_yaw = .78539816f;
    take(world.set_transform(ragdoll_entity.id(),
                             w::Trs{{-10, 2, 0}, {0, std::sin(half_yaw), 0, std::cos(half_yaw)}, {1, 1, 1}}));
    committed(world.start());
    const auto ragdoll = take(scene->create_ragdoll(
        ragdoll_entity.id(),
        std::array<p::RagdollPart, 3>{
            {{"hip", -1, {0, 0, 0}}, {"head", 0, {0, .6f, 0}, .15f, .15f}, {"arm", 0, {.6f, 0, 0}, .15f, .15f}}}));
    const auto first_pose = take(scene->read_ragdoll_pose(ragdoll));
    require(first_pose.positions.size() == 3 && std::abs(first_pose.positions[2].x + 10.f) < .02f &&
                std::abs(first_pose.positions[2].z + .6f) < .02f,
            "ragdoll local joint offset follows the Entity Entity rotation");
    take(scene->destroy_ragdoll(ragdoll));
    committed(world.stop());
}
