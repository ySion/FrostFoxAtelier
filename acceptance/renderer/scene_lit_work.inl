// Ordinary Renderer frames verify skipped work through completed GPU queries.
void quality_work_elision(Fixture& f) {
    using atelier::geometry::Vertex;
    using atelier::geometry::Mesh;
    f.frame.scene_viewport = {24, 32, 320, 180};
    std::ofstream evidence(f.output / "work-elision-gpu.csv");
    evidence
        << "case,submission,case_first_frame,completed_sequence,completed_frame,current_logical_resource_bytes,visibility_queries,probe_queries,ddgi_hit_visibility,ddgi_gather_visibility\n";
    check(evidence.good(), "work-elision GPU evidence opened");
    std::uint64_t previous_sequence = 0;
    const auto completed = [&](const char* name, std::uint64_t first_frame, bool force_visibility = false) {
        std::array<render::FrameStats, 3> samples{};
        std::size_t count = 0;
        for (unsigned attempt = 0; attempt < 80 && count < samples.size(); ++attempt) {
            if (force_visibility)
                f.frame.camera_cut = true;
            const auto stats = f.step().stats;
            if (!stats.scene_lit_gpu_timing_valid || stats.scene_lit_gpu_sample_sequence <= previous_sequence ||
                stats.scene_lit_gpu_sample_frame_id < first_frame)
                continue;
            check(stats.scene_lit_gpu_sample_frame_id <= f.drawing.id,
                  "work-elision completion belongs to an already submitted case frame");
            const auto& options = f.frame.view.lit;
            const auto active = [&](unsigned bit) { return (stats.scene_lit_gpu_pass_mask & (1u << bit)) != 0; };
            check(active(2) && active(12) && active(14),
                  "completed direct scope mask records light lists and fused shading");
            const bool probes =
                options.ddgi && std::ranges::any_of(options.probe_volumes, [](const auto& v) { return v.enabled; });
            check(active(3) == options.ddgi && active(8) == probes && active(9) == options.ddgi &&
                      active(10) == probes && active(11) == probes,
                  "completed DDGI scope mask follows this case instead of delayed previous settings");
            const double direct_parts = double(stats.scene_lit_direct_cluster_gpu_ms) +
                                        stats.scene_lit_shadow_cache_gpu_ms + stats.scene_lit_direct_shading_gpu_ms;
            check(std::isfinite(direct_parts) && direct_parts >= 0 && direct_parts <= stats.scene_lit_gpu_ms[2] + .01,
                  "direct subpass durations are finite and belong to the completed direct scope");
            for (float value : stats.scene_lit_ddgi_update_gpu_ms)
                check(std::isfinite(value) && value >= 0, "DDGI update subpass duration is finite");
            previous_sequence = stats.scene_lit_gpu_sample_sequence;
            samples[count++] = stats;
            evidence << name << ',' << f.drawing.id << ',' << first_frame << ',' << previous_sequence << ','
                     << stats.scene_lit_gpu_sample_frame_id << ',' << stats.scene_lit_resource_bytes << ','
                     << stats.scene_lit_shadow_rays << ',' << stats.scene_lit_probe_rays << ','
                     << stats.scene_lit_ddgi_visibility_rays[0] << ',' << stats.scene_lit_ddgi_visibility_rays[1]
                     << '\n';
        }
        check(count == samples.size(), "work-elision receives three distinct completed samples from the current case");
        evidence.flush();
        check(evidence.good(), "work-elision GPU evidence written");
        return samples;
    };

    f.room.meshes.assign(1, {});
    auto& receiver = f.room.meshes.front();
    receiver.stable_key = 1;
    receiver.world = box(0, 0, 0, 1, 1, 1);
    receiver.geometry = std::make_shared<const Mesh>(std::vector<Vertex>{{{-6, -4, 4}, {0, 0, -1}},
                                                                         {{6, -4, 4}, {0, 0, -1}},
                                                                         {{6, 4, 4}, {0, 0, -1}},
                                                                         {{-6, -4, 4}, {0, 0, -1}},
                                                                         {{6, 4, 4}, {0, 0, -1}},
                                                                         {{-6, 4, 4}, {0, 0, -1}}});
    receiver.color = {.4f, .4f, .4f, 1};
    receiver.material = {.roughness = 1};
    f.lights.assign(1, {});
    auto& light = f.lights.front();
    light.stable_key = 100;
    light.position = {0, 0, .5f};
    light.range = 30;
    light.casts_shadow = true;
    f.frame.environment = {};
    f.frame.view.shading = render::Shading::scene_lit;
    auto& options = f.frame.view.lit;
    options = {};
    options.ddgi = options.gtao = options.sssr = options.bloom = false;
    options.shadows = true;
    options.antialiasing = render::Antialiasing::none;
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    const float aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.projection = {1 / (2.5f * aspect), 0, 0, 0, 0,          1 / 2.5f, 0, 0, 0, 0,
                                 -1 / 39.9f,          0, 0, 0, 40 / 39.9f, 1};
    ++f.frame.scene_identity;
    f.edited();
    f.frame.camera_cut = true;
    const auto region = project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0, 0, 4}, 12);

    // Direct visibility comes from the shadow atlas: no light kind, size or
    // candidate setting traces a ray for screen receivers.
    for (auto kind : {render::LightKind::point, render::LightKind::directional}) {
        light.kind = kind;
        light.intensity = kind == render::LightKind::point ? 30.f : 2.f;
        const std::string prefix = kind == render::LightKind::point ? "work-point-" : "work-directional-";
        for (const float radius : {0.f, kind == render::LightKind::point ? .2f : .02f})
            for (auto candidate_count : {1u, 32u}) {
                light.source_radius = radius;
                options.direct_candidates = candidate_count;
                f.edited();
                const auto first_frame = f.drawing.id + 1;
                const auto name = prefix + (radius > 0 ? "finite-" : "delta-") + std::to_string(candidate_count);
                const auto image = f.capture(name.c_str(), 4);
                check(mean_rgb(image, region)[0] > 5, "direct work case lights its receiver");
                for (const auto& stats : completed(name.c_str(), first_frame))
                    check(stats.scene_lit_shadow_rays == 0 && stats.scene_lit_probe_rays == 0,
                          "screen direct lighting traces no visibility rays");
            }
    }
    light.source_radius = 0;
    options.direct_candidates = 8;

    // Cover the physical last row/column of a partial 8-pixel compute group
    // and a partial 16-pixel light-list tile.
    const auto original_extent = f.drawing.extent;
    const auto original_viewport = f.frame.scene_viewport;
    const auto original_camera = f.frame.camera;
    f.window.resize(337, 191);
    f.drawing.extent = take(f.renderer.sync_window_target()).extent;
    check(f.drawing.extent == ui::PixelExtent{337, 191},
          "reference tail case uses the exact nonaligned render-target extent");
    f.frame.scene_viewport = {0, 0, 337, 191};
    f.frame.camera.projection[0] = 1 / (2.5f * (337.f / 191.f));
    light.kind = render::LightKind::point;
    light.intensity = 30;
    f.frame.camera_cut = true;
    f.edited();
    const auto tail = f.capture("work-point-337x191", 4);
    check(std::ranges::all_of(tail.depth, [](float value) { return value > 0; }),
          "tail case has a receiver at every physical pixel including the last row and column");
    for (unsigned x = 0; x < 337; ++x)
        check(pixel(tail, x, 190)[0] > 5, "direct lighting illuminates the physical last row");
    for (unsigned y = 0; y < 191; ++y)
        check(pixel(tail, 336, y)[0] > 5, "direct lighting illuminates the physical last column");
    f.window.resize(static_cast<int>(original_extent.width), static_cast<int>(original_extent.height));
    f.drawing.extent = take(f.renderer.sync_window_target()).extent;
    check(f.drawing.extent == original_extent, "tail case restores the render target before DDGI lifecycle cases");
    f.frame.scene_viewport = original_viewport;
    f.frame.camera = original_camera;
    f.frame.camera_cut = true;
    f.edited();

    f.lights.clear();
    f.frame.environment = {.05f, .05f, .05f};
    options.ddgi = true;
    options.probe_volumes[0].minimum = {-6, -4, 0};
    options.probe_volumes[0].maximum = {6, 4, 8};
    options.probe_volumes[0].counts = {4, 4, 4};
    options.probe_update_budget = 64;
    options.probe_rays = 64;
    const auto gather_case = [&](const char* name, bool needs_gather) {
        f.edited();
        const auto image = f.capture(name, 32);
        // Exercise material work elimination independently of the exact LOS
        // cache. Camera cuts retain the probe field but require fresh segments.
        const auto uncached_first = f.drawing.id + 1;
        for (const auto& stats : completed(name, uncached_first, true)) {
            check(stats.scene_lit_probe_rays > 0, "DDGI work elision retains real probe updates");
            check(needs_gather ? stats.scene_lit_ddgi_visibility_rays[1] > 0
                               : stats.scene_lit_ddgi_visibility_rays[1] == 0,
                  "DDGI receiver queries match exact material support and current consumers");
        }
        return image;
    };
    receiver.material.metallic = 1;
    gather_case("work-ddgi-metal", false);
    receiver.material.metallic = 0;
    receiver.color = {0, 0, 0, 1};
    gather_case("work-ddgi-black", false);
    receiver.color = {1e-5f, 1e-5f, 1e-5f, 1};
    gather_case("work-ddgi-tiny-nonzero", true);
    receiver.color = {.4f, .4f, .4f, 1};
    receiver.material.metallic = 1;
    options.probe_debug = true;
    const auto debug = gather_case("work-ddgi-metal-debug", false);
    const auto debug_color = mean_rgb(debug, region);
    check(*std::max_element(debug_color.begin(), debug_color.end()) > 5,
          "receiver coverage remains visible on a zero-diffuse material without changing lighting gather counters");
    options.probe_debug = false;
    // The sole open plane cannot classify probes as inside. Its nearest grid
    // layers are 4/3 m away, beyond relocation clearance, so all 64 probes can
    // issue illumination rays. Change the per-dispatch stride and scheduled
    // count in place; no new scene domain can hide stale scratch indexing.
    receiver.material.metallic = 0;
    options.probe_bounce = 0;
    for (const auto budget_rays :
         std::array{std::array{1u, 1u}, std::array{64u, 256u}, std::array{3u, 7u}, std::array{64u, 64u}}) {
        options.probe_update_budget = budget_rays[0];
        options.probe_rays = budget_rays[1];
        f.edited();
        const auto first_frame = f.drawing.id + 1;
        const auto name = "work-ddgi-span-" + std::to_string(budget_rays[0]) + "x" + std::to_string(budget_rays[1]);
        for (const auto& stats : completed(name.c_str(), first_frame))
            check(
                stats.scene_lit_probe_rays == std::uint64_t(budget_rays[0]) * (32u + budget_rays[1]),
                "growing and shrinking DDGI update spans trace exactly 32 fixed rays plus the requested illumination rays per scheduled probe");
    }
    // Populate scratch with real surface hits that perform lighting visibility
    // before making every probe inactive. The light is outside the later solid,
    // so stale hit shading would remain observable in the hit-visibility counter.
    f.lights.assign(1, {});
    auto& outside_light = f.lights.front();
    outside_light.stable_key = 100;
    outside_light.position = {0, 0, -30};
    outside_light.range = 100;
    outside_light.intensity = 3000;
    outside_light.casts_shadow = true;
    f.frame.environment = {};
    f.edited();
    auto first_frame = f.drawing.id + 1;
    for (const auto& stats : completed("work-ddgi-active-hits", first_frame))
        check(stats.scene_lit_probe_rays == 64u * (32u + 64u) && stats.scene_lit_ddgi_visibility_rays[0] > 0,
              "active probe scratch contains real receiver hits with measured lighting visibility");
    render::MeshInstance enclosure;
    enclosure.stable_key = 99;
    enclosure.geometry = atelier::geometry::cube_mesh();
    enclosure.world = box(0, 0, 4, 40, 40, 40);
    enclosure.color = {.4f, .4f, .4f, 1};
    // Positive-scale closed cube uses outward authored normals. The entire
    // [-6,6]x[-4,4]x[0,8] volume and every allowed relocation remain inside it.
    f.room.meshes.push_back(enclosure);
    f.edited();
    first_frame = f.drawing.id + 1;
    for (const auto& stats : completed("work-ddgi-all-inactive", first_frame))
        check(stats.scene_lit_probe_rays == 64u * 32u && stats.scene_lit_ddgi_visibility_rays[0] == 0,
              "inactive probes retain fixed classification rays but skip illumination tracing and scratch-hit shading");
    render::Capture enclosed;
    for (unsigned i = 0; i < 32; ++i)
        check(f.step(i == 31 ? &enclosed : nullptr).stats.scene_lit_history_resets == 0,
              "enclosing the receiver keeps the scene history domain");
    png(enclosed, f.output / "work-ddgi-inactive-dark.png");
    const auto enclosed_color = mean_rgb(enclosed, region);
    options.ddgi = false;
    const auto without_gi = f.capture("work-ddgi-inactive-without-gi", 1);
    const auto no_gi_color = mean_rgb(without_gi, region);
    std::printf("Inactive receiver after %llu frames: DDGI on=(%.6f,%.6f,%.6f), DDGI off=(%.6f,%.6f,%.6f)\n",
                static_cast<unsigned long long>(enclosed.frame_id - first_frame + 1), enclosed_color[0],
                enclosed_color[1], enclosed_color[2], no_gi_color[0], no_gi_color[1], no_gi_color[2]);
    check(*std::max_element(no_gi_color.begin(), no_gi_color.end()) <= 1,
          "enclosure blocks the outside light's direct contribution");
    check(*std::max_element(enclosed_color.begin(), enclosed_color.end()) <= 1,
          "inactive probes do not leak prior surface radiance into the enclosed receiver");
    std::puts(
        "Renderer SceneLit work elision: PASS (ray-free direct visibility, zero/tiny diffuse support, debug consumer, DDGI span resize and inactive scratch lifecycle)");
}
