// Real Renderer snapshots exercise the shared source table, its world-space
// proposal grid (DDGI hit lighting) and the stable-key lifecycle. Screen direct
// light is deterministic, so equal configurations must give equal receivers.
void light_set_cases(Fixture& f) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    std::ofstream evidence(f.output / "light-set-energy.csv");
    evidence << "case,r,g,b\n";
    check(evidence.good(), "light set energy evidence opened");
    std::ofstream spatial_evidence(f.output / "light-set-spatial.csv");
    spatial_evidence << "case,frame_id,retained_sources,nonempty_cells,logical_bytes,uploaded,history_resets\n";
    check(spatial_evidence.good(), "light set spatial lifecycle evidence opened");
    f.frame.scene_viewport = {24, 32, 320, 180};
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    const float aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.projection = {1 / (2.5f * aspect), 0, 0, 0, 0,          1 / 2.5f, 0, 0, 0, 0,
                                 -1 / 39.9f,          0, 0, 0, 40 / 39.9f, 1};
    f.frame.environment = {};
    f.frame.view.shading = render::Shading::scene_lit;
    f.frame.lighting_focus = {};
    auto& options = f.frame.view.lit;
    options = {};
    options.ddgi = options.gtao = options.sssr = options.bloom = false;
    options.shadows = true;
    options.antialiasing = render::Antialiasing::none;
    f.room.meshes.assign(1, {});
    auto& plane = f.room.meshes.front();
    plane.stable_key = 1;
    plane.world = box(0, 0, 0, 1, 1, 1);
    plane.geometry = std::make_shared<const Mesh>(std::vector<Vertex>{{{-6, -4, 4}, {0, 0, -1}},
                                                                      {{6, -4, 4}, {0, 0, -1}},
                                                                      {{6, 4, 4}, {0, 0, -1}},
                                                                      {{-6, -4, 4}, {0, 0, -1}},
                                                                      {{6, 4, 4}, {0, 0, -1}},
                                                                      {{-6, 4, 4}, {0, 0, -1}}});
    plane.color = {.5f, .5f, .5f, 1};
    plane.material = {.roughness = 1};
    f.lights.assign(272, {});
    for (std::size_t i = 0; i < f.lights.size(); ++i) {
        auto& light = f.lights[i];
        light.stable_key = 100 + i;
        light.position = {-2.f + .04f * float(i % 8), 2.f + .04f * float((i / 8) % 8), .5f};
        light.color = {1.f, .7f + .025f * float(i % 8), .6f + .03f * float(i % 8)};
        light.intensity = 90.f / 272;
        light.range = 30;
        light.source_radius = 0;
        light.casts_shadow = true;
    }
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    const auto sources = [&](std::size_t expected) {
        const auto stats = f.step().stats;
        check(stats.scene_lit_light_sources == expected && stats.scene_lit_shadow_sources == expected,
              "all sources beyond 256 and all shadow casters beyond four reach the ordinary frame without fallback");
        return stats;
    };
    const auto spatial = [&](const char* name, const render::FrameStats& stats, unsigned retained, unsigned cells) {
        spatial_evidence << name << ',' << f.drawing.id << ',' << stats.scene_lit_spatial_sources << ','
                         << stats.scene_lit_spatial_cells << ',' << stats.scene_lit_spatial_bytes << ','
                         << stats.scene_lit_spatial_changed << ',' << stats.scene_lit_history_resets << '\n';
        spatial_evidence.flush();
        check(stats.scene_lit_spatial_sources == retained && stats.scene_lit_spatial_cells == cells,
              "spatial proposal publishes the expected retained sources and nonempty bins");
        check(retained
                  ? stats.scene_lit_spatial_bytes >= 32768u + 16u * retained &&
                        stats.scene_lit_spatial_bytes <= 32768ull + 16ull * 65536 + 4ull * stats.scene_lit_light_sources
                  : stats.scene_lit_spatial_bytes == (f.room.meshes.empty() ? 0u : 36u),
              "spatial GPU tables remain bounded, empty proposals use sentinels, and empty scenes retire them");
    };
    const auto receiver = [&](const char* name, Vec3 point) {
        const auto region = project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, point, 10);
        const auto value = lighting_mean(f, name, region, 4, 2);
        evidence << name;
        for (auto channel : value)
            evidence << ',' << channel;
        evidence << '\n';
        evidence.flush();
        return value;
    };
    const auto same = [](const std::array<double, 3>& a, const std::array<double, 3>& b, const char* message) {
        for (unsigned channel = 0; channel < 3; ++channel)
            check(std::abs(a[channel] - b[channel]) <= .5, message);
    };
    spatial("top32-overflow", sources(272), 32, 1);
    f.settle_shadows(f.drawing.id);
    const auto initial = receiver("lights-272-sources", {0, 0, 4});
    check(initial[0] > 10, "light set comparison samples a lit receiver");
    options.spatial_light_sampling = false;
    same(initial, receiver("lights-272-hit-proposals-global", {0, 0, 4}),
         "the DDGI hit proposal diagnostic leaves screen direct lighting unchanged");
    options.spatial_light_sampling = true;
    auto stable = f.step().stats;
    spatial("stable", stable, 32, 1);
    check(!stable.scene_lit_spatial_changed, "unchanged source grid does not upload again after the diagnostic toggle");
    const auto stable_bytes = stable.scene_lit_spatial_bytes;
    f.frame.lighting_focus = {.5f, .5f, .5f};
    stable = f.step().stats;
    spatial("same-cell-focus", stable, 32, 1);
    check(!stable.scene_lit_spatial_changed && stable.scene_lit_spatial_bytes == stable_bytes &&
              stable.scene_lit_history_resets == 0,
          "focus movement within the snapped cell preserves spatial tables and history");
    f.frame.lighting_focus = {4096, 4096, 4096};
    const auto away = f.step().stats;
    spatial("focus-away", away, 0, 0);
    check(away.scene_lit_spatial_changed && away.scene_lit_history_resets == 0,
          "rolling focus outside all sources uploads an empty proposal without resetting the scene domain");
    same(initial, receiver("lights-focus-away", {0, 0, 4}), "a receiver outside the proposal grid keeps every light");
    f.frame.lighting_focus = {};
    const auto returned = f.step().stats;
    spatial("focus-return", returned, 32, 1);
    check(returned.scene_lit_spatial_changed && returned.scene_lit_history_resets == 0,
          "returning focus rebuilds spatial tables without resetting the scene domain");
    const auto original_position = f.lights.back().position;
    f.lights.back().position = {9, 2, .5f};
    f.edited();
    const auto moved = f.step().stats;
    spatial("light-cross-cell", moved, 33, 2);
    check(moved.scene_lit_spatial_changed && moved.scene_lit_history_resets == 0,
          "moving one light between bins updates the proposal while preserving the scene domain");
    f.lights.back().position = original_position;
    f.edited();
    spatial("light-return", f.step().stats, 32, 1);
    std::ranges::reverse(f.lights);
    f.edited();
    sources(272);
    same(initial, receiver("lights-reordered", {0, 0, 4}), "source reorder preserves the receiver exactly");
    const auto removed = f.lights.back();
    f.lights.pop_back();
    auto inserted = removed;
    inserted.stable_key = 9000;
    inserted.position = {1, -1, 0};
    inserted.color = {.2f, .4f, 1};
    inserted.intensity = 40;
    f.lights.insert(f.lights.begin() + 37, inserted);
    f.edited();
    sources(272);
    const auto replaced = receiver("lights-delete-insert", {0, 0, 4});
    check(replaced[2] > initial[2] + 4, "an inserted blue light appears in the same frame as its edit");
    for (unsigned i = 0; i < 16; ++i) {
        f.lights[37].position[0] += .1f;
        f.room.meshes[0].world[14] += .0625f;
        f.edited();
        check(f.step().stats.scene_lit_history_resets == 0,
              "light and receiver motion keep their established scene domain");
    }
    // Every casting light keeps an atlas shadow. A screen-offset blocker shades
    // a still-visible receiver, including sources above the old 256 cap. The
    // receiver shrinks to 2x2 m and the blocker sits near it, so each light needs
    // only its +Z, +X and -Y faces: 271x3 minimum-size faces fit the 1024 cells.
    render::MeshInstance blocker;
    blocker.stable_key = 2;
    blocker.geometry = atelier::geometry::cube_mesh();
    blocker.world = box(-.6f, .6f, 3, 1, 1, .4f);
    blocker.color = {.5f, .5f, .5f, 1};
    f.room.meshes.push_back(blocker);
    f.room.meshes[0].world = box(0, 0, 0, 1 / 6.f, .25f, 1);
    f.lights.erase(f.lights.begin() + 37);
    f.edited();
    sources(271);
    const auto shadow_region = project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0, 0, 4}, 3);
    options.shadows = false;
    const auto unshadowed = lighting_mean(f, "lights-all-sources-no-shadow", shadow_region, 4, 2);
    options.shadows = true;
    f.settle_shadows();
    const auto shadowed = lighting_mean(f, "lights-all-sources-shadow", shadow_region, 4, 2);
    check(f.step().stats.scene_lit_shadow_cache_dropped_lights == 0,
          "271 three-face point shadows fit the atlas without dropping a light");
    check(unshadowed[0] > 10 && shadowed[0] < unshadowed[0] * .2,
          "more than 256 local sources retain geometric shadows instead of contributing unshadowed fallback");
    f.room.meshes[0].world = box(0, 0, 0, 1, 1, 1);
    f.lights.clear();
    f.edited();
    spatial("zero-sources", sources(0), 0, 0);
    const auto dark = lighting_mean(f, "lights-all-sources-deleted", shadow_region, 1, 2);
    check(*std::max_element(dark.begin(), dark.end()) == 0,
          "deleting the complete source set removes its radiance in the same frame");

    // A source whose anchor lies outside the grid still illuminates the receiver,
    // and a nearby light whose range cannot reach it adds exactly nothing.
    f.room.meshes.resize(1);
    render::Light distant;
    distant.stable_key = 12000;
    distant.position = {0, 0, -80};
    distant.intensity = 6000;
    distant.range = 200;
    distant.source_radius = 0;
    distant.casts_shadow = true;
    f.lights = {distant};
    f.edited();
    spatial("far-source", sources(1), 0, 0);
    const auto far_only = receiver("lights-far-source", {0, 0, 4});
    check(far_only[0] > 10, "a light anchored outside the proposal grid illuminates the receiver");
    render::Light nearby;
    nearby.stable_key = 12001;
    nearby.position = {1, 1, .5f};
    nearby.intensity = 90;
    nearby.range = 1;
    nearby.source_radius = 0;
    nearby.casts_shadow = true;
    f.lights.push_back(nearby);
    f.edited();
    spatial("near-zero-far-main", sources(2), 1, 1);
    same(far_only, receiver("lights-near-zero-far-main", {0, 0, 4}),
         "an out-of-range nearby light adds no receiver energy");
    const auto receiver_copy = f.room.meshes.front();
    f.room.meshes.clear();
    f.lights.clear();
    f.edited();
    render::Capture empty;
    spatial("scene-release", f.step(&empty).stats, 0, 0);
    check(std::ranges::all_of(empty.depth, [](float value) { return value == 0; }),
          "empty scene retires spatial resources and captures background depth through Renderer");
    f.room.meshes = {receiver_copy};
    distant.position = {0, 0, .5f};
    distant.intensity = 90;
    f.lights = {distant};
    f.edited();
    spatial("scene-rebuild", sources(1), 1, 1);
    check(receiver("lights-spatial-scene-rebuild", {0, 0, 4})[0] > 10, "a rebuilt scene lights its receiver again");

    // Sunlight is evaluated separately at DDGI hits. Adding an irrelevant local
    // pool must not amplify the sun by that pool's inverse selection probability.
    f.room = Room{};
    f.lights = {room_lights()[0]};
    f.lights[0].source_radius = 0;
    f.room.meshes[0].material = {.roughness = 1};
    f.room.meshes[7].material.emissive = {};
    f.frame.camera = camera({320, 180});
    options = {};
    options.gtao = options.sssr = options.bloom = false;
    options.antialiasing = render::Antialiasing::none;
    options.probe_volumes[0].minimum = {-5, -1, -1};
    options.probe_volumes[0].maximum = {5, 5, 8};
    options.probe_volumes[0].counts = {8, 5, 8};
    options.probe_update_budget = 320;
    options.probe_rays = 64;
    f.frame.environment = {};
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    const auto bounce_region =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {-3.15f, .01f, 1.f}, 2);
    options.ddgi = false;
    const auto direct = lighting_mean(f, "lights-ddgi-sun-direct", bounce_region, 32, 16);
    options.ddgi = true;
    const auto sun = lighting_mean(f, "lights-ddgi-sun-only", bounce_region, 192, 24);
    for (unsigned i = 0; i < 96; ++i) {
        render::Light irrelevant;
        irrelevant.stable_key = 1000 + i;
        irrelevant.position = {1000.f + float(i), 1000, 1000};
        irrelevant.range = 1;
        irrelevant.intensity = 100;
        irrelevant.source_radius = 0;
        irrelevant.casts_shadow = true;
        f.lights.push_back(irrelevant);
    }
    ++f.frame.scene_identity;
    f.edited();
    sources(97);
    const auto with_locals = lighting_mean(f, "lights-ddgi-sun-96-irrelevant-locals", bounce_region, 192, 24);
    evidence << "ddgi-sun-with-96-irrelevant-locals";
    for (auto value : with_locals)
        evidence << ',' << value;
    evidence << '\n';
    check(sun[0] - direct[0] > 2, "sun-only fixture contains measurable DDGI bounce energy");
    for (unsigned channel = 0; channel < 3; ++channel)
        check(std::abs(with_locals[channel] - sun[channel]) <=
                  std::max(3., std::abs(sun[channel] - direct[channel]) * .2),
              "96 noncontributing local lights preserve the sun's converged DDGI energy");
    check(evidence.good() && spatial_evidence.good(), "light set lifecycle and DDGI energy evidence written");
    std::puts(
        "Renderer light set: PASS (272 shadow sources, spatial top32/rolling/lifetime, exact reorder/insert/delete/focus, out-of-range lights, sun-only DDGI energy)");
}
