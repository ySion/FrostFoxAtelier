void verify_scene_v1(editor::Project& project) {
    // Fixed v1 input, independent of the current serializer and C++ math names.
    constexpr std::string_view source=R"({"version":1,"actors":[{
        "type":"world.StaticMeshActor","name":"旧场景","enabled":true,"root":1,"components":[
            {"id":1,"type":"world.StaticMeshComponent","enabled":true,"parent":0,
             "position":[1.25,-2.5,3.75],"rotation":[0,0,1,0],"scale":[2,3,4],
             "mesh":"builtin/sphere","base_color":[0.125,0.25,0.5]},
            {"id":2,"type":"world.SceneComponent","enabled":true,"parent":1,
             "position":[0.5,0.25,-1],"rotation":[0,0,0,1],"scale":[1,2,1]}
        ]
    }]})";
    const auto path=take(project.resolve("Content/Scenes/Scene-v1.scene"));
    take(editor::write_file(path,source,editor::WriteMode::create));
    auto document=take(editor::Session::deserialize(take(editor::read_file(path)),project));
    auto& runtime=document->world().runtime();
    require(take(runtime.native_type<math::Vector3>())==document->types().vector3 &&
        take(runtime.native_type<math::Quaternion>())==document->types().quaternion &&
        !runtime.find_type("world.Vec3") && !runtime.find_type("world.Quaternion"),
        "Scene loading did not use the canonical Runtime math types.");
    const auto actor=take(document->world().members()).front().id();
    const auto root=component(*document,actor,document->types().static_mesh_component);
    const auto child=component(*document,actor,document->types().scene_component);
    require(take(world::root_component(runtime,actor))==root && take(world::scene_parent(runtime,child))==root &&
        document->name(actor)==L"旧场景","Scene v1 lost its root, parent or Unicode name.");
    const auto matrix=take(world::world_matrix(runtime,child));
    require(matrix[0]==-2 && matrix[5]==-6 && matrix[10]==4 &&
        matrix[12]==.25f && matrix[13]==-3.25f && matrix[14]==-.25f,
        "Scene v1 changed xyzw rotation, float TRS values or row-vector hierarchy composition.");
    const auto mesh=take(world::read_static_mesh(runtime,root));
    require(mesh.mesh.source=="builtin/sphere" && mesh.mesh.resolved==take(project.mesh("builtin/sphere")).mesh &&
        mesh.material.base_color==math::Vector3{.125f,.25f,.5f},
        "Scene v1 lost its mesh source or material.");
    take(editor::write_file(path,take(document->serialize())));
    auto reopened=take(editor::Session::deserialize(take(editor::read_file(path)),project));
    auto upgraded=nlohmann::json::parse(source); upgraded["version"]=2;
    for (auto& actor:upgraded["actors"]) {
        actor["properties"]=nlohmann::json::object();
        for (auto& component:actor["components"]) {
            component["properties"]=nlohmann::json::object();
            if(component["type"]=="world.StaticMeshComponent") {
                const world::Material material;
                component["roughness"]=material.roughness;component["metallic"]=material.metallic;
                component["emissive"]=nlohmann::json::array({0.f,0.f,0.f});
            }
        }
    }
    require(nlohmann::json::parse(take(reopened->serialize()))==upgraded,
        "Scene v1 migration lost authored values or did not write the explicit v2 schema.");
}

void verify_physics_test_project() {
    const auto folder=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()/"acceptance/editor/fixtures/physics-project";
    auto project=take(editor::Project::open(folder));
    auto document=take(editor::Session::deserialize(
        take(editor::read_file(take(project.resolve(project.startup_scene())))),project));
    require(take(document->world().members()).size()==7,
        "The sample physics project lost an Actor.");
    const auto find_actor=[&](std::wstring_view name) {
        for (const auto& actor:take(document->world().members()))
            if (document->name(actor.id())==name) return actor.id();
        throw std::runtime_error("The sample physics project lost a named Actor.");
    };
    const auto ground=find_actor(L"Static Triangle Ground");
    const auto box=find_actor(L"Dynamic Box");
    const auto sphere=find_actor(L"Dynamic Sphere Through Sensor");
    const auto capsule=find_actor(L"Actor Capsule Body");
    const auto sensor=find_actor(L"Contact Sensor");
    const auto platform=find_actor(L"Kinematic Platform");
    const auto target=find_actor(L"Force And Impulse Target");
    auto& world=const_cast<world::World&>(document->world());
    const auto body_settings=[&](ObjectId actor) {
        return take(physics::read_settings(world.runtime(),component(*document,actor,
            document->physics_types().body_component)));
    };
    require(body_settings(ground).shape==physics::ShapeKind::triangle_mesh &&
        body_settings(box).motion==physics::MotionKind::dynamic &&
        body_settings(sphere).shape==physics::ShapeKind::sphere &&
        body_settings(capsule).shape==physics::ShapeKind::capsule &&
        body_settings(sensor).sensor &&
        body_settings(platform).motion==physics::MotionKind::kinematic,
        "The sample physics project lost its Body configurations.");
    const auto saved=take(document->serialize());
    auto reopened=take(editor::Session::deserialize(saved,project));
    require(take(reopened->serialize())==saved,
        "The sample physics scene changed after an editor save and reopen.");
    auto scene=take(physics::PhysicsScene::create(world,document->physics_types()));
    bool entered_sensor=false;
    scene->on_contact([&](const physics::ContactEvent& event)->Result<void> {
        if (event.phase==physics::ContactPhase::began && event.trigger &&
            ((event.first_actor==sphere && event.second_actor==sensor) ||
             (event.second_actor==sphere && event.first_actor==sensor))) entered_sensor=true;
        return {};
    });
    changed(world.start());
    require(scene->body_count()==7,"The sample physics project did not activate every Body.");
    take(scene->add_force(target,{10,0,0}));
    take(scene->add_impulse(target,{1,0,0}));
    const auto platform_handle=take(scene->body_handle(platform));
    take(scene->set_kinematic_target(platform_handle,{-1,.5f,2},{0,0,0,1}));
    take(scene->update(1.f/60.f));
    require(take(scene->read_body_state(take(scene->body_handle(target)))).linear_velocity.x>0 &&
        take(scene->read_body_state(platform_handle)).position.x==-1.f,
        "The sample physics commands did not affect the first completed step.");
    for (unsigned step=1;step<90;++step) take(scene->update(1.f/60.f));
    require(take(scene->read_body_state(take(scene->body_handle(box)))).position.y<1.f &&
        entered_sensor,"The sample physics project did not exercise gravity and sensor contact.");
    changed(world.stop());
}

void verify_project_document() {
    namespace fs=std::filesystem;
    const auto evidence=fs::current_path()/"project-document-evidence"/std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    fs::create_directories(evidence);
    const auto folder=evidence/L"工程";
    auto project=take(editor::Project::create(folder));
    for (const auto* directory:{L"Config",L"Content",L"Source",L"Cache"})
        require(fs::is_directory(folder/directory),"Project did not create its four real folders.");
    require(!editor::Project::create(folder),"Create Project overwrote an existing folder.");
    require(!project.resolve("../outside.scene"),"Project accepted a path outside its root.");
    const auto external=evidence/"Triangle.obj";
    take(editor::write_file(external,"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
    const auto resource=take(project.import_mesh(external));
    const auto original_mesh=take(project.mesh(resource)).mesh;
    const math::Vector3 collision_scale{2,3,4};
    const auto collision_file=take(project.cook_collision(resource,collision_scale));
    const auto collision_bytes=take(project.collision(resource,collision_scale));
    require(fs::is_regular_file(collision_file) && !collision_bytes->empty(),
        "Imported mesh collision was not cooked into the project cache.");
    take(editor::write_file(collision_file,"broken"));
    require(!project.collision(resource,collision_scale),"Corrupt collision cache was accepted.");
    take(project.cook_collision(resource,collision_scale));
    require(*take(project.collision(resource,collision_scale))==*collision_bytes,
        "Re-cooking did not restore the project collision asset.");
    require(!project.collision(resource,{1,1,1}),"Collision cache accepted a different scale.");
    const auto revision=project.revision();
    take(project.refresh());take(project.refresh());
    require(project.revision()==revision && take(project.mesh(resource)).mesh==original_mesh,
        "An unchanged refresh changed catalog identity or discarded a live mesh.");
    verify_project_workspace(project);
    verify_scene_v1(project);
    require(!project.import_mesh(external),"Import silently overwrote an existing asset.");
    editor::Session document;
    changed(document.create_static_mesh(take(project.mesh(resource))));
    const auto actor=document.selection().actor;
    take(document.rename(actor,L"主角 Triangle"));
    const auto mesh=component(document,actor,document.types().static_mesh_component);
    take(const_cast<world::World&>(document.world()).set_transform(mesh,{{1,2,3},{0,0,0,1},{2,3,4}}));
    changed(document.add_component(document.types().scene_component));
    changed(document.add_component(document.types().static_mesh_component)); // Null mesh must survive.
    const auto saved=take(document.serialize());
    take(editor::write_file(take(project.resolve(project.startup_scene())),saved));document.mark_saved();
    take(document.rename(actor,L"Changed"));require(document.dirty(),"Rename did not dirty the document.");
    changed(document.undo());require(!document.dirty(),"Undo to the saved history position remained dirty.");
    auto loaded=take(editor::Session::deserialize(saved,project));
    require(take(loaded->serialize())==saved,"Scene roundtrip lost identity relations, names, transforms or null meshes.");
    {
        editor::Session built_in;
        changed(built_in.create_static_mesh(take(project.mesh("builtin/sphere"))));
        const auto built_in_actor=built_in.selection().actor;
        changed(built_in.add_component(built_in.physics_types().body_component));
        const auto body=component(built_in,built_in_actor,built_in.physics_types().body_component);
        physics::BodySettings settings;
        settings.shape=physics::ShapeKind::triangle_mesh;
        take(built_in.configure_body(body,settings));
        auto reopened=take(editor::Session::deserialize(take(built_in.serialize()),project));
        const auto restored_body=component(*reopened,take(reopened->world().members()).front().id(),
            reopened->physics_types().body_component);
        const auto restored_settings=take(physics::read_settings(reopened->world().runtime(),restored_body));
        require(restored_settings.shape==physics::ShapeKind::triangle_mesh && !restored_settings.cooked_shape,
            "Built-in triangle collision required a project Cook cache after reopening.");
        auto& simulation=const_cast<world::World&>(reopened->world());
        auto physics_scene=take(physics::PhysicsScene::create(simulation,reopened->physics_types()));
        changed(simulation.start());
        take(physics_scene->update(1.f/60.f));
        require(physics_scene->body_count()==1,"Reopened built-in triangle collision did not create a body.");
        changed(simulation.stop());
    }
    {
        editor::Session physical;
        changed(physical.create_static_mesh(take(project.mesh(resource))));
        const auto physical_actor=physical.selection().actor;
        const auto physical_root=component(physical,physical_actor,physical.types().static_mesh_component);
        take(const_cast<world::World&>(physical.world()).set_transform(physical_root,
            {{0,0,0},{0,0,0,1},collision_scale}));
        changed(physical.add_component(physical.physics_types().body_component));
        const auto body=component(physical,physical_actor,physical.physics_types().body_component);
        physics::BodySettings settings;
        settings.shape=physics::ShapeKind::triangle_mesh;
        settings.material.friction=.65f;
        take(physical.configure_body(body,settings));
        changed(physical.undo());
        require(take(physics::read_settings(physical.world().runtime(),body)).shape==physics::ShapeKind::box,
            "Physics configuration undo did not restore the previous body.");
        changed(physical.redo());
        const auto physical_scene=take(physical.serialize());
        auto restored_physics=take(editor::Session::deserialize(physical_scene,project));
        require(take(restored_physics->serialize())==physical_scene,
            "Physics body settings changed across scene save and reopen.");
        auto& simulation=const_cast<world::World&>(restored_physics->world());
        auto physics_scene=take(physics::PhysicsScene::create(simulation,restored_physics->physics_types()));
        changed(simulation.start());
        take(physics_scene->update(1.f/60.f));
        require(physics_scene->body_count()==1,"Reopened collision asset did not create a World body.");
        changed(simulation.stop());
        physics_scene.reset();
        take(editor::write_file(collision_file,"invalid"));
        require(!editor::Session::deserialize(physical_scene,project),
            "Scene accepted a corrupt cooked collision asset.");
        take(project.cook_collision(resource,collision_scale));
    }
    require(!editor::Session::deserialize("{\"version\":999,\"actors\":[]}",project),"Unknown scene version was accepted.");
    require(!editor::Session::deserialize("{broken",project),"Broken scene was accepted.");
    const auto serialized=nlohmann::json::parse(saved);
    for (const auto& value:{nlohmann::json(1.5),nlohmann::json(-1),nlohmann::json(true)}) {
        auto invalid=serialized;invalid["actors"][0]["components"][0]["id"]=value;
        require(!editor::Session::deserialize(invalid.dump(),project),"Scene coerced a non-integer identity.");
        invalid=serialized;invalid["actors"][0]["root"]=value;
        require(!editor::Session::deserialize(invalid.dump(),project),"Scene coerced a non-integer root.");
        invalid=serialized;invalid["actors"][0]["components"][0]["parent"]=value;
        require(!editor::Session::deserialize(invalid.dump(),project),"Scene coerced a non-integer parent.");
    }
    for (const auto field:{"position","scale","rotation","base_color"}) {
        auto invalid=serialized;invalid["actors"][0]["components"][0][field].push_back(999);
        require(!editor::Session::deserialize(invalid.dump(),project),"Scene silently truncated a vector.");
    }
    auto invalid=serialized;invalid["actors"][0]["name"]="   ";
    require(!editor::Session::deserialize(invalid.dump(),project),"Scene accepted a name rejected by editing.");
    require(!editor::Session::deserialize("{\"version\":999,\"version\":1,\"actors\":[]}",project),"Scene accepted duplicate JSON fields.");
    require(!editor::Session::deserialize(std::string(64,'[')+std::string(64,']'),project),"Scene accepted excessive JSON nesting.");
    require(!editor::Session::deserialize(std::string(16*1024*1024+1,' '),project),"Scene direct input bypassed the file size bound.");
    auto bounded=serialized;
    auto& bounded_components=bounded["actors"][0]["components"];
    while (bounded_components.size()<editor::Session::maximum_components) {
        auto entry=bounded_components[1];entry["id"]=bounded_components.size()+1;bounded_components.push_back(std::move(entry));
    }
    auto full=take(editor::Session::deserialize(bounded.dump(),project));
    const auto full_actor=take(full->world().members()).front().id();take(full->select(full_actor));
    require(!full->add_component(full->types().scene_component) && take(world::components(full->world().runtime(),full_actor)).size()==editor::Session::maximum_components,
        "Editor admitted a component beyond its scene format limit.");
    full.reset();
    require(!editor::write_file(folder/"Content/Scenes/Main.scene","overwrite",editor::WriteMode::create) &&
        take(editor::read_file(folder/"Content/Scenes/Main.scene"))==saved,"Create-only file publication overwrote an existing scene.");
    const auto metadata=folder/"Content/Meshes/Triangle.obj.asset.json";
    const auto metadata_text=take(editor::read_file(metadata));
    take(editor::write_file(metadata,"{broken"));
    const auto retry=evidence/"Retry.obj";take(editor::write_file(retry,take(editor::read_file(external))));
    require(!project.import_mesh(retry) && !fs::exists(folder/"Content/Meshes/Retry.obj") &&
        !fs::exists(folder/"Content/Meshes/Retry.obj.asset.json"),"Failed import left a target that prevented retry.");
    require(take(project.mesh(resource)).mesh==original_mesh,"Failed refresh destroyed the previously accepted resource cache.");
    take(editor::write_file(metadata,metadata_text));
    const auto retried=take(project.import_mesh(retry));
    require(take(project.mesh(retried)).mesh!=nullptr,"Import could not recover after catalog repair.");
    take(editor::write_file(folder/"Content/Meshes/Orphan.obj.asset.json",metadata_text));
    const auto orphan=evidence/"Orphan.obj";take(editor::write_file(orphan,take(editor::read_file(external))));
    require(!project.import_mesh(orphan) && !fs::exists(folder/"Content/Meshes/Orphan.obj") &&
        take(editor::read_file(folder/"Content/Meshes/Orphan.obj.asset.json"))==metadata_text,"Import claimed or replaced orphan resource metadata.");
    // Exercise failure after OBJ and metadata publication, not just preflight.
    const auto capacity_entries=10000-project.entries().size();
    for (std::size_t i=0;i<capacity_entries;++i) fs::create_directory(folder/"Source"/("Limit"+std::to_string(i)));
    take(project.refresh());const auto full_revision=project.revision();
    require(project.entries().size()==10000,"Catalog capacity fixture is not at the admission boundary.");
    const auto capacity_mesh=evidence/"Capacity.obj";take(editor::write_file(capacity_mesh,take(editor::read_file(external))));
    require(!project.import_mesh(capacity_mesh) && !fs::exists(folder/"Content/Meshes/Capacity.obj") &&
        !fs::exists(folder/"Content/Meshes/Capacity.obj.asset.json") && project.revision()==full_revision &&
        take(project.mesh(resource)).mesh==original_mesh,"Post-publication import failure did not roll back owned files and retain the accepted catalog.");
    for (std::size_t i=0;i<capacity_entries;++i) fs::remove(folder/"Source"/("Limit"+std::to_string(i)));
    require(take(project.mesh(take(project.import_mesh(capacity_mesh)))).mesh!=nullptr,"Import retry failed after capacity was restored.");
    const auto original=take(editor::read_file(folder/"Content/Scenes/Main.scene"));
    fs::create_directory(folder/"Content/Scenes/blocked.scene");
    require(!editor::write_file(folder/"Content/Scenes/blocked.scene",saved),"Saving over a directory did not fail.");
    require(take(editor::read_file(folder/"Content/Scenes/Main.scene"))==original,"A failed save damaged the original scene.");
    // Move both source and metadata, preserving the persistent resource identity.
    fs::rename(folder/"Content/Meshes/Triangle.obj",folder/"Content/Meshes/Renamed.obj");
    fs::rename(folder/"Content/Meshes/Triangle.obj.asset.json",folder/"Content/Meshes/Renamed.obj.asset.json");
    take(project.refresh());require(take(project.mesh(resource)).name==L"Renamed","Asset rename changed its persistent identity.");
    require(take(project.mesh(resource)).mesh==original_mesh,"Asset rename unnecessarily duplicated unchanged geometry.");
    const auto renamed=folder/"Content/Meshes/Renamed.obj";
    const auto old_time=fs::last_write_time(renamed);
    take(editor::write_file(renamed,"v 0 0 0\nv 4 0 0\nv 0 4 0\nf 1 2 3\n"));
    fs::last_write_time(renamed,old_time+std::chrono::seconds(1));
    take(project.refresh());
    const auto updated_mesh=take(project.mesh(resource)).mesh;
    require(updated_mesh!=original_mesh && updated_mesh->vertices()[1].position[0]==4 && original_mesh->vertices()[1].position[0]==1,
        "Changed mesh did not create a new resource version while preserving old live geometry.");
    require(!project.collision(resource,collision_scale),"Collision cache accepted changed source geometry.");
    take(project.cook_collision(resource,collision_scale));
    require(*take(project.collision(resource,collision_scale))!=*collision_bytes,
        "Changed geometry produced the old cooked shape.");
    fs::remove_all(folder/"Cache"); // Rebuildable data cannot be required to open the project.
    const auto moved=evidence/L"搬移后";fs::rename(folder,moved);
    auto reopened=take(editor::Project::open(moved));
    require(!reopened.collision(resource,collision_scale),"Removed collision cache was silently reused.");
    take(reopened.cook_collision(resource,collision_scale));
    require(bool(reopened.collision(resource,collision_scale)),"Reopened project could not rebuild collision cache.");
    auto restored=take(editor::Session::deserialize(take(editor::read_file(take(reopened.resolve(reopened.startup_scene())))),reopened));
    require(take(restored->serialize())==saved,"Moving a project or removing Cache broke the scene.");
    fs::rename(moved/"Content/Meshes/Renamed.obj",moved/"Content/Meshes/Renamed.missing");
    take(reopened.refresh());require(!editor::Session::deserialize(saved,reopened),"A missing resource was silently replaced.");
    require(take(document.serialize())==saved,"Failed loading changed the original document.");
    const auto material=take(world::read_static_mesh(document.world().runtime(),mesh)).material;
    take(document.world().runtime().with_native<world::StaticMeshComponent>(mesh,[](auto& value)->Result<void>{value.material.base_color.x=2;return {};}));
    require(!document.serialize(),"Save produced a material that its own loader rejects.");
    take(document.world().runtime().with_native<world::StaticMeshComponent>(mesh,[&](auto& value)->Result<void>{value.material=material;return {};}));
    take(world::set_geometry(document.world().runtime(),mesh,editor::mesh_assets().front().mesh));
    require(take(world::read_static_mesh(document.world().runtime(),mesh)).mesh.source.empty() && !document.serialize(),
        "Unattributed procedural geometry retained stale provenance or was silently saved.");
    std::ofstream(evidence/"acceptance.txt")<<"Project document acceptance: PASS\nCreate/open; UTF-8; OBJ import; stable resource ID; full composition; dirty undo; version/read/write failure; moved project; missing resource; Cache removal; fixed Scene v1 file preserves canonical math, TRS hierarchy, mesh source and material across reopen.\n";
    std::puts("Project document acceptance: PASS");
}
