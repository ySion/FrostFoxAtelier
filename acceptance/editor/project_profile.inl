// Loads the real project through Session and measures existing CPU consumers.
// No window, graphics device, author edits or alternate World are constructed.
void profile_scene_cpu(const std::filesystem::path& root,std::string_view scene,const std::filesystem::path& output,
                       const std::filesystem::path& camera_report) {
    using Json=nlohmann::json;
    using Clock=std::chrono::steady_clock;
    require(output.is_absolute() && !std::filesystem::exists(output),"Choose a new absolute CPU profile file.");
    auto project=take(editor::Project::open(root));
    const auto path=take(project.resolve(scene));
    const auto source=take(editor::read_file(path));
    auto document=take(editor::Session::deserialize(source,project));
    const auto original=take(document->serialize());
    application::SceneSnapshot snapshot;take(snapshot.refresh(*document));
    const auto geometry=take(document->world().render_items());
    std::unordered_set<const geometry::Mesh*> unique;
    std::unordered_set<ObjectId> materials;
    std::unordered_map<const material::Image*,unsigned> image_modes;
    const geometry::Mesh* previous{};
    std::size_t batches{},triangles{},material_bindings{};
    for(const auto& mesh:snapshot.meshes) {
        unique.insert(mesh.geometry.get());triangles+=mesh.geometry->vertices().size()/3;
        if(mesh.geometry.get()!=previous) {++batches;previous=mesh.geometry.get();}
        for(std::size_t slot=0;slot<mesh.geometry->materials().size();++slot) {
            const auto& surface=mesh.surfaces?(*mesh.surfaces)[slot]:mesh.geometry->materials()[slot].surface;
            for(std::size_t input=0;input<static_cast<std::size_t>(material::Input::count);++input) {
                const auto& image=surface->description().textures[input].image;
                if(image) image_modes[image.get()]|=input==static_cast<std::size_t>(material::Input::base_color) ||
                    input==static_cast<std::size_t>(material::Input::emissive)?2u:1u;
            }
        }
    }
    std::size_t sampled_images{};
    for(const auto& [image,modes]:image_modes) {(void)image;sampled_images+=(modes&1)!=0;sampled_images+=(modes&2)!=0;}
    for(const auto& item:geometry) for(const auto& binding:take(world::read_material_bindings(document->world().runtime(),item.component))) {
        materials.insert(binding.resource.id());++material_bindings;
    }
    Json report{{"schema","atelier.editor.scene-cpu.v1"},{"project",editor::utf8(root.wstring())},{"scene",scene},
        {"gpu",false},{"samples",121},{"warmup",20},{"source_bytes",source.size()},
        {"entities",take(document->world().members()).size()},{"meshes",snapshot.meshes.size()},
        {"unique_geometries",unique.size()},{"adjacent_geometry_batches",batches},{"instance_triangles",triangles},
        {"unique_sampled_images",sampled_images},{"material_texture_table_entries",sampled_images+1},
        {"material_bindings",material_bindings},{"unique_bound_materials",materials.size()},
        {"semantics","Independent CPU calls, including temporary destruction, in one loaded project. No GPU, Present, UI prepare or FPS estimate. Workspace includes DSL commit; no accepted UI frame is supplied."}};
    if(!camera_report.empty()) {
        // Consume the accepted product matrices, including its actual aspect.
        const auto reference=Json::parse(take(editor::read_file(camera_report)));
        const auto& viewport=reference.at("viewport");const auto& camera=reference.at("camera");
        const auto width=viewport.at("receiver_pixel_extent").at(0).get<float>();
        const auto height=viewport.at("receiver_pixel_extent").at(1).get<float>();
        const renderer::detail::SceneFrustum frustum(camera.at("view_matrix").get<std::array<float,16>>(),
            camera.at("projection_matrix").get<std::array<float,16>>(),width,height);
        std::size_t visible{},visible_triangles{},visible_runs{};
        previous=nullptr;bool continuing=false;
        for(const auto& mesh:snapshot.meshes) {
            if(frustum.visible(mesh.geometry->bounds(),mesh.world)) {
                ++visible;visible_triangles+=mesh.geometry->vertices().size()/3;
                if(!continuing || mesh.geometry.get()!=previous) ++visible_runs;
                continuing=true;
            } else continuing=false;
            previous=mesh.geometry.get();
        }
        report["visibility"]={{"camera_report",editor::utf8(camera_report.wstring())},{"camera",camera},
            {"pixel_extent",{width,height}},{"visible_instances",visible},{"culled_instances",snapshot.meshes.size()-visible},
            {"visible_geometry_runs",visible_runs},{"visible_instance_triangles",visible_triangles}};
    }
#ifdef NDEBUG
    report["configuration"]="release";
#else
    report["configuration"]="dev";
#endif
    const auto measure=[&](std::string_view name,auto&& operation) {
        std::vector<double> samples;samples.reserve(121);
        for(unsigned i=0;i<141;++i) {
            const auto start=Clock::now();operation();
            const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            if(i>=20) samples.push_back(elapsed);
        }
        const auto total=std::accumulate(samples.begin(),samples.end(),0.);
        std::ranges::sort(samples);
        report["stages"][name]={{"mean_ms",total/samples.size()},{"median_ms",samples[60]},{"p95_ms",samples[114]}};
        std::printf("Scene CPU %.*s: %.3f ms median\n",static_cast<int>(name.size()),name.data(),samples[60]);
    };
    measure("render_items",[&]{take(document->world().render_items());});
    measure("light_items",[&]{take(document->world().light_items());});
    measure("material_surfaces",[&]{for(const auto& item:geometry) take(world::material_surfaces(document->world().runtime(),item.component));});
    measure("scene_snapshot",[&]{take(snapshot.refresh(*document));});
    editor::Workspace workspace;
    workspace.project_open=true;
    workspace.dock.maximize(editor::control::scene_tab);
    const auto describe=[&] {
        auto controls=workspace.describe(*document,{1779,1154},nullptr,&project);
        require(workspace.ui_error().empty() && !controls.empty(),"CPU profile UI description failed.");
        workspace.commit_ui_controls();
    };
    measure("workspace_maximized",describe);
    workspace.dock.maximize(editor::control::scene_tab);
    measure("workspace_docked",describe);
    require(take(document->serialize())==original && take(editor::read_file(path))==source &&
        !document->dirty() && document->undo_count()==0 && document->redo_count()==0,
        "CPU profile changed the document or source Scene.");
    report["status"]="pass";
    std::filesystem::create_directories(output.parent_path());
    take(editor::write_file(output,report.dump(2)+"\n",editor::WriteMode::create));
}

// Optional measurement through the existing document acceptance executable.
// It creates its own fixture; it never opens or changes a user's project.
void profile_project_json(const std::filesystem::path& output) {
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    namespace fs = std::filesystem;
    require(!fs::exists(output), "Choose a new JSON measurement output directory.");
    fs::create_directories(output);
    auto project = take(editor::Project::create(output / "Project"));
    Json report = {{"schema", 1}, {"json_library", "nlohmann/json 3.12.0"},
        {"rounds", 21}, {"clock", "steady_clock"},
        {"notes", "Warm file-system cache; same process. Parse excludes DOM destruction. Scene load includes parse, validation, resource resolution and fresh Runtime/World construction; excludes document destruction. No GUI or GPU work."},
        {"scenes", Json::array()}};
#ifdef NDEBUG
    report["configuration"] = "release";
#else
    report["configuration"] = "dev";
#endif
    const auto measure = [](auto&& operation) {
        std::vector<double> samples;
        for (int round = -2; round < 21; ++round) {
            const auto start = Clock::now();
            auto result = operation();
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (round >= 0) samples.push_back(elapsed);
            // Keep the operation's output alive until after the end timestamp.
            (void)result;
        }
        std::ranges::sort(samples);
        return Json{{"median_ms", samples[10]}, {"p95_ms", samples[19]}, {"max_ms", samples.back()}};
    };
    for (const auto [entities, components] : {std::pair{1, 1}, {64, 1}, {256, 1}, {256, 16}}) {
        editor::Session document;
        for (int entity = 0; entity < entities; ++entity) {
            changed(document.create_static_mesh(editor::mesh_assets()[static_cast<std::size_t>(entity) % editor::mesh_assets().size()]));
            take(document.rename(document.selection().entity, L"测量 Entity " + std::to_wstring(entity)));
            for (int component = 1; component < components; ++component)
                changed(document.add_component(document.types().component));
        }
        const auto text = take(document.serialize());
        const auto path = output / (std::to_string(entities) + "x" + std::to_string(components) + ".scene");
        take(editor::write_file(path, text));
        auto roundtrip = take(editor::Session::deserialize(text, project));
        require(take(roundtrip->serialize()) == text, "JSON measurement fixture did not roundtrip.");
        roundtrip.reset();
        auto dom = Json::parse(text);
        Json row = {{"entities", entities}, {"components_per_entity", components}, {"bytes", text.size()},
            {"read_cached", measure([&] { return take(editor::read_file(path)); })},
            {"parse_dom", measure([&] { return Json::parse(text); })},
            {"parse_format", measure([&] { return editor::json_format::parse(text); })},
            {"dump_dom", measure([&] { return dom.dump(2); })},
            {"save_scene_to_text", measure([&] { return take(document.serialize()); })},
            {"load_scene_from_text", measure([&] { return take(editor::Session::deserialize(text, project)); })},
            {"load_scene_from_file", measure([&] { return take(editor::Session::deserialize(take(editor::read_file(path)), project)); })}};
        row["parse_mb_per_second"] = static_cast<double>(text.size()) / row["parse_dom"]["median_ms"].get<double>() / 1000.;
        std::printf("JSON %dx%d: %zu bytes, parse %.3f ms (%.1f MB/s), scene load %.3f ms\n",
            entities, components, text.size(), row["parse_dom"]["median_ms"].get<double>(),
            row["parse_mb_per_second"].get<double>(), row["load_scene_from_text"]["median_ms"].get<double>());
        report["scenes"].push_back(std::move(row));
    }
    take(editor::write_file(output / "json-profile.json", report.dump(2) + "\n"));
}
