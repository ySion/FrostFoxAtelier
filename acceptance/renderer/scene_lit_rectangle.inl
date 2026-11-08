// One area-light image, judged by BRDF quadrature rather than another render.
void rectangle_cases(Fixture& f) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    f.restore();
    f.frame.scene_viewport = {23.25f, 32.75f, 95.5f, 65.25f};
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    const float aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.projection = {1 / (1.5f * aspect), 0, 0, 0, 0,          1 / 1.5f, 0, 0, 0, 0,
                                 -1 / 39.9f,          0, 0, 0, 40 / 39.9f, 1};
    f.frame.camera_cut = true;
    f.frame.environment = {};
    auto& options = f.frame.view.lit;
    options = {};
    options.ddgi = options.gtao = options.sssr = options.bloom = options.shadows = false;
    options.antialiasing = render::Antialiasing::none;
    f.room.meshes.assign(1, {});
    auto& plane = f.room.meshes.front();
    plane.stable_key = 1;
    plane.world = box(0, 0, 0, 1, 1, 1);
    plane.geometry = take(Mesh::create(std::vector<Vertex>{{{-6, -4, 4}, {0, 0, -1}},
                                                           {{6, -4, 4}, {0, 0, -1}},
                                                           {{6, 4, 4}, {0, 0, -1}},
                                                           {{-6, -4, 4}, {0, 0, -1}},
                                                           {{6, 4, 4}, {0, 0, -1}},
                                                           {{-6, 4, 4}, {0, 0, -1}}}));
    plane.color = {.5f, .5f, .5f, 1};
    plane.material = {.roughness = .5f, .metallic = 0};
    f.lights.assign(1, {.stable_key = 100, .kind = render::LightKind::rectangle});
    auto& light = f.lights.front();
    light.position = {.3f, -.2f, 3.4f};
    light.width = 1.2f;
    light.height = .6f;
    light.range = 12;
    light.direction = {std::sin(.5f), 0, std::cos(.5f)};
    light.right = {0, 1, 0};
    light.up = {std::cos(.5f), 0, -std::sin(.5f)};
    light.intensity = 4;
    f.edited();
    const auto image = f.capture("area-dielectric", 2);
    const auto result = analytic_rectangle_plane(f.frame, image, light, 4, {.5, .5, .5}, .5, 0, 96, .25, .005);
    f.metrics << "rectangle-area-dielectric," << result.max_error << ",0," << result.failures << ',' << result.pixels
              << '\n';
    check(result.pixels > 1000 && result.failures == 0,
          "rectangle radiance matches the quadrature truth on a rough dielectric");
    std::puts("Renderer rectangle light: PASS (area-dielectric quadrature truth)");
}
