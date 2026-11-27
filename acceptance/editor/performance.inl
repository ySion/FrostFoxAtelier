// This consumer advances the product Application. It never constructs a second
// Renderer or Frame, and calibrates against the renderer's shared pixel range.
// Measurement entry, not acceptance: no timing or rate is a pass condition. The checks below only reject invalid samples.
atelier::Result<void> profile_editor_render_performance(Application& app, const std::filesystem::path& output) {
    using namespace atelier;
    using Access = application::EditorAcceptanceAccess;
    using Clock = std::chrono::steady_clock;
    ImageApartment apartment;
    ATELIER_TRY(apartment.ready());
    ATELIER_TRY(prepare_output(output));
    const auto report_error = [](const json::Error& error) {
        return Error{ErrorCode::invalid_definition, error.describe()};
    };
    const auto project = Access::project_root(app);
    const auto scene = Access::scene_path(app);
    if (!(!project.empty() && !scene.empty()))
        return command_error("Render performance requires a loaded project Scene.");
    const auto scene_file = project / scene;
    const auto file_hash = foundation::sha256_file(scene_file);
    if (!file_hash)
        return command_error(file_hash.error(), ErrorCode::invalid_storage);
    // Lock authoring state before either a product frame or a diagnostic write.
    ATELIER_TRY_BIND(original_scene, app.document().serialize());
    ATELIER_TRY_BIND(original_world, app.document().presentation().environment());
    const auto environment_fields = [&]() -> json::Result<json::Document> {
        ATELIER_TRY_BIND(parsed, assets::json_format::parse(*original_scene));
        ATELIER_TRY_BIND(entity_list, parsed->root().at("entities"));
        ATELIER_TRY_BIND(entities, entity_list->array());
        for (auto entity : *entities) {
            ATELIER_TRY_BIND(component_list, entity.at("components"));
            ATELIER_TRY_BIND(parts, component_list->array());
            for (auto part : *parts) {
                ATELIER_TRY_BIND(type, part.string({"type"}));
                if (*type == "world.EnvironmentComponent") {
                    ATELIER_TRY_BIND(properties, part.find("properties"));
                    return *properties ? json::Document{**properties} : json::Document{};
                }
            }
        }
        return json::Document::literal<"null">();
    };
    ATELIER_TRY_BIND(loaded_environment, environment_fields().transform_error(report_error));
    const auto* original_document = &app.document();
    const auto history = app.document().undo_count();
    const auto redo = app.document().redo_count();
    const auto revision = app.document().revision();
    const auto dirty = app.document().dirty();
    const auto unchanged_author = [&]() -> Result<void> {
        ATELIER_TRY_BIND(environment, app.document().presentation().environment());
        if (!(&app.document() == original_document && Access::project_root(app) == project &&
              Access::scene_path(app) == scene && app.document().undo_count() == history &&
              app.document().redo_count() == redo && app.document().revision() == revision &&
              app.document().dirty() == dirty && *environment == *original_world))
            return command_error(
                "Performance preparation or sampling changed the loaded Scene or its authoring history.");
        return {};
    };
    const auto unchanged_scene = [&]() -> Result<void> {
        ATELIER_TRY(unchanged_author());
        ATELIER_TRY_BIND(current, app.document().serialize());
        if (*current != *original_scene)
            return command_error("Performance comparison changed Scene contents.");
        const auto current_hash = foundation::sha256_file(scene_file);
        if (!current_hash)
            return command_error(current_hash.error(), ErrorCode::invalid_storage);
        if (*current_hash != *file_hash)
            return command_error("Performance comparison changed the source Scene file.");
        return {};
    };
    const auto apply_view = [&](const renderer::ViewSettings& candidate) -> Result<void> {
        ATELIER_TRY(unchanged_author());
        if (static_cast<const rendering::SceneLightingSettings&>(candidate.lit) != *original_world)
            return command_error("Performance diagnostics must preserve the loaded World author settings.");
        ATELIER_TRY(Access::performance_view(app, candidate));
        ATELIER_TRY(unchanged_author());
        return {};
    };
    const auto project_text = platform::utf16_to_utf8(project.native());
    const auto scene_text = platform::utf16_to_utf8(scene.native());
    if (!(project_text && scene_text))
        return command_error("Invalid UTF-16 performance input path.");
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
    ATELIER_TRY(report.set(root, "revision", build_info::revision).transform_error(report_error));
    ATELIER_TRY(report.set(root, "configuration", build_info::configuration).transform_error(report_error));
    ATELIER_TRY(report.set(root, "project", *project_text).transform_error(report_error));
    ATELIER_TRY(report.set(root, "scene", *scene_text).transform_error(report_error));
    ATELIER_TRY(report.set(root, "scene_sha256", *file_hash).transform_error(report_error));
    ATELIER_TRY(report.set(root, "gpu_validation", Access::gpu_validation(app)).transform_error(report_error));
    ATELIER_TRY(report.set(root, "vsync", Access::vsync(app)).transform_error(report_error));
    ATELIER_TRY(report.set(baseline, "loaded_environment", loaded_environment->root()).transform_error(report_error));
    ATELIER_TRY(report.set(baseline, "undo", history).transform_error(report_error));
    ATELIER_TRY(report.set(baseline, "redo", redo).transform_error(report_error));
    ATELIER_TRY(report.set(baseline, "revision", revision).transform_error(report_error));
    ATELIER_TRY(report.set(baseline, "dirty", dirty).transform_error(report_error));
    const auto write_report = [&] {
        const auto bytes = json::write(root, {true, true});
        return editor::write_file(output / "render-performance.json", {bytes.data(), bytes.size()});
    };
    ATELIER_TRY(write_report());
    const auto sample = [&]() -> Result<void> {
        if (Access::vsync(app))
            return command_error("Native render performance must disable main-surface VSync at Application startup.");
        if (!(!dirty && history == 0 && redo == 0))
            return command_error("Render performance requires an unchanged freshly loaded Scene.");
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
        const auto next_frame = [&](Clock::time_point deadline) -> Result<Clock::time_point> {
            const auto before = app.accepted_frames();
            // Neither a Lit fallback nor a retained group during material preparation
            // is a frame of the current measured request. Application owns prepare/draw.
            while (app.accepted_frames() == before || Access::fallback_presented(app) || !current_group_presented()) {
                if (Clock::now() >= deadline)
                    return command_error("Render performance timed out waiting for an accepted product frame.");
                ATELIER_TRY(advance(app, "Render performance editor closed before completion."));
                ATELIER_TRY(unchanged_author());
                // A popup renders its own surface within the step. The profile never opens one, so it came from external input.
                if (app.menu_window())
                    return command_error(
                        "A menu popup opened during measurement (external input); the window must stay untouched while profiling.");
            }
            if (!(app.presented() && current_group_presented()))
                return command_error("Accepted Scene View does not own the current adopted draw group.");
            const auto& record = app.frame_record();
            if (record.structure_errors != 0 || record.count(renderer::phases::frame) != 1)
                return command_error(
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
        ATELIER_TRY(next_frame(Clock::now() + std::chrono::minutes(5)));
        ATELIER_TRY(unchanged_scene());
        const auto original_view = Access::scene_lit_view(app);
        if (static_cast<const rendering::SceneLightingSettings&>(original_view.lit) != *original_world)
            return command_error("Accepted view does not contain the loaded World author settings.");
        struct RestoreView {
            Application& app;

            renderer::ViewSettings view;

            const editor::Session* document;

            bool active{true};

            ~RestoreView() {
                // A failure must never restore old author values over a changed document.
                if (!active || &app.document() != document || app.document().world().closed())
                    return;
                const auto environment = app.document().presentation().environment();
                if (environment && static_cast<const rendering::SceneLightingSettings&>(view.lit) == *environment)
                    static_cast<void>(Access::performance_view(app, view));
            }
        } restore{app, original_view, original_document};
        auto view = original_view;
        view.shading = renderer::Shading::scene_lit;
        view.wireframe = renderer::Wireframe::off;
        view.lit.direct_cluster_inspect = false;
        ATELIER_TRY(apply_view(view));
        const auto calibrate = [&](bool docked) -> Result<void> {
            // Acceptance may need a client larger than the desktop to retain
            // the Dock ratios. Bypass the default tracking-size clamp, while
            // preserving WM_WINDOWPOSCHANGED/WM_SIZE and product resize work.
            constexpr UINT resize_flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING;
            const auto report_numbers = [&](json::Value node, std::string_view key, const auto& values) {
                return report.numbers(node, key, Span{values});
            };
            const auto calibration = report.object_validated(*root.at("calibration"), docked ? "docked" : "maximized");
            ATELIER_TRY(report.set(calibration, "status", "preparing").transform_error(report_error));
            ATELIER_TRY(report_numbers(calibration, "target_pixel_extent", std::array{2560, 1440})
                            .transform_error(report_error));
            ATELIER_TRY(report.set(calibration, "resize_flags", resize_flags).transform_error(report_error));
            ATELIER_TRY(report_numbers(calibration, "system_max_track_extent",
                                       std::array{GetSystemMetrics(SM_CXMAXTRACK), GetSystemMetrics(SM_CYMAXTRACK)})
                            .transform_error(report_error));
            const auto attempts = *report.array(calibration, "attempts");
            const auto deadline = Clock::now() + std::chrono::minutes(5);
            Array<std::array<LONG, 4>> visited;
            const auto resize = [&]() -> Result<void> {
                for (unsigned attempt = 0; attempt <= (docked ? 32u : 8u); ++attempt) {
                    ATELIER_TRY(next_frame(deadline));
                    ATELIER_TRY(next_frame(deadline));
                    const auto bounds = Access::accepted_view(app)->bounds;
                    const auto pixels = renderer::detail::raster_viewport(bounds).scissor;
                    const int width = pixels.right - pixels.left;
                    const int height = pixels.bottom - pixels.top;
                    RECT outer{};
                    RECT client{};
                    if (!(GetWindowRect(app.window(), &outer) != 0 && GetClientRect(app.window(), &client) != 0))
                        return command_error("Cannot read the performance window rectangle.");
                    const std::array<LONG, 4> state{outer.right - outer.left, outer.bottom - outer.top, width, height};
                    const auto extent = app.presented()->drawing->extent;
                    const auto sample = report.append_object(attempts);
                    ATELIER_TRY(report.set(sample, "attempt", attempt).transform_error(report_error));
                    ATELIER_TRY(report.set(sample, "accepted_frame", app.presented()->drawing->id)
                                    .transform_error(report_error));
                    ATELIER_TRY(report.numbers(sample, "outer_extent", Span<const LONG>{state.data(), 2})
                                    .transform_error(report_error));
                    ATELIER_TRY(report_numbers(sample, "client_extent",
                                               std::array{client.right - client.left, client.bottom - client.top})
                                    .transform_error(report_error));
                    ATELIER_TRY(report_numbers(sample, "frame_extent", std::array{extent.width, extent.height})
                                    .transform_error(report_error));
                    ATELIER_TRY(report.set(sample, "dpi_scale", app.presented()->drawing->dpi_scale)
                                    .transform_error(report_error));
                    ATELIER_TRY(report_numbers(sample, "viewport_rect",
                                               std::array{bounds.x, bounds.y, bounds.width, bounds.height})
                                    .transform_error(report_error));
                    ATELIER_TRY(report_numbers(sample, "pixel_center_rect",
                                               std::array{pixels.left, pixels.top, pixels.right, pixels.bottom})
                                    .transform_error(report_error));
                    ATELIER_TRY(report_numbers(sample, "pixel_error", std::array{2560 - width, 1440 - height})
                                    .transform_error(report_error));
                    if (width == 2560 && height == 1440) {
                        ATELIER_TRY(report.set(calibration, "status", "pass").transform_error(report_error));
                        return {};
                    }
                    if (!(width > 0 && height > 0))
                        return command_error("Scene View has no calibratable pixel extent.");
                    if (std::ranges::find(visited, state) != visited.end())
                        return command_error(
                            "Scene View calibration repeated a window/pixel state; see calibration attempts.");
                    visited.add(state);
                    if (!(attempt < (docked ? 32u : 8u)))
                        return command_error(
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
                    ATELIER_TRY(report_numbers(sample, "requested_outer_extent", std::array{next_width, next_height})
                                    .transform_error(report_error));
                    if (!(next_width > 0 && next_height > 0 && next_width <= 16384 && next_height <= 16384))
                        return command_error(
                            "Scene View calibration exceeds the 16384-pixel window limit; resolution was not reduced.");
                    const bool resized = SetWindowPos(app.window(), nullptr, 0, 0, static_cast<int>(next_width),
                                                      static_cast<int>(next_height), resize_flags) != 0;
                    const auto resize_error = resized ? ERROR_SUCCESS : GetLastError();
                    ATELIER_TRY(report.set(sample, "resize_succeeded", resized).transform_error(report_error));
                    ATELIER_TRY(
                        report.set(sample, "resize_error", std::uint64_t{resize_error}).transform_error(report_error));
                    if (!resized)
                        return command_error(
                            "Performance window resize failed: " +
                            platform::error_message(std::error_code(resize_error, std::system_category())));
                    if (!GetWindowRect(app.window(), &outer) || !GetClientRect(app.window(), &client))
                        return command_error("Cannot read the resized performance window rectangle.");
                    ATELIER_TRY(report_numbers(sample, "actual_outer_extent",
                                               std::array{outer.right - outer.left, outer.bottom - outer.top})
                                    .transform_error(report_error));
                    ATELIER_TRY(report_numbers(sample, "actual_client_extent",
                                               std::array{client.right - client.left, client.bottom - client.top})
                                    .transform_error(report_error));
                    if (!(outer.right - outer.left == next_width && outer.bottom - outer.top == next_height))
                        return command_error(
                            "Windows constrained the requested calibration size; see requested and actual extents.");
                }
                return command_error("Scene View calibration did not converge.");
            };
            auto resized = resize();
            if (!resized) {
                report.set_validated(calibration, "status", "failed");
                report.set_validated(calibration, "error", application::diagnostic_utf8(resized.error().message));
            }
            return resized;
        };
        ATELIER_TRY(Access::set_scene_maximized(app, true));
        ATELIER_TRY(calibrate(false));
        auto bounds = Access::accepted_view(app)->bounds;
        auto extent = app.presented()->drawing->extent;
        auto dpi = app.presented()->drawing->dpi_scale;
        const auto camera = Access::camera_state(app);
        auto accepted_camera = *Access::accepted_view(app);
        auto pixel_range = renderer::detail::raster_viewport(bounds).scissor;
        std::string_view layout = "maximized";
        const auto report_numbers = [&](json::Value node, std::string_view key, const auto& values) {
            return report.numbers(node, key, Span{values});
        };
        const auto viewport_json = [&](json::Value parent) -> json::Result<void> {
            const auto value = report.object_validated(parent, "viewport");
            ATELIER_TRY(report_numbers(value, "logical_rect_physical_px",
                                       std::array{bounds.x, bounds.y, bounds.width, bounds.height}));
            ATELIER_TRY(report_numbers(
                value, "logical_rect_dip",
                std::array{bounds.x / dpi, bounds.y / dpi, bounds.width / dpi, bounds.height / dpi}));
            ATELIER_TRY(report_numbers(
                value, "pixel_center_rect",
                std::array{pixel_range.left, pixel_range.top, pixel_range.right, pixel_range.bottom}));
            ATELIER_TRY(report_numbers(
                value, "receiver_pixel_extent",
                std::array{pixel_range.right - pixel_range.left, pixel_range.bottom - pixel_range.top}));
            ATELIER_TRY(report_numbers(value, "frame_extent", std::array{extent.width, extent.height}));
            ATELIER_TRY(report.set(value, "dpi_scale", dpi));
            return {};
        };
        const auto settings_json = [&](json::Value parent, std::string_view key,
                                       const renderer::ViewSettings& settings) -> json::Result<void> {
            const auto values = report.object_validated(parent, key);
            for (const auto& option : editor::view_options()) {
                const auto value = editor::view_value(settings, option.option);
                if (option.kind == editor::ViewValueKind::boolean)
                    ATELIER_TRY(report.set(values, option.name, value != 0));
                else if (option.kind == editor::ViewValueKind::enumeration)
                    ATELIER_TRY(report.set(values, option.name, option.choices[static_cast<std::size_t>(value)]));
                else
                    ATELIER_TRY(report.set(values, option.name, value));
            }
            return {};
        };
        const auto camera_json = [&](json::Value parent) -> json::Result<void> {
            const auto value = report.object_validated(parent, "camera");
            ATELIER_TRY(report.set(value, "view", static_cast<unsigned>(camera.view)));
            ATELIER_TRY(report_numbers(value, "target", std::array{camera.target.x, camera.target.y, camera.target.z}));
            ATELIER_TRY(report.set(value, "yaw", camera.yaw));
            ATELIER_TRY(report.set(value, "pitch", camera.pitch));
            ATELIER_TRY(report.set(value, "distance", camera.distance));
            ATELIER_TRY(report.numbers(value, "view_matrix", Span{accepted_camera.view.elements}));
            ATELIER_TRY(report.numbers(value, "projection_matrix", Span{accepted_camera.projection.elements}));
            ATELIER_TRY(report.set(value, "far_depth", accepted_camera.far_depth));
            return {};
        };
        const auto active_panes = [&](json::Value parent) -> Result<json::Value> {
            const auto panes = *report.array(parent, "active_panes");
            for (const auto& pane : Access::dock_presentation(app).panes)
                if (pane.active) {
                    if (pane.bounds.width <= 0 || pane.bounds.height <= 0)
                        continue;
                    const auto title = platform::utf16_to_utf8(pane.active->title);
                    if (!bool(title))
                        return command_error("Invalid UTF-16 profile pane title.");
                    const auto item = report.append_object(panes);
                    ATELIER_TRY(report.set(item, "tab", pane.active->key).transform_error(report_error));
                    ATELIER_TRY(report.set(item, "title", *title).transform_error(report_error));
                    ATELIER_TRY(report.set(item, "maximized", pane.maximized).transform_error(report_error));
                }
            return panes;
        };
        ATELIER_TRY(viewport_json(root).transform_error(report_error));
        ATELIER_TRY(settings_json(root, "original_view", original_view).transform_error(report_error));
        ATELIER_TRY(camera_json(root).transform_error(report_error));
        const auto maximized = report.object_validated(*root.at("layouts"), layout);
        ATELIER_TRY(viewport_json(maximized).transform_error(report_error));
        ATELIER_TRY(camera_json(maximized).transform_error(report_error));
        ATELIER_TRY(active_panes(maximized));
        bool camera_fixed = true;
        const auto invariant = [&]() -> Result<void> {
            if (!(Access::accepted_view(app) && Access::accepted_view(app)->bounds == bounds &&
                  app.presented()->drawing->extent == extent && app.presented()->drawing->dpi_scale == dpi &&
                  (!camera_fixed || Access::camera_state(app) == camera)))
                return command_error(
                    "Viewport, render target, DPI or camera changed during the performance comparison.");
            const auto range = renderer::detail::raster_viewport(Access::accepted_view(app)->bounds).scissor;
            if (!(range.right - range.left == 2560 && range.bottom - range.top == 1440))
                return command_error("Actual receiver dispatch pixel coverage changed from native 1440p.");
            if (Access::scene_lit_view(app) != view)
                return command_error("View or World settings changed during the performance comparison.");
            return {};
        };
        // The orbit case moves continuously: a triangle wave of .2 degrees of
        // yaw per accepted frame within +-12 degrees, without camera cuts.
        unsigned orbit_frame = 0;
        const auto orbit_step = [&]() -> Result<void> {
            if (camera_fixed)
                return {};
            const auto phase = orbit_frame++ % 240;
            auto state = camera;
            state.yaw += (phase < 120 ? float(phase) - 60.f : 180.f - float(phase)) * .2f * .0174532925f;
            ATELIER_TRY(Access::move_camera(app, state));
            return {};
        };
        const auto warmup = [&](unsigned frames) -> Result<std::uint64_t> {
            const auto deadline = Clock::now() + std::chrono::minutes(10);
            for (unsigned i = 0; i < frames; ++i) {
                ATELIER_TRY(orbit_step());
                ATELIER_TRY(next_frame(deadline));
                ATELIER_TRY(invariant());
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
            ATELIER_TRY_ASSIGN(cold_end, next_frame(cold_deadline));
            ATELIER_TRY(invariant());
            ++prepared;
            if (prepared == 1)
                cold_begin = app.frame_record().begin;
            const auto& stats = app.frame_statistics();
            stable = bytes == stats.scene_lit_resource_bytes && !stats.material_upload_count ? stable + 1 : 0;
            bytes = stats.scene_lit_resource_bytes;
        }
        if (app.frame_statistics().scene_draw_calls <= 0)
            return command_error("The selected Scene contains no rendered geometry.");
        const auto cold = report.object_validated(root, "cold_preparation");
        ATELIER_TRY(report.set(cold, "accepted_frames", prepared).transform_error(report_error));
        ATELIER_TRY(
            report.set(cold, "elapsed_ms", std::chrono::duration<double, std::milli>(cold_end - cold_begin).count())
                .transform_error(report_error));
        ATELIER_TRY(report.set(cold, "logical_resource_bytes", bytes).transform_error(report_error));
        ATELIER_TRY(unchanged_scene());
        const auto pass_order = *report.array(root, "gpu_pass_order");
        for (std::size_t i = 0; i < editor::FramePerformance::total_pass; ++i)
            ATELIER_TRY(
                report.append(pass_order, editor::FramePerformance::passes()[i].key).transform_error(report_error));
        // Obtain receiver counts separately. Diagnostic work is absent from
        // both measured paths, including their subsequent 192-frame warmups.
        view.lit.direct_cluster_inspect = true;
        ATELIER_TRY(apply_view(view));
        ATELIER_TRY(next_frame(Clock::now() + std::chrono::minutes(5)));
        ATELIER_TRY(invariant());
        const auto diagnostic_first = app.presented()->drawing->id;
        const auto diagnostic_deadline = Clock::now() + std::chrono::minutes(5);
        for (;;) {
            ATELIER_TRY(next_frame(diagnostic_deadline));
            ATELIER_TRY(invariant());
            const auto& s = app.frame_statistics();
            if (!s.scene_lit_gpu_timing_valid || s.scene_lit_gpu_sample_frame_id < diagnostic_first)
                continue;
            if (!((s.scene_lit_gpu_pass_mask & (1u << 12)) != 0))
                return command_error("Receiver inspection did not execute the light list pass.");
            const auto receivers = std::accumulate(s.scene_lit_direct_receiver_histogram.begin(),
                                                   s.scene_lit_direct_receiver_histogram.end(), std::uint64_t{});
            if (!(receivers > 0 && receivers <= 2560ull * 1440))
                return command_error("Receiver histogram did not cover valid visible Scene pixels.");
            const auto inspection = report.object_validated(root, "receiver_inspection");
            ATELIER_TRY(
                report.set(inspection, "sequence", s.scene_lit_gpu_sample_sequence).transform_error(report_error));
            ATELIER_TRY(
                report.set(inspection, "frame_id", s.scene_lit_gpu_sample_frame_id).transform_error(report_error));
            ATELIER_TRY(report.set(inspection, "valid_receiver_pixels", receivers).transform_error(report_error));
            ATELIER_TRY(report.numbers(inspection, "histogram", Span{s.scene_lit_direct_receiver_histogram})
                            .transform_error(report_error));
            ATELIER_TRY(report.set(inspection, "max_sources", s.scene_lit_direct_receiver_max_sources)
                            .transform_error(report_error));
            ATELIER_TRY(
                report
                    .set(
                        inspection, "semantics",
                        "Valid GBuffer receivers covered by analytic local-light support spheres; bins 0..64 exact, 65 >64; excludes sun/emissive meshes and does not test occlusion. Excluded from timed cases.")
                    .transform_error(report_error));
            ATELIER_TRY(settings_json(inspection, "view", view).transform_error(report_error));
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
        const auto distribution = [&](json::Value parent, std::string_view key,
                                      Array<double> values) -> Result<void> {
            if (values.empty())
                return command_error("Performance distribution has no observations.");
            const auto sum = std::accumulate(values.begin(), values.end(), 0.);
            std::sort(values.begin(), values.end());
            const auto result = report.object_validated(parent, key);
            ATELIER_TRY(report.set(result, "count", values.size()).transform_error(report_error));
            ATELIER_TRY(report.set(result, "mean", sum / double(values.size())).transform_error(report_error));
            ATELIER_TRY(
                report.set(result, "p50", values[(values.size() * 50 + 99) / 100 - 1]).transform_error(report_error));
            ATELIER_TRY(
                report.set(result, "p95", values[(values.size() * 95 + 99) / 100 - 1]).transform_error(report_error));
            return {};
        };
        struct CpuSample {
            std::uint64_t frame_id{};

            std::array<std::uint64_t, 4> draws{};

            std::uint64_t scene_instances{};

            std::uint64_t scene_culled_instances{};

            timing::FrameRecord record;
        };
        // Called right after next_frame, so the last step is the accepted one.
        const auto collect_cpu = [&](Array<CpuSample>& samples) {
            const auto& stats = app.frame_statistics();
            samples.add(
                {app.presented()->drawing->id,
                 {stats.scene_draw_calls, stats.ui_draw_calls, stats.viewport_draw_calls, stats.overlay_draw_calls},
                 stats.scene_instances,
                 stats.scene_culled_instances,
                 app.frame_record()});
        };
        const auto cpu_json = [&](json::Value parent, const Array<CpuSample>& samples) -> Result<void> {
            const auto milliseconds = [](std::chrono::nanoseconds value) {
                return std::chrono::duration<double, std::milli>(value).count();
            };
            // Phases entered in any of these steps, in catalog order; the last stage is the unattributed rest of the frame.
            Array<const timing::Marker*> entered;
            for (const auto* marker : timing::catalog())
                if (std::ranges::any_of(samples, [&](const auto& sample) { return sample.record.count(*marker) != 0; }))
                    entered.add(marker);
            Array<Array<double>> stages(entered.size() + 1);
            for (auto& stage : stages)
                stage.reserve(samples.size());
            const auto result = report.object_validated(parent, "cpu");
            const auto stage_values = report.object_validated(result, "stages");
            const auto rows = *report.array(result, "samples");
            for (const auto& sample : samples) {
                const auto row = report.append_object(rows);
                ATELIER_TRY(report.set(row, "accepted_frame_id", sample.frame_id).transform_error(report_error));
                ATELIER_TRY(report.set(row, "timing_sequence", sample.record.sequence).transform_error(report_error));
                ATELIER_TRY(report.set(row, "scene_draw_calls", sample.draws[0]).transform_error(report_error));
                ATELIER_TRY(report.set(row, "ui_draw_calls", sample.draws[1]).transform_error(report_error));
                ATELIER_TRY(report.set(row, "viewport_draw_calls", sample.draws[2]).transform_error(report_error));
                ATELIER_TRY(report.set(row, "overlay_draw_calls", sample.draws[3]).transform_error(report_error));
                ATELIER_TRY(report.set(row, "scene_instances", sample.scene_instances).transform_error(report_error));
                ATELIER_TRY(report.set(row, "scene_culled_instances", sample.scene_culled_instances)
                                .transform_error(report_error));
                const auto entries = report.object_validated(row, "entries");
                double children{};
                for (std::size_t i = 0; i < entered.size(); ++i) {
                    const auto value = milliseconds(sample.record.time(*entered[i]));
                    stages[i].add(value);
                    ATELIER_TRY(report.set(row, entered[i]->key(), value).transform_error(report_error));
                    // Entries of the phase in this step; a phase may run once per pass or not at all.
                    ATELIER_TRY(report.set(entries, entered[i]->key(), sample.record.count(*entered[i]))
                                    .transform_error(report_error));
                    if (entered[i]->parent() == &timing::frame)
                        children += value;
                }
                const auto unattributed = std::max(0., milliseconds(sample.record.time(timing::frame)) - children);
                stages.back().add(unattributed);
                ATELIER_TRY(report.set(row, "frame.unattributed", unattributed).transform_error(report_error));
            }
            for (std::size_t i = 0; i < entered.size(); ++i)
                ATELIER_TRY(distribution(stage_values, entered[i]->key(), std::move(stages[i])));
            ATELIER_TRY(distribution(stage_values, "frame.unattributed", std::move(stages.back())));
            // SceneSnapshot also composes inherited selection over every member when something is selected.
            ATELIER_TRY(report.set(result, "selected_entities", app.document().selected_entities().size())
                            .transform_error(report_error));
            return {};
        };
        for (const bool orbit : {false, true}) {
            ATELIER_TRY(apply_view(view));
            camera_fixed = !orbit;
            ATELIER_TRY_BIND(fence, warmup(192));
            Array<Sample> samples;
            samples.reserve(240);
            Array<double> wall;
            wall.reserve(512);
            Array<CpuSample> cpu;
            cpu.reserve(512);
            std::uint64_t sequence = 0;
            unsigned accepted = 0;
            auto previous = frame_end(app.frame_record());
            const auto measurement_start = previous;
            const auto deadline = Clock::now() + std::chrono::minutes(10);
            while (samples.size() < 240) {
                ATELIER_TRY(orbit_step());
                ATELIER_TRY_BIND(presented, next_frame(deadline));
                ATELIER_TRY(invariant());
                wall.add(std::chrono::duration<double, std::milli>(*presented - previous).count());
                previous = *presented;
                ++accepted;
                collect_cpu(cpu);
                const auto& s = app.frame_statistics();
                const auto observed = app.presented()->drawing->id;
                if (!s.scene_lit_gpu_timing_valid || s.scene_lit_gpu_sample_frame_id <= *fence ||
                    s.scene_lit_gpu_sample_sequence <= sequence)
                    continue;
                if (s.scene_lit_gpu_sample_frame_id > observed)
                    return command_error("Completed GPU sample refers to a future frame.");
                if (!(s.scene_lit_direct_receiver_max_sources == 0 &&
                      std::ranges::all_of(s.scene_lit_direct_receiver_histogram, [](auto n) { return n == 0; })))
                    return command_error("Receiver diagnostics leaked into timed samples.");
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
                if (!(std::ranges::all_of(sample.ms, [](auto n) { return std::isfinite(n) && n >= 0; })))
                    return command_error("Invalid completed GPU duration.");
                samples.add(sample);
                sequence = sample.sequence;
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(previous - measurement_start).count();
            const std::string name = orbit ? "clustered_orbit" : "clustered";
            const auto result = report.append_object(*root.at("cases"));
            ATELIER_TRY(report.set(result, "name", name).transform_error(report_error));
            ATELIER_TRY(report.set(result, "layout", layout).transform_error(report_error));
            ATELIER_TRY(report.set(result, "warmup_last_frame", *fence).transform_error(report_error));
            ATELIER_TRY(report.set(result, "accepted_frames", accepted).transform_error(report_error));
            ATELIER_TRY(report.set(result, "elapsed_ms", elapsed).transform_error(report_error));
            ATELIER_TRY(report.set(result, "accepted_steps_per_second", double(accepted) * 1000. / elapsed)
                            .transform_error(report_error));
            ATELIER_TRY(report.set(result, "gpu_sample_state", "completed").transform_error(report_error));
            ATELIER_TRY(
                report.numbers(result, "wall_present_intervals_ms", Span{wall}).transform_error(report_error));
            const auto& motion =
                orbit ? json::Document::literal<
                            R"({"yaw_degrees_per_frame":0.2,"yaw_amplitude_degrees":12,"camera_cut":false})">()
                      : json::Document::literal<"null">();
            ATELIER_TRY(report.set(result, "camera_motion", motion.root()).transform_error(report_error));
            ATELIER_TRY(settings_json(result, "view", view).transform_error(report_error));
            ATELIER_TRY(viewport_json(result).transform_error(report_error));
            ATELIER_TRY(camera_json(result).transform_error(report_error));
            ATELIER_TRY(cpu_json(result, cpu));
            ATELIER_TRY(distribution(result, "wall_present_interval_ms", wall));
            const auto gpu_passes = report.object_validated(result, "gpu_passes");
            const auto rows = *report.array(result, "samples");
            Array<double> totals;
            totals.reserve(samples.size());
            const auto definitions = editor::FramePerformance::passes();
            for (std::size_t pass = 0; pass < editor::FramePerformance::total_pass; ++pass) {
                Array<double> values;
                values.reserve(samples.size());
                unsigned active = 0;
                for (const auto& sample : samples) {
                    values.add(sample.ms[pass]);
                    active += (sample.mask & (1u << pass)) != 0;
                }
                const auto item = report.object_validated(gpu_passes, definitions[pass].key);
                ATELIER_TRY(distribution(item, "ms", std::move(values)));
                ATELIER_TRY(report.set(item, "active_samples", active).transform_error(report_error));
                ATELIER_TRY((definitions[pass].parent < 0
                                 ? report.set_null(item, "parent")
                                 : report.set(item, "parent", definitions[definitions[pass].parent].key))
                                .transform_error(report_error));
            }
            for (const auto& sample : samples) {
                totals.add(std::accumulate(sample.ms.begin(), sample.ms.begin() + 8, 0.));
                const auto row = report.append_object(rows);
                ATELIER_TRY(report.set(row, "sequence", sample.sequence).transform_error(report_error));
                ATELIER_TRY(report.set(row, "frame_id", sample.frame).transform_error(report_error));
                ATELIER_TRY(report.set(row, "observed_at_frame", sample.observed_frame).transform_error(report_error));
                ATELIER_TRY(report.set(row, "pass_mask", sample.mask).transform_error(report_error));
                ATELIER_TRY(report.numbers(row, "pass_ms", Span{sample.ms}).transform_error(report_error));
                ATELIER_TRY(report.set(row, "logical_resource_bytes", sample.resources).transform_error(report_error));
                ATELIER_TRY(report.set(row, "probe_rays", sample.probe_rays).transform_error(report_error));
                ATELIER_TRY(report.set(row, "shadow_rays", sample.shadow_rays).transform_error(report_error));
                ATELIER_TRY(report.numbers(row, "ddgi_visibility_rays", Span{sample.ddgi_visibility_rays})
                                .transform_error(report_error));
                ATELIER_TRY(report.set(row, "reflection_work_items", sample.reflection_work_items)
                                .transform_error(report_error));
                const auto shadow = report.object_validated(row, "shadow_cache");
                ATELIER_TRY(report.set(shadow, "faces", sample.shadows[0]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "reused_faces", sample.shadows[1]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "static_updates", sample.shadows[2]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "dynamic_updates", sample.shadows[3]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "draws", sample.shadows[4]).transform_error(report_error));
                ATELIER_TRY(
                    report.set(shadow, "resolution_downgrades", sample.shadows[5]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "dropped_lights", sample.shadows[6]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "near_plane_faces", sample.shadows[7]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "missing_taps", sample.shadows[8]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "plane_fallbacks", sample.shadows[9]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "deferred_faces", sample.shadows[10]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "deferral_frames", sample.shadows[11]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "atlas_cells", sample.shadows[12]).transform_error(report_error));
                ATELIER_TRY(report.set(shadow, "pending_lights", sample.shadows[13]).transform_error(report_error));
                const auto cluster = report.object_validated(row, "direct_cluster");
                ATELIER_TRY(report.set(cluster, "cells", sample.clusters[0]).transform_error(report_error));
                ATELIER_TRY(report.set(cluster, "overflows", sample.clusters[1]).transform_error(report_error));
                ATELIER_TRY(report.set(cluster, "max_sources", sample.clusters[2]).transform_error(report_error));
            }
            ATELIER_TRY(distribution(result, "measured_main_total_ms", std::move(totals)));
            ATELIER_TRY(unchanged_scene());
            ATELIER_TRY(report.set(root, "status", "sampling").transform_error(report_error));
            ATELIER_TRY(write_report());
            ATELIER_TRY(capture(app, output / (name + ".png")));
        }
        camera_fixed = true;
        ATELIER_TRY(Access::move_camera(app, camera));
        const auto non_lit_case = [&](renderer::Shading shading, std::string_view name) -> Result<void> {
            view.shading = shading;
            ATELIER_TRY(apply_view(view));
            ATELIER_TRY_BIND(fence, warmup(192));
            Array<double> wall;
            wall.reserve(240);
            Array<CpuSample> cpu;
            cpu.reserve(240);
            auto previous = frame_end(app.frame_record());
            const auto measurement_start = previous;
            const auto deadline = Clock::now() + std::chrono::minutes(10);
            for (unsigned accepted = 0; accepted < 240; ++accepted) {
                ATELIER_TRY_BIND(presented, next_frame(deadline));
                ATELIER_TRY(invariant());
                wall.add(std::chrono::duration<double, std::milli>(*presented - previous).count());
                previous = *presented;
                collect_cpu(cpu);
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(previous - measurement_start).count();
            // Non-Lit modes have no Scene Lit GPU scopes. Never reuse the last
            // completed Lit sample or infer GPU time from accepted step rate.
            const auto result = report.append_object(*root.at("cases"));
            ATELIER_TRY(report.set(result, "name", name).transform_error(report_error));
            ATELIER_TRY(report.set(result, "layout", layout).transform_error(report_error));
            ATELIER_TRY(report.set(result, "warmup_last_frame", *fence).transform_error(report_error));
            ATELIER_TRY(report.set(result, "accepted_frames", 240u).transform_error(report_error));
            ATELIER_TRY(report.set(result, "elapsed_ms", elapsed).transform_error(report_error));
            ATELIER_TRY(
                report.set(result, "accepted_steps_per_second", 240000. / elapsed).transform_error(report_error));
            ATELIER_TRY(
                report.numbers(result, "wall_present_intervals_ms", Span{wall}).transform_error(report_error));
            ATELIER_TRY(report.set(result, "gpu_sample_state", "unavailable_non_lit").transform_error(report_error));
            ATELIER_TRY(report.set_null(result, "gpu_passes").transform_error(report_error));
            ATELIER_TRY(report.set_null(result, "measured_main_total_ms").transform_error(report_error));
            ATELIER_TRY(report.array(result, "samples").transform_error(report_error));
            ATELIER_TRY(settings_json(result, "view", view).transform_error(report_error));
            ATELIER_TRY(viewport_json(result).transform_error(report_error));
            ATELIER_TRY(camera_json(result).transform_error(report_error));
            ATELIER_TRY(cpu_json(result, cpu));
            ATELIER_TRY(distribution(result, "wall_present_interval_ms", wall));
            ATELIER_TRY(unchanged_scene());
            ATELIER_TRY(report.set(root, "status", "sampling").transform_error(report_error));
            ATELIER_TRY(write_report());
            ATELIER_TRY(capture(app, output / (std::string(name) + ".png")));
            return {};
        };
        ATELIER_TRY(non_lit_case(renderer::Shading::smooth, "smooth"));
        ATELIER_TRY(non_lit_case(renderer::Shading::matcap, "matcap"));
        // Restore the retained Dock tree/ratios, not a freshly reset layout.
        ATELIER_TRY(Access::set_scene_maximized(app, false));
        ATELIER_TRY(calibrate(true));
        if (Access::camera_state(app) != camera)
            return command_error("Dock restoration changed the comparison camera.");
        if (app.presented()->drawing->dpi_scale != dpi)
            return command_error("Dock restoration changed the comparison DPI.");
        layout = "docked";
        bounds = Access::accepted_view(app)->bounds;
        extent = app.presented()->drawing->extent;
        dpi = app.presented()->drawing->dpi_scale;
        accepted_camera = *Access::accepted_view(app);
        pixel_range = renderer::detail::raster_viewport(bounds).scissor;
        const auto docked = report.object_validated(*root.at("layouts"), layout);
        ATELIER_TRY_BIND(active, active_panes(docked));
        ATELIER_TRY_BIND(panes, active->array().transform_error(report_error));
        if (panes->size() <= 1)
            return command_error("Dock restoration did not reveal the normal editor panels.");
        for (const auto pane : *panes)
            if (!(!*pane.boolean({"maximized"})))
                return command_error("Dock restoration left a maximized editor panel.");
        ATELIER_TRY(viewport_json(docked).transform_error(report_error));
        ATELIER_TRY(camera_json(docked).transform_error(report_error));
        ATELIER_TRY(non_lit_case(renderer::Shading::smooth, "docked_smooth"));
        ATELIER_TRY(non_lit_case(renderer::Shading::matcap, "docked_matcap"));
        ATELIER_TRY(apply_view(original_view));
        restore.active = false;
        ATELIER_TRY(unchanged_scene());
        ATELIER_TRY(report.set(root, "status", "pass").transform_error(report_error));
        ATELIER_TRY(settings_json(root, "restored_view", Access::scene_lit_view(app)).transform_error(report_error));
        ATELIER_TRY(write_report());
        return {};
    };
    auto sampled = sample();
    if (!sampled) {
        report.set_validated(root, "status", "failed");
        report.set_validated(root, "error", application::diagnostic_utf8(sampled.error().message));
        if (auto saved = write_report(); !saved)
            std::fprintf(stderr, "Cannot save failed performance report: %s\n", saved.error().message.c_str());
        return sampled;
    }
    std::puts(
        "Editor render performance profile: written (native 1440p, VSync off, maximized Lit/Smooth/MatCap, restored Dock Smooth/MatCap, CPU stage timings)");
    return {};
}
