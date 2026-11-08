void static_mesh_entity_creation() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world;
    take(world.initialize(runtime, types));
    const auto scene = take(p::Scene::create(world, types));
    p::StaticMeshEntityDesc invalid;
    invalid.mesh_source = std::string_view("mesh\0key", 8);
    reject(p::create_static_mesh_entity(world, invalid), "Factory accepted an invalid mesh source key.");
    require(runtime.live_objects() == 0 && runtime.root_count() == 0,
            "A failed mesh binding leaked the already-created Entity or component.");

    auto mesh = std::make_shared<const geometry::Mesh>(
        std::vector<geometry::Vertex>{{{0, 0, 0}, {0, 0, 1}}, {{2, 0, 0}, {0, 0, 1}}, {{0, 3, 0}, {0, 0, 1}}});
    std::weak_ptr<const geometry::Mesh> resource = mesh;
    auto entity = take(p::create_static_mesh_entity(world, {mesh}));
    const auto entity_id = entity.id();
    const auto component_id = take(w::components(runtime, entity_id)).front().id();
    committed(world.attach(entity_id));
    auto draws = take(scene.render_items());
    require(draws.size() == 1 && draws[0].geometry == mesh, "Scene snapshot did not retain the authored mesh.");
    mesh.reset();
    committed(w::destroy_entity(runtime, entity_id));
    require(!runtime.type_of(entity_id) && !runtime.type_of(component_id) && !resource.expired(),
            "Entity destruction retained its component or invalidated a held render resource.");
    draws.clear();
    take(scene.update());
    require(resource.expired(), "Mesh content outlived all component/snapshot/cache owners.");
    committed(world.close());
    std::puts("[pass] StaticMeshEntity factory: binding failure cleanup and Scene snapshot resource ownership");
}
