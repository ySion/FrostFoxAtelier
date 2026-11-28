void spot_sky_tonemap_and_pick_cases(Fixture& f) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;

    // 1. Spot light smooth cone falloff + 1-view perspective VSM static reuse + umbra occlusion:
    f.restore();
    f.frame.scene_viewport = {24.25f, 32.75f, 159.5f, 111.25f};
    f.frame.camera.view = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1}};
    const float aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.projection = {
        {1 / (3.2f * aspect), 0, 0, 0, 0, 1 / 3.2f, 0, 0, 0, 0, -1 / 39.9f, 0, 0, 0, 40 / 39.9f, 1}};
    f.frame.camera_cut = true;
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
    receiver.world = box(0, 0, 4, 14, 10, 1);
    receiver.color = {.5f, .5f, .5f, 1};
    receiver.material.roughness = 1.f;

    auto& blocker = f.room.meshes[1];
    blocker.stable_key = 2;
    blocker.geometry = atelier::geometry::cube_mesh();
    blocker.world = box(0, 0, 2, .45f, .45f, .2f);
    blocker.color = {.2f, .2f, .2f, 1};

    f.lights.assign(1, {});
    auto& spot = f.lights.front();
    spot.stable_key = 300;
    spot.kind = render::LightKind::spot;
    spot.position = {0, 0, -1};
    spot.direction = {0, 0, 1};
    spot.range = 12.f;
    spot.intensity = 55.f;
    spot.spot_inner_radians = 15.f * (std::numbers::pi_v<float> / 180.f);
    spot.spot_outer_radians = 28.f * (std::numbers::pi_v<float> / 180.f);
    spot.casts_shadow = true;
    f.edited();

    const auto spot_first = f.step().stats;
    check(spot_first.scene_lit_vsm_static_renders > 0 && spot_first.scene_lit_vsm_draws > 0,
          "spot light perspective VSM bakes initial static pages");
    render::Capture spot_cap;
    const auto spot_cached = f.step(&spot_cap).stats;
    png(spot_cap, f.output / "spot-vsm-cone.png");
    check(spot_cached.scene_lit_vsm_static_renders == 0 && spot_cached.scene_lit_vsm_dynamic_renders == 0 &&
              spot_cached.scene_lit_vsm_reused_pages > 0,
          "stationary spot light reuses static perspective VSM pages");

    const auto spot_inner_reg =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {1.0f, 0.0f, 4.0f}, 1);
    const auto spot_pen_reg =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {2.1f, 0.0f, 4.0f}, 1);
    const auto spot_out_reg =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {3.5f, 0.0f, 4.0f}, 1);
    const auto spot_umbra_reg =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {0.0f, 1.0f, 4.0f}, 1);
    // Blocker at (0,0,2) occludes (0,0,4), while (0,1,4) is unoccluded inner cone; check (0,0,4) via plane projection:
    const auto umbra_clip =
        (atelier::math::Vector4{0.f, 0.f, 4.f, 1.f} * f.frame.camera.view) * f.frame.camera.projection;
    const auto umbra_x = static_cast<unsigned>(f.frame.scene_viewport.x +
                                               (umbra_clip.x / umbra_clip.w * .5f + .5f) * f.frame.scene_viewport.width);
    const auto umbra_y = static_cast<unsigned>(f.frame.scene_viewport.y +
                                               (.5f - umbra_clip.y / umbra_clip.w * .5f) * f.frame.scene_viewport.height);
    // Note: camera is orthographic looking along +Z from z=-4, so blocker at (0,0,2) covers (0,0) in view!
    // Move spot slightly off-axis so blocker casts umbra onto visible receiver next to blocker:
    spot.position = {-1.5f, 0.f, -1.f};
    spot.direction = *atelier::math::try_normalize(Vec3{1.5f, 0.f, 5.f});
    f.edited();
    static_cast<void>(f.step());
    const auto offaxis_cap = f.capture("spot-vsm-umbra", 2);
    // Ray from (-1.5, 0, -1) through blocker (0, 0, 2) hits z=4 at x = -1.5 + (5/3)*1.5 = +1.0!
    const double unocc_lum = mean_rgb(spot_cap, spot_umbra_reg)[0];
    const double pen_lum = mean_rgb(spot_cap, spot_pen_reg)[0];
    const double out_lum = mean_rgb(spot_cap, spot_out_reg)[0];
    const double umbra_lum = mean_rgb(offaxis_cap, spot_inner_reg)[0];
    (void)umbra_x;
    (void)umbra_y;
    check(unocc_lum > 30.0 && pen_lum > 2.0 && pen_lum < unocc_lum * 0.80 && out_lum <= 1.0,
          "spot light attenuates smoothly between inner and outer cone angles and cuts off outside outer cone");
    check(umbra_lum <= 2.0, "spot light VSM & shadow ray occlude receiver in blocker umbra");

    // 2. Physical Atmosphere sky mode (`SkyMode::physical_atmosphere`):
    f.restore();
    auto& sky_opts = f.frame.view.lit;
    sky_opts = {};
    sky_opts.indirect_lighting = sky_opts.ssilvb = sky_opts.reflections = sky_opts.bloom = sky_opts.shadows = false;
    sky_opts.antialiasing = render::Antialiasing::none;
    sky_opts.sky_mode = atelier::rendering::SkyMode::physical_atmosphere;
    sky_opts.sky_intensity = 1.0f;
    f.room.meshes.assign(1, {});
    f.room.meshes[0].geometry = atelier::geometry::cube_mesh();
    f.room.meshes[0].world = box(0, -30, 0, .1f, .1f, .1f);
    f.room.meshes[0].stable_key = 1;
    f.lights.assign(1, {});
    f.lights[0].stable_key = 301;
    f.lights[0].kind = render::LightKind::directional;
    f.lights[0].direction = *atelier::math::try_normalize(Vec3{0.25f, -0.45f, 0.85f});
    f.lights[0].intensity = 3.0f;
    f.edited();
    const float vp_aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera = [&] {
        render::Camera c;
        c.view = *atelier::math::try_look_at_lh(Vec3{0, 1.5f, 0}, Vec3{0, 3.5f, 6.f}, Vec3{0, 1, 0});
        c.projection = {{1.4f / vp_aspect, 0, 0, 0, 0, 1.4f, 0, 0, 0, 0, -.1f / 39.9f, 1, 0, 0, 4 / 39.9f, 0}};
        return c;
    }();
    f.frame.camera_cut = true;
    const auto sky_cap = f.capture("physical-atmosphere-sky", 2);
    const unsigned sky_cx = static_cast<unsigned>(f.frame.scene_viewport.x + 0.5f * f.frame.scene_viewport.width);
    const unsigned sky_top_y = static_cast<unsigned>(f.frame.scene_viewport.y + 0.2f * f.frame.scene_viewport.height);
    const auto* zenith_px = pixel(sky_cap, sky_cx, sky_top_y);
    check(zenith_px[2] > zenith_px[0] + 15 && zenith_px[2] > 40,
          "physical atmosphere produces Rayleigh blue-shifted sky radiance");

    // 3. Tonemapper (`Tonemapper::pbr_neutral` vs `Tonemapper::agx`) + 4. GPU Picking (`pick_pixel` & `pick_box`):
    f.restore();
    auto& pick_opts = f.frame.view.lit;
    pick_opts = {};
    pick_opts.indirect_lighting = pick_opts.ssilvb = pick_opts.reflections = pick_opts.bloom = pick_opts.shadows = false;
    pick_opts.antialiasing = render::Antialiasing::none;
    f.lights.clear();
    f.frame.environment = {0.15f, 0.15f, 0.15f};
    f.room.meshes.assign(2, {});
    f.room.meshes[0].stable_key = 10;
    f.room.meshes[0].geometry = atelier::geometry::cube_mesh();
    f.room.meshes[0].world = box(-1.4f, 0.8f, 3.0f, 1.2f, 1.2f, 1.2f);
    f.room.meshes[0].color = {0.8f, 0.2f, 0.1f, 1.0f};
    f.room.meshes[0].material.emissive = {9.0f, 1.2f, 0.05f};

    f.room.meshes[1].stable_key = 11;
    f.room.meshes[1].geometry = atelier::geometry::cube_mesh();
    f.room.meshes[1].world = box(1.4f, 0.8f, 3.0f, 1.2f, 1.2f, 1.2f);
    f.room.meshes[1].color = {0.1f, 0.5f, 0.85f, 1.0f};
    f.room.meshes[1].material.emissive = {0.1f, 2.5f, 8.0f};
    f.edited();

    pick_opts.tonemapper = atelier::rendering::Tonemapper::pbr_neutral;
    const auto pbr_cap = f.capture("tonemap-pbr-neutral", 2);
    pick_opts.tonemapper = atelier::rendering::Tonemapper::agx;
    const auto agx_cap = f.capture("tonemap-agx", 2);

    const auto left_reg =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {-1.4f, 0.8f, 2.4f}, 2);
    const auto right_reg =
        project_region(f.frame.camera, f.frame.scene_viewport, f.drawing.extent, {1.4f, 0.8f, 2.4f}, 2);
    const auto pbr_rgb = mean_rgb(pbr_cap, left_reg);
    const auto agx_rgb = mean_rgb(agx_cap, left_reg);
    check(pbr_rgb[0] > 180.0 && agx_rgb[0] > 180.0 &&
              (std::abs(agx_rgb[1] - pbr_rgb[1]) > 2.0 || std::abs(agx_rgb[2] - pbr_rgb[2]) > 2.0),
          "AgX and Khronos PBR Neutral produce distinct highlight chroma compression");

    const std::uint32_t px_left = (left_reg.left + left_reg.right) / 2u;
    const std::uint32_t py_left = (left_reg.top + left_reg.bottom) / 2u;
    const std::uint32_t px_right = (right_reg.left + right_reg.right) / 2u;
    const std::uint32_t py_right = (right_reg.top + right_reg.bottom) / 2u;
    const std::uint32_t px_bg = static_cast<std::uint32_t>(f.frame.scene_viewport.x + 8.f);
    const std::uint32_t py_bg = static_cast<std::uint32_t>(f.frame.scene_viewport.y + 8.f);

    const auto pick0 = take(f.renderer.pick_pixel(px_left, py_left));
    const auto pick1 = take(f.renderer.pick_pixel(px_right, py_right));
    const auto pick_empty = take(f.renderer.pick_pixel(px_bg, py_bg));
    check(pick0.has_value() && pick0->instance_index == 0u && pick0->depth > 0.f,
          "GPU pick_pixel identifies visible instance 0 and its hardware depth");
    check(pick1.has_value() && pick1->instance_index == 1u && pick1->depth > 0.f,
          "GPU pick_pixel identifies visible instance 1 and its hardware depth");
    check(!pick_empty.has_value(), "GPU pick_pixel returns empty on background sky pixel");

    const auto box_hits = take(f.renderer.pick_box(std::min(px_left, px_right) - 4u, std::min(py_left, py_right) - 4u,
                                                   std::max(px_left, px_right) + 4u, std::max(py_left, py_right) + 4u));
    check(box_hits == atelier::Array<std::uint32_t>{0u, 1u}, "GPU pick_box returns sorted visible instance indices");

    std::puts("Renderer spot VSM, physical sky, AgX/PBR-Neutral tonemap & GPU pick: PASS");
}
