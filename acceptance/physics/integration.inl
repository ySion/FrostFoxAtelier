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
    const auto load = [&](std::string bytes, std::string_view source, atelier::math::Vector3 scale) {
        atelier::Runtime runtime;
        const auto world_types = take(w::register_types(runtime));
        (void)take(pr::register_types(runtime));
        const auto physics_types = take(p::register_types(runtime));
        w::World world;
        take(world.initialize(runtime, world_types));
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
