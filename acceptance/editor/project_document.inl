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
    const auto revision=project.revision();
    take(project.refresh());take(project.refresh());
    require(project.revision()==revision && take(project.mesh(resource)).mesh==original_mesh,
        "An unchanged refresh changed catalog identity or discarded a live mesh.");
    verify_project_workspace(project);
    require(!project.import_mesh(external),"Import silently overwrote an existing asset.");
    editor::Session document;
    changed(document.create_static_mesh(take(project.mesh(resource))));
    const auto actor=document.selected();
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
    fs::remove(folder/"Cache"); // Empty, rebuildable directory only.
    const auto moved=evidence/L"搬移后";fs::rename(folder,moved);
    auto reopened=take(editor::Project::open(moved));
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
    std::ofstream(evidence/"acceptance.txt")<<"Project document acceptance: PASS\nCreate/open; UTF-8; OBJ import; stable resource ID; full composition; dirty undo; version/read/write failure; moved project; missing resource; Cache removal.\n";
    std::puts("Project document acceptance: PASS");
}
