// Each sample is one frame of a profile Recorder, so samples and nested phases use the product clock.
constinit timing::Marker profile_calibration{"profile.calibration", "Scope cost calibration", &timing::frame};

constexpr std::array<timing::Marker*, 1> profile_markers{&profile_calibration};

// Steps a Play copy of the scene at 60 Hz, one fixed step per frame, until no rigid
// body has been active for 120 frames (at most 1800 frames), and reports the physics
// phases of frames that end with and without active bodies, the SceneSnapshot refresh after each frame,
// the Play start and each of the first five frames.
Result<void> profile_physics_cpu(const std::filesystem::path& root, std::string_view scene,
                                 const std::filesystem::path& output) {
    ATELIER_TRY_BIND(exists, acceptance::file_exists(output));
    if (!output.is_absolute() || *exists)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Choose a new absolute physics profile file."});
    application::install_frame_timing(profile_markers);
    timing::Recorder recorder;
    ATELIER_TRY_BIND(opened, editor::Project::open(root));
    auto& project = *opened;
    ATELIER_TRY_BIND(source, project.read_scene(scene));
    ATELIER_TRY_BIND(loaded, editor::Session::deserialize(*source, project));
    auto& play = *loaded;
    {
        timing::Recorder::Frame frame{recorder};
        ATELIER_TRY(changed(play->start()));
        require(frame.finish().structure_errors == 0,
                "Physics profile recorded a misplaced or unregistered phase in Play start.");
    }
    const std::chrono::duration<double, std::milli> started = recorder.last().time(timing::frame);
    const auto& physics = *play->physics_scene();
    const std::array phases{&physics::phases::step, &physics::phases::reconcile, &physics::phases::solve,
                            &physics::phases::write_back};
    std::array<Array<double>, 5> active;
    std::array<Array<double>, 5> sleeping;
    // Costs that only the first steps pay, such as building the broad phase, vanish in the medians.
    json::Builder report;
    const auto first_frames = take(report.array(report.root(), "first_frames"));
    application::SceneSnapshot snapshot;
    std::size_t steps{};
    std::size_t quiet{};
    std::size_t frames{};
    for (; frames < 1800 && quiet < 120; ++frames) {
        {
            timing::Recorder::Frame frame{recorder};
            take(play->update(1.f / 60));
            {
                timing::Scope refreshing{application::phases::scene};
                take(snapshot.refresh(*play));
            }
            require(frame.finish().structure_errors == 0,
                    "Physics profile recorded a misplaced or unregistered phase.");
        }
        steps += physics.last_step().steps;
        if (frames < 5) {
            const auto frame = report.append_object(first_frames);
            take(report.set(
                frame, "step_ms",
                std::chrono::duration<double, std::milli>(recorder.last().time(physics::phases::step)).count()));
            take(report.set(
                frame, "solve_ms",
                std::chrono::duration<double, std::milli>(recorder.last().time(physics::phases::solve)).count()));
        }
        const bool awake = physics.active_body_count() != 0;
        auto& bucket = awake ? active : sleeping;
        for (std::size_t i = 0; i < phases.size(); ++i)
            bucket[i].add(std::chrono::duration<double, std::milli>(recorder.last().time(*phases[i])).count());
        bucket[4].add(
            std::chrono::duration<double, std::milli>(recorder.last().time(application::phases::scene)).count());
        quiet = awake ? 0 : quiet + 1;
    }
    const auto root_text = platform::utf16_to_utf8(root.native());
    require(bool(root_text), "Invalid UTF-16 profile root.");
    take(report.set(report.root(), "schema", "atelier.editor.physics-cpu.v1"));
    take(report.set(report.root(), "project", *root_text));
    take(report.set(report.root(), "scene", scene));
    take(report.set(report.root(), "bodies", physics.body_count()));
    take(report.set(report.root(), "frames", frames));
    take(report.set(report.root(), "steps", steps));
    take(report.set(report.root(), "active_frames", active[0].size()));
    take(report.set(report.root(), "sleeping_frames", sleeping[0].size()));
    take(report.set(report.root(), "start_ms", started.count()));
#ifdef NDEBUG
    take(report.set(report.root(), "configuration", "release"));
#else
    take(report.set(report.root(), "configuration", "dev"));
#endif
    for (const auto& [name, bucket] : {std::pair{"active", &active}, std::pair{"sleeping", &sleeping}}) {
        if ((*bucket)[0].empty())
            continue;
        const auto group = take(report.object(report.root(), name));
        for (std::size_t i = 0; i < phases.size(); ++i) {
            auto values = (*bucket)[i];
            std::ranges::sort(values);
            const auto phase = take(report.object(group, phases[i]->key()));
            take(report.set(phase, "median_ms", values[values.size() / 2]));
            take(report.set(phase, "p95_ms", values[values.size() * 95 / 100]));
        }
        auto refreshes = (*bucket)[4];
        std::ranges::sort(refreshes);
        const auto refresh = take(report.object(group, "snapshot.refresh"));
        take(report.set(refresh, "median_ms", refreshes[refreshes.size() / 2]));
        take(report.set(refresh, "p95_ms", refreshes[refreshes.size() * 95 / 100]));
        std::printf("Physics CPU %s frames %zu: reconcile %.3f, solve %.3f, write_back %.3f, snapshot %.3f ms median\n",
                    name, (*bucket)[0].size(), take(group.number({"physics.reconcile", "median_ms"})),
                    take(group.number({"physics.solve", "median_ms"})),
                    take(group.number({"physics.write_back", "median_ms"})),
                    take(group.number({"snapshot.refresh", "median_ms"})));
    }
    ATELIER_TRY(changed(play->stop()));
    const auto bytes = json::write(report.root(), {.pretty = true, .trailing_lf = true});
    return editor::write_file(output, {bytes.data(), bytes.size()}, editor::WriteMode::create);
}

// Loads the real project through Session and measures existing CPU consumers.
// No window, graphics device or author edits; only the Play stages use a second World.
Result<void> profile_scene_cpu(const std::filesystem::path& root, std::string_view scene,
                               const std::filesystem::path& output, const std::filesystem::path& camera_report) {
    ATELIER_TRY_BIND(exists, acceptance::file_exists(output));
    if (!output.is_absolute() || *exists)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Choose a new absolute CPU profile file."});
    application::install_frame_timing(profile_markers);
    timing::Recorder recorder;
    const auto milliseconds = [](std::chrono::nanoseconds value) {
        return std::chrono::duration<double, std::milli>(value).count();
    };
    // Temporaries destroyed by the operation are part of its sample.
    const auto sample = [&](auto&& operation) -> Result<double> {
        timing::Recorder::Frame frame{recorder};
        ATELIER_TRY(operation());
        const auto& record = frame.finish();
        require(record.structure_errors == 0, "CPU profile recorded a misplaced or unregistered phase.");
        return milliseconds(record.time(timing::frame));
    };
    ATELIER_TRY_BIND(opened, editor::Project::open(root));
    auto& project = *opened;
    ATELIER_TRY_BIND(path, project.resolve(scene));
    ATELIER_TRY_BIND(bytes_read, assets::read_file(*path));
    const auto& source = *bytes_read;
    ATELIER_TRY_BIND(loaded, editor::Session::deserialize(source, project));
    auto& document = *loaded;
    const auto original = take(document->serialize());
    application::SceneSnapshot snapshot;
    take(snapshot.refresh(*document));
    const auto geometry = take(document->presentation().render_items());
    Set<const geometry::Mesh*> unique;
    Set<ObjectId> materials;
    Map<const material::Image*, unsigned> image_modes;
    std::size_t triangles{};
    std::size_t material_bindings{};
    for (const auto& mesh : snapshot.picking()->meshes) {
        unique.add(mesh.geometry.get());
        triangles += mesh.geometry->vertices().size() / 3;
        require(mesh.material_slots && mesh.material_slots->size() == mesh.geometry->materials().size(),
                "CPU profile received an unresolved material slot table.");
        for (std::size_t slot = 0; slot < mesh.geometry->materials().size(); ++slot) {
            const auto index = (*mesh.material_slots)[slot];
            if (index >= snapshot.picking()->materials.size())
                return std::unexpected(Error{ErrorCode::invalid_member, "CPU profile material slot is out of range."});
            const auto& surface = snapshot.picking()->materials[index];
            require(surface != nullptr, "CPU profile received an empty resolved material.");
            for (const auto& input : surface->inputs()) {
                if (input.image)
                    *image_modes.try_emplace(input.image.get(), 0u).value |=
                        input.color == material::TextureColor::srgb ? 2u : 1u;
            }
        }
    }
    std::size_t sampled_images{};
    for (const auto [image, modes] : image_modes) {
        (void)image;
        sampled_images += (modes & 1) != 0;
        sampled_images += (modes & 2) != 0;
    }
    for (const auto& item : geometry)
        for (const auto& binding :
             take(presentation::read_material_bindings(document->world().runtime(), item.component))) {
            materials.add(binding.resource.id());
            ++material_bindings;
        }
    // MaterialBatch deduplicates by image and declared transfer function. Empty
    // inputs use typed values; only an entirely empty table needs one sentinel.
    const auto root_text = platform::utf16_to_utf8(root.native());
    require(bool(root_text), "Invalid UTF-16 profile root.");
    json::Builder report{
        json::Document::literal<R"({"schema":"atelier.editor.scene-cpu.v4","gpu":false,"samples":121,"warmup":20})">()};
    take(report.set(report.root(), "project", *root_text));
    take(report.set(report.root(), "scene", scene));
    take(report.set(report.root(), "source_bytes", source.size()));
    take(report.set(report.root(), "entities", take(document->world().members()).size()));
    take(report.set(report.root(), "selected_entities", document->selected_entities().size()));
    take(report.set(report.root(), "meshes", snapshot.meshes.size()));
    take(report.set(report.root(), "unique_geometries", unique.size()));
    take(report.set(report.root(), "instance_triangles", triangles));
    take(report.set(report.root(), "unique_sampled_images", sampled_images));
    take(report.set(report.root(), "material_texture_table_entries", std::max(std::size_t{1}, sampled_images)));
    take(report.set(report.root(), "material_bindings", material_bindings));
    take(report.set(report.root(), "unique_bound_materials", materials.size()));
    take(report.set(
        report.root(), "semantics",
        "Independent CPU calls, including temporary destruction, in one loaded project; each sample is one frame-timing Recorder frame. No GPU, Present or FPS estimate. Workspace stages include DSL commit without an accepted UI frame; ui_docked adds headless Session set_controls, prepare and accept."));
    if (!camera_report.empty()) {
        // Consume the accepted product matrices, including its actual aspect.
        const auto reference = take(assets::json_format::parse(take(assets::read_file(camera_report))));
        const auto viewport = take(reference.root().path({"viewport", "receiver_pixel_extent"}));
        const auto camera = take(reference.root().at("camera"));
        const auto width = take(take(viewport.at(0)).f32());
        const auto height = take(take(viewport.at(1)).f32());
        const renderer::detail::SceneFrustum frustum(
            math::Matrix4{take(assets::json_format::vector<16>(take(camera.at("view_matrix"))))},
            math::Matrix4{take(assets::json_format::vector<16>(take(camera.at("projection_matrix"))))}, width, height,
            take(camera.f32({"far_depth"})));
        // The Renderer draws each visible geometry once per surface pass; parts count its material slots in use.
        Set<const geometry::Mesh*> drawn;
        std::size_t visible{};
        std::size_t visible_triangles{};
        std::size_t visible_geometry_parts{};
        for (const auto& mesh : snapshot.meshes) {
            if (!frustum.visible(mesh.geometry->bounds(), mesh.world))
                continue;
            ++visible;
            visible_triangles += mesh.geometry->vertices().size() / 3;
            if (drawn.add(mesh.geometry.get()))
                visible_geometry_parts += mesh.geometry->parts().size();
        }
        const auto camera_report_text = platform::utf16_to_utf8(camera_report.native());
        require(bool(camera_report_text), "Invalid UTF-16 camera report path.");
        const auto visibility = take(report.object(report.root(), "visibility"));
        take(report.set(visibility, "camera_report", *camera_report_text));
        take(report.set(visibility, "camera", camera));
        const auto extent = take(report.array(visibility, "pixel_extent"));
        take(report.append(extent, width));
        take(report.append(extent, height));
        take(report.set(visibility, "visible_instances", visible));
        take(report.set(visibility, "culled_instances", snapshot.meshes.size() - visible));
        take(report.set(visibility, "visible_geometries", drawn.size()));
        take(report.set(visibility, "visible_geometry_parts", visible_geometry_parts));
        take(report.set(visibility, "visible_instance_triangles", visible_triangles));
    }
#ifdef NDEBUG
    take(report.set(report.root(), "configuration", "release"));
#else
    take(report.set(report.root(), "configuration", "dev"));
#endif
    const auto stages = take(report.object(report.root(), "stages"));
    // An optional inspector receives each sample's frame record, warmup included.
    const auto measure = [&](std::string_view name, auto&& operation, auto&&... inspect) -> Result<void> {
        Array<double> samples;
        samples.reserve(121);
        for (unsigned i = 0; i < 141; ++i) {
            ATELIER_TRY_BIND(elapsed, sample(operation));
            (inspect(recorder.last()), ...);
            if (i >= 20)
                samples.add(*elapsed);
        }
        const auto total = std::accumulate(samples.begin(), samples.end(), 0.);
        std::ranges::sort(samples);
        const auto stage = take(report.object(stages, name));
        take(report.set(stage, "mean_ms", total / samples.size()));
        take(report.set(stage, "median_ms", samples[60]));
        take(report.set(stage, "p95_ms", samples[114]));
        std::printf("Scene CPU %.*s: %.3f ms median\n", static_cast<int>(name.size()), name.data(), samples[60]);
        return {};
    };
    ATELIER_TRY(measure("render_items", [&] { return document->presentation().render_items(); }));
    ATELIER_TRY(measure("light_items", [&] { return document->presentation().light_items(); }));
    if (auto measured =
            measure("material_surfaces",
                    [&]() -> Result<void> {
                        for (const auto& item : geometry)
                            ATELIER_TRY(presentation::material_surfaces(document->world().runtime(), item.component));
                        return {};
                    });
        !measured)
        return std::unexpected(std::move(measured).error());
    // A refresh opens app.scene.extract, so timed refreshes run under their host phase.
    const auto refresh = [&] {
        timing::Scope scene{application::phases::scene};
        return snapshot.refresh(*document);
    };
    ATELIER_TRY(measure("scene_snapshot", refresh));
    {
        // Changed frames: the positions of 1 or 50 leaf mesh Entities alternate through Runtime writes.
        auto& runtime = document->world().runtime();
        const auto& x = document->types().position_fields[0];
        Array<std::pair<ObjectId, float>> leaves;
        for (const auto& item : geometry) {
            if (leaves.size() == 50)
                break;
            if (std::ranges::find(leaves, item.entity, &std::pair<ObjectId, float>::first) != leaves.end() ||
                !take(world::children(runtime, item.entity)).empty())
                continue;
            leaves.emplace(item.entity, std::get<float>(take(runtime.read(item.entity, x))));
        }
        require(leaves.size() == 50, "CPU profile requires 50 leaf mesh Entities.");
        bool moved{};
        const auto move = [&](std::size_t count) -> Result<void> {
            moved = !moved;
            for (std::size_t i = 0; i < count; ++i)
                ATELIER_TRY(runtime.write(leaves[i].first, x, Scalar{moved ? leaves[i].second + 1 : leaves[i].second}));
            return refresh();
        };
        ATELIER_TRY(measure("scene_snapshot_changed", [&] { return move(1); }));
        ATELIER_TRY(measure("scene_snapshot_changed_50", [&] { return move(50); }));
        for (const auto& [entity, original] : leaves)
            take(runtime.write(entity, x, Scalar{original}));
        take(snapshot.refresh(*document));
    }
    {
        // Paired World edits restore the same graph; the document history is not involved.
        auto& world = const_cast<world::World&>(document->world());
        auto& runtime = world.runtime();
        ObjectId leaf;
        std::size_t spatial{};
        for (const auto& member : take(world.members())) {
            if (take(world::children(runtime, member.id())).empty())
                leaf = member.id();
            spatial += take(world::has_transform(runtime, member.id()));
        }
        require(leaf != ObjectId{}, "CPU profile requires a leaf Entity.");
        take(report.set(report.root(), "spatial_entities", spatial));
        const bool enabled = take(world::inspect_entity(runtime, leaf)).enabled;
        if (auto measured = measure("enabled_toggle_pair",
                                    [&]() -> Result<void> {
                                        ATELIER_TRY(changed(world.set_enabled(leaf, !enabled)));
                                        return changed(world.set_enabled(leaf, enabled));
                                    });
            !measured)
            return std::unexpected(std::move(measured).error());
        if (auto measured = measure("detach_restore_leaf",
                                    [&]() -> Result<void> {
                                        ATELIER_TRY_BIND(removal, world.detach(leaf));
                                        return changed(world.restore(*removal));
                                    });
            !measured)
            return std::unexpected(std::move(measured).error());
        if (auto measured = measure("world_matrix_each_entity",
                                    [&]() -> Result<void> {
                                        ATELIER_TRY_BIND(members, world.members());
                                        for (const auto& member : *members) {
                                            ATELIER_TRY_BIND(transform, world::has_transform(runtime, member.id()));
                                            if (*transform)
                                                ATELIER_TRY(world::world_matrix(runtime, member.id()));
                                        }
                                        return {};
                                    });
            !measured)
            return std::unexpected(std::move(measured).error());
        const auto once = [&](std::string_view name, auto&& operation) -> Result<void> {
            ATELIER_TRY_BIND(elapsed, sample(operation));
            take(report.set(take(report.object(stages, name)), "single_ms", *elapsed));
            std::printf("Scene CPU %.*s: %.3f ms single\n", static_cast<int>(name.size()), name.data(), *elapsed);
            return {};
        };
        // Like the Play process, Play runs on its own copy of the source, so dynamic bodies leave the document unchanged.
        auto play = take(editor::Session::deserialize(source, project));
        ATELIER_TRY(once("play_start", [&] { return changed(play->start()); }));
        // Structure is checked on every sample; phase durations are diagnostics only.
        const std::array per_update{&world::phases::update,
                                    &world::phases::pre_physics,
                                    &world::phases::pre_physics_tick,
                                    &world::phases::pre_physics_tasks,
                                    &world::phases::pre_physics_commands,
                                    &world::phases::pre_physics_flush,
                                    &world::phases::simulation,
                                    &world::phases::post_physics,
                                    &world::phases::post_physics_tick,
                                    &world::phases::post_physics_commands,
                                    &world::phases::post_physics_flush,
                                    &world::phases::late,
                                    &world::phases::late_tick,
                                    &world::phases::late_commands,
                                    &world::phases::late_flush,
                                    &physics::phases::step,
                                    &physics::phases::reconcile,
                                    &physics::phases::events};
        Array<Array<double>> phases;
        phases.resize(timing::catalog().size());
        if (auto measured = measure(
                "play_update", [&] { return play->update(1.f / 60); },
                [&](const timing::FrameRecord& record) {
                    const auto steps = play->physics_scene()->last_step().steps;
                    require(std::ranges::all_of(per_update,
                                                [&](const auto* marker) { return record.count(*marker) == 1; }) &&
                                record.count(physics::phases::substep) == steps &&
                                record.count(physics::phases::solve) == steps &&
                                record.count(physics::phases::write_back) == steps,
                            "play_update did not record each World and physics phase once per frame.");
                    for (const auto* marker : timing::catalog())
                        phases[marker->index()].add(milliseconds(record.time(*marker)));
                });
            !measured)
            return std::unexpected(std::move(measured).error());
        const auto phase_records = take(report.object(take(stages.at("play_update")), "phases"));
        for (const auto module : {Span<timing::Marker* const>(world::phases::markers),
                                  Span<timing::Marker* const>(physics::phases::markers)})
            for (const auto* marker : module) {
                Span<double> values = Span<double>(phases[marker->index()]).slice(20);
                std::ranges::sort(values);
                const auto record = take(report.object(phase_records, marker->key()));
                take(report.set(record, "median_ms", values[60]));
                take(report.set(record, "p95_ms", values[114]));
            }
        ATELIER_TRY(once("play_stop", [&] { return changed(play->stop()); }));
    }
    editor::Workspace workspace;
    take(workspace.initialize());
    workspace.project_open = true;
    workspace.dock.maximize(editor::control::scene_tab);
    const auto describe = [&]() -> Result<void> {
        ATELIER_TRY_BIND(controls, workspace.describe(*document, {1779, 1154}, nullptr, &project));
        require(workspace.ui_error().empty() && !controls->empty(), "CPU profile UI description failed.");
        return workspace.commit_ui_controls();
    };
    ATELIER_TRY(measure("workspace_maximized", describe));
    workspace.dock.maximize(editor::control::scene_tab);
    ATELIER_TRY(measure("workspace_docked", describe));
    {
        // The docked editor's UI step on a headless Session: description against the accepted frame, then layout.
        const auto directory = take(editor::asset_directory());
        const std::array fonts{(directory / "fonts/SarasaUiSC-Regular.ttf").wstring()};
        ui::Session surface;
        take(surface.initialize(fonts));
        const auto theme = ui::StyleSheet::parse(take(assets::read_file(directory / "editor.ss")));
        require(theme && surface.set_style_sheet(*theme) && surface.set_surface({{1779, 1154}, 1, 1}),
                "CPU profile UI surface failed.");
        std::size_t controls{};
        std::size_t rows{};
        if (auto measured = measure("ui_docked",
                                    [&]() -> Result<void> {
                                        const auto described = take(workspace.describe(
                                            *document, {1779, 1154}, surface.presented().get(), &project, &surface));
                                        require(workspace.ui_error().empty() && surface.set_controls(described),
                                                "CPU profile UI description was refused.");
                                        take(workspace.commit_ui_controls());
                                        const auto frame = surface.prepare();
                                        require(frame && surface.accept(*frame), "CPU profile UI layout failed.");
                                        controls = described.size();
                                        rows = (*frame)->tree_rows.size();
                                        return {};
                                    });
            !measured)
            return std::unexpected(std::move(measured).error());
        take(report.set(take(stages.at("ui_docked")), "controls", controls));
        take(report.set(take(stages.at("ui_docked")), "tree_rows", rows));
        std::printf("Scene CPU ui_docked: %zu controls, %zu tree rows\n", controls, rows);
    }
    {
        // Overhead estimate on the same clock and machine; never a pass condition.
        constexpr std::uint32_t scopes = 10000;
        Array<double> samples;
        for (unsigned i = 0; i < 41; ++i) {
            timing::Recorder::Frame frame{recorder};
            for (std::uint32_t n = 0; n < scopes; ++n)
                timing::Scope scope{profile_calibration};
            const auto& record = frame.finish();
            require(record.structure_errors == 0 && record.count(profile_calibration) == scopes,
                    "Scope calibration lost entries.");
            if (i >= 10)
                samples.add(double(record.time(timing::frame).count()) / scopes);
        }
        std::ranges::sort(samples);
        const auto timing_scope = take(report.object(report.root(), "timing_scope_ns"));
        take(report.set(timing_scope, "median", samples[15]));
        take(report.set(timing_scope, "scopes_per_sample", scopes));
        take(report.set(timing_scope, "semantics",
                        "Frame root time divided by sequential empty scopes, including loop overhead."));
        std::printf("Scene CPU timing scope: %.1f ns median\n", samples[15]);
    }
    require(take(document->serialize()) == original && take(assets::read_file(*path)) == source && !document->dirty() &&
                document->undo_count() == 0 && document->redo_count() == 0,
            "CPU profile changed the document or source Scene.");
    take(report.set(report.root(), "status", "pass"));
    const auto bytes = json::write(report.root(), {.pretty = true, .trailing_lf = true});
    return editor::write_file(output, {bytes.data(), bytes.size()}, editor::WriteMode::create);
}
