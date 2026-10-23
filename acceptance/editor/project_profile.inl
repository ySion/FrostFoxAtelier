// Each sample is one frame of a profile Recorder, so samples and nested phases use the product clock.
constinit timing::Marker profile_calibration{"profile.calibration","Scope cost calibration",&timing::frame};
constexpr std::array<timing::Marker*,1> profile_markers{&profile_calibration};

// Steps a Play copy of the scene at 60 Hz, one fixed step per frame, until no rigid
// body has been active for 120 frames (at most 1800 frames), and reports the physics
// phases of frames that end with and without active bodies, the SceneSnapshot refresh after each frame,
// the Play start and each of the first five frames.
void profile_physics_cpu(const std::filesystem::path& root,std::string_view scene,const std::filesystem::path& output) {
    using Json=nlohmann::json;
    require(output.is_absolute() && !std::filesystem::exists(output),"Choose a new absolute physics profile file.");
    timing::install({world::phases::markers,physics::phases::markers,profile_markers});
    timing::Recorder recorder;
    auto project=take(editor::Project::open(root));
    auto play=take(editor::Session::deserialize(take(editor::read_file(take(project.resolve(scene)))),project));
    const auto starting=std::chrono::steady_clock::now();
    changed(play->start());
    const std::chrono::duration<double,std::milli> started=std::chrono::steady_clock::now()-starting;
    const auto& physics=*play->physics_scene();
    const std::array phases{&physics::phases::step,&physics::phases::reconcile,&physics::phases::solve,&physics::phases::write_back};
    std::array<std::vector<double>,5> active,sleeping;
    // Costs that only the first steps pay, such as building the broad phase, vanish in the medians.
    Json first_frames=Json::array();
    application::SceneSnapshot snapshot;
    std::size_t steps{},quiet{},frames{};
    for(;frames<1800 && quiet<120;++frames) {
        {
            timing::Recorder::Frame frame{recorder};take(play->update(1.f/60));
            require(frame.finish().structure_errors==0,"Physics profile recorded a misplaced or unregistered phase.");
        }
        const auto refresh_start=std::chrono::steady_clock::now();take(snapshot.refresh(*play));
        const std::chrono::duration<double,std::milli> refreshed=std::chrono::steady_clock::now()-refresh_start;
        steps+=physics.last_step().steps;
        if(frames<5) first_frames.push_back({{"step_ms",std::chrono::duration<double,std::milli>(recorder.last().time(physics::phases::step)).count()},
            {"solve_ms",std::chrono::duration<double,std::milli>(recorder.last().time(physics::phases::solve)).count()}});
        const bool awake=physics.active_body_count()!=0;
        auto& bucket=awake?active:sleeping;
        for(std::size_t i=0;i<phases.size();++i)
            bucket[i].push_back(std::chrono::duration<double,std::milli>(recorder.last().time(*phases[i])).count());
        bucket[4].push_back(refreshed.count());
        quiet=awake?0:quiet+1;
    }
    Json report{{"schema","atelier.editor.physics-cpu.v1"},{"project",editor::utf8(root.wstring())},{"scene",scene},
        {"bodies",physics.body_count()},{"frames",frames},{"steps",steps},{"active_frames",active[0].size()},{"sleeping_frames",sleeping[0].size()},
        {"start_ms",started.count()},{"first_frames",first_frames}};
#ifdef NDEBUG
    report["configuration"]="release";
#else
    report["configuration"]="dev";
#endif
    for(const auto& [name,bucket]:{std::pair{"active",&active},std::pair{"sleeping",&sleeping}}) {
        if((*bucket)[0].empty()) continue;
        for(std::size_t i=0;i<phases.size();++i) {
            auto values=(*bucket)[i];std::ranges::sort(values);
            report[name][std::string(phases[i]->key())]={{"median_ms",values[values.size()/2]},{"p95_ms",values[values.size()*95/100]}};
        }
        auto refreshes=(*bucket)[4];std::ranges::sort(refreshes);
        report[name]["snapshot.refresh"]={{"median_ms",refreshes[refreshes.size()/2]},{"p95_ms",refreshes[refreshes.size()*95/100]}};
        std::printf("Physics CPU %s frames %zu: reconcile %.3f, solve %.3f, write_back %.3f, snapshot %.3f ms median\n",name,(*bucket)[0].size(),
            report[name]["physics.reconcile"]["median_ms"].get<double>(),report[name]["physics.solve"]["median_ms"].get<double>(),
            report[name]["physics.write_back"]["median_ms"].get<double>(),report[name]["snapshot.refresh"]["median_ms"].get<double>());
    }
    changed(play->stop());
    take(editor::write_file(output,report.dump(2)+"\n"));
}
// Loads the real project through Session and measures existing CPU consumers.
// No window, graphics device or author edits; only the Play stages use a second World.
void profile_scene_cpu(const std::filesystem::path& root,std::string_view scene,const std::filesystem::path& output,
                       const std::filesystem::path& camera_report) {
    using Json=nlohmann::json;
    require(output.is_absolute() && !std::filesystem::exists(output),"Choose a new absolute CPU profile file.");
    timing::install({world::phases::markers,physics::phases::markers,profile_markers});
    timing::Recorder recorder;
    const auto milliseconds=[](std::chrono::nanoseconds value){return std::chrono::duration<double,std::milli>(value).count();};
    // Temporaries destroyed by the operation are part of its sample.
    const auto sample=[&](auto&& operation) {
        timing::Recorder::Frame frame{recorder};operation();
        const auto& record=frame.finish();
        require(record.structure_errors==0,"CPU profile recorded a misplaced or unregistered phase.");
        return milliseconds(record.time(timing::frame));
    };
    auto project=take(editor::Project::open(root));
    const auto path=take(project.resolve(scene));
    const auto source=take(editor::read_file(path));
    auto document=take(editor::Session::deserialize(source,project));
    const auto original=take(document->serialize());
    application::SceneSnapshot snapshot;take(snapshot.refresh(*document));
    const auto geometry=take(document->presentation().render_items());
    std::unordered_set<const geometry::Mesh*> unique;
    std::unordered_set<ObjectId> materials;
    std::unordered_map<const material::Image*,unsigned> image_modes;
    std::size_t triangles{},material_bindings{};
    for(const auto& mesh:snapshot.meshes) {
        unique.insert(mesh.geometry.get());triangles+=mesh.geometry->vertices().size()/3;
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
    for(const auto& item:geometry) for(const auto& binding:take(presentation::read_material_bindings(document->world().runtime(),item.component))) {
        materials.insert(binding.resource.id());++material_bindings;
    }
    Json report{{"schema","atelier.editor.scene-cpu.v3"},{"project",editor::utf8(root.wstring())},{"scene",scene},
        {"gpu",false},{"samples",121},{"warmup",20},{"source_bytes",source.size()},
        {"entities",take(document->world().members()).size()},{"selected_entities",document->selected_entities().size()},{"meshes",snapshot.meshes.size()},
        {"unique_geometries",unique.size()},{"instance_triangles",triangles},
        {"unique_sampled_images",sampled_images},{"material_texture_table_entries",sampled_images+1},
        {"material_bindings",material_bindings},{"unique_bound_materials",materials.size()},
        {"semantics","Independent CPU calls, including temporary destruction, in one loaded project; each sample is one frame-timing Recorder frame. No GPU, Present or FPS estimate. Workspace stages include DSL commit without an accepted UI frame; ui_docked adds headless Session set_controls, prepare and accept."}};
    if(!camera_report.empty()) {
        // Consume the accepted product matrices, including its actual aspect.
        const auto reference=Json::parse(take(editor::read_file(camera_report)));
        const auto& viewport=reference.at("viewport");const auto& camera=reference.at("camera");
        const auto width=viewport.at("receiver_pixel_extent").at(0).get<float>();
        const auto height=viewport.at("receiver_pixel_extent").at(1).get<float>();
        const renderer::detail::SceneFrustum frustum(camera.at("view_matrix").get<std::array<float,16>>(),
            camera.at("projection_matrix").get<std::array<float,16>>(),width,height);
        // The Renderer draws each visible geometry's material parts once per surface pass.
        std::unordered_set<const geometry::Mesh*> drawn;
        std::size_t visible{},visible_triangles{},visible_geometry_parts{};
        for(const auto& mesh:snapshot.meshes) {
            if(!frustum.visible(mesh.geometry->bounds(),mesh.world)) continue;
            ++visible;visible_triangles+=mesh.geometry->vertices().size()/3;
            if(drawn.insert(mesh.geometry.get()).second) visible_geometry_parts+=mesh.geometry->parts().size();
        }
        report["visibility"]={{"camera_report",editor::utf8(camera_report.wstring())},{"camera",camera},
            {"pixel_extent",{width,height}},{"visible_instances",visible},{"culled_instances",snapshot.meshes.size()-visible},
            {"visible_geometries",drawn.size()},{"visible_geometry_parts",visible_geometry_parts},{"visible_instance_triangles",visible_triangles}};
    }
#ifdef NDEBUG
    report["configuration"]="release";
#else
    report["configuration"]="dev";
#endif
    // An optional inspector receives each sample's frame record, warmup included.
    const auto measure=[&](std::string_view name,auto&& operation,auto&&... inspect) {
        std::vector<double> samples;samples.reserve(121);
        for(unsigned i=0;i<141;++i) {
            const auto elapsed=sample(operation);
            (inspect(recorder.last()),...);
            if(i>=20) samples.push_back(elapsed);
        }
        const auto total=std::accumulate(samples.begin(),samples.end(),0.);
        std::ranges::sort(samples);
        report["stages"][name]={{"mean_ms",total/samples.size()},{"median_ms",samples[60]},{"p95_ms",samples[114]}};
        std::printf("Scene CPU %.*s: %.3f ms median\n",static_cast<int>(name.size()),name.data(),samples[60]);
    };
    measure("render_items",[&]{take(document->presentation().render_items());});
    measure("light_items",[&]{take(document->presentation().light_items());});
    measure("material_surfaces",[&]{for(const auto& item:geometry) take(presentation::material_surfaces(document->world().runtime(),item.component));});
    measure("scene_snapshot",[&]{take(snapshot.refresh(*document));});
    {
        // Changed frames: the positions of 1 or 50 leaf mesh Entities alternate through Runtime writes.
        auto& runtime=document->world().runtime();const auto& x=document->types().position_fields[0];
        std::vector<std::pair<ObjectId,float>> leaves;
        for(const auto& item:geometry) {
            if(leaves.size()==50) break;
            if(std::ranges::find(leaves,item.entity,&std::pair<ObjectId,float>::first)!=leaves.end() ||
               !take(world::children(runtime,item.entity)).empty()) continue;
            leaves.emplace_back(item.entity,std::get<float>(take(runtime.read(item.entity,x))));
        }
        require(leaves.size()==50,"CPU profile requires 50 leaf mesh Entities.");
        bool moved{};
        const auto move=[&](std::size_t count) {
            moved=!moved;
            for(std::size_t i=0;i<count;++i) take(runtime.write(leaves[i].first,x,Scalar{moved?leaves[i].second+1:leaves[i].second}));
            take(snapshot.refresh(*document));
        };
        measure("scene_snapshot_changed",[&]{move(1);});
        measure("scene_snapshot_changed_50",[&]{move(50);});
        for(const auto& [entity,original]:leaves) take(runtime.write(entity,x,Scalar{original}));
        take(snapshot.refresh(*document));
    }
    {
        // Paired World edits restore the same graph; the document history is not involved.
        auto& world=const_cast<world::World&>(document->world());
        auto& runtime=world.runtime();
        ObjectId leaf;std::size_t spatial{};
        for(const auto& member:take(world.members())) {
            if(take(world::children(runtime,member.id())).empty()) leaf=member.id();
            spatial+=take(world::has_transform(runtime,member.id()));
        }
        require(leaf!=ObjectId{},"CPU profile requires a leaf Entity.");
        report["spatial_entities"]=spatial;
        const bool enabled=take(world::inspect_entity(runtime,leaf)).enabled;
        measure("enabled_toggle_pair",[&]{changed(world.set_enabled(leaf,!enabled));changed(world.set_enabled(leaf,enabled));});
        measure("detach_restore_leaf",[&]{auto removal=take(world.detach(leaf));changed(world.restore(removal));});
        measure("world_matrix_each_entity",[&]{
            for(const auto& member:take(world.members()))
                if(take(world::has_transform(runtime,member.id()))) take(world::world_matrix(runtime,member.id()));
        });
        const auto once=[&](std::string_view name,auto&& operation) {
            const auto elapsed=sample(operation);
            report["stages"][name]={{"single_ms",elapsed}};
            std::printf("Scene CPU %.*s: %.3f ms single\n",static_cast<int>(name.size()),name.data(),elapsed);
        };
        // Like the Play process, Play runs on its own copy of the source, so dynamic bodies leave the document unchanged.
        auto play=take(editor::Session::deserialize(source,project));
        once("play_start",[&]{changed(play->start());});
        // Structure is checked on every sample; phase durations are diagnostics only.
        const std::array per_update{&world::phases::update,&world::phases::pre_physics,&world::phases::pre_physics_tick,&world::phases::pre_physics_tasks,
            &world::phases::pre_physics_commands,&world::phases::pre_physics_flush,&world::phases::simulation,&world::phases::post_physics,
            &world::phases::post_physics_tick,&world::phases::post_physics_commands,&world::phases::post_physics_flush,&world::phases::late,
            &world::phases::late_tick,&world::phases::late_commands,&world::phases::late_flush,&physics::phases::step,
            &physics::phases::reconcile,&physics::phases::events};
        std::vector<std::vector<double>> phases(timing::catalog().size());
        measure("play_update",[&]{take(play->update(1.f/60));},[&](const timing::FrameRecord& record) {
            const auto steps=play->physics_scene()->last_step().steps;
            require(std::ranges::all_of(per_update,[&](const auto* marker){return record.count(*marker)==1;}) &&
                record.count(physics::phases::substep)==steps &&
                record.count(physics::phases::solve)==steps && record.count(physics::phases::write_back)==steps,
                "play_update did not record each World and physics phase once per frame.");
            for(const auto* marker:timing::catalog()) phases[marker->index()].push_back(milliseconds(record.time(*marker)));
        });
        for(const auto* marker:timing::catalog()) {
            if(marker==&timing::frame || marker==&profile_calibration) continue;
            auto& values=phases[marker->index()];values.erase(values.begin(),values.begin()+20);std::ranges::sort(values);
            report["stages"]["play_update"]["phases"][std::string(marker->key())]={{"median_ms",values[60]},{"p95_ms",values[114]}};
        }
        once("play_stop",[&]{changed(play->stop());});
    }
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
    {
        // The docked editor's UI step on a headless Session: description against the accepted frame, then layout.
        const std::array fonts{(editor::asset_directory()/"fonts/SarasaUiSC-Regular.ttf").wstring()};
        ui::Session surface(fonts);
        const auto theme=ui::StyleSheet::parse(take(editor::read_file(editor::asset_directory()/"editor.ss")));
        require(theme && surface.set_style_sheet(*theme) && surface.set_surface({{1779,1154},1,1}),"CPU profile UI surface failed.");
        std::size_t controls{},rows{};
        measure("ui_docked",[&] {
            const auto described=workspace.describe(*document,{1779,1154},surface.presented().get(),&project,&surface);
            require(workspace.ui_error().empty() && surface.set_controls(described),"CPU profile UI description was refused.");
            workspace.commit_ui_controls();
            const auto frame=surface.prepare();require(frame && surface.accept(*frame),"CPU profile UI layout failed.");
            controls=described.size();rows=(*frame)->tree_rows.size();
        });
        report["stages"]["ui_docked"]["controls"]=controls;report["stages"]["ui_docked"]["tree_rows"]=rows;
        std::printf("Scene CPU ui_docked: %zu controls, %zu tree rows\n",controls,rows);
    }
    {
        // Overhead estimate on the same clock and machine; never a pass condition.
        constexpr std::uint32_t scopes=10000;
        std::vector<double> samples;
        for(unsigned i=0;i<41;++i) {
            timing::Recorder::Frame frame{recorder};
            for(std::uint32_t n=0;n<scopes;++n) timing::Scope scope{profile_calibration};
            const auto& record=frame.finish();
            require(record.structure_errors==0 && record.count(profile_calibration)==scopes,"Scope calibration lost entries.");
            if(i>=10) samples.push_back(double(record.time(timing::frame).count())/scopes);
        }
        std::ranges::sort(samples);
        report["timing_scope_ns"]={{"median",samples[15]},{"scopes_per_sample",scopes},
            {"semantics","Frame root time divided by sequential empty scopes, including loop overhead."}};
        std::printf("Scene CPU timing scope: %.1f ns median\n",samples[15]);
    }
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
    namespace fs = std::filesystem;
    require(!fs::exists(output), "Choose a new JSON measurement output directory.");
    timing::install({});
    timing::Recorder recorder;
    fs::create_directories(output);
    auto project = take(editor::Project::create(output / "Project"));
    Json report = {{"schema", 1}, {"json_library", "nlohmann/json 3.12.0"},
        {"rounds", 21}, {"clock", "frame timing Recorder (steady_clock)"},
        {"notes", "Warm file-system cache; same process. Parse excludes DOM destruction. Scene load includes parse, validation, resource resolution and fresh Runtime/World construction; excludes document destruction. No GUI or GPU work."},
        {"scenes", Json::array()}};
#ifdef NDEBUG
    report["configuration"] = "release";
#else
    report["configuration"] = "dev";
#endif
    const auto measure = [&](auto&& operation) {
        std::vector<double> samples;
        for (int round = -2; round < 21; ++round) {
            timing::Recorder::Frame frame{recorder};
            // Keep the operation's output alive until the frame has finished.
            auto result = operation();
            const auto elapsed = std::chrono::duration<double, std::milli>(frame.finish().time(timing::frame)).count();
            if (round >= 0) samples.push_back(elapsed);
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
