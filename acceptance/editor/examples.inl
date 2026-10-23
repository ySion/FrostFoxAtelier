void verify_official_examples() {
    namespace fs=std::filesystem;
    const auto output=fs::absolute("out/official-examples-document")/std::to_string(GetTickCount64());
    fs::create_directories(output);
    const auto previous=fs::current_path();
    struct RestoreDirectory {fs::path path;~RestoreDirectory(){fs::current_path(path);}} restore{previous};
    fs::current_path(output); // No source tree or executable-relative cwd dependency.
    const auto source=editor::asset_directory()/"examples";
    auto shipped=take(editor::Project::open(source));
    const auto copy=take(editor::prepare_examples(output/"Workspace"));
    require(copy!=source,"Official examples opened their immutable template for editing.");
    auto project=take(editor::Project::open(copy));
    for(const auto* folder:{"Examples","Examples/Sponza","Examples/Scene Lit Room","Examples/Physic","Examples/Dungeons"})
        require(std::ranges::any_of(project.entries(),[&](const auto& entry){
            return entry.path==folder && entry.kind==editor::AssetKind::folder;
        }),"Official examples are absent from the shared Project directory catalog.");
    for(const auto& example:editor::official_examples) {
        const auto source_scene=take(shipped.resolve(example.scene));
        const auto original=take(editor::read_file(source_scene));
        const auto scene_path=take(project.resolve(example.scene));
        auto document=take(editor::Session::deserialize(take(editor::read_file(scene_path)),project));
        const auto meshes=take(document->presentation().render_items());
        require(example.command==editor::Command::example_dungeons?meshes.size()==3425:
            example.command==editor::Command::example_physic?meshes.size()==14:
            meshes.size()==(example.command==editor::Command::example_sponza?1u:10u),"Official example lost geometry.");
        const auto fields=take(document->properties(meshes.front().component));
        const auto roughness=std::ranges::find(fields,"material.roughness",&editor::Property::name);
        require(roughness!=fields.end(),"Official example has no editable material.");
        take(document->set_property(meshes.front().component,roughness->path,.61f,881001));
        const auto saved=take(document->serialize());take(editor::write_file(scene_path,saved));
        require(take(editor::prepare_examples(output/"Workspace"))==copy &&
            take(editor::read_file(scene_path))==saved,"Opening an example again overwrote saved user changes.");
        require(take(editor::read_file(source_scene))==original,"Editing an example changed the shipped template.");
    }
    // Simulate an older valid workspace. Publishing a new folder must retain
    // existing edited examples and be idempotent on the following open.
    const auto legacy=output/"LegacyWorkspace";
    auto legacy_project=take(editor::Project::create(legacy));
    const auto existing=legacy/editor::official_examples[1].scene;
    take(editor::write_file(existing,"user-owned scene marker"));
    take(editor::prepare_examples(legacy));
    require(take(editor::read_file(existing))=="user-owned scene marker" &&
        fs::is_regular_file(legacy/"Examples/Physic/Scenes/Ragdoll.scene"),"Adding newly shipped examples damaged an existing folder.");
    take(editor::prepare_examples(legacy));
    require(take(editor::read_file(existing))=="user-owned scene marker","Repeated example hydration replaced user data.");
    const auto added="Examples/Another Scene/Custom.scene";
    take(editor::write_file(take(project.resolve(added)),take(editor::read_file(copy/editor::official_examples[1].scene))));
    take(project.refresh());
    require(std::ranges::any_of(project.entries(),[&](const auto& entry){return entry.path==added && entry.kind==editor::AssetKind::scene;}),
        "Adding another Examples folder did not refresh the Project catalog.");
    require(editor::Project::is_scene_path(added) && !editor::Project::is_scene_path("Config/Custom.scene"),"Scene storage boundary changed unexpectedly.");
    take(editor::write_file(copy/"Config/Project.json","broken user project"));
    require(!editor::prepare_examples(output/"Workspace") && take(editor::read_file(copy/"Config/Project.json"))=="broken user project",
        "Broken existing example data was overwritten instead of reported.");
    std::puts("Official examples document acceptance: PASS (shared Project/Examples hierarchy, extensible scene folders, bundled assets, arbitrary cwd, save/reopen, template and invalid-copy preservation)");
}
