// One point-light image: authored receiver plane and segment/AABB occlusion.
void shadow_cases(Fixture& f) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    f.restore();
    f.frame.scene_viewport = {24.25f, 32.75f, 159.5f, 111.25f};
    f.frame.camera.view = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1}};
    const float aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.projection = {
        {1 / (2.5f * aspect), 0, 0, 0, 0, 1 / 2.5f, 0, 0, 0, 0, -1 / 39.9f, 0, 0, 0, 40 / 39.9f, 1}};
    f.frame.environment = {};
    auto& options = f.frame.view.lit;
    options = {};
    options.indirect_lighting = options.ssilvb = options.reflections = options.bloom = false;
    options.shadows = true;
    options.antialiasing = render::Antialiasing::none;
    f.room.meshes.assign(2, {});
    auto& receiver = f.room.meshes[0];
    receiver.stable_key = 1;
    receiver.geometry = take(Mesh::create(atelier::Array<Vertex>{{{-.5f, -.5f, 0}, {0, 0, -1}},
                                                                 {{.5f, -.5f, 0}, {0, 0, -1}},
                                                                 {{.5f, .5f, 0}, {0, 0, -1}},
                                                                 {{-.5f, -.5f, 0}, {0, 0, -1}},
                                                                 {{.5f, .5f, 0}, {0, 0, -1}},
                                                                 {{-.5f, .5f, 0}, {0, 0, -1}}}));
    receiver.world = box(0, 0, 4, 12, 8, 1);
    receiver.color = {.5f, .5f, .5f, 1};
    receiver.material.roughness = 1;
    auto& blocker = f.room.meshes[1];
    blocker.stable_key = 2;
    blocker.geometry = atelier::geometry::cube_mesh();
    blocker.world = box(-1, .5f, 2, .5f, .6f, .2f);
    blocker.color = {.3f, .3f, .3f, 1};
    f.lights.assign(1, {});
    auto& light = f.lights.front();
    light.stable_key = 100;
    light.position = {-2, 1, 0};
    light.range = 10;
    light.intensity = 35;
    f.edited();
    const auto first_vsm = f.step().stats;
    check(first_vsm.scene_lit_vsm_static_renders > 0 && first_vsm.scene_lit_vsm_draws > 0,
          "initial VSM frame bakes static physical pages");
    const auto cached_vsm = f.step().stats;
    check(cached_vsm.scene_lit_vsm_static_renders == 0 &&
              cached_vsm.scene_lit_vsm_dynamic_renders == 0 &&
              cached_vsm.scene_lit_vsm_reused_pages > 0,
          "stationary scene reuses baked static VSM pages with zero redraws");

    const auto image = f.capture("shadow-point-settled", 18);
    const auto bright_region =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {1.8f, -.8f, 4}, 2);
    check(mean_rgb(image, bright_region)[0] > 20, "unoccluded authored receiver is lit");
    constexpr Rgb3 center{-1, .5, 2};
    constexpr Rgb3 half{.25, .3, .1};
    constexpr double margin = .03;
    std::size_t occluded = 0;
    std::size_t failures = 0;
    plane_pixels(f.frame, image, 4, [&](unsigned x, unsigned y, const Rgb3& p, const Rgb3&) {
        // Exclude the camera-facing blocker footprint using author geometry,
        // not the captured depth. The remaining points belong to the z=4 plane.
        if (std::abs(p[0] - center[0]) <= half[0] + margin && std::abs(p[1] - center[1]) <= half[1] + margin)
            return;
        double t_in = 1e-4;
        double t_out = 1;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const double low = center[axis] - half[axis] + margin;
            const double high = center[axis] + half[axis] - margin;
            const double direction = light.position[axis] - p[axis];
            if (std::abs(direction) < 1e-12) {
                if (p[axis] < low || p[axis] > high)
                    return;
                continue;
            }
            double t0 = (low - p[axis]) / direction;
            double t1 = (high - p[axis]) / direction;
            if (t0 > t1)
                std::swap(t0, t1);
            t_in = std::max(t_in, t0);
            t_out = std::min(t_out, t1);
        }
        if (t_in <= t_out) {
            ++occluded;
            const auto* color = pixel(image, x, y);
            failures += std::max(color[0], std::max(color[1], color[2])) > 2;
        }
    });
    check(occluded >= 64 && failures == 0, "geometrically occluded receiver is black outside the silhouette margin");

    // Verify Dynamic Copy-on-Write VSM update when continuously moving blocker over stationary static receiver:
    blocker.world = box(-.95f, .5f, 2, .5f, .6f, .2f);
    f.edited();
    f.step(); // transition frame: evicts blocker from static base into dynamic CoW
    blocker.world = box(-1.0f, .5f, 2, .5f, .6f, .2f);
    f.edited();
    const auto cow_vsm = f.step().stats;
    check(cow_vsm.scene_lit_vsm_static_renders == 0 && cow_vsm.scene_lit_vsm_dynamic_renders > 0 &&
              cow_vsm.scene_lit_vsm_reused_pages > 0,
          "moving blocker updates VSM via Copy-on-Write without re-rendering static receiver pages");

    // Verify finite-radius sphere light under non-temporal SMAA stays byte-identical (zero 1-spp penumbra boiling):
    light.source_radius = .05f;
    options.antialiasing = render::Antialiasing::smaa;
    f.edited();
    for (unsigned settle = 0; settle < 4; ++settle)
        f.step();
    render::Capture smaa_a;
    render::Capture smaa_b;
    f.step(&smaa_a);
    f.step(&smaa_b);
    check(smaa_a.rgba == smaa_b.rgba, "finite-radius light shadow under SMAA is deterministic across frames");
    check(mean_rgb(smaa_a, bright_region)[0] > 20, "finite-radius sphere light illuminates unoccluded receiver");

    // Verify wide Rectangle area light (`width = 3.0m` at z=-5 -> receiver z_v = 9.0m) wraps around narrow blocker
    // (`0.9m` at z=-3 -> z_occ = 2.0m) via `PenumbraQueue` without stamping a false hard-umbra core:
    options.antialiasing = render::Antialiasing::none;
    blocker.world = box(-2.0f, 0.0f, -3.0f, 0.9f, 1.0f, 0.2f);
    light = {};
    light.stable_key = 101;
    light.kind = render::LightKind::rectangle;
    light.position = {-2.5f, 0.0f, -5.0f};
    light.direction = {0.0f, 0.0f, 1.0f};
    light.right = {1.0f, 0.0f, 0.0f};
    light.up = {0.0f, 1.0f, 0.0f};
    light.width = 3.0f;
    light.height = 1.0f;
    light.range = 18.0f;
    light.intensity = 90.0f;
    light.casts_shadow = false;
    f.edited();
    render::Capture rect_unocc_cap;
    f.step(&rect_unocc_cap);
    light.casts_shadow = true;
    f.edited();
    for (unsigned settle = 0; settle < 4; ++settle)
        f.step();
    render::Capture rect_cap;
    f.step(&rect_cap);
    const auto rect_pen_region =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {-.25f, 0.0f, 4.0f}, 2);
    const auto rect_lit_region =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {2.0f, 0.0f, 4.0f}, 2);
    const double rect_unocc_lum = mean_rgb(rect_unocc_cap, rect_pen_region)[0];
    const double rect_pen_lum = mean_rgb(rect_cap, rect_pen_region)[0];
    check(rect_unocc_lum > 40.0 && rect_pen_lum > 10.0 && rect_pen_lum < rect_unocc_lum * 0.90,
          "wide rectangle area light produces wrapped soft penumbra instead of false hard umbra");

    // Verify 4-Cascade Directional Sun VSM static reuse & geometric umbra occlusion:
    blocker.world = box(-2.0f, 0.0f, 0.0f, 0.8f, 1.0f, 0.2f);
    light = {};
    light.stable_key = 102;
    light.kind = render::LightKind::directional;
    light.direction = {0.4472136f, 0.0f, 0.8944272f};
    light.intensity = 6.0f;
    light.casts_shadow = true;
    f.edited();
    f.frame.camera_cut = true;
    const auto sun_first = f.step().stats;
    f.frame.camera_cut = false;
    check(sun_first.scene_lit_vsm_static_renders > 0, "directional sun VSM bakes initial cascade pages");
    render::Capture sun_cap;
    const auto sun_second = f.step(&sun_cap).stats;
    check(sun_second.scene_lit_vsm_static_renders == 0 &&
              sun_second.scene_lit_vsm_dynamic_renders == 0 &&
              sun_second.scene_lit_vsm_reused_pages > 0,
          "stationary directional sun VSM reuses static cascade pages");
    const auto sun_umbra_region =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0.0f, 0.0f, 4.0f}, 1);
    check(mean_rgb(sun_cap, rect_lit_region)[0] > 20.0 && mean_rgb(sun_cap, sun_umbra_region)[0] <= 2.0,
          "directional sun VSM illuminates unoccluded receiver and occludes geometric umbra");

    std::puts("Renderer shadow: PASS (point, SMAA sphere, rectangle wrap penumbra & directional clipmap VSM)");
}
