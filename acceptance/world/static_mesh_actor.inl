void static_mesh_actor_creation() {
    Runtime runtime;
    const auto types = take(w::register_types(runtime));
    require(take(runtime.find_type("world.StaticMeshActor")) == types.static_mesh_actor &&
        take(runtime.native_type<w::StaticMeshActor>()) == types.static_mesh_actor &&
        take(runtime.is_a(types.static_mesh_actor, types.actor)), "StaticMeshActor is not a real registered Actor subtype.");
    w::World world(runtime, types);
    auto mesh = std::make_shared<const geometry::Mesh>(std::vector<geometry::Vertex>{
        {{0,0,0},{0,0,1}}, {{2,0,0},{0,0,1}}, {{0,3,0},{0,0,1}}});
    std::weak_ptr<const geometry::Mesh> resource = mesh;
    w::StaticMeshActorDesc description{mesh, {{4,5,6},{0,0,0,2},{2,3,4}}, {{.2f,.4f,.6f}}};
    std::vector<std::pair<char,ObjectId>> events;
    take(world.bind_behavior(types.actor, {
        .begin = [&](Runtime&, ObjectId id) -> Result<void> {
            events.emplace_back('B',id);
            const auto count = runtime.live_objects();
            reject(w::create_static_mesh_actor(world,description), "Factory reentered Actor Begin.");
            require(runtime.live_objects() == count, "Rejected factory callback created objects.");
            return {};
        },
        .end = [&](Runtime&, ObjectId id) -> Result<void> { events.emplace_back('E',id); return {}; }
    }));
    take(world.bind_behavior(types.static_mesh_component, {
        .begin = [&](Runtime&, ObjectId id) -> Result<void> { events.emplace_back('B',id); return {}; },
        .end = [&](Runtime&, ObjectId id) -> Result<void> { events.emplace_back('E',id); return {}; }
    }));
    take(world.observe_lifecycle([&](w::World&, w::Lifecycle phase, ObjectId id) -> Result<void> {
        if (phase == w::Lifecycle::attached) events.emplace_back('R',id);
        if (phase == w::Lifecycle::detached) events.emplace_back('U',id);
        return {};
    }));
    auto actor = take(w::create_static_mesh_actor(world,description));
    const auto actor_id = actor.id();
    const auto component_id = take(w::root_component(runtime,actor_id)).value();
    const auto status = take(w::inspect_actor(runtime,actor_id));
    require(status.type == types.static_mesh_actor && status.state == w::ActorState::detached &&
        take(runtime.type_of(component_id)) == types.static_mesh_component &&
        take(w::inspect_component(runtime,component_id)).owner == actor_id &&
        take(w::scene_parent(runtime,component_id)) == ObjectId{} &&
        take(w::components(runtime,actor_id)).size() == 1, "Factory did not return the complete typed root composition.");
    require(events.empty() && take(world.members()).empty() && take(world.render_items()).empty() &&
        take(world.query(types.component)).empty(), "Factory registered or began an unpublished Actor.");
    require(take(w::read_transform(runtime,component_id)) == take(w::canonical_transform(description.transform)),
        "Factory failed to validate and canonicalize the initial TRS.");
    committed(world.attach(actor_id));
    require(events == std::vector<std::pair<char,ObjectId>>{{'R',actor_id},{'R',component_id}},
        "Complete Actor registration was not a single group boundary.");
    auto draws = take(world.render_items());
    require(draws.size() == 1 && draws[0].actor == actor_id && draws[0].component == component_id &&
        draws[0].geometry == mesh && draws[0].geometry->vertices().size() == 3 &&
        draws[0].material.base_color == description.material.base_color &&
        near(draws[0].world[12],4) && near(draws[0].world[13],5) && near(draws[0].world[14],6),
        "Factory projection substituted a cube or lost initial material/transform.");

    // One existing Actor removal record retains the entire creation for an
    // editor's single create/undo/redo transaction, including shared mesh content.
    auto removal = take(world.detach(actor_id));
    actor.reset(); mesh.reset(); description.mesh.reset(); draws.clear();
    collect(runtime);
    require(!resource.expired() && take(w::root_component(runtime,actor_id)) == component_id,
        "Whole-Actor creation history failed to retain the component/resource.");
    committed(world.restore(removal)); removal.root.reset();
    require(take(world.members()).size() == 1 && take(w::root_component(runtime,actor_id)) == component_id,
        "Whole-Actor redo replaced object identities.");
    events.clear(); committed(world.start());
    require(events == std::vector<std::pair<char,ObjectId>>{{'B',actor_id},{'B',component_id}},
        "StaticMeshActor did not inherit Actor lifecycle before its component.");
    events.clear();
    auto empty_actor = take(w::create_static_mesh_actor(world,{}));
    const auto empty_root = take(w::root_component(runtime,empty_actor.id())).value();
    require(events.empty() && take(world.members()).size() == 1, "Factory began a Detached Actor in a running World.");
    committed(world.attach(empty_actor.id()));
    require(events == std::vector<std::pair<char,ObjectId>>{{'R',empty_actor.id()},{'R',empty_root},
        {'B',empty_actor.id()},{'B',empty_root}} && take(world.render_items()).size() == 1,
        "Running attachment or null mesh behavior violated the factory contract.");
    committed(world.stop());

    const auto objects_before = runtime.live_objects(), roots_before = runtime.root_count();
    auto invalid = description;
    invalid.transform.scale.x = 0;
    reject(w::create_static_mesh_actor(world,invalid), "Factory accepted singular scale.");
    invalid = description; invalid.transform.rotation = {0,0,0,0};
    reject(w::create_static_mesh_actor(world,invalid), "Factory accepted a zero quaternion.");
    invalid = description; invalid.material.base_color.x = std::numeric_limits<float>::quiet_NaN();
    reject(w::create_static_mesh_actor(world,invalid), "Factory accepted non-finite material.");
    invalid = description; invalid.material.base_color.z = 1.1f;
    reject(w::create_static_mesh_actor(world,invalid), "Factory accepted out-of-range material.");
    require(runtime.live_objects() == objects_before && runtime.root_count() == roots_before,
        "Invalid description changed object lifetime or roots.");
    auto incompatible = types;
    incompatible.static_mesh_component = types.actor;
    w::World bad_catalog(runtime,incompatible);
    reject(w::create_static_mesh_actor(bad_catalog,description), "Factory accepted an incompatible component type.");
    require(runtime.live_objects() == objects_before && runtime.root_count() == roots_before,
        "Failed component creation leaked the already-created Actor.");
    committed(bad_catalog.close());

    draws = take(world.render_items());
    committed(w::destroy_actor(runtime,actor_id));
    require(!runtime.type_of(actor_id) && !runtime.type_of(component_id) && !resource.expired(),
        "Actor destruction retained its component or invalidated a held render resource.");
    draws.clear();
    require(resource.expired(), "Mesh content outlived all component/history/snapshot owners.");
    committed(world.close());
    const auto closed_objects = runtime.live_objects();
    reject(w::create_static_mesh_actor(world,{}), "Factory created objects through a closed World.");
    require(runtime.live_objects() == closed_objects, "Closed factory request created a partial graph.");
    committed(w::destroy_actor(runtime,empty_actor.id()));
    std::puts("[pass] StaticMeshActor factory: real reflected type, complete detached root, mesh/material projection, identity history, failure cleanup and resource ownership");
}
