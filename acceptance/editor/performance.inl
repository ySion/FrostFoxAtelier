// This consumer advances the product Application. It never constructs a second
// Renderer or Frame, and calibrates against the renderer's shared pixel range.
// Measurement entry, not acceptance: no timing or rate is a pass condition. The checks below only reject invalid samples.
int profile_editor_render_performance(Application& app, const char* output_directory) {
    using namespace atelier;
    using Access = application::EditorAcceptanceAccess;
    using Json = atelier::control::Json;
    using Clock = std::chrono::steady_clock;
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr(com);
    struct ComScope {
        ~ComScope() { CoUninitialize(); }
    } com_scope;
    const auto output = std::filesystem::path(assets::wide(output_directory));
    require(output.is_absolute(), "Render performance evidence needs an absolute directory.");
    std::filesystem::create_directories(output);
    const auto project = Access::project_root(app);
    const auto scene = Access::scene_path(app);
    require(!project.empty() && !scene.empty(), "Render performance requires a loaded project Scene.");
    const auto scene_file = project / scene;
    const auto file_hash = atelier::control::sha256_file(scene_file);
    // Lock authoring state before either a product frame or a diagnostic write.
    const auto original_scene = model(app.document().serialize());
    const auto original_world = model(app.document().presentation().environment());
    const auto loaded_environment = [&] {
        for (const auto& entity : Json::parse(original_scene).at("entities"))
            for (const auto& part : entity.at("components"))
                if (part.at("type") == "world.EnvironmentComponent")
                    return part.value("properties", Json::object());
        return Json(nullptr);
    }();
    const auto* original_document = &app.document();
    const auto history = app.document().undo_count();
    const auto redo = app.document().redo_count();
    const auto revision = app.document().revision();
    const auto dirty = app.document().dirty();
    const auto unchanged_author = [&] {
        require(&app.document() == original_document && Access::project_root(app) == project &&
                    Access::scene_path(app) == scene && app.document().undo_count() == history &&
                    app.document().redo_count() == redo && app.document().revision() == revision &&
                    app.document().dirty() == dirty &&
                    model(app.document().presentation().environment()) == original_world,
                "Performance preparation or sampling changed the loaded Scene or its authoring history.");
    };
    const auto unchanged_scene = [&] {
        unchanged_author();
        require(model(app.document().serialize()) == original_scene, "Performance comparison changed Scene contents.");
        require(atelier::control::sha256_file(scene_file) == file_hash,
                "Performance comparison changed the source Scene file.");
    };
    const auto apply_view = [&](const renderer::ViewSettings& candidate) {
        unchanged_author();
        require(static_cast<const rendering::SceneLightingSettings&>(candidate.lit) == original_world,
                "Performance diagnostics must preserve the loaded World author settings.");
        model(Access::performance_view(app, candidate));
        unchanged_author();
    };
    Json report{
        {"schema", "atelier.editor.render-performance.v7"},
        {"status", "preparing"},
        {"revision", build_info::revision},
        {"configuration", build_info::configuration},
        {"project", assets::utf8(project.wstring())},
        {"scene", assets::utf8(scene.wstring())},
        {"scene_sha256", file_hash},
        {"authoring_baseline",
         {{"loaded_environment", loaded_environment},
          {"undo", history},
          {"redo", redo},
          {"revision", revision},
          {"dirty", dirty}}},
        {"gpu_validation", Access::gpu_validation(app)},
        {"vsync", Access::vsync(app)},
        {"wall_semantics",
         "Intervals between the frame timing ends of consecutive accepted Application steps; include skipped calls, waits and harness work between steps. GPU durations are not FPS."},
        {"cpu_semantics",
         "CPU wall time in milliseconds of current accepted Application steps, read from the product's frame timing record of each step. frame is the whole step; app.* are host phases and app.scene contains app.scene.extract; render contains prepare, encode and frame; encode contains scene (materials, upload, draws, lit) and ui (upload); frame contains submit, present, wait and recycle. A phase sums every entry in the step and entries counts them; frame.unattributed is the frame minus its direct children. In studio modes only, render.encode.scene.draws divided by scene_draw_calls is the mean cost of one recorded scene draw; Scene Lit surface draws count in scene_draw_calls but are timed under render.encode.scene.lit. Do not sum parent and child stages. timing_sequence is the step's frame timing ordinal, separate from completed GPU sequence/frame_id."},
        {"resource_semantics",
         "Current CPU logical Scene Lit bytes, tagged with observation frame; not completed-frame physical VRAM."},
        {"warmup_accepted_frames", 192},
        {"completed_gpu_samples_per_scene_lit_case", 240},
        {"non_lit_accepted_steps_per_case", 240},
        {"cases", Json::array()}};
    const auto write_report = [&] {
        std::ofstream stream(output / "render-performance.json", std::ios::binary | std::ios::trunc);
        stream << report.dump(2) << '\n';
        stream.flush();
        require(stream.good(), "Cannot write render performance evidence.");
    };
    try {
        require(!Access::vsync(app),
                "Native render performance must disable main-surface VSync at Application startup.");
        require(!dirty && history == 0 && redo == 0, "Render performance requires an unchanged freshly loaded Scene.");
        // Each product step records its own frame; accepted intervals and
        // CPU stages are read from those records, never from a harness clock.
        const auto frame_end = [](const timing::FrameRecord& record) {
            return record.begin + record.time(timing::frame);
        };
        const auto current_group_presented = [&] {
            const auto& snapshot = Access::scene_snapshot(app);
            const auto* draw = snapshot.draw();
            const auto& accepted = Access::accepted_view(app);
            return draw && accepted && accepted->scene && accepted->scene == draw->picking &&
                   draw->picking == snapshot.picking();
        };
        const auto next_frame = [&](Clock::time_point deadline) {
            const auto before = app.accepted_frames();
            // Neither a Lit fallback nor a retained group during material preparation
            // is a frame of the current measured request. Application owns prepare/draw.
            while (app.accepted_frames() == before || Access::fallback_presented(app) || !current_group_presented()) {
                require(Clock::now() < deadline, "Render performance timed out waiting for an accepted product frame.");
                require(app.step(), "Render performance editor closed before completion.");
                unchanged_author();
                // A popup renders its own surface within the step. The profile never opens one, so it came from external input.
                require(
                    !app.menu_window(),
                    "A menu popup opened during measurement (external input); the window must stay untouched while profiling.");
            }
            require(app.presented() && current_group_presented(),
                    "Accepted Scene View does not own the current adopted draw group.");
            const auto& record = app.frame_record();
            if (record.structure_errors != 0 || record.count(renderer::phases::frame) != 1)
                throw std::runtime_error(
                    "Accepted step did not record exactly one renderer frame phase: structure_errors=" +
                    std::to_string(record.structure_errors) +
                    " first_error=" + (record.first_error ? std::string(record.first_error->key()) : "none") +
                    " render=" + std::to_string(record.count(renderer::phases::render)) +
                    " prepare=" + std::to_string(record.count(renderer::phases::prepare)) +
                    " encode=" + std::to_string(record.count(renderer::phases::encode)) +
                    " frame=" + std::to_string(record.count(renderer::phases::frame)) +
                    " accepted_frame=" + std::to_string(app.presented()->drawing->id) + " preparation=\"" +
                    assets::utf8(Access::render_notice(app)) + "\"");
            return frame_end(record);
        };
        // Let the product publish its loaded World into the accepted UI/view.
        // No stale workspace defaults may flow back into Session authoring.
        next_frame(Clock::now() + std::chrono::minutes(5));
        unchanged_scene();
        const auto original_view = Access::scene_lit_view(app);
        require(static_cast<const rendering::SceneLightingSettings&>(original_view.lit) == original_world,
                "Accepted view does not contain the loaded World author settings.");
        struct RestoreView {
            Application& app;

            renderer::ViewSettings view;

            const editor::Session* document;

            bool active{true};

            ~RestoreView() {
                // A failure must never restore old author values over a changed document.
                if (active && &app.document() == document && !app.document().world().closed() &&
                    static_cast<const rendering::SceneLightingSettings&>(view.lit) ==
                        model(app.document().presentation().environment()))
                    static_cast<void>(Access::performance_view(app, view));
            }
        } restore{app, original_view, original_document};
        auto view = original_view;
        view.shading = renderer::Shading::scene_lit;
        view.wireframe = renderer::Wireframe::off;
        view.lit.direct_cluster_inspect = false;
        apply_view(view);
        const auto calibrate = [&](bool docked) {
            // Acceptance may need a client larger than the desktop to retain
            // the Dock ratios. Bypass the default tracking-size clamp, while
            // preserving WM_WINDOWPOSCHANGED/WM_SIZE and product resize work.
            constexpr UINT resize_flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING;
            auto& calibration = report["calibration"][docked ? "docked" : "maximized"];
            calibration = {
                {"status", "preparing"},
                {"target_pixel_extent", {2560, 1440}},
                {"resize_flags", resize_flags},
                {"system_max_track_extent", {GetSystemMetrics(SM_CXMAXTRACK), GetSystemMetrics(SM_CYMAXTRACK)}},
                {"attempts", Json::array()}};
            const auto deadline = Clock::now() + std::chrono::minutes(5);
            std::vector<std::array<LONG, 4>> visited;
            try {
                for (unsigned attempt = 0; attempt <= (docked ? 32u : 8u); ++attempt) {
                    next_frame(deadline);
                    next_frame(deadline);
                    const auto bounds = Access::accepted_view(app)->bounds;
                    const auto pixels = renderer::detail::raster_viewport(bounds).scissor;
                    const int width = pixels.right - pixels.left;
                    const int height = pixels.bottom - pixels.top;
                    RECT outer{};
                    RECT client{};
                    require(GetWindowRect(app.window(), &outer) != 0 && GetClientRect(app.window(), &client) != 0,
                            "Cannot read the performance window rectangle.");
                    const std::array<LONG, 4> state{outer.right - outer.left, outer.bottom - outer.top, width, height};
                    const auto extent = app.presented()->drawing->extent;
                    calibration["attempts"].push_back(
                        {{"attempt", attempt},
                         {"accepted_frame", app.presented()->drawing->id},
                         {"outer_extent", {state[0], state[1]}},
                         {"client_extent", {client.right - client.left, client.bottom - client.top}},
                         {"frame_extent", {extent.width, extent.height}},
                         {"dpi_scale", app.presented()->drawing->dpi_scale},
                         {"viewport_rect", {bounds.x, bounds.y, bounds.width, bounds.height}},
                         {"pixel_center_rect", {pixels.left, pixels.top, pixels.right, pixels.bottom}},
                         {"pixel_error", {2560 - width, 1440 - height}}});
                    auto& sample = calibration["attempts"].back();
                    if (width == 2560 && height == 1440) {
                        calibration["status"] = "pass";
                        return;
                    }
                    require(width > 0 && height > 0, "Scene View has no calibratable pixel extent.");
                    require(std::ranges::find(visited, state) == visited.end(),
                            "Scene View calibration repeated a window/pixel state; see calibration attempts.");
                    visited.push_back(state);
                    require(
                        attempt < (docked ? 32u : 8u),
                        "Scene View pixel-centre coverage could not reach exactly 2560x1440; see calibration attempts.");
                    // Scale coarse corrections by the actual pane/client ratio.
                    // Small steps retain integer sizes near fractional split edges.
                    const auto dx = std::lround(double(2560 - width) * (docked && std::abs(2560 - width) > 2
                                                                            ? double(client.right - client.left) / width
                                                                            : 1.));
                    const auto dy = std::lround(
                        double(1440 - height) *
                        (docked && std::abs(1440 - height) > 2 ? double(client.bottom - client.top) / height : 1.));
                    const auto next_width = state[0] + dx;
                    const auto next_height = state[1] + dy;
                    sample["requested_outer_extent"] = {next_width, next_height};
                    require(next_width > 0 && next_height > 0 && next_width <= 16384 && next_height <= 16384,
                            "Scene View calibration exceeds the 16384-pixel window limit; resolution was not reduced.");
                    const bool resized = SetWindowPos(app.window(), nullptr, 0, 0, static_cast<int>(next_width),
                                                      static_cast<int>(next_height), resize_flags) != 0;
                    const auto resize_error = resized ? ERROR_SUCCESS : GetLastError();
                    sample["resize_succeeded"] = resized;
                    sample["resize_error"] = resize_error;
                    require(GetWindowRect(app.window(), &outer) != 0 && GetClientRect(app.window(), &client) != 0,
                            "Cannot read the resized performance window rectangle.");
                    sample["actual_outer_extent"] = {outer.right - outer.left, outer.bottom - outer.top};
                    sample["actual_client_extent"] = {client.right - client.left, client.bottom - client.top};
                    require(resized, "Performance window resize failed; see calibration attempts.");
                    require(outer.right - outer.left == next_width && outer.bottom - outer.top == next_height,
                            "Windows constrained the requested calibration size; see requested and actual extents.");
                }
            } catch (const std::exception& error) {
                calibration["status"] = "failed";
                calibration["error"] = error.what();
                throw;
            }
        };
        Access::set_scene_maximized(app, true);
        calibrate(false);
        auto bounds = Access::accepted_view(app)->bounds;
        auto extent = app.presented()->drawing->extent;
        auto dpi = app.presented()->drawing->dpi_scale;
        const auto camera = Access::camera_state(app);
        auto accepted_camera = *Access::accepted_view(app);
        auto pixel_range = renderer::detail::raster_viewport(bounds).scissor;
        std::string_view layout = "maximized";
        const auto viewport_json = [&] {
            return Json{
                {"logical_rect_physical_px", {bounds.x, bounds.y, bounds.width, bounds.height}},
                {"logical_rect_dip", {bounds.x / dpi, bounds.y / dpi, bounds.width / dpi, bounds.height / dpi}},
                {"pixel_center_rect", {pixel_range.left, pixel_range.top, pixel_range.right, pixel_range.bottom}},
                {"receiver_pixel_extent", {pixel_range.right - pixel_range.left, pixel_range.bottom - pixel_range.top}},
                {"frame_extent", {extent.width, extent.height}},
                {"dpi_scale", dpi}};
        };
        const auto settings_json = [](const renderer::ViewSettings& settings) {
            Json values = Json::object();
            for (const auto& option : editor::view_options()) {
                const auto value = editor::view_value(settings, option.option);
                if (option.kind == editor::ViewValueKind::boolean)
                    values[option.name] = value != 0;
                else if (option.kind == editor::ViewValueKind::enumeration)
                    values[option.name] = option.choices[static_cast<std::size_t>(value)];
                else
                    values[option.name] = value;
            }
            return values;
        };
        const auto camera_json = [&] {
            return Json{{"view", static_cast<unsigned>(camera.view)},
                        {"target", {camera.target.x, camera.target.y, camera.target.z}},
                        {"yaw", camera.yaw},
                        {"pitch", camera.pitch},
                        {"distance", camera.distance},
                        {"view_matrix", accepted_camera.view.elements},
                        {"projection_matrix", accepted_camera.projection.elements},
                        {"far_depth", accepted_camera.far_depth}};
        };
        const auto active_panes = [&] {
            auto panes = Json::array();
            for (const auto& pane : Access::dock_presentation(app).panes)
                if (pane.active) {
                    if (pane.bounds.width <= 0 || pane.bounds.height <= 0)
                        continue;
                    panes.push_back({{"tab", pane.active->key},
                                     {"title", assets::utf8(pane.active->title)},
                                     {"maximized", pane.maximized}});
                }
            return panes;
        };
        report["viewport"] = viewport_json();
        report["original_view"] = settings_json(original_view);
        report["camera"] = camera_json();
        report["layouts"][layout] = {
            {"viewport", viewport_json()}, {"camera", camera_json()}, {"active_panes", active_panes()}};
        bool camera_fixed = true;
        const auto invariant = [&] {
            require(Access::accepted_view(app) && Access::accepted_view(app)->bounds == bounds &&
                        app.presented()->drawing->extent == extent && app.presented()->drawing->dpi_scale == dpi &&
                        (!camera_fixed || Access::camera_state(app) == camera),
                    "Viewport, render target, DPI or camera changed during the performance comparison.");
            const auto range = renderer::detail::raster_viewport(Access::accepted_view(app)->bounds).scissor;
            require(range.right - range.left == 2560 && range.bottom - range.top == 1440,
                    "Actual receiver dispatch pixel coverage changed from native 1440p.");
            require(Access::scene_lit_view(app) == view,
                    "View or World settings changed during the performance comparison.");
        };
        // The orbit case moves continuously: a triangle wave of .2 degrees of
        // yaw per accepted frame within +-12 degrees, without camera cuts.
        unsigned orbit_frame = 0;
        const auto orbit_step = [&] {
            if (camera_fixed)
                return;
            const auto phase = orbit_frame++ % 240;
            auto state = camera;
            state.yaw += (phase < 120 ? float(phase) - 60.f : 180.f - float(phase)) * .2f * .0174532925f;
            require(Access::move_camera(app, state), "The orbit camera state was rejected.");
        };
        const auto warmup = [&](unsigned frames) {
            const auto deadline = Clock::now() + std::chrono::minutes(10);
            for (unsigned i = 0; i < frames; ++i) {
                orbit_step();
                next_frame(deadline);
                invariant();
            }
            return app.presented()->drawing->id;
        };
        // Finish cold uploads and obtain a real completed sample before the two
        // timed cases. The fixed case warmup is additional to this preparation.
        std::uint64_t bytes = 0;
        unsigned stable = 0;
        unsigned prepared = 0;
        const auto cold_deadline = Clock::now() + std::chrono::minutes(5);
        Clock::time_point cold_begin{};
        Clock::time_point cold_end{};
        while (stable < 16 || prepared < 32 || !app.frame_statistics().scene_lit_gpu_timing_valid) {
            cold_end = next_frame(cold_deadline);
            invariant();
            ++prepared;
            if (prepared == 1)
                cold_begin = app.frame_record().begin;
            const auto& stats = app.frame_statistics();
            stable = bytes == stats.scene_lit_resource_bytes && !stats.material_upload_count ? stable + 1 : 0;
            bytes = stats.scene_lit_resource_bytes;
        }
        require(app.frame_statistics().scene_draw_calls > 0, "The selected Scene contains no rendered geometry.");
        report["cold_preparation"] = {
            {"accepted_frames", prepared},
            {"elapsed_ms", std::chrono::duration<double, std::milli>(cold_end - cold_begin).count()},
            {"logical_resource_bytes", bytes}};
        unchanged_scene();
        report["gpu_pass_order"] = Json::array();
        for (std::size_t i = 0; i < editor::FramePerformance::total_pass; ++i)
            report["gpu_pass_order"].push_back(editor::FramePerformance::passes()[i].key);
        // Obtain receiver counts separately. Diagnostic work is absent from
        // both measured paths, including their subsequent 192-frame warmups.
        view.lit.direct_cluster_inspect = true;
        apply_view(view);
        next_frame(Clock::now() + std::chrono::minutes(5));
        invariant();
        const auto diagnostic_first = app.presented()->drawing->id;
        const auto diagnostic_deadline = Clock::now() + std::chrono::minutes(5);
        for (;;) {
            next_frame(diagnostic_deadline);
            invariant();
            const auto& s = app.frame_statistics();
            if (!s.scene_lit_gpu_timing_valid || s.scene_lit_gpu_sample_frame_id < diagnostic_first)
                continue;
            require((s.scene_lit_gpu_pass_mask & (1u << 12)) != 0,
                    "Receiver inspection did not execute the light list pass.");
            const auto receivers = std::accumulate(s.scene_lit_direct_receiver_histogram.begin(),
                                                   s.scene_lit_direct_receiver_histogram.end(), std::uint64_t{});
            require(receivers > 0 && receivers <= 2560ull * 1440,
                    "Receiver histogram did not cover valid visible Scene pixels.");
            report["receiver_inspection"] = {
                {"sequence", s.scene_lit_gpu_sample_sequence},
                {"frame_id", s.scene_lit_gpu_sample_frame_id},
                {"view", settings_json(view)},
                {"valid_receiver_pixels", receivers},
                {"histogram", s.scene_lit_direct_receiver_histogram},
                {"max_sources", s.scene_lit_direct_receiver_max_sources},
                {"semantics",
                 "Valid GBuffer receivers covered by analytic local-light support spheres; bins 0..64 exact, 65 >64; excludes sun/emissive meshes and does not test occlusion. Excluded from timed cases."}};
            break;
        }
        view.lit.direct_cluster_inspect = false;
        struct Sample {
            std::uint64_t sequence{};

            std::uint64_t frame{};

            std::uint64_t observed_frame{};

            std::uint64_t resources{};

            std::uint32_t mask{};

            std::array<std::uint32_t, 3> clusters{};

            std::array<float, editor::FramePerformance::total_pass> ms{};

            std::array<std::uint32_t, 14> shadows{};

            std::uint64_t probe_rays{};

            std::uint64_t shadow_rays{};

            std::array<std::uint64_t, 2> ddgi_visibility_rays{};

            std::uint32_t reflection_work_items{};
        };
        const auto distribution = [](std::vector<double> values) {
            require(!values.empty(), "Performance distribution has no observations.");
            const auto sum = std::accumulate(values.begin(), values.end(), 0.);
            std::sort(values.begin(), values.end());
            return Json{{"count", values.size()},
                        {"mean", sum / double(values.size())},
                        {"p50", values[(values.size() * 50 + 99) / 100 - 1]},
                        {"p95", values[(values.size() * 95 + 99) / 100 - 1]}};
        };
        struct CpuSample {
            std::uint64_t frame_id{};

            std::array<std::uint64_t, 4> draws{};

            std::uint64_t scene_instances{};

            std::uint64_t scene_culled_instances{};

            timing::FrameRecord record;
        };
        // Called right after next_frame, so the last step is the accepted one.
        const auto collect_cpu = [&](std::vector<CpuSample>& samples) {
            const auto& stats = app.frame_statistics();
            samples.push_back(
                {app.presented()->drawing->id,
                 {stats.scene_draw_calls, stats.ui_draw_calls, stats.viewport_draw_calls, stats.overlay_draw_calls},
                 stats.scene_instances,
                 stats.scene_culled_instances,
                 app.frame_record()});
        };
        const auto cpu_json = [&](const std::vector<CpuSample>& samples) {
            const auto milliseconds = [](std::chrono::nanoseconds value) {
                return std::chrono::duration<double, std::milli>(value).count();
            };
            // Phases entered in any of these steps, in catalog order; the last stage is the unattributed rest of the frame.
            std::vector<const timing::Marker*> entered;
            for (const auto* marker : timing::catalog())
                if (std::ranges::any_of(samples, [&](const auto& sample) { return sample.record.count(*marker) != 0; }))
                    entered.push_back(marker);
            std::vector<std::vector<double>> stages(entered.size() + 1);
            for (auto& stage : stages)
                stage.reserve(samples.size());
            Json result{{"stages", Json::object()}, {"samples", Json::array()}};
            for (const auto& sample : samples) {
                Json row{{"accepted_frame_id", sample.frame_id},
                         {"timing_sequence", sample.record.sequence},
                         {"scene_draw_calls", sample.draws[0]},
                         {"ui_draw_calls", sample.draws[1]},
                         {"viewport_draw_calls", sample.draws[2]},
                         {"overlay_draw_calls", sample.draws[3]},
                         {"scene_instances", sample.scene_instances},
                         {"scene_culled_instances", sample.scene_culled_instances}};
                double children{};
                for (std::size_t i = 0; i < entered.size(); ++i) {
                    const auto value = milliseconds(sample.record.time(*entered[i]));
                    stages[i].push_back(value);
                    row[std::string(entered[i]->key())] = value;
                    // Entries of the phase in this step; a phase may run once per pass or not at all.
                    row["entries"][std::string(entered[i]->key())] = sample.record.count(*entered[i]);
                    if (entered[i]->parent() == &timing::frame)
                        children += value;
                }
                const auto unattributed = std::max(0., milliseconds(sample.record.time(timing::frame)) - children);
                stages.back().push_back(unattributed);
                row["frame.unattributed"] = unattributed;
                result["samples"].push_back(std::move(row));
            }
            for (std::size_t i = 0; i < entered.size(); ++i)
                result["stages"][std::string(entered[i]->key())] = distribution(std::move(stages[i]));
            result["stages"]["frame.unattributed"] = distribution(std::move(stages.back()));
            // SceneSnapshot also composes inherited selection over every member when something is selected.
            result["selected_entities"] = app.document().selected_entities().size();
            return result;
        };
        for (const bool orbit : {false, true}) {
            apply_view(view);
            camera_fixed = !orbit;
            const auto fence = warmup(192);
            std::vector<Sample> samples;
            samples.reserve(240);
            std::vector<double> wall;
            wall.reserve(512);
            std::vector<CpuSample> cpu;
            cpu.reserve(512);
            std::uint64_t sequence = 0;
            unsigned accepted = 0;
            auto previous = frame_end(app.frame_record());
            const auto measurement_start = previous;
            const auto deadline = Clock::now() + std::chrono::minutes(10);
            while (samples.size() < 240) {
                orbit_step();
                const auto presented = next_frame(deadline);
                invariant();
                wall.push_back(std::chrono::duration<double, std::milli>(presented - previous).count());
                previous = presented;
                ++accepted;
                collect_cpu(cpu);
                const auto& s = app.frame_statistics();
                const auto observed = app.presented()->drawing->id;
                if (!s.scene_lit_gpu_timing_valid || s.scene_lit_gpu_sample_frame_id <= fence ||
                    s.scene_lit_gpu_sample_sequence <= sequence)
                    continue;
                require(s.scene_lit_gpu_sample_frame_id <= observed, "Completed GPU sample refers to a future frame.");
                require(s.scene_lit_direct_receiver_max_sources == 0 &&
                            std::ranges::all_of(s.scene_lit_direct_receiver_histogram, [](auto n) { return n == 0; }),
                        "Receiver diagnostics leaked into timed samples.");
                Sample sample{s.scene_lit_gpu_sample_sequence,
                              s.scene_lit_gpu_sample_frame_id,
                              observed,
                              s.scene_lit_resource_bytes,
                              s.scene_lit_gpu_pass_mask,
                              {s.scene_lit_direct_cluster_cells, s.scene_lit_direct_cluster_overflows,
                               s.scene_lit_direct_cluster_max_sources},
                              {}};
                std::copy(s.scene_lit_gpu_ms.begin(), s.scene_lit_gpu_ms.end(), sample.ms.begin());
                std::copy(s.scene_lit_ddgi_gpu_ms.begin(), s.scene_lit_ddgi_gpu_ms.end(), sample.ms.begin() + 8);
                std::copy(s.scene_lit_ddgi_update_gpu_ms.begin(), s.scene_lit_ddgi_update_gpu_ms.end(),
                          sample.ms.begin() + 10);
                sample.ms[12] = s.scene_lit_direct_cluster_gpu_ms;
                sample.ms[13] = s.scene_lit_shadow_cache_gpu_ms;
                sample.ms[14] = s.scene_lit_direct_shading_gpu_ms;
                sample.probe_rays = s.scene_lit_probe_rays;
                sample.shadow_rays = s.scene_lit_shadow_rays;
                sample.ddgi_visibility_rays = s.scene_lit_ddgi_visibility_rays;
                sample.reflection_work_items = s.scene_lit_reflection_work_items;
                sample.shadows = {
                    s.scene_lit_shadow_cache_faces,          s.scene_lit_shadow_cache_reused_faces,
                    s.scene_lit_shadow_cache_static_updates, s.scene_lit_shadow_cache_dynamic_updates,
                    s.scene_lit_shadow_cache_draws,          s.scene_lit_shadow_cache_resolution_downgrades,
                    s.scene_lit_shadow_cache_dropped_lights, s.scene_lit_shadow_cache_near_plane_faces,
                    s.scene_lit_shadow_cache_missing_taps,   s.scene_lit_shadow_cache_plane_fallbacks,
                    s.scene_lit_shadow_cache_deferred_faces, s.scene_lit_shadow_cache_deferral_frames,
                    s.scene_lit_shadow_cache_atlas_cells,    s.scene_lit_shadow_cache_pending_lights};
                require(std::ranges::all_of(sample.ms, [](auto n) { return std::isfinite(n) && n >= 0; }),
                        "Invalid completed GPU duration.");
                samples.push_back(sample);
                sequence = sample.sequence;
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(previous - measurement_start).count();
            const std::string name = orbit ? "clustered_orbit" : "clustered";
            Json result{{"name", name},
                        {"layout", layout},
                        {"camera_motion",
                         orbit
                             ? Json{{"yaw_degrees_per_frame", .2}, {"yaw_amplitude_degrees", 12}, {"camera_cut", false}}
                             : Json(nullptr)},
                        {"view", settings_json(view)},
                        {"viewport", viewport_json()},
                        {"camera", camera_json()},
                        {"warmup_last_frame", fence},
                        {"accepted_frames", accepted},
                        {"elapsed_ms", elapsed},
                        {"accepted_steps_per_second", double(accepted) * 1000. / elapsed},
                        {"gpu_sample_state", "completed"},
                        {"cpu", cpu_json(cpu)},
                        {"wall_present_interval_ms", distribution(wall)},
                        {"wall_present_intervals_ms", wall},
                        {"gpu_passes", Json::object()},
                        {"samples", Json::array()}};
            std::vector<double> totals;
            totals.reserve(samples.size());
            const auto definitions = editor::FramePerformance::passes();
            for (std::size_t pass = 0; pass < editor::FramePerformance::total_pass; ++pass) {
                std::vector<double> values;
                values.reserve(samples.size());
                unsigned active = 0;
                for (const auto& sample : samples) {
                    values.push_back(sample.ms[pass]);
                    active += (sample.mask & (1u << pass)) != 0;
                }
                result["gpu_passes"][definitions[pass].key] = {
                    {"ms", distribution(std::move(values))},
                    {"active_samples", active},
                    {"parent",
                     definitions[pass].parent < 0 ? Json(nullptr) : Json(definitions[definitions[pass].parent].key)}};
            }
            for (const auto& sample : samples) {
                totals.push_back(std::accumulate(sample.ms.begin(), sample.ms.begin() + 8, 0.));
                result["samples"].push_back({{"sequence", sample.sequence},
                                             {"frame_id", sample.frame},
                                             {"observed_at_frame", sample.observed_frame},
                                             {"pass_mask", sample.mask},
                                             {"pass_ms", sample.ms},
                                             {"logical_resource_bytes", sample.resources},
                                             {"probe_rays", sample.probe_rays},
                                             {"shadow_rays", sample.shadow_rays},
                                             {"ddgi_visibility_rays", sample.ddgi_visibility_rays},
                                             {"reflection_work_items", sample.reflection_work_items},
                                             {"shadow_cache",
                                              {{"faces", sample.shadows[0]},
                                               {"reused_faces", sample.shadows[1]},
                                               {"static_updates", sample.shadows[2]},
                                               {"dynamic_updates", sample.shadows[3]},
                                               {"draws", sample.shadows[4]},
                                               {"resolution_downgrades", sample.shadows[5]},
                                               {"dropped_lights", sample.shadows[6]},
                                               {"near_plane_faces", sample.shadows[7]},
                                               {"missing_taps", sample.shadows[8]},
                                               {"plane_fallbacks", sample.shadows[9]},
                                               {"deferred_faces", sample.shadows[10]},
                                               {"deferral_frames", sample.shadows[11]},
                                               {"atlas_cells", sample.shadows[12]},
                                               {"pending_lights", sample.shadows[13]}}},
                                             {"direct_cluster",
                                              {{"cells", sample.clusters[0]},
                                               {"overflows", sample.clusters[1]},
                                               {"max_sources", sample.clusters[2]}}}});
            }
            result["measured_main_total_ms"] = distribution(std::move(totals));
            unchanged_scene();
            report["cases"].push_back(std::move(result));
            report["status"] = "sampling";
            write_report();
            capture(app, output / (name + ".png"));
        }
        camera_fixed = true;
        require(Access::move_camera(app, camera), "The comparison camera could not be restored.");
        const auto non_lit_case = [&](renderer::Shading shading, std::string_view name) {
            view.shading = shading;
            apply_view(view);
            const auto fence = warmup(192);
            std::vector<double> wall;
            wall.reserve(240);
            std::vector<CpuSample> cpu;
            cpu.reserve(240);
            auto previous = frame_end(app.frame_record());
            const auto measurement_start = previous;
            const auto deadline = Clock::now() + std::chrono::minutes(10);
            for (unsigned accepted = 0; accepted < 240; ++accepted) {
                const auto presented = next_frame(deadline);
                invariant();
                wall.push_back(std::chrono::duration<double, std::milli>(presented - previous).count());
                previous = presented;
                collect_cpu(cpu);
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(previous - measurement_start).count();
            // Non-Lit modes have no Scene Lit GPU scopes. Never reuse the last
            // completed Lit sample or infer GPU time from accepted step rate.
            Json result{{"name", name},
                        {"layout", layout},
                        {"view", settings_json(view)},
                        {"viewport", viewport_json()},
                        {"camera", camera_json()},
                        {"warmup_last_frame", fence},
                        {"accepted_frames", 240},
                        {"elapsed_ms", elapsed},
                        {"accepted_steps_per_second", 240000. / elapsed},
                        {"wall_present_interval_ms", distribution(wall)},
                        {"wall_present_intervals_ms", wall},
                        {"cpu", cpu_json(cpu)},
                        {"gpu_sample_state", "unavailable_non_lit"},
                        {"gpu_passes", nullptr},
                        {"measured_main_total_ms", nullptr},
                        {"samples", Json::array()}};
            unchanged_scene();
            report["cases"].push_back(std::move(result));
            report["status"] = "sampling";
            write_report();
            capture(app, output / (std::string(name) + ".png"));
        };
        non_lit_case(renderer::Shading::smooth, "smooth");
        non_lit_case(renderer::Shading::matcap, "matcap");
        // Restore the retained Dock tree/ratios, not a freshly reset layout.
        Access::set_scene_maximized(app, false);
        calibrate(true);
        require(Access::camera_state(app) == camera, "Dock restoration changed the comparison camera.");
        require(app.presented()->drawing->dpi_scale == dpi, "Dock restoration changed the comparison DPI.");
        layout = "docked";
        bounds = Access::accepted_view(app)->bounds;
        extent = app.presented()->drawing->extent;
        dpi = app.presented()->drawing->dpi_scale;
        accepted_camera = *Access::accepted_view(app);
        pixel_range = renderer::detail::raster_viewport(bounds).scissor;
        const auto panes = active_panes();
        require(
            panes.size() > 1 &&
                std::ranges::none_of(panes, [](const auto& pane) { return pane.at("maximized").template get<bool>(); }),
            "Dock restoration did not reveal the normal editor panels.");
        report["layouts"][layout] = {{"viewport", viewport_json()}, {"camera", camera_json()}, {"active_panes", panes}};
        non_lit_case(renderer::Shading::smooth, "docked_smooth");
        non_lit_case(renderer::Shading::matcap, "docked_matcap");
        apply_view(original_view);
        restore.active = false;
        unchanged_scene();
        report["status"] = "pass";
        report["restored_view"] = settings_json(Access::scene_lit_view(app));
        write_report();
        app.close();
        std::puts(
            "Editor render performance profile: written (native 1440p, VSync off, maximized Lit/Smooth/MatCap, restored Dock Smooth/MatCap, CPU stage timings)");
        return 0;
    } catch (const std::exception& error) {
        report["status"] = "failed";
        report["error"] = error.what();
        write_report();
        throw;
    }
}
