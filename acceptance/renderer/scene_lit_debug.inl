void probe_debug_cases(Fixture& f) {
    f.room.meshes.resize(1);
    auto& receiver = f.room.meshes.front();
    receiver.world = box(0, 1, 4.1f, 10, 8, .2f);
    receiver.color = {.4f, .4f, .4f, 1};
    receiver.material = {.roughness = .5f, .metallic = 1};
    f.lights.clear();
    f.frame.environment = {0, 0, 0};
    f.edited();
    auto& options = f.frame.view.lit;
    options = {};
    options.gtao = options.sssr = options.bloom = false;
    options.antialiasing = render::Antialiasing::smaa;
    options.probe_volumes[0].minimum = {-2, -1, 0};
    options.probe_volumes[0].maximum = {2, 3, 6};
    options.probe_volumes[0].counts = {3, 3, 3};
    options.probe_update_budget = 27;
    options.probe_rays = 64;
    options.probe_debug = true;
    const auto dark_region = f.region({0, 1, 4});
    const auto outside_region = f.region({3, 1, 4});
    const auto coverage = f.capture("debug-02-coverage-dark-outside", 32);
    const auto dark = mean_rgb(coverage, dark_region);
    const auto outside = mean_rgb(coverage, outside_region);
    check(dark[1] > dark[0] + 70 && dark[2] > dark[0] + 70, "valid zero-irradiance receiver support is cyan");
    check(outside[2] > outside[1] + 90 && outside[2] > outside[0] + 120, "receiver outside the real volume is blue");
    std::puts("Renderer probe diagnostics: PASS (zero-irradiance cyan, outside-volume blue)");
}
