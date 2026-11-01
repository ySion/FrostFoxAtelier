void static_mesh_entity_creation() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    require(take(runtime.find_type("world.StaticMeshEntity")) == types.static_mesh_entity &&
                take(runtime.native_type<p::StaticMeshEntity>()) == types.static_mesh_entity &&
                take(runtime.is_a(types.static_mesh_entity, types.entity)),
            "StaticMeshEntity is not a real registered Entity subtype.");
    w::World world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    auto mesh = std::make_shared<const geometry::Mesh>(
        std::vector<geometry::Vertex>{{{0, 0, 0}, {0, 0, 1}}, {{2, 0, 0}, {0, 0, 1}}, {{0, 3, 0}, {0, 0, 1}}});
    std::weak_ptr<const geometry::Mesh> resource = mesh;
    auto material_source = take(material::create_source(runtime, "", {mesh_material_definition(), {}}));
    const auto surface = take(material::capture_program(runtime, material_source.id()));
    p::StaticMeshEntityDesc description{
        mesh, {{4, 5, 6}, {0, 0, 0, 2}, {2, 3, 4}}, {}, {{"slot/0", material_source, false}}};
    std::vector<std::pair<char, ObjectId>> events;
    take(world.bind_behavior(
        types.entity, {.begin = [&](Runtime&, ObjectId id) -> Result<void> {
                           events.emplace_back('B', id);
                           const auto count = runtime.live_objects();
                           reject(p::create_static_mesh_entity(world, description), "Factory reentered Entity Begin.");
                           require(runtime.live_objects() == count, "Rejected factory callback created objects.");
                           return {};
                       },
                       .end = [&](Runtime&, ObjectId id) -> Result<void> {
                           events.emplace_back('E', id);
                           return {};
                       }}));
    take(world.bind_behavior(types.static_mesh_component, {.begin = [&](Runtime&, ObjectId id) -> Result<void> {
                                                               events.emplace_back('B', id);
                                                               return {};
                                                           },
                                                           .end = [&](Runtime&, ObjectId id) -> Result<void> {
                                                               events.emplace_back('E', id);
                                                               return {};
                                                           }}));
    take(world.observe_lifecycle([&](w::World&, w::Lifecycle phase, ObjectId id) -> Result<void> {
        if (phase == w::Lifecycle::attached)
            events.emplace_back('R', id);
        if (phase == w::Lifecycle::detached)
            events.emplace_back('U', id);
        return {};
    }));
    auto entity = take(p::create_static_mesh_entity(world, description));
    const auto entity_id = entity.id();
    const auto component_id = take(w::components(runtime, entity_id)).front().id();
    const auto status = take(w::inspect_entity(runtime, entity_id));
    require(status.type == types.static_mesh_entity && status.state == w::EntityState::detached &&
                take(runtime.type_of(component_id)) == types.static_mesh_component &&
                take(w::inspect_component(runtime, component_id)).owner == entity_id &&
                take(w::has_transform(runtime, entity_id)) && take(w::components(runtime, entity_id)).size() == 1,
            "Factory did not return the complete typed spatial composition.");
    require(events.empty() && take(world.members()).empty() && take(scene.render_items()).empty() &&
                take(world.query(types.component)).empty(),
            "Factory registered or began an unpublished Entity.");
    require(take(w::read_transform(runtime, entity_id)) == take(w::canonical_transform(description.transform)),
            "Factory failed to validate and canonicalize the initial TRS.");
    // A Dungeon rotation whose float norm² stays below one: renormalizing walked a component one ulp per save.
    const w::Trs saved{
        {}, {0.01535156648606062f, -0.7645430564880371f, -0.013860960491001606f, 0.64424067735672f}, {1, 1, 1}};
    const auto once = take(w::canonical_transform(saved));
    require(take(w::canonical_transform(once)) == once, "Canonicalizing a canonical transform changed it.");
    committed(world.attach(entity_id));
    require(events == std::vector<std::pair<char, ObjectId>>{{'R', entity_id}, {'R', component_id}},
            "Complete Entity registration was not a single group boundary.");
    auto draws = take(scene.render_items());
    require(draws.size() == 1 && draws[0].entity == entity_id && draws[0].component == component_id &&
                draws[0].geometry == mesh && draws[0].geometry->vertices().size() == 3 &&
                draws[0].surfaces->front() == surface && near(draws[0].world[12], 4) && near(draws[0].world[13], 5) &&
                near(draws[0].world[14], 6),
            "Factory projection substituted a cube or lost initial material/transform.");

    // One existing Entity removal record retains the entire creation for an
    // editor's single create/undo/redo transaction, including shared mesh content.
    auto removal = take(world.detach(entity_id));
    entity.reset();
    mesh.reset();
    description.mesh.reset();
    description.default_materials.clear();
    draws.clear();
    collect(runtime);
    require(!resource.expired() && take(w::components(runtime, entity_id)).front().id() == component_id,
            "Whole-Entity creation history failed to retain the component/resource.");
    committed(world.restore(removal));
    removal.root.reset();
    require(take(world.members()).size() == 1 && take(w::components(runtime, entity_id)).front().id() == component_id,
            "Whole-Entity redo replaced object identities.");
    events.clear();
    committed(world.start());
    require(events == std::vector<std::pair<char, ObjectId>>{{'B', entity_id}, {'B', component_id}},
            "StaticMeshEntity did not inherit Entity lifecycle before its component.");
    events.clear();
    auto empty_entity = take(p::create_static_mesh_entity(world, {}));
    const auto empty_root = take(w::components(runtime, empty_entity.id())).front().id();
    require(events.empty() && take(world.members()).size() == 1, "Factory began a Detached Entity in a running World.");
    committed(world.attach(empty_entity.id()));
    require(events ==
                    std::vector<std::pair<char, ObjectId>>{
                        {'R', empty_entity.id()}, {'R', empty_root}, {'B', empty_entity.id()}, {'B', empty_root}} &&
                take(scene.render_items()).size() == 1,
            "Running attachment or null mesh behavior violated the factory contract.");
    committed(world.stop());

    const auto objects_before = runtime.live_objects();
    const auto roots_before = runtime.root_count();
    auto invalid = description;
    invalid.transform.scale.x = std::numeric_limits<float>::infinity();
    reject(p::create_static_mesh_entity(world, invalid), "Factory accepted non-finite scale.");
    invalid = description;
    invalid.transform.rotation = {0, 0, 0, 0};
    reject(p::create_static_mesh_entity(world, invalid), "Factory accepted a zero quaternion.");
    invalid = description;
    invalid.mesh = geometry::cube_mesh();
    invalid.default_materials = {{"missing", material_source, false}};
    reject(p::create_static_mesh_entity(world, invalid), "Factory accepted a material default for a missing slot.");
    invalid.default_materials = {{"slot/0", {}, false}};
    reject(p::create_static_mesh_entity(world, invalid), "Factory accepted an invalid material resource identity.");
    invalid.default_materials.clear();
    require(runtime.live_objects() == objects_before && runtime.root_count() == roots_before,
            "Invalid description changed object lifetime or roots.");
    invalid = description;
    invalid.mesh_source = std::string_view("mesh\0key", 8);
    reject(p::create_static_mesh_entity(world, invalid), "Factory accepted an invalid mesh source key.");
    require(runtime.live_objects() == objects_before && runtime.root_count() == roots_before,
            "A failed mesh binding leaked the already-created Entity or component.");

    draws = take(scene.render_items());
    committed(w::destroy_entity(runtime, entity_id));
    require(!runtime.type_of(entity_id) && !runtime.type_of(component_id) && !resource.expired(),
            "Entity destruction retained its component or invalidated a held render resource.");
    draws.clear();
    take(scene.update());
    require(resource.expired(), "Mesh content outlived all component/history/snapshot/cache owners.");
    committed(world.close());
    const auto closed_objects = runtime.live_objects();
    reject(p::create_static_mesh_entity(world, {}), "Factory created objects through a closed World.");
    require(runtime.live_objects() == closed_objects, "Closed factory request created a partial graph.");
    committed(w::destroy_entity(runtime, empty_entity.id()));
    std::puts(
        "[pass] StaticMeshEntity factory: real reflected type, complete detached spatial Entity, mesh/material projection, identity history, failure cleanup and resource ownership");
}
