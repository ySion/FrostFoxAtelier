// One point-light image: authored receiver plane and segment/AABB occlusion.
void shadow_cases(Fixture& f) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    f.restore();
    f.frame.scene_viewport = {24.25f, 32.75f, 159.5f, 111.25f};
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    const float aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.projection = {1 / (2.5f * aspect), 0, 0, 0, 0,          1 / 2.5f, 0, 0, 0, 0,
                                 -1 / 39.9f,          0, 0, 0, 40 / 39.9f, 1};
    f.frame.environment = {};
    auto& options = f.frame.view.lit;
    options = {};
    options.ddgi = options.gtao = options.sssr = options.bloom = false;
    options.shadows = true;
    options.antialiasing = render::Antialiasing::none;
    f.room.meshes.assign(2, {});
    auto& receiver = f.room.meshes[0];
    receiver.stable_key = 1;
    receiver.geometry = take(Mesh::create(std::vector<Vertex>{{{-.5f, -.5f, 0}, {0, 0, -1}},
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
    const auto image = f.capture("shadow-point-settled", 20);
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
    std::puts("Renderer shadow: PASS (authored point-light occlusion)");
}
