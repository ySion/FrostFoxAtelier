// This consumer advances the product Application. It never constructs a second
// Renderer or Frame, and calibrates against the renderer's shared pixel range.
// Measurement entry, not acceptance: no timing or rate is a pass condition. The checks below only reject invalid samples.
int profile_editor_render_performance(Application& app, const char* output_directory) {
    using namespace atelier;
    using Access = application::EditorAcceptanceAccess;
    using Clock = std::chrono::steady_clock;
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr(com);
    struct ComScope {
        ~ComScope() { CoUninitialize(); }
    } com_scope;
    const auto output_text = platform::utf8_to_utf16(output_directory);
    require(bool(output_text), "Invalid UTF-8 performance evidence directory.");
    const auto output = std::filesystem::path(*output_text);
    require(output.is_absolute(), "Render performance evidence needs an absolute directory.");
    std::filesystem::create_directories(output);
    const auto project = Access::project_root(app);
    const auto scene = Access::scene_path(app);
    require(!project.empty() && !scene.empty(), "Render performance requires a loaded project Scene.");
    const auto scene_file = project / scene;
    const auto file_hash = foundation::sha256_file(scene_file);
    if (!file_hash)
        throw std::runtime_error(file_hash.error());
    // Lock authoring state before either a product frame or a diagnostic write.
    const auto original_scene = model(app.document().serialize());
    const auto original_world = model(app.document().presentation().environment());
    const auto loaded_environment = [&]() -> json::Document {
        const auto parsed = assets::json_format::parse(original_scene);
        require(parsed.has_value(), "Performance authoring baseline is invalid JSON.");
        const auto entities = parsed->root().at("entities")->array();
        require(entities.has_value(), "Performance authoring baseline lacks entities.");
        for (auto entity : *entities) {
            const auto parts = entity.at("components")->array();
            require(parts.has_value(), "Performance authoring baseline lacks components.");
            for (auto part : *parts)
                if (part.string({"type"}) == "world.EnvironmentComponent") {
                    const auto properties = part.at("properties");
                    return properties ? json::Document{*properties} : json::Document{};
                }
        }
        return json::Document::literal<"null">();
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
        const auto current_hash = foundation::sha256_file(scene_file);
        if (!current_hash)
            throw std::runtime_error(current_hash.error());
        require(*current_hash == *file_hash, "Performance comparison changed the source Scene file.");
    };
    const auto apply_view = [&](const renderer::ViewSettings& candidate) {
        unchanged_author();
        require(static_cast<const rendering::SceneLightingSettings&>(candidate.lit) == original_world,
                "Performance diagnostics must preserve the loaded World author settings.");
        model(Access::performance_view(app, candidate));
        unchanged_author();
    };
    const auto project_text = platform::utf16_to_utf8(project.native());
    const auto scene_text = platform::utf16_to_utf8(scene.native());
    require(project_text && scene_text, "Invalid UTF-16 performance input path.");
    json::Builder report{json::Document::literal<R"({
        "schema":"atelier.editor.render-performance.v7","status":"preparing",
        "wall_semantics":"Intervals between the frame timing ends of consecutive accepted Application steps; include skipped calls, waits and harness work between steps. GPU durations are not FPS.",
        "cpu_semantics":"CPU wall time in milliseconds of current accepted Application steps, read from the product's frame timing record of each step. frame is the whole step; app.* are host phases and app.scene contains app.scene.extract; render contains prepare, encode and frame; encode contains scene (materials, upload, draws, lit) and ui (upload); frame contains submit, present, wait and recycle. A phase sums every entry in the step and entries counts them; frame.unattributed is the frame minus its direct children. In studio modes only, render.encode.scene.draws divided by scene_draw_calls is the mean cost of one recorded scene draw; Scene Lit surface draws count in scene_draw_calls but are timed under render.encode.scene.lit. Do not sum parent and child stages. timing_sequence is the step's frame timing ordinal, separate from completed GPU sequence/frame_id.",
        "resource_semantics":"Current CPU logical Scene Lit bytes, tagged with observation frame; not completed-frame physical VRAM.",
        "warmup_accepted_frames":192,"completed_gpu_samples_per_scene_lit_case":240,
        "non_lit_accepted_steps_per_case":240,"cases":[],"calibration":{},"layouts":{},"authoring_baseline":{}
    })">()};
    const auto root = report.root();
    const auto baseline = *root.at("authoring_baseline");
    require(report.set(root, "revision", build_info::revision) &&
                report.set(root, "configuration", build_info::configuration) &&
                report.set(root, "project", *project_text) && report.set(root, "scene", *scene_text) &&
                report.set(root, "scene_sha256", *file_hash) &&
                report.set(root, "gpu_validation", Access::gpu_validation(app)) &&
                report.set(root, "vsync", Access::vsync(app)) &&
                report.set(baseline, "loaded_environment", loaded_environment.root()) &&
                report.set(baseline, "undo", history) && report.set(baseline, "redo", redo) &&
                report.set(baseline, "revision", revision) && report.set(baseline, "dirty", dirty),
            "Performance report construction failed.");
    const auto write_report = [&] {
        std::ofstream stream(output / "render-performance.json", std::ios::binary | std::ios::trunc);
        const auto bytes = json::write(root, {true, true});
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
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
                    application::diagnostic_text(Access::render_notice(app)) + "\"");
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
            const auto calibration = *report.object(*root.at("calibration"), docked ? "docked" : "maximized");
            require(
                report.set(calibration, "status", "preparing") &&
                    report.numbers(calibration, "target_pixel_extent", std::span<const int>{std::array{2560, 1440}}) &&
                    report.set(calibration, "resize_flags", resize_flags) &&
                    report.numbers(calibration, "system_max_track_extent",
                                   std::span<const int>{
                                       std::array{GetSystemMetrics(SM_CXMAXTRACK), GetSystemMetrics(SM_CYMAXTRACK)}}),
                "Cannot construct calibration report.");
            const auto attempts = *report.array(calibration, "attempts");
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
                    const auto sample = report.append_object(attempts);
                    require(report.set(sample, "attempt", attempt) &&
                                report.set(sample, "accepted_frame", app.presented()->drawing->id) &&
                                report.numbers(sample, "outer_extent", std::span<const LONG>{state.data(), 2}) &&
                                report.numbers(sample, "client_extent",
                                               std::span<const LONG>{std::array{client.right - client.left,
                                                                                client.bottom - client.top}}) &&
                                report.numbers(sample, "frame_extent",
                                               std::span<const unsigned>{std::array{extent.width, extent.height}}) &&
                                report.set(sample, "dpi_scale", app.presented()->drawing->dpi_scale) &&
                                report.numbers(sample, "viewport_rect",
                                               std::span<const float>{
                                                   std::array{bounds.x, bounds.y, bounds.width, bounds.height}}) &&
                                report.numbers(sample, "pixel_center_rect",
                                               std::span<const int>{
                                                   std::array{pixels.left, pixels.top, pixels.right, pixels.bottom}}) &&
                                report.numbers(sample, "pixel_error",
                                               std::span<const int>{std::array{2560 - width, 1440 - height}}),
                            "Cannot construct calibration sample.");
                    if (width == 2560 && height == 1440) {
                        require(bool(report.set(calibration, "status", "pass")), "Cannot record calibration status.");
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
                    require(bool(report.numbers(sample, "requested_outer_extent",
                                                std::span<const LONG>{std::array{next_width, next_height}})),
                            "Cannot record requested extent.");
                    require(next_width > 0 && next_height > 0 && next_width <= 16384 && next_height <= 16384,
                            "Scene View calibration exceeds the 16384-pixel window limit; resolution was not reduced.");
                    const bool resized = SetWindowPos(app.window(), nullptr, 0, 0, static_cast<int>(next_width),
                                                      static_cast<int>(next_height), resize_flags) != 0;
                    const auto resize_error = resized ? ERROR_SUCCESS : GetLastError();
                    require(report.set(sample, "resize_succeeded", resized) &&
                                report.set(sample, "resize_error", std::uint64_t{resize_error}),
                            "Cannot record resize status.");
                    require(GetWindowRect(app.window(), &outer) != 0 && GetClientRect(app.window(), &client) != 0,
                            "Cannot read the resized performance window rectangle.");
                    require(report.numbers(sample, "actual_outer_extent",
                                           std::span<const LONG>{
                                               std::array{outer.right - outer.left, outer.bottom - outer.top}}) &&
                                report.numbers(sample, "actual_client_extent",
                                               std::span<const LONG>{
                                                   std::array{client.right - client.left, client.bottom - client.top}}),
                            "Cannot record actual extent.");
                    require(resized, "Performance window resize failed; see calibration attempts.");
                    require(outer.right - outer.left == next_width && outer.bottom - outer.top == next_height,
                            "Windows constrained the requested calibration size; see requested and actual extents.");
                }
            } catch (const std::exception& error) {
                require(report.set(calibration, "status", "failed") &&
                            report.set(calibration, "error", std::string_view{error.what()}),
                        "Cannot record calibration failure.");
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
        const auto viewport_json = [&](json::Value parent) {
            const auto value = *report.object(parent, "viewport");
            require(
                report.numbers(value, "logical_rect_physical_px",
                               std::span<const float>{std::array{bounds.x, bounds.y, bounds.width, bounds.height}}) &&
                    report.numbers(value, "logical_rect_dip",
                                   std::span<const float>{std::array{bounds.x / dpi, bounds.y / dpi, bounds.width / dpi,
                                                                     bounds.height / dpi}}) &&
                    report.numbers(value, "pixel_center_rect",
                                   std::span<const int>{std::array{pixel_range.left, pixel_range.top, pixel_range.right,
                                                                   pixel_range.bottom}}) &&
                    report.numbers(value, "receiver_pixel_extent",
                                   std::span<const int>{std::array{pixel_range.right - pixel_range.left,
                                                                   pixel_range.bottom - pixel_range.top}}) &&
                    report.numbers(value, "frame_extent",
                                   std::span<const unsigned>{std::array{extent.width, extent.height}}) &&
                    report.set(value, "dpi_scale", dpi),
                "Cannot record viewport.");
        };
        const auto settings_json = [&](json::Value parent, std::string_view key,
                                       const renderer::ViewSettings& settings) {
            const auto values = *report.object(parent, key);
            for (const auto& option : editor::view_options()) {
                const auto value = editor::view_value(settings, option.option);
                if (option.kind == editor::ViewValueKind::boolean)
                    require(bool(report.set(values, option.name, value != 0)), "Cannot record boolean view option.");
                else if (option.kind == editor::ViewValueKind::enumeration)
                    require(bool(report.set(values, option.name, option.choices[static_cast<std::size_t>(value)])),
                            "Cannot record enum view option.");
                else
                    require(bool(report.set(values, option.name, value)), "Cannot record numeric view option.");
            }
        };
        const auto camera_json = [&](json::Value parent) {
            const auto value = *report.object(parent, "camera");
            require(report.set(value, "view", static_cast<unsigned>(camera.view)) &&
                        report.numbers(
                            value, "target",
                            std::span<const float>{std::array{camera.target.x, camera.target.y, camera.target.z}}) &&
                        report.set(value, "yaw", camera.yaw) && report.set(value, "pitch", camera.pitch) &&
                        report.set(value, "distance", camera.distance) &&
                        report.numbers(value, "view_matrix", std::span{accepted_camera.view.elements}) &&
                        report.numbers(value, "projection_matrix", std::span{accepted_camera.projection.elements}) &&
                        report.set(value, "far_depth", accepted_camera.far_depth),
                    "Cannot record camera.");
        };
        const auto active_panes = [&](json::Value parent) {
            const auto panes = *report.array(parent, "active_panes");
            for (const auto& pane : Access::dock_presentation(app).panes)
                if (pane.active) {
                    if (pane.bounds.width <= 0 || pane.bounds.height <= 0)
                        continue;
                    const auto title = platform::utf16_to_utf8(pane.active->title);
                    require(bool(title), "Invalid UTF-16 profile pane title.");
                    const auto item = report.append_object(panes);
                    require(report.set(item, "tab", pane.active->key) && report.set(item, "title", *title) &&
                                report.set(item, "maximized", pane.maximized),
                            "Cannot record active pane.");
                }
            return panes;
        };
        viewport_json(root);
        settings_json(root, "original_view", original_view);
        camera_json(root);
        const auto maximized = *report.object(*root.at("layouts"), layout);
        viewport_json(maximized);
        camera_json(maximized);
        active_panes(maximized);
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
        const auto cold = *report.object(root, "cold_preparation");
        require(report.set(cold, "accepted_frames", prepared) &&
                    report.set(cold, "elapsed_ms",
                               std::chrono::duration<double, std::milli>(cold_end - cold_begin).count()) &&
                    report.set(cold, "logical_resource_bytes", bytes),
                "Cannot record cold preparation.");
        unchanged_scene();
        const auto pass_order = *report.array(root, "gpu_pass_order");
        for (std::size_t i = 0; i < editor::FramePerformance::total_pass; ++i)
            require(bool(report.append(pass_order, editor::FramePerformance::passes()[i].key)),
                    "Cannot record GPU pass order.");
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
            const auto inspection = *report.object(root, "receiver_inspection");
            require(
                report.set(inspection, "sequence", s.scene_lit_gpu_sample_sequence) &&
                    report.set(inspection, "frame_id", s.scene_lit_gpu_sample_frame_id) &&
                    report.set(inspection, "valid_receiver_pixels", receivers) &&
                    report.numbers(inspection, "histogram", std::span{s.scene_lit_direct_receiver_histogram}) &&
                    report.set(inspection, "max_sources", s.scene_lit_direct_receiver_max_sources) &&
                    report.set(
                        inspection, "semantics",
                        "Valid GBuffer receivers covered by analytic local-light support spheres; bins 0..64 exact, 65 >64; excludes sun/emissive meshes and does not test occlusion. Excluded from timed cases."),
                "Cannot record receiver inspection.");
            settings_json(inspection, "view", view);
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
        const auto distribution = [&](json::Value parent, std::string_view key, std::vector<double> values) {
            require(!values.empty(), "Performance distribution has no observations.");
            const auto sum = std::accumulate(values.begin(), values.end(), 0.);
            std::sort(values.begin(), values.end());
            const auto result = *report.object(parent, key);
            require(report.set(result, "count", values.size()) &&
                        report.set(result, "mean", sum / double(values.size())) &&
                        report.set(result, "p50", values[(values.size() * 50 + 99) / 100 - 1]) &&
                        report.set(result, "p95", values[(values.size() * 95 + 99) / 100 - 1]),
                    "Cannot record distribution.");
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
        const auto cpu_json = [&](json::Value parent, const std::vector<CpuSample>& samples) {
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
            const auto result = *report.object(parent, "cpu");
            const auto stage_values = *report.object(result, "stages");
            const auto rows = *report.array(result, "samples");
            for (const auto& sample : samples) {
                const auto row = report.append_object(rows);
                require(report.set(row, "accepted_frame_id", sample.frame_id) &&
                            report.set(row, "timing_sequence", sample.record.sequence) &&
                            report.set(row, "scene_draw_calls", sample.draws[0]) &&
                            report.set(row, "ui_draw_calls", sample.draws[1]) &&
                            report.set(row, "viewport_draw_calls", sample.draws[2]) &&
                            report.set(row, "overlay_draw_calls", sample.draws[3]) &&
                            report.set(row, "scene_instances", sample.scene_instances) &&
                            report.set(row, "scene_culled_instances", sample.scene_culled_instances),
                        "Cannot record CPU sample.");
                const auto entries = *report.object(row, "entries");
                double children{};
                for (std::size_t i = 0; i < entered.size(); ++i) {
                    const auto value = milliseconds(sample.record.time(*entered[i]));
                    stages[i].push_back(value);
                    require(bool(report.set(row, entered[i]->key(), value)), "Cannot record CPU phase duration.");
                    // Entries of the phase in this step; a phase may run once per pass or not at all.
                    require(bool(report.set(entries, entered[i]->key(), sample.record.count(*entered[i]))),
                            "Cannot record CPU phase entries.");
                    if (entered[i]->parent() == &timing::frame)
                        children += value;
                }
                const auto unattributed = std::max(0., milliseconds(sample.record.time(timing::frame)) - children);
                stages.back().push_back(unattributed);
                require(bool(report.set(row, "frame.unattributed", unattributed)), "Cannot record unattributed time.");
            }
            for (std::size_t i = 0; i < entered.size(); ++i)
                distribution(stage_values, entered[i]->key(), std::move(stages[i]));
            distribution(stage_values, "frame.unattributed", std::move(stages.back()));
            // SceneSnapshot also composes inherited selection over every member when something is selected.
            require(bool(report.set(result, "selected_entities", app.document().selected_entities().size())),
                    "Cannot record selection size.");
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
            const auto result = report.append_object(*root.at("cases"));
            require(report.set(result, "name", name) && report.set(result, "layout", layout) &&
                        report.set(result, "warmup_last_frame", fence) &&
                        report.set(result, "accepted_frames", accepted) && report.set(result, "elapsed_ms", elapsed) &&
                        report.set(result, "accepted_steps_per_second", double(accepted) * 1000. / elapsed) &&
                        report.set(result, "gpu_sample_state", "completed") &&
                        report.numbers(result, "wall_present_intervals_ms", std::span{wall}),
                    "Cannot record Lit case.");
            const auto& motion =
                orbit ? json::Document::literal<
                            R"({"yaw_degrees_per_frame":0.2,"yaw_amplitude_degrees":12,"camera_cut":false})">()
                      : json::Document::literal<"null">();
            require(bool(report.set(result, "camera_motion", motion.root())), "Cannot record camera motion.");
            settings_json(result, "view", view);
            viewport_json(result);
            camera_json(result);
            cpu_json(result, cpu);
            distribution(result, "wall_present_interval_ms", wall);
            const auto gpu_passes = *report.object(result, "gpu_passes");
            const auto rows = *report.array(result, "samples");
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
                const auto item = *report.object(gpu_passes, definitions[pass].key);
                distribution(item, "ms", std::move(values));
                require(bool(report.set(item, "active_samples", active)), "Cannot record active samples.");
                require(bool(definitions[pass].parent < 0
                                 ? report.set_null(item, "parent")
                                 : report.set(item, "parent", definitions[definitions[pass].parent].key)),
                        "Cannot record GPU pass parent.");
            }
            for (const auto& sample : samples) {
                totals.push_back(std::accumulate(sample.ms.begin(), sample.ms.begin() + 8, 0.));
                const auto row = report.append_object(rows);
                require(report.set(row, "sequence", sample.sequence) && report.set(row, "frame_id", sample.frame) &&
                            report.set(row, "observed_at_frame", sample.observed_frame) &&
                            report.set(row, "pass_mask", sample.mask) &&
                            report.numbers(row, "pass_ms", std::span{sample.ms}) &&
                            report.set(row, "logical_resource_bytes", sample.resources) &&
                            report.set(row, "probe_rays", sample.probe_rays) &&
                            report.set(row, "shadow_rays", sample.shadow_rays) &&
                            report.numbers(row, "ddgi_visibility_rays", std::span{sample.ddgi_visibility_rays}) &&
                            report.set(row, "reflection_work_items", sample.reflection_work_items),
                        "Cannot record GPU sample.");
                const auto shadow = *report.object(row, "shadow_cache");
                require(report.set(shadow, "faces", sample.shadows[0]) &&
                            report.set(shadow, "reused_faces", sample.shadows[1]) &&
                            report.set(shadow, "static_updates", sample.shadows[2]) &&
                            report.set(shadow, "dynamic_updates", sample.shadows[3]) &&
                            report.set(shadow, "draws", sample.shadows[4]) &&
                            report.set(shadow, "resolution_downgrades", sample.shadows[5]) &&
                            report.set(shadow, "dropped_lights", sample.shadows[6]) &&
                            report.set(shadow, "near_plane_faces", sample.shadows[7]) &&
                            report.set(shadow, "missing_taps", sample.shadows[8]) &&
                            report.set(shadow, "plane_fallbacks", sample.shadows[9]) &&
                            report.set(shadow, "deferred_faces", sample.shadows[10]) &&
                            report.set(shadow, "deferral_frames", sample.shadows[11]) &&
                            report.set(shadow, "atlas_cells", sample.shadows[12]) &&
                            report.set(shadow, "pending_lights", sample.shadows[13]),
                        "Cannot record shadow cache.");
                const auto cluster = *report.object(row, "direct_cluster");
                require(report.set(cluster, "cells", sample.clusters[0]) &&
                            report.set(cluster, "overflows", sample.clusters[1]) &&
                            report.set(cluster, "max_sources", sample.clusters[2]),
                        "Cannot record direct cluster.");
            }
            distribution(result, "measured_main_total_ms", std::move(totals));
            unchanged_scene();
            require(bool(report.set(root, "status", "sampling")), "Cannot record sampling status.");
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
            const auto result = report.append_object(*root.at("cases"));
            require(report.set(result, "name", name) && report.set(result, "layout", layout) &&
                        report.set(result, "warmup_last_frame", fence) && report.set(result, "accepted_frames", 240u) &&
                        report.set(result, "elapsed_ms", elapsed) &&
                        report.set(result, "accepted_steps_per_second", 240000. / elapsed) &&
                        report.numbers(result, "wall_present_intervals_ms", std::span{wall}) &&
                        report.set(result, "gpu_sample_state", "unavailable_non_lit") &&
                        report.set_null(result, "gpu_passes") && report.set_null(result, "measured_main_total_ms") &&
                        report.array(result, "samples"),
                    "Cannot record non-Lit case.");
            settings_json(result, "view", view);
            viewport_json(result);
            camera_json(result);
            cpu_json(result, cpu);
            distribution(result, "wall_present_interval_ms", wall);
            unchanged_scene();
            require(bool(report.set(root, "status", "sampling")), "Cannot record sampling status.");
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
        const auto docked = *report.object(*root.at("layouts"), layout);
        const auto panes = *active_panes(docked).array();
        require(panes.size() > 1, "Dock restoration did not reveal the normal editor panels.");
        for (const auto pane : panes)
            require(!*pane.boolean({"maximized"}), "Dock restoration left a maximized editor panel.");
        viewport_json(docked);
        camera_json(docked);
        non_lit_case(renderer::Shading::smooth, "docked_smooth");
        non_lit_case(renderer::Shading::matcap, "docked_matcap");
        apply_view(original_view);
        restore.active = false;
        unchanged_scene();
        require(bool(report.set(root, "status", "pass")), "Cannot record final status.");
        settings_json(root, "restored_view", Access::scene_lit_view(app));
        write_report();
        app.close();
        std::puts(
            "Editor render performance profile: written (native 1440p, VSync off, maximized Lit/Smooth/MatCap, restored Dock Smooth/MatCap, CPU stage timings)");
        return 0;
    } catch (const std::exception& error) {
        require(report.set(root, "status", "failed") && report.set(root, "error", std::string_view{error.what()}),
                "Cannot record performance failure.");
        write_report();
        throw;
    }
}
