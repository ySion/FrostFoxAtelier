// Product Renderer acceptance, not a cache unit test. Visibility is judged
// against geometric truth (segment-box tests on reconstructed receivers); the
// band within each declared margin of a blocker silhouette is not judged.
// Passing these small scenes does not prove 1440p cost or equivalence for every
// source radius, thin occluder, or grazing receiver.
void shadow_cases(Fixture& f) {
    namespace material = standard_fixture;
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    using Stats = render::FrameStats;
    f.frame.scene_viewport = {24.25f, 32.75f, 159.5f, 111.25f};
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    const float aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.projection = {1 / (2.5f * aspect), 0, 0, 0, 0,          1 / 2.5f, 0, 0, 0, 0,
                                 -1 / 39.9f,          0, 0, 0, 40 / 39.9f, 1};
    f.frame.environment = {};
    f.frame.view.shading = render::Shading::scene_lit;
    auto& options = f.frame.view.lit;
    options = {};
    options.ddgi = options.gtao = options.sssr = options.bloom = false;
    options.shadows = true;
    options.antialiasing = render::Antialiasing::none;
    const auto quad = std::make_shared<const Mesh>(std::vector<Vertex>{{{-.5f, -.5f, 0}, {0, 0, -1}},
                                                                       {{.5f, -.5f, 0}, {0, 0, -1}},
                                                                       {{.5f, .5f, 0}, {0, 0, -1}},
                                                                       {{-.5f, -.5f, 0}, {0, 0, -1}},
                                                                       {{.5f, .5f, 0}, {0, 0, -1}},
                                                                       {{-.5f, .5f, 0}, {0, 0, -1}}});
    const auto receiver = [&](std::uint64_t key, float x, float z, float width, float height) {
        render::MeshInstance mesh;
        mesh.stable_key = key;
        mesh.geometry = quad;
        mesh.world = box(x, 0, z, width, height, 1);
        mesh.color = {.5f, .5f, .5f, 1};
        mesh.material.roughness = 1;
        return mesh;
    };
    const auto blocker = [](std::uint64_t key) {
        render::MeshInstance mesh;
        mesh.stable_key = key;
        mesh.geometry = atelier::geometry::cube_mesh();
        mesh.world = box(-1, .5f, 2, .5f, .6f, .2f);
        mesh.color = {.3f, .3f, .3f, 1};
        return mesh;
    };
    // Unit-cube blockers placed by box(): world AABB from translation and scale.
    const auto bounds = [](const render::MeshInstance& mesh) {
        return Box{{mesh.world[12], mesh.world[13], mesh.world[14]},
                   {std::abs(mesh.world[0]) / 2, std::abs(mesh.world[5]) / 2, std::abs(mesh.world[10]) / 2}};
    };
    const auto reset_point = [&] {
        f.room.meshes = {receiver(1, 0, 4, 12, 8), blocker(2)};
        f.lights.assign(1, {});
        auto& light = f.lights.front();
        light.stable_key = 100;
        light.position = {-2, 1, 0};
        light.range = 10;
        light.intensity = 35;
        ++f.frame.scene_identity;
        f.frame.camera_cut = true;
        f.edited();
    };
    std::ofstream counters(f.output / "shadow-cache-completed.csv");
    check(counters.good(), "shadow cache completed evidence file opened");
    counters
        << "scenario,submitted_frame,completed_frame,completed_sequence,shadow_ms,faces,reused,static_updates,dynamic_updates,draws,dropped_lights,near_plane_faces,missing_taps,plane_fallbacks,deferred_faces,deferral_frames,atlas_cells,pending_lights\n";
    // Capturing the changed frame completes its submission. Inspect its exact
    // completed id before settling history: a later warm sample would hide the
    // one-frame invalidation being tested.
    const auto completed = [&](std::uint64_t target) {
        Stats stats;
        for (unsigned i = 0; i < 60; ++i) {
            stats = f.step().stats;
            if (!stats.scene_lit_gpu_timing_valid)
                continue;
            check(stats.scene_lit_gpu_sample_frame_id <= target,
                  "shadow changed-frame completed sample was not skipped");
            if (stats.scene_lit_gpu_sample_frame_id == target)
                return stats;
        }
        check(false, "shadow exact completed GPU sample did not arrive");
        return stats;
    };
    const auto record = [&](const char* name, std::uint64_t submitted, const Stats& stats) {
        counters << name << ',' << submitted << ',' << stats.scene_lit_gpu_sample_frame_id << ','
                 << stats.scene_lit_gpu_sample_sequence << ',' << stats.scene_lit_shadow_cache_gpu_ms << ','
                 << stats.scene_lit_shadow_cache_faces << ',' << stats.scene_lit_shadow_cache_reused_faces << ','
                 << stats.scene_lit_shadow_cache_static_updates << ',' << stats.scene_lit_shadow_cache_dynamic_updates
                 << ',' << stats.scene_lit_shadow_cache_draws << ',' << stats.scene_lit_shadow_cache_dropped_lights
                 << ',' << stats.scene_lit_shadow_cache_near_plane_faces << ','
                 << stats.scene_lit_shadow_cache_missing_taps << ',' << stats.scene_lit_shadow_cache_plane_fallbacks
                 << ',' << stats.scene_lit_shadow_cache_deferred_faces << ','
                 << stats.scene_lit_shadow_cache_deferral_frames << ',' << stats.scene_lit_shadow_cache_atlas_cells
                 << ',' << stats.scene_lit_shadow_cache_pending_lights << '\n';
        counters.flush();
    };
    const auto submit = [&](const char* name) {
        render::Capture image;
        const auto result = f.step(&image);
        png(image, f.output / (std::string(name) + "-first.png"));
        const auto stats = completed(result.accepted_id);
        check(stats.scene_lit_gpu_sample_sequence > 0 && std::isfinite(stats.scene_lit_shadow_cache_gpu_ms) &&
                  stats.scene_lit_shadow_cache_gpu_ms >= 0,
              "shadow counters and finite timing identify a completed submission");
        check(stats.scene_lit_shadow_cache_missing_taps == 0, "all requested shadow taps have current-frame residency");
        check(stats.scene_lit_shadow_cache_dropped_lights == 0,
              "small shadow fixture keeps every casting light resident");
        check((stats.scene_lit_gpu_pass_mask & ((1u << 12) | (1u << 14))) == ((1u << 12) | (1u << 14)),
              "direct lighting records its light-list and fused shading scopes");
        record(name, result.accepted_id, stats);
        return stats;
    };
    const auto warm = [&](const char* name) {
        const auto stats = submit(name);
        check(stats.scene_lit_shadow_cache_faces > 0 &&
                  stats.scene_lit_shadow_cache_reused_faces == stats.scene_lit_shadow_cache_faces &&
                  stats.scene_lit_shadow_cache_static_updates == 0 &&
                  stats.scene_lit_shadow_cache_dynamic_updates == 0 && stats.scene_lit_shadow_cache_draws == 0 &&
                  (stats.scene_lit_gpu_pass_mask & (1u << 13)) == 0,
              "stationary shadow cache reuses every requested face without redraw");
        return stats;
    };
    // Compares a settled shadowed capture with the same frame without shadows.
    // Disabling shadows retires the atlas, so two frames re-enter the cache
    // before any later edit is expected to preserve it.
    const auto truth = [&](const char* name, const render::Capture& shadowed, std::vector<Box> blockers, double margin,
                           std::size_t occluded) {
        options.shadows = false;
        const auto unshadowed = f.capture((std::string(name) + "-unshadowed").c_str(), 2);
        options.shadows = true;
        const auto result = shadow_truth(f.frame, shadowed, unshadowed, f.lights.front(), blockers, margin);
        f.metrics << name << ',' << result.max_lit_delta << ',' << result.max_occluded << ',' << result.failures << ','
                  << result.occluded << '\n';
        std::printf("Shadow truth %s: occluded=%zu lit=%zu failures=%zu max_lit_delta=%u max_occluded=%u\n", name,
                    result.occluded, result.lit, result.failures, result.max_lit_delta, result.max_occluded);
        check(result.lit >= 256 && result.occluded >= occluded && result.failures == 0,
              "cached visibility darkens geometrically occluded receivers and leaves unoccluded receivers unchanged");
        f.step();
        f.step();
    };
    reset_point();
    auto stats = submit("shadow-point-cold");
    check(stats.scene_lit_shadow_cache_faces > 0 && stats.scene_lit_shadow_cache_static_updates > 0 &&
              stats.scene_lit_shadow_cache_draws > 0 && (stats.scene_lit_gpu_pass_mask & (1u << 13)) != 0,
          "first shadow frame renders complete requested static depth and records update scope");
    const auto dark_region = project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0, 0, 4}, 2);
    const auto bright_region =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {1.8f, -.8f, 4}, 2);
    const auto baseline = f.capture("shadow-point-settled", 20);
    const auto blocked = mean_rgb(baseline, dark_region);
    const auto bright = mean_rgb(baseline, bright_region);
    check(bright[0] > 20 && blocked[0] < bright[0] * .12,
          "off-camera-ray caster produces an actual dark receiver shadow");
    warm("shadow-point-warm");
    const auto stable = f.capture("shadow-point-stable", 4);
    auto delta = f.compare("shadow-point-stability", baseline, stable);
    check(delta.mean == 0 && delta.p95 == 0, "stationary cached point visibility is identical across frames");
    truth("shadow-point-truth", stable, {bounds(f.room.meshes[1])}, .03, 64);

    f.lights.front().color = {.3f, .6f, 1};
    f.edited();
    stats = submit("shadow-light-color");
    check(stats.scene_lit_shadow_cache_static_updates == 0 && stats.scene_lit_shadow_cache_dynamic_updates == 0,
          "light RGB edits reuse visibility depth");
    const auto colored = f.capture("shadow-light-color-settled", 16);
    const auto color = mean_rgb(colored, bright_region);
    check(color[2] > color[0] + 10, "cached visibility still responds to authored light RGB");
    f.lights.front().color = {1, 1, 1};
    f.room.meshes.front().material.roughness = .7f;
    f.edited();
    stats = submit("shadow-surface-roughness");
    check(stats.scene_lit_shadow_cache_static_updates == 0 && stats.scene_lit_shadow_cache_dynamic_updates == 0,
          "surface roughness and light radiance changes do not invalidate caster coverage");

    f.room.meshes[1].world[13] = 1;
    f.edited();
    stats = submit("shadow-first-caster-motion");
    check(stats.scene_lit_shadow_cache_static_updates > 0 && stats.scene_lit_shadow_cache_dynamic_updates > 0,
          "first movement removes the old static shadow and writes the dynamic layer");
    const auto moved = f.capture("shadow-caster-moved", 16);
    check(mean_rgb(moved, dark_region)[0] > blocked[0] + 20, "moving a caster clears its previous shadow");
    f.room.meshes[1].world[13] = 1.25f;
    f.edited();
    stats = submit("shadow-next-caster-motion");
    check(stats.scene_lit_shadow_cache_static_updates == 0 && stats.scene_lit_shadow_cache_dynamic_updates > 0,
          "subsequent dynamic motion does not redraw stationary geometry");
    f.room.meshes.resize(1);
    f.edited();
    stats = submit("shadow-dynamic-delete");
    check(stats.scene_lit_shadow_cache_static_updates == 0 && stats.scene_lit_shadow_cache_dynamic_updates > 0,
          "deleting a dynamic caster clears old dynamic coverage without redrawing static depth");
    const auto deleted = f.capture("shadow-dynamic-deleted", 16);
    check(mean_rgb(deleted, dark_region)[0] > blocked[0] + 20, "deleted caster leaves no phantom shadow");
    truth("shadow-deleted-truth", deleted, {}, 0, 0);

    f.room.meshes.push_back(blocker(3));
    f.edited();
    stats = submit("shadow-caster-recreated");
    check(stats.scene_lit_shadow_cache_static_updates > 0, "new caster identity updates stationary visibility");
    const auto opacity = [&](std::uint8_t value) {
        material::Description description;
        description.parameters.coverage = material::Coverage::mask;
        description.parameters.base_color = {.3f, .3f, .3f, 1};
        description.texture(material::Input::opacity).image =
            std::make_shared<const material::Image>(1, 1, std::vector<std::uint8_t>{value, value, value, 255});
        auto surfaces = std::make_shared<std::vector<material::SurfaceRef>>();
        for (const auto& slot : f.room.meshes[1].geometry->materials()) {
            (void)slot;
            surfaces->push_back(std::make_shared<const material::Surface>(description));
        }
        f.room.meshes[1].surfaces = std::move(surfaces);
        f.edited();
    };
    opacity(0);
    stats = submit("shadow-mask-transparent");
    check(stats.scene_lit_shadow_cache_static_updates + stats.scene_lit_shadow_cache_dynamic_updates > 0,
          "Mask opacity content change invalidates previous occlusion");
    const auto transparent = f.capture("shadow-mask-transparent-settled", 16);
    check(mean_rgb(transparent, dark_region)[0] > blocked[0] + 20, "transparent Mask texels stop casting shadows");
    opacity(255);
    stats = submit("shadow-mask-opaque");
    check(stats.scene_lit_shadow_cache_static_updates == 0 && stats.scene_lit_shadow_cache_dynamic_updates > 0,
          "subsequent Mask edits remain in the dynamic layer");
    const auto opaque = f.capture("shadow-mask-opaque-settled", 16);
    check(mean_rgb(opaque, dark_region)[0] < mean_rgb(transparent, dark_region)[0] * .12,
          "opaque Mask texels restore the occluder instead of reusing transparent depth");
    truth("shadow-mask-truth", opaque, {bounds(f.room.meshes[1])}, .03, 64);

    f.lights.front().position[0] = -1.5f;
    f.edited();
    stats = submit("shadow-light-move");
    check(stats.scene_lit_shadow_cache_static_updates > 0 && stats.scene_lit_shadow_cache_dynamic_updates > 0,
          "moving a light invalidates both layers of its changed projections");
    const auto light_moved = f.capture("shadow-light-moved-settled", 16);
    truth("shadow-light-moved-truth", light_moved, {bounds(f.room.meshes[1])}, .03, 64);
    f.frame.camera.view[12] = .15f;
    f.frame.camera_cut = true;
    submit("shadow-camera-cut");
    f.capture("shadow-camera-cut-settled", 12);
    warm("shadow-camera-cut-warm");

    // The caster is wholly outside the camera's horizontal frustum, while the
    // line from (-8,1,0) through (-4,.5,2) reaches the visible (0,0,4) probe.
    f.frame.camera.view[12] = 0;
    f.frame.camera_cut = true;
    f.lights.front().position = {-8, 1, 0};
    f.lights.front().range = 16;
    f.lights.front().intensity = 200;
    f.room.meshes[1].world = box(-4, .5f, 2, .5f, .6f, .2f);
    f.edited();
    submit("shadow-offscreen-caster");
    const auto offscreen = f.capture("shadow-offscreen-caster-settled", 16);
    const auto offscreen_lit = mean_rgb(offscreen, bright_region)[0];
    check(offscreen_lit > 20 && mean_rgb(offscreen, dark_region)[0] < offscreen_lit * .12,
          "shadow caster discovery includes geometry outside main-camera visibility");
    truth("shadow-offscreen-caster-truth", offscreen, {bounds(f.room.meshes[1])}, .05, 64);

    f.room.meshes.clear();
    f.lights.clear();
    ++f.frame.scene_identity;
    f.edited();
    const auto empty = f.capture("shadow-empty-world", 2);
    check(std::ranges::all_of(empty.depth, [](float value) { return value == 0; }),
          "empty world removes every old shadow receiver");
    // Empty scenes intentionally have no SceneLit completed query. The next
    // nonempty scene must publish its own cold sample, not the old warm result.
    f.frame.camera.view[12] = 0;
    reset_point();
    stats = submit("shadow-world-reopened");
    check(stats.scene_lit_shadow_cache_static_updates > 0 && stats.scene_lit_shadow_cache_reused_faces == 0,
          "reopening a world cannot reuse retired atlas contents from another scene domain");
    // A finite sphere keeps one depth projection; outside the penumbra band its
    // receivers must still match the unshadowed frame exactly.
    f.lights.front().source_radius = .15f;
    f.edited();
    submit("shadow-finite-point");
    const auto soft = f.capture("shadow-finite-point-settled", 24);
    truth("shadow-finite-point-truth", soft, {bounds(f.room.meshes[1])}, .35, 0);
    warm("shadow-finite-point-warm");

    // Four disjoint receiver columns at camera view Z .15, .5, 1.5 and 6 lie in
    // the four cascades of the conservative fractions of [.1, 8] (ends .22,
    // .59, 2.07, 8), outside every blend band. Fitting the measured [.15, 6]
    // keeps these ends: its logarithmic splits all lie farther. Each column
    // has its own blocker 1 m in front of it and .5 m lower; the sun rises .5
    // per unit depth, so the camera sees each shadow on its column above the
    // blocker's image. The sun drifts along y only, so the x-disjoint columns
    // never shadow one another.
    const std::array<Vec3, 4> columns{{{-2.4f, 0, -3.85f}, {-.8f, 0, -3.5f}, {.8f, 0, -2.5f}, {2.4f, 0, 2}}};
    f.room.meshes.clear();
    std::vector<Box> sun_blockers;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto [x, y, z] = columns[i];
        f.room.meshes.push_back(receiver(11 + i, x, z, 1.1f, 3));
        auto column_blocker = blocker(15 + i);
        column_blocker.world = box(x, -.5f, z - 1, .6f, .4f, .1f);
        sun_blockers.push_back(bounds(column_blocker));
        f.room.meshes.push_back(column_blocker);
    }
    f.lights.assign(1, {});
    auto& sun = f.lights.front();
    sun.stable_key = 200;
    sun.kind = render::LightKind::directional;
    sun.direction = normalize({0, .5f, 1});
    sun.intensity = 2;
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    stats = submit("shadow-csm-cold");
    check(stats.scene_lit_shadow_cache_faces == 4 && stats.scene_lit_shadow_cache_static_updates == 4,
          "directional light renders four complete requested cascades");
    const auto cascades = f.capture("shadow-csm-settled", 20);
    warm("shadow-csm-warm");
    truth("shadow-csm-truth", cascades, sun_blockers, .03, 300);
    for (const auto& [x, y, z] : columns) {
        const auto lit = mean_rgb(
            cascades, project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {x, 1, z}, 1))[0];
        check(lit > 20 && mean_rgb(cascades, project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent,
                                                            {x, 0, z}, 1))[0] < lit * .1,
              "every cascade shadows its own column under its blocker and leaves the rest lit");
    }
    sun.direction = normalize({.03f, .5f, 1});
    f.edited();
    stats = submit("shadow-sun-moved");
    check(stats.scene_lit_shadow_cache_static_updates == 4, "moving the sun invalidates every cascade projection");
    const auto moved_sun = f.capture("shadow-sun-moved-settled", 16);
    truth("shadow-sun-moved-truth", moved_sun, sun_blockers, .03, 300);
    f.frame.camera.view[12] = .35f;
    f.frame.camera_cut = true;
    submit("shadow-csm-camera-move");
    const auto moved_camera = f.capture("shadow-csm-camera-moved-settled", 16);
    truth("shadow-csm-camera-truth", moved_camera, sun_blockers, .03, 300);
    warm("shadow-csm-camera-warm");

    // Grazing sun, 70 degrees from the receiver normal, and a fin standing 1 cm
    // in front of the receiver. One interior texel (about 4.6 mm) of normal
    // offset moves the far shadow edge about 1.3 cm, inside the 5.6 cm judged
    // band; ten texels move it past the band. Without slope handling the lit
    // slope shadows itself.
    f.frame.scene_viewport = {0, 0, float(f.drawing.extent.width), float(f.drawing.extent.height)};
    const float wide = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    f.frame.camera.projection = {1 / (.25f * wide), 0, 0, 0, 0,          1 / .25f, 0, 0, 0, 0,
                                 -1 / 39.9f,        0, 0, 0, 40 / 39.9f, 1};
    // The fin's half thickness exceeds the margin, so its shrunk box is not empty.
    auto fin = blocker(16);
    fin.world = box(0, 0, -.06f, .04f, 1, .1f);
    f.room.meshes = {receiver(17, 0, 0, 2.4f, 1.6f), fin};
    f.lights.front().direction = normalize({2.75f, 0, 1});
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    submit("shadow-grazing-contact-cold");
    truth("shadow-grazing-contact-truth", f.capture("shadow-grazing-contact-settled", 16), {bounds(fin)}, .015, 1000);
    warm("shadow-grazing-contact-warm");

    // Lamps 0.6 m in front of one wall each project to about 360 pixels, the
    // 256 tier, and need the same faces. Halving intensity per index outweighs
    // the spread of camera solid angles, so importance rank is the index.
    f.frame.camera.projection = {.4f / wide, 0, 0, 0, 0, .4f, 0, 0, 0, 0, -1 / 39.9f, 0, 0, 0, 40 / 39.9f, 1};
    f.room.meshes = {receiver(18, 0, -1.8f, 12, 8)};
    const auto lamps = [&](std::size_t count) {
        f.lights.assign(count, {});
        for (std::size_t i = 0; i < count; ++i) {
            auto& light = f.lights[i];
            light.stable_key = 70000 + i;
            light.range = 1;
            light.position = {-1.2f + .6f * float(i % 5), -.6f + .4f * float(i / 5), -2.4f};
            light.intensity = 4 * std::pow(.5f, float(i));
        }
        f.edited();
    };
    lamps(16);
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    f.settle_shadows(submit("shadow-tier-16-cold").scene_lit_gpu_sample_frame_id);
    const auto tier16 = warm("shadow-tier-16-warm");
    check(tier16.scene_lit_shadow_cache_atlas_cells == 4 * tier16.scene_lit_shadow_cache_faces &&
              tier16.scene_lit_shadow_cache_resolution_downgrades == 0,
          "sixteen lamps all keep their 256 diameter tier");
    lamps(20);
    stats = submit("shadow-tier-20");
    const auto faces = stats.scene_lit_shadow_cache_faces;
    const auto added = faces - tier16.scene_lit_shadow_cache_faces;
    check(added > 0 && stats.scene_lit_shadow_cache_atlas_cells == tier16.scene_lit_shadow_cache_atlas_cells + added &&
              stats.scene_lit_shadow_cache_reused_faces == tier16.scene_lit_shadow_cache_faces &&
              stats.scene_lit_shadow_cache_static_updates == added &&
              stats.scene_lit_shadow_cache_resolution_downgrades == 0,
          "only the sixteen most important lamps keep 256 faces; dimmer lamps add 128 faces without redrawing them");
    warm("shadow-tier-20-warm");
    // Every lamp moves 1 mm per frame, so every face is stale every frame. A
    // lamp's k faces redraw together from one origin: floor(24/k) lamps per
    // frame, longest-waiting first, so a lamp waits at most rotation-1 frames.
    // Faces deferred near cube seams still project from their drawn origin.
    // Once still, stale faces drain at the same rate.
    constexpr std::uint32_t budget = 24;
    const auto per_lamp = faces / 20;
    const auto per_frame = budget / per_lamp * per_lamp;
    const auto rotation = (20 + budget / per_lamp - 1) / (budget / per_lamp);
    check(faces % 20 == 0 && per_lamp >= 2 && per_lamp <= 6 && faces > 2 * per_frame,
          "budget fixture lamps need equal face sets beyond two frames of budget");
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::uint64_t seen = 0;
    unsigned observed = 0;
    for (unsigned frame = 0; frame < 12; ++frame) {
        for (auto& light : f.lights)
            light.position[0] += .001f;
        f.edited();
        const auto result = f.step();
        if (frame == 0)
            first = result.accepted_id;
        last = result.accepted_id;
        const auto& sample = result.stats;
        const auto id = sample.scene_lit_gpu_sample_frame_id;
        if (!sample.scene_lit_gpu_timing_valid || id < first || id == seen)
            continue;
        seen = id;
        ++observed;
        record("shadow-budget-motion", result.accepted_id, sample);
        check(sample.scene_lit_shadow_cache_faces == faces &&
                  sample.scene_lit_shadow_cache_static_updates == per_frame &&
                  sample.scene_lit_shadow_cache_deferred_faces == faces - per_frame &&
                  sample.scene_lit_shadow_cache_deferral_frames < rotation &&
                  sample.scene_lit_shadow_cache_missing_taps == 0,
              "moving lamps redraw whole lamps within the face budget, longest-waiting first, with every tap resident");
    }
    check(observed >= 6 && seen >= first + rotation, "budget motion observes completed samples past one full rotation");
    bool drained = false;
    for (unsigned frame = 0; frame < 20 && !drained; ++frame) {
        const auto result = f.step();
        const auto& sample = result.stats;
        const auto id = sample.scene_lit_gpu_sample_frame_id;
        if (!sample.scene_lit_gpu_timing_valid || id <= last || id == seen)
            continue;
        seen = id;
        record("shadow-budget-still", result.accepted_id, sample);
        const auto pending = std::uint64_t(per_frame) * (id - last + 1);
        check(sample.scene_lit_shadow_cache_deferred_faces == (faces > pending ? faces - pending : 0) &&
                  sample.scene_lit_shadow_cache_missing_taps == 0,
              "stopped lamps redraw their remaining stale faces at the same rate");
        drained = sample.scene_lit_shadow_cache_deferred_faces == 0;
    }
    check(drained, "stale faces drain once lamps stop");
    warm("shadow-budget-settled");

    // Thirty casting lights arrive at once, clustered around the first case's
    // light. Their faces draw at the budget per frame, and a light adds no
    // direct light until all of its faces exist, so the umbra stays dark in
    // every frame. Once all are drawn and faded in, the unoccluded receiver
    // matches the unshadowed frame.
    f.frame.scene_viewport = {24.25f, 32.75f, 159.5f, 111.25f};
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    f.frame.camera.projection = {1 / (2.5f * aspect), 0, 0, 0, 0,          1 / 2.5f, 0, 0, 0, 0,
                                 -1 / 39.9f,          0, 0, 0, 40 / 39.9f, 1};
    f.room.meshes = {receiver(19, 0, 4, 12, 8), blocker(20)};
    f.lights.assign(30, {});
    for (std::size_t i = 0; i < f.lights.size(); ++i) {
        auto& light = f.lights[i];
        light.stable_key = 80000 + i;
        light.position = {-2.05f + .02f * float(i % 6), .96f + .02f * float(i / 6), 0};
        light.range = 10;
        light.intensity = 35.f / 30;
    }
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    std::uint64_t arrival = 0;
    unsigned withheld = 0;
    bool resident = false;
    std::vector<double> umbra;
    for (unsigned frame = 0; frame < 60 && !resident; ++frame) {
        render::Capture image;
        const auto result = f.step(&image);
        if (frame == 0) {
            arrival = result.accepted_id;
            png(image, f.output / "shadow-arrival-first.png");
        }
        umbra.push_back(mean_rgb(image, dark_region)[0]);
        const auto& sample = result.stats;
        const auto id = sample.scene_lit_gpu_sample_frame_id;
        if (!sample.scene_lit_gpu_timing_valid || id < arrival)
            continue;
        record("shadow-arrival", result.accepted_id, sample);
        check(sample.scene_lit_shadow_cache_static_updates <= budget &&
                  sample.scene_lit_shadow_cache_missing_taps == 0 && sample.scene_lit_shadow_cache_dropped_lights == 0,
              "arriving lights redraw at most the face budget per frame");
        if (sample.scene_lit_shadow_cache_pending_lights) {
            ++withheld;
            check(id - arrival <= (sample.scene_lit_shadow_cache_faces + 18) / 19 + 8,
                  "pending shadow lights clear within ceil(faces/19)+8 frames");
        } else
            resident = true;
    }
    check(withheld >= 2 && resident, "thirty arriving lights wait over several frames, then all become resident");
    const auto arrived = f.capture("shadow-arrival-settled", 8);
    const auto arrived_lit = mean_rgb(arrived, bright_region)[0];
    const auto arrived_umbra = mean_rgb(arrived, dark_region)[0];
    check(arrived_lit > 20 && arrived_umbra < arrived_lit * .12,
          "resident arriving lights light and shadow the receiver");
    std::printf("Shadow arrival: pending samples=%u frames=%zu umbra max=%.3f settled umbra=%.3f lit=%.3f\n", withheld,
                umbra.size(), *std::ranges::max_element(umbra), arrived_umbra, arrived_lit);
    for (const auto value : umbra)
        check(value <= arrived_umbra + 1, "lights with undrawn faces add nothing to the occluded receiver");
    options.shadows = false;
    const auto arrival_unshadowed = f.capture("shadow-arrival-unshadowed", 2);
    options.shadows = true;
    for (unsigned channel = 0; channel < 3; ++channel)
        check(std::abs(mean_rgb(arrived, bright_region)[channel] -
                       mean_rgb(arrival_unshadowed, bright_region)[channel]) <= 1,
              "faded-in lights reach exactly their unshadowed energy on unoccluded receivers");
    // Lights without a stable key keep no cached faces between frames, so
    // they redraw every frame outside the budget instead of waiting forever.
    for (auto& light : f.lights)
        light.stable_key = 0;
    f.edited();
    f.settle_shadows();
    const auto keyless = f.capture("shadow-keyless-settled", 2);
    for (unsigned channel = 0; channel < 3; ++channel)
        check(std::abs(mean_rgb(keyless, bright_region)[channel] -
                       mean_rgb(arrival_unshadowed, bright_region)[channel]) <= 1,
              "thirty lights without stable keys redraw outside the budget and keep their full energy");
    check(mean_rgb(keyless, dark_region)[0] <= arrived_umbra + 1, "lights without stable keys keep their shadows");

    // Thirty dim lamps each light one small patch 0.3 m ahead and need only
    // their +Z face; moving 1 mm per frame they keep the budget full. A probe
    // lamp alternates every three frames between P0, where its patch straddles
    // the +X/+Z diagonal, and P1 0.5 m to the right, where the patch needs only
    // +Z. The probe ranks last, so the frame it moves its redraw waits behind
    // the fillers. While it waits, the shader selects faces from the
    // origin they were drawn from, so every face that origin selects must stay
    // in the plan: no tap may miss. A second probe Q also shrinks its range
    // as it moves 1.2 m right: R3 is lit only from Q0 and R2 only from Q1, and
    // from Q0 R2 lies in +X, so the faces kept for the drawn origin must come
    // from the receivers in the current support, not the old one.
    f.frame.scene_viewport = {0, 0, float(f.drawing.extent.width), float(f.drawing.extent.height)};
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    f.frame.camera.projection = {.4f / wide, 0, 0, 0, 0, .4f, 0, 0, 0, 0, -1 / 39.9f, 0, 0, 0, 40 / 39.9f, 1};
    const auto patch = [&](std::uint64_t key, float x, float y, float z, float size) {
        auto mesh = receiver(key, 0, 0, 1, 1);
        mesh.world = box(x, y, z, size, size, 1);
        return mesh;
    };
    f.room.meshes.clear();
    f.lights.assign(32, {});
    for (std::size_t i = 0; i < 30; ++i) {
        const float x = -2 + .6f * float(i % 6);
        const float y = -1.2f + .6f * float(i / 6);
        f.room.meshes.push_back(patch(90000 + i, x, y, -2.7f, .05f));
        auto& light = f.lights[i];
        light.stable_key = 90000 + i;
        light.position = {x, y, -3};
        light.range = .4f;
        light.intensity = .5f;
    }
    const std::array<float, 3> p0{3.2f, 0, -3};
    const std::array<float, 3> p1{3.7f, 0, -3};
    f.room.meshes.push_back(patch(90100, 4.2f, 0, -2, .2f));
    auto& probe = f.lights[30];
    probe.stable_key = 90100;
    probe.position = p0;
    probe.range = 2;
    probe.intensity = .02f;
    const std::array<float, 3> q0{3, -2.1f, -3};
    const std::array<float, 3> q1{4.2f, -2.1f, -3};
    f.room.meshes.push_back(patch(90101, 3, -2.1f, -2.5f, .1f));
    f.room.meshes.push_back(patch(90102, 4.2f, -2.1f, -2.4f, .1f));
    auto& q = f.lights[31];
    q.stable_key = 90101;
    q.position = q0;
    q.range = 1;
    q.intensity = .02f;
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    f.settle_shadows(submit("shadow-origin-lag-cold").scene_lit_gpu_sample_frame_id);
    std::uint64_t lag_first = 0;
    unsigned lag_samples = 0;
    seen = 0;
    for (unsigned frame = 0; frame < 36; ++frame) {
        for (std::size_t i = 0; i < 30; ++i)
            f.lights[i].position[0] += .001f;
        const bool right = frame / 3 % 2;
        f.lights[30].position = right ? p1 : p0;
        f.lights[31].position = right ? q1 : q0;
        f.lights[31].range = right ? .7f : 1;
        f.edited();
        const auto result = f.step();
        if (frame == 0)
            lag_first = result.accepted_id;
        const auto& sample = result.stats;
        const auto id = sample.scene_lit_gpu_sample_frame_id;
        if (!sample.scene_lit_gpu_timing_valid || id < lag_first || id == seen)
            continue;
        seen = id;
        ++lag_samples;
        record("shadow-origin-lag", result.accepted_id, sample);
        check(sample.scene_lit_shadow_cache_deferred_faces > 0 &&
                  sample.scene_lit_shadow_cache_static_updates <= budget,
              "origin-lag fixture keeps the redraw budget full");
        check(sample.scene_lit_shadow_cache_missing_taps == 0,
              "a deferred light keeps every face its drawn origin selects, so no tap misses");
    }
    check(lag_samples >= 24, "origin-lag motion observes completed samples");
    f.settle_shadows();

    // A point light 13 mm, then 4 mm, in front of an unoccluded plane, 1 m to
    // the side of the judged region: N.L is about .013 and .004, below a 128
    // face's one-texel offset ratio (.0161), so the offset point lies beyond
    // the plane as seen from the light. Extrapolating with the offset plane's
    // slope flips its sign; with the plane's own slope every texel the plane
    // covers compares lit. At .004 the extrapolation also passes the far plane,
    // where empty texels must still compare lit.
    f.frame.scene_viewport = {24.25f, 32.75f, 159.5f, 111.25f};
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    // Perspective: the light projects to about 148 pixels, the 128 tier.
    f.frame.camera.projection = {4 / aspect, 0, 0, 0, 0, 4, 0, 0, 0, 0, -.1f / 39.9f, 1, 0, 0, 4 / 39.9f, 0};
    struct Grazing {
        float height;

        float intensity;

        const char* label;
    };
    for (const auto& [height, intensity, label] : std::array<Grazing, 2>{{{.013f, 150, "13mm"}, {.004f, 500, "4mm"}}}) {
        f.room.meshes = {receiver(96000, 0, 4, 12, 8)};
        f.lights.assign(1, {});
        auto& light = f.lights.front();
        light.stable_key = 96000;
        light.position = {-.5f, 0, 4 - height};
        light.range = 2;
        light.intensity = intensity;
        ++f.frame.scene_identity;
        f.frame.camera_cut = true;
        f.edited();
        const auto name = std::string("shadow-grazing-plane-") + label;
        const auto cold = submit((name + "-cold").c_str());
        check(cold.scene_lit_shadow_cache_faces > 0 &&
                  cold.scene_lit_shadow_cache_atlas_cells == cold.scene_lit_shadow_cache_faces,
              "grazing plane light uses 128 faces, whose one-texel offset ratio is .0161");
        const auto shadowed = f.capture((name + "-shadowed").c_str(), 4);
        options.shadows = false;
        const auto unshadowed = f.capture((name + "-unshadowed").c_str(), 2);
        options.shadows = true;
        const auto region = project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {.5f, 0, 4}, 1);
        const auto lit = mean_rgb(shadowed, region);
        const auto reference = mean_rgb(unshadowed, region);
        std::printf("Shadow grazing plane %s: shadowed=%.3f unshadowed=%.3f\n", label, lit[0], reference[0]);
        check(reference[0] > 20, "grazing plane fixture receives measurable direct light");
        for (unsigned channel = 0; channel < 3; ++channel)
            check(std::abs(lit[channel] - reference[channel]) <= 1,
                  "an unoccluded receiver lit at grazing incidence stays lit");
        f.step();
        f.step();
    }

    // A completely transparent cube requests six conservative directions
    // without changing the real GBuffer or visibility. 272*6 minimum 128 tiles
    // exceed the fixed atlas's cells: the plan keeps whole lights in importance
    // order and drops the rest, which then contribute no direct light at all.
    // The first frame draws only the face budget, four whole lights.
    f.frame.scene_viewport = {24.25f, 32.75f, 31, 23};
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    f.frame.camera.projection = {
        1 / (2.5f * (31.f / 23)), 0, 0, 0, 0, 1 / 2.5f, 0, 0, 0, 0, -1 / 39.9f, 0, 0, 0, 40 / 39.9f, 1};
    auto invisible = blocker(302);
    invisible.world = box(0, 0, 0, 20, 20, 20);
    material::Description mask;
    mask.parameters.coverage = material::Coverage::mask;
    mask.texture(material::Input::opacity).image =
        std::make_shared<const material::Image>(1, 1, std::vector<std::uint8_t>{0, 0, 0, 255});
    invisible.surfaces = std::make_shared<const std::vector<material::SurfaceRef>>(
        invisible.geometry->materials().size(), std::make_shared<const material::Surface>(mask));
    f.room.meshes = {receiver(301, 0, 4, 12, 8), std::move(invisible)};
    f.lights.assign(272, {});
    for (std::size_t i = 0; i < f.lights.size(); ++i) {
        auto& light = f.lights[i];
        light.stable_key = 30000 + i;
        light.position = {0, 0, 0};
        light.range = 10;
        light.intensity = 24.f / 272;
    }
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    const auto pressure_submission = f.step();
    Stats pressure_stats;
    bool have_pressure = false;
    for (unsigned drain = 0; drain < 60 && !have_pressure; ++drain) {
        const auto sample = f.step().stats;
        if (sample.scene_lit_gpu_timing_valid &&
            sample.scene_lit_gpu_sample_frame_id == pressure_submission.accepted_id) {
            pressure_stats = sample;
            have_pressure = true;
        }
    }
    check(have_pressure, "pressure sample retains its exact completed frame identity");
    record("shadow-pressure-272-first", pressure_submission.accepted_id, pressure_stats);
    const auto dropped = pressure_stats.scene_lit_shadow_cache_dropped_lights;
    check(
        dropped > 0 && dropped < 272 && pressure_stats.scene_lit_shadow_cache_faces == (272 - dropped) * 6 &&
            pressure_stats.scene_lit_shadow_cache_static_updates == budget &&
            pressure_stats.scene_lit_shadow_cache_pending_lights == 272 - dropped - budget / 6 &&
            pressure_stats.scene_lit_shadow_cache_missing_taps == 0,
        "atlas pressure keeps whole lights with all six faces, drops the remainder and first draws only the face budget");
    f.settle_shadows(pressure_submission.accepted_id);
    render::Capture pressure_image;
    f.step(&pressure_image);
    png(pressure_image, f.output / "shadow-pressure-272.png");
    // Equal importance ties keep the lowest indices: the resident set alone is
    // the exact expected image, so a dropped light leaking energy shows here.
    f.lights.assign(272 - dropped, {});
    for (std::size_t i = 0; i < f.lights.size(); ++i) {
        auto& light = f.lights[i];
        light.stable_key = 30000 + i;
        light.position = {0, 0, 0};
        light.range = 10;
        light.intensity = 24.f / 272;
    }
    f.frame.camera_cut = true;
    f.edited();
    const auto resident_image = f.capture("shadow-pressure-resident", 2);
    const Region pressure_region{
        24, 33, 55, 56,
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0, 0, 4}, 1).expected_depth};
    for (unsigned y = pressure_region.top; y < pressure_region.bottom; ++y)
        for (unsigned x = pressure_region.left; x < pressure_region.right; ++x) {
            check(std::abs(depth(pressure_image, x, y) - pressure_region.expected_depth) < 2e-4f &&
                      std::abs(depth(resident_image, x, y) - pressure_region.expected_depth) < 2e-4f,
                  "transparent pressure cube does not replace any real wall receiver in the ROI");
        }
    const auto pressure_delta = region_difference(pressure_image, resident_image, pressure_region);
    f.metrics << "shadow-pressure-resident-roi," << pressure_delta.mean << ',' << pressure_delta.p95 << ','
              << pressure_delta.changed << ",0\n";
    std::printf("Shadow pressure ROI: mean=%.4f p95=%u dropped=%u\n", pressure_delta.mean, pressure_delta.p95,
                static_cast<unsigned>(dropped));
    check(mean_rgb(resident_image, pressure_region)[0] > 20 && pressure_delta.mean <= .7 && pressure_delta.p95 <= 2,
          "dropped lights contribute nothing and resident lights keep their exact energy");

    // Point, sun and rectangle accumulate exactly once each. Captures are
    // decoded through the documented display transform to linear radiance.
    const auto linear = [](std::uint8_t code) {
        const double s = code / 255.;
        const double y = s <= .04045 ? s / 12.92 : std::pow((s + .055) / 1.055, 2.4);
        const double a = 2.51 - 2.43 * y;
        const double b = .03 - .59 * y;
        const double c = -.14 * y;
        return (-b + std::sqrt(b * b - 4 * a * c)) / (2 * a);
    };
    const auto radiance = [&](const render::Capture& image, Region region) {
        double sum = 0;
        unsigned count = 0;
        for (unsigned y = region.top; y < region.bottom; ++y)
            for (unsigned x = region.left; x < region.right; ++x) {
                sum += linear(pixel(image, x, y)[0]);
                ++count;
            }
        return sum / count;
    };
    f.room.meshes = {receiver(401, 0, 4, 12, 8)};
    f.lights.assign(3, {});
    f.lights[0].stable_key = 40000;
    f.lights[0].position = {0, 0, 0};
    f.lights[0].range = 10;
    f.lights[0].intensity = 4;
    f.lights[1].stable_key = 40001;
    f.lights[1].kind = render::LightKind::directional;
    f.lights[1].direction = {0, 0, 1};
    f.lights[1].intensity = .25f;
    f.lights[2].stable_key = 40002;
    f.lights[2].kind = render::LightKind::rectangle;
    f.lights[2].position = {1.5f, 0, 0};
    f.lights[2].direction = {0, 0, 1};
    f.lights[2].range = 10;
    f.lights[2].width = f.lights[2].height = .02f;
    f.lights[2].intensity = 12000;
    f.lights[2].casts_shadow = false;
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    stats = submit("shadow-kernel-mixed");
    check(stats.scene_lit_light_sources == 3, "mixed fixture includes one point, one sun and one rectangle");
    const auto mixed_region = project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0, 0, 4}, 3);
    const auto mixed = radiance(f.capture("shadow-kernel-mixed-settled", 8), mixed_region);
    double components = 0;
    const std::array<const char*, 3> component_names{"shadow-kernel-only-point", "shadow-kernel-only-sun",
                                                     "shadow-kernel-only-rectangle"};
    const auto intensities = std::array{f.lights[0].intensity, f.lights[1].intensity, f.lights[2].intensity};
    for (std::size_t component = 0; component < component_names.size(); ++component) {
        for (std::size_t i = 0; i < 3; ++i)
            f.lights[i].intensity = i == component ? intensities[i] : 0;
        f.frame.camera_cut = true;
        f.edited();
        const auto alone = radiance(f.capture(component_names[component], 4), mixed_region);
        check(alone > mixed * .1, "each mixed component contributes measurable receiver radiance");
        components += alone;
    }
    for (std::size_t i = 0; i < 3; ++i)
        f.lights[i].intensity = intensities[i];
    f.frame.camera_cut = true;
    f.edited();
    std::printf("Shadow mixed radiance: together=%.4f sum-of-parts=%.4f\n", mixed, components);
    check(mixed < .9 && std::abs(mixed - components) <= mixed * .03,
          "point, sun and rectangle radiance accumulate exactly once each");

    // The visible wall is entirely inside radius .7, then entirely outside .1.
    // range=10 keeps both projections in far=16 and their face demand/tier
    // unchanged: the radius changes shading only, not cached depth.
    f.frame.camera.projection = {
        1 / (.2f * (31.f / 23)), 0, 0, 0, 0, 1 / .2f, 0, 0, 0, 0, -1 / 39.9f, 0, 0, 0, 40 / 39.9f, 1};
    f.room.meshes = {receiver(501, 0, .4f, .6f, .5f)};
    f.lights.assign(1, {});
    f.lights[0].stable_key = 50000;
    f.lights[0].position = {0, 0, 0};
    f.lights[0].range = 10;
    f.lights[0].intensity = 1;
    f.lights[0].source_radius = .7f;
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    const auto sphere_sample = [&](const char* name, render::Capture& image) {
        const auto result = f.step(&image);
        png(image, f.output / (std::string(name) + ".png"));
        const auto sample = completed(result.accepted_id);
        record(name, result.accepted_id, sample);
        check(sample.scene_lit_shadow_cache_missing_taps == 0 && sample.scene_lit_shadow_cache_dropped_lights == 0,
              "sphere-interior fixture has complete bounded shadow residency");
        return sample;
    };
    const auto sphere_region = project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0, 0, .4f}, 3);
    render::Capture inside_image;
    const auto inside_stats = sphere_sample("shadow-sphere-inside", inside_image);
    check(inside_stats.scene_lit_shadow_cache_faces > 0 && mean_rgb(inside_image, sphere_region)[0] > 10,
          "sphere-interior receivers keep cached visibility and direct energy");
    f.lights[0].source_radius = .1f;
    f.frame.camera_cut = true;
    f.edited();
    render::Capture outside_image;
    const auto outside_stats = sphere_sample("shadow-sphere-outside", outside_image);
    check(outside_stats.scene_lit_shadow_cache_faces == inside_stats.scene_lit_shadow_cache_faces &&
              outside_stats.scene_lit_shadow_cache_reused_faces == outside_stats.scene_lit_shadow_cache_faces &&
              outside_stats.scene_lit_shadow_cache_static_updates == 0 &&
              outside_stats.scene_lit_shadow_cache_dynamic_updates == 0 &&
              outside_stats.scene_lit_shadow_cache_draws == 0 && mean_rgb(outside_image, sphere_region)[0] > 10,
          "same-far radius shrink changes shading without redrawing cached depth");
    f.lights[0].source_radius = .7f;
    f.lights[0].casts_shadow = false;
    f.frame.camera_cut = true;
    f.edited();
    render::Capture unshadowed_image;
    const auto unshadowed_stats = sphere_sample("shadow-sphere-unshadowed", unshadowed_image);
    check(unshadowed_stats.scene_lit_shadow_cache_faces == 0 && mean_rgb(unshadowed_image, sphere_region)[0] > 10,
          "a non-casting sphere needs no atlas faces and still contributes");

    // A legal infinite-far reversed-Z perspective has an invertible projection
    // but w=0 when its z=0 far endpoint is unprojected. CSM must derive finite
    // coverage from current receivers instead of rejecting the Frame contract.
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    f.frame.camera.projection = {2 / (31.f / 23), 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, .1f, 0};
    f.room.meshes = {receiver(601, 0, 4, 6, 5)};
    f.lights.assign(1, {});
    f.lights[0].stable_key = 60000;
    f.lights[0].kind = render::LightKind::directional;
    f.lights[0].direction = {0, 0, 1};
    f.lights[0].intensity = 2;
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    stats = submit("shadow-csm-infinite-perspective-cold");
    check(stats.scene_lit_shadow_cache_faces == 4 && stats.scene_lit_shadow_cache_static_updates == 4,
          "infinite-far perspective initializes all four finite receiver-bounded cascades");
    const auto infinite_image = f.capture("shadow-csm-infinite-perspective-settled", 4);
    const auto infinite_region = project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0, 0, 4}, 3);
    check(mean_rgb(infinite_image, infinite_region)[0] > 20,
          "infinite-far perspective produces actual directional lighting on its projected receiver");
    truth("shadow-csm-infinite-perspective-truth", infinite_image, {}, 0, 0);
    stats = warm("shadow-csm-infinite-perspective-warm");
    check(stats.scene_lit_shadow_cache_faces == 4, "infinite-far perspective reuses all four complete cascades");

    // Independent far clipping limits receivers, not the scene's caster storage.
    // The only receiver is at z=4: far=3 removes it, far=5 restores its lighting.
    f.frame.camera.far_depth = .1f / 3;
    f.frame.camera_cut = true;
    stats = submit("shadow-csm-finite-clip-empty");
    const auto beyond_far = f.capture("shadow-csm-finite-clip-empty-image", 4);
    check(stats.scene_lit_shadow_cache_faces == 0 && stats.scene_instances == 1 &&
              std::ranges::all_of(beyond_far.depth, [](float z) { return z == 0; }),
          "finite far clips receivers without removing the scene instance");
    f.frame.camera.far_depth = .1f / 5;
    f.frame.camera_cut = true;
    stats = submit("shadow-csm-finite-clip-restored");
    const auto restored_far = f.capture("shadow-csm-finite-clip-restored-image", 4);
    check(stats.scene_lit_shadow_cache_faces == 4 && mean_rgb(restored_far, infinite_region)[0] > 20,
          "finite far change restores receiver-bounded directional lighting");
    f.frame.camera.far_depth = 0;

    // Cascades fit the measured receiver depth. A wall 3 m away hides a far
    // receiver at 1000 m, so the conservative receiver bound is 1024 m and its
    // first cascade has about 3.7 cm texels at the wall; the fitted [2, 4] m
    // range gives about 0.7 cm. A 10 cm bar 0.3 m in front of the wall casts a
    // 12 cm shadow beside its own image. One fitted texel of filter and normal
    // offset stays inside the 2.5 cm unjudged band; 3.7 cm texels do not.
    f.frame.scene_viewport = {0, 0, float(f.drawing.extent.width), float(f.drawing.extent.height)};
    const float fit_aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    f.frame.camera.projection = {2 / fit_aspect, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, .1f, 0};
    auto bar = blocker(702);
    bar.world = box(0, 0, 2.7f, .1f, 1.2f, .06f);
    const auto far_receiver = receiver(703, 0, 1000, 4000, 3000);
    const auto fit_scene = [&] { return std::vector{receiver(701, 0, 3, 8, 5), far_receiver, bar}; };
    f.room.meshes = fit_scene();
    f.lights.assign(1, {});
    f.lights[0].stable_key = 60100;
    f.lights[0].kind = render::LightKind::directional;
    f.lights[0].direction = normalize({.5f, 0, 1});
    f.lights[0].intensity = 2;
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    submit("shadow-fit-cold");
    const auto fitted = f.capture("shadow-fit-settled", 16);
    truth("shadow-fit-truth", fitted, {bounds(bar)}, .025, 1000);
    warm("shadow-fit-warm");
    const auto fallback_rays = [](const Stats& sample) {
        return sample.scene_lit_shadow_rays;
    }; // DDGI is off: direct rays only.
    // A dynamic cube 1.4 to 1.6 m away is the nearest receiver; its quantized
    // near bound alternates between two quarter octaves. The range holds the
    // nearer one, so after the first cycle the static layer never redraws.
    auto mover = blocker(704);
    const auto place = [&](float z) {
        mover.world = box(-1, -.5f, z, .2f, .2f, .2f);
        f.room.meshes = fit_scene();
        f.room.meshes.push_back(mover);
        f.edited();
    };
    place(1.7f);
    f.capture("shadow-fit-mover-added", 8);
    constexpr std::array<float, 8> path{1.5f, 1.55f, 1.6f, 1.65f, 1.7f, 1.65f, 1.6f, 1.55f};
    for (unsigned frame = 0; frame < 16; ++frame) {
        place(path[frame % 8]);
        f.step();
    }
    const auto hold = f.drawing.id + 1;
    unsigned held = 0;
    for (unsigned frame = 16; frame < 64; ++frame) {
        place(path[frame % 8]);
        const auto result = f.step();
        const auto& sample = result.stats;
        if (!sample.scene_lit_gpu_timing_valid || sample.scene_lit_gpu_sample_frame_id < hold)
            continue;
        ++held;
        record("shadow-fit-mover", result.accepted_id, sample);
        check(sample.scene_lit_shadow_cache_static_updates == 0 && sample.scene_lit_shadow_cache_dynamic_updates > 0 &&
                  fallback_rays(sample) == 0,
              "an oscillating nearest receiver redraws only the dynamic layer");
    }
    check(held >= 32, "oscillating receiver observes completed samples");
    // Removing the cube moves the near reading four quarter octaves inward.
    // The range contracts once, after 32 plans of that reading, which arrives
    // three frames late: one cascade redraw 32 to 40 frames after removal.
    f.room.meshes = fit_scene();
    f.edited();
    const auto removed = f.drawing.id + 1;
    std::vector<std::uint64_t> contractions;
    for (unsigned frame = 0; frame < 64; ++frame) {
        const auto result = f.step();
        const auto& sample = result.stats;
        const auto id = sample.scene_lit_gpu_sample_frame_id;
        if (!sample.scene_lit_gpu_timing_valid || id < removed)
            continue;
        record("shadow-fit-contract", result.accepted_id, sample);
        if (sample.scene_lit_shadow_cache_static_updates)
            contractions.push_back(id - removed);
    }
    check(contractions.size() == 1 && contractions[0] >= 32 && contractions[0] <= 40,
          "a receiver range moved inward contracts the cascades once, after 32 plans");
    // A camera cut starts a new depth epoch. Until its own reading arrives the
    // cascades use the conservative bound, never the previous view's [2, 4] m.
    f.frame.camera.view[14] = 20;
    f.frame.camera_cut = true;
    const auto cut = f.drawing.id + 1;
    unsigned after_cut = 0;
    for (unsigned frame = 0; frame < 16; ++frame) {
        const auto result = f.step();
        const auto& sample = result.stats;
        if (!sample.scene_lit_gpu_timing_valid || sample.scene_lit_gpu_sample_frame_id < cut)
            continue;
        ++after_cut;
        record("shadow-fit-cut", result.accepted_id, sample);
        check(fallback_rays(sample) == 0, "after a camera cut every receiver lies inside the cascades");
    }
    check(after_cut >= 8, "camera cut observes completed samples");
    f.frame.camera.view[14] = 0;
    f.frame.camera_cut = true;
    f.capture("shadow-fit-return", 16);
    // Removing the wall without a cut reveals the far receiver beyond the
    // fitted range until a new reading arrives. It is traced, not left black.
    f.room.meshes = {far_receiver};
    f.edited();
    render::Capture revealed;
    const auto revealed_id = f.step(&revealed).accepted_id;
    png(revealed, f.output / "shadow-fit-reveal-first.png");
    const auto reveal = completed(revealed_id);
    record("shadow-fit-reveal", revealed_id, reveal);
    check(reveal.scene_lit_shadow_cache_missing_taps == 0 && fallback_rays(reveal) > 0,
          "the revealed receiver beyond the fitted cascades traces its visibility");
    // The range grows on the first reading of the revealed frame.
    for (unsigned frame = 0; frame < 12; ++frame) {
        const auto result = f.step();
        const auto& sample = result.stats;
        const auto id = sample.scene_lit_gpu_sample_frame_id;
        if (!sample.scene_lit_gpu_timing_valid || id <= revealed_id)
            continue;
        record("shadow-fit-grow", result.accepted_id, sample);
        check(fallback_rays(sample) == 0 || id <= revealed_id + 5,
              "cascades grow over a revealed receiver as soon as its reading arrives");
    }
    const auto far_region = project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0, 0, 1000}, 8);
    const auto revealed_settled = f.capture("shadow-fit-reveal-settled", 64);
    const auto first_far = mean_rgb(revealed, far_region)[0];
    const auto settled_far = mean_rgb(revealed_settled, far_region)[0];
    check(first_far > 20 && std::abs(first_far - settled_far) <= 2, "the revealed receiver is lit on its first frame");
    truth("shadow-fit-reveal-truth", revealed_settled, {}, 0, 0);
    stats = warm("shadow-fit-reveal-warm");
    check(fallback_rays(stats) == 0, "settled cascades cover the revealed receiver without tracing");
    counters.flush();
    f.metrics.flush();
    std::puts(
        "Renderer shadow cache: PASS (exact completed samples, cold/warm, static/dynamic motion and deletion, Mask, light edits, scene replacement, geometric visibility truth, four CSM cascades with one shadow each including infinite-far perspective, cascades fitted to the measured receiver depth with held growth and 32-plan contraction, per-epoch readings and traced receivers beyond them, grazing near-caster normal offset, sixteen high-tier lamps, 24-face redraw budget with whole-lamp longest-waiting rotation, thirty arriving lights withheld until drawn then faded in, keyless lights outside the budget, deferred lights sampled from their drawn origin, unoccluded grazing receivers past their own horizon, whole-light atlas drops, exact mixed accumulation, sphere-interior receivers)");
}
