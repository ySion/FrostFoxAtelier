void selection_depth_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    ui::Frame drawing{.id = 18000, .extent = extent, .draws = {}};
    render::MeshInstance mesh;
    mesh.stable_key = 1;
    mesh.selected = true;
    mesh.color = {.06f, .06f, .06f, 1};
    mesh.material.emissive = {.01f, .01f, .01f};
    mesh.world = {1.071f, 0, 0, 0, 0, .931f, 0, 0, 0, 0, 1.301f, 0, -.731f, .427f, 2.173f, 1};
    mesh.geometry = std::make_shared<const Mesh>(std::vector<Vertex>{{{-3, -2, -.6f}, {0, 0, -1}},
                                                                     {{-3, 2, .8f}, {0, 0, -1}},
                                                                     {{3, 2, 1.2f}, {0, 0, -1}},
                                                                     {{-3, -2, -.6f}, {0, 0, -1}},
                                                                     {{3, 2, 1.2f}, {0, 0, -1}},
                                                                     {{3, -2, -.2f}, {0, 0, -1}}});
    render::Frame frame{drawing, {}, {0, 0, float(extent.width), float(extent.height)}, {&mesh, 1}};
    frame.camera.projection = {1.8f * float(extent.height) / float(extent.width),
                               0,
                               0,
                               0,
                               0,
                               1.8f,
                               0,
                               0,
                               0,
                               0,
                               -.1f / 99.9f,
                               1,
                               0,
                               0,
                               10.f / 99.9f,
                               0};
    frame.view.cavity.enabled = false;
    frame.view.lit.shadows = frame.view.lit.ddgi = frame.view.lit.gtao = false;
    frame.view.lit.sssr = frame.view.lit.bloom = false;
    const auto yellow = [](const std::uint8_t* p) { return p[0] > 225 && p[1] > 150 && p[2] < 80; };
    render::Capture capture;
    const auto draw = [&] {
        ++drawing.id;
        check(take(prepared_render(renderer, frame, &capture)).status == render::FrameStatus::accepted,
              "selection depth frame accepted");
    };
    bool continuous = true;
    for (unsigned pose = 0; pose < 8; ++pose) {
        const float angle = .317f + pose * .027f, c = std::cos(angle), s = std::sin(angle);
        const float offset = pose < 4 ? 0 : 37.173f * float(pose - 3);
        mesh.world[12] = -.731f + offset;
        mesh.world[13] = .427f - offset * .137f;
        mesh.world[14] = 2.173f + offset * .713f;
        frame.camera.view = {c,
                             0,
                             s,
                             0,
                             0,
                             1,
                             0,
                             0,
                             -s,
                             0,
                             c,
                             0,
                             .137f + pose * .013f - c * offset + s * offset * .713f,
                             -.239f + offset * .137f,
                             8.731f - s * offset - c * offset * .713f,
                             1};
        frame.view.shading = render::Shading::flat;
        draw();
        const auto reference = capture;
        for (const auto aa : {render::Antialiasing::none, render::Antialiasing::smaa, render::Antialiasing::taa,
                              render::Antialiasing::smaa_t2x}) {
            frame.view.shading = render::Shading::scene_lit;
            frame.view.lit.antialiasing = aa;
            draw();
            const auto name =
                "selection-depth-" + std::to_string(pose) + "-" + std::to_string(static_cast<unsigned>(aa)) + ".ppm";
            snapshot(capture, name.c_str());
            std::size_t expected{}, visible{};
            for (unsigned y = 0; y < extent.height; ++y)
                for (unsigned x = 0; x < extent.width; ++x) {
                    if (!yellow(pixel(reference, x, y)))
                        continue;
                    ++expected;
                    if (yellow(pixel(capture, x, y)))
                        ++visible;
                }
            std::printf("Selection depth pose=%u aa=%u: %zu/%zu edge pixels\n", pose, static_cast<unsigned>(aa),
                        visible, expected);
            check(expected > 200, "transformed selection fixture has enough edge pixels");
            continuous = continuous && visible * 100 >= expected * 98;
        }
    }
    check(continuous, "Scene Lit selection must retain continuous surface edges across camera poses and AA modes");
    // Spatial diagnostics use private depth, then restore the editing target
    // before the selected-wire pass. Exercise that boundary in every AA mode.
    auto& lighting = frame.view.lit;
    lighting.probe_volumes[0].counts = {2, 2, 2};
    for (unsigned axis = 0; axis < 3; ++axis) {
        lighting.probe_volumes[0].minimum[axis] = mesh.world[12 + axis] - 2;
        lighting.probe_volumes[0].maximum[axis] = mesh.world[12 + axis] + 2;
    }
    for (const auto aa : {render::Antialiasing::none, render::Antialiasing::smaa, render::Antialiasing::taa,
                          render::Antialiasing::smaa_t2x}) {
        lighting.antialiasing = aa;
        draw();
        const auto reference = capture;
        lighting.probe_visualization = render::ProbeVisualization::state;
        lighting.probe_volume = true;
        draw();
        check(capture.depth == reference.depth, "DDGI overlays preserve selected surface depth");
        for (unsigned y = 0; y < extent.height; ++y)
            for (unsigned x = 0; x < extent.width; ++x)
                if (yellow(pixel(reference, x, y)))
                    check(yellow(pixel(capture, x, y)), "DDGI overlays retain selected edges");
        lighting.probe_visualization = render::ProbeVisualization::off;
        lighting.probe_volume = false;
    }
    // Cover the selected plane with the same slope, extended beyond its edges
    // and only 2 mm nearer the camera. A broad depth bias must not reveal it.
    auto cover = mesh;
    cover.selected = false;
    cover.stable_key = 2;
    cover.geometry = std::make_shared<const Mesh>(std::vector<Vertex>{{{-4.5f, -3, -1.05f}, {0, 0, -1}},
                                                                      {{-4.5f, 3, 1.05f}, {0, 0, -1}},
                                                                      {{4.5f, 3, 1.65f}, {0, 0, -1}},
                                                                      {{-4.5f, -3, -1.05f}, {0, 0, -1}},
                                                                      {{4.5f, 3, 1.65f}, {0, 0, -1}},
                                                                      {{4.5f, -3, -.45f}, {0, 0, -1}}});
    cover.world[12] -= frame.camera.view[2] * .002f;
    cover.world[14] -= frame.camera.view[10] * .002f;
    std::array instances{mesh, cover};
    frame.meshes = instances;
    for (const auto aa : {render::Antialiasing::none, render::Antialiasing::smaa, render::Antialiasing::taa,
                          render::Antialiasing::smaa_t2x}) {
        frame.view.lit.antialiasing = aa;
        instances[0].selected = false;
        draw();
        const auto reference = capture;
        instances[0].selected = true;
        draw();
        check(capture.depth == reference.depth, "occluded selection preserves surface depth");
        for (unsigned y = 0; y < extent.height; ++y)
            for (unsigned x = 0; x < extent.width; ++x)
                check(!yellow(pixel(capture, x, y)), "nearby foreground surface hides selected edges");
    }
    std::puts(
        "Renderer selection depth: PASS (camera poses, translated scenes, four AA modes, DDGI overlay coexistence, nearby occlusion)");
}

void selection_clipping_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    ui::Frame drawing{.id = 12000, .extent = extent, .draws = {}};
    render::MeshInstance floor;
    floor.color = {.2f, .2f, .2f, 1};
    floor.material.emissive = {.04f, .05f, .06f};
    floor.stable_key = 1;
    render::Frame frame{drawing, {}, {0, 0, float(extent.width), float(extent.height)}, {&floor, 1}};
    constexpr float near_plane = .1f, far_plane = 100.f;
    const render::Camera perspective{.projection = {1.5f * float(extent.height) / float(extent.width), 0, 0, 0, 0, 1.5f,
                                                    0, 0, 0, 0, -near_plane / (far_plane - near_plane), 1, 0, 0,
                                                    near_plane * far_plane / (far_plane - near_plane), 0}};
    frame.view.lit.shadows = frame.view.lit.ddgi = frame.view.lit.gtao = false;
    frame.view.lit.sssr = frame.view.lit.bloom = false;
    render::Capture capture;
    const auto draw = [&] {
        ++drawing.id;
        check(take(prepared_render(renderer, frame, &capture)).status == render::FrameStatus::accepted,
              "selection clipping frame is accepted");
    };
    bool interiors_preserved = true;
    const auto run_case = [&](const char* name, float front_z, render::Shading shading,
                              render::Antialiasing aa = render::Antialiasing::none, float scale = 1.f,
                              bool mirror = false, float wire_width = 1.25f) {
        frame.camera = perspective;
        frame.camera.projection[14] *= scale;
        floor.world = translation(0, 0, 0);
        floor.world[0] = mirror ? -scale : scale;
        floor.world[5] = floor.world[10] = scale;
        frame.view.shading = shading;
        frame.view.lit.antialiasing = aa;
        frame.view.wireframe = render::Wireframe::off;
        frame.view.triangle_edges = false;
        frame.view.wire_width = wire_width;
        // One polygon crosses the eye/near plane. Its diagonal is not an authored
        // edge, and the centre-lower viewport is far from every true boundary.
        floor.geometry = std::make_shared<const Mesh>(std::vector<Vertex>{{{-8, -.4f, front_z}, {0, 1, 0}},
                                                                          {{-8, -.4f, 8}, {0, 1, 0}},
                                                                          {{8, -.4f, 8}, {0, 1, 0}},
                                                                          {{-8, -.4f, front_z}, {0, 1, 0}},
                                                                          {{8, -.4f, 8}, {0, 1, 0}},
                                                                          {{8, -.4f, front_z}, {0, 1, 0}}},
                                                      std::vector<std::uint8_t>{5, 3});
        floor.selected = false;
        draw();
        const auto reference = capture;
        snapshot(reference, (std::string{"selection-"} + name + "-base.ppm").c_str());
        floor.selected = true;
        draw();
        snapshot(capture, (std::string{"selection-"} + name + ".ppm").c_str());
        std::size_t samples = 0, changed = 0, yellow = 0;
        for (unsigned y = extent.height * 3 / 5; y < extent.height * 9 / 10; ++y)
            for (unsigned x = extent.width * 3 / 8; x < extent.width * 5 / 8; ++x) {
                if (depth(reference, x, y) == 0)
                    continue;
                ++samples;
                const auto* before = pixel(reference, x, y);
                const auto* after = pixel(capture, x, y);
                if (std::abs(int(before[0]) - after[0]) > 2 || std::abs(int(before[1]) - after[1]) > 2 ||
                    std::abs(int(before[2]) - after[2]) > 2)
                    ++changed;
                if (after[0] > 220 && after[1] > 160 && after[2] < 50)
                    ++yellow;
            }
        std::printf("Selection %s: interior=%zu changed=%zu yellow=%zu\n", name, samples, changed, yellow);
        check(samples > 1000, "clipped selection fixture covers a substantial interior");
        check(capture.depth == reference.depth, "selection does not change FP32 surface depth");
        interiors_preserved &= changed == 0;
        // The original far edge stays visible even with an original vertex at w=0.
        const int edge_y = static_cast<int>((.5f + .3f / 8.f) * float(extent.height));
        unsigned edge_pixels = 0;
        for (int y = edge_y - 6; y <= edge_y + 6; ++y) {
            const auto* before = pixel(reference, extent.width / 2, static_cast<unsigned>(y));
            const auto* after = pixel(capture, extent.width / 2, static_cast<unsigned>(y));
            if (std::abs(int(after[0]) - before[0]) > 2 || std::abs(int(after[1]) - before[1]) > 2 ||
                std::abs(int(after[2]) - before[2]) > 2)
                ++edge_pixels;
        }
        std::printf("Selection %s: original-edge pixels=%u\n", name, edge_pixels);
        check(edge_pixels >= (wire_width >= 8 ? 3u : 1u) &&
                  edge_pixels <= static_cast<unsigned>(std::ceil(std::max(wire_width, 2.f) * .5f)) + 1,
              "original edge retains a bounded pixel width after clipping");
        const auto polygon = capture;
        frame.view.triangle_edges = true;
        draw();
        std::size_t diagonal = 0;
        for (unsigned y = extent.height * 11 / 20; y < extent.height * 9 / 10; ++y)
            for (unsigned x = extent.width / 4; x < extent.width * 3 / 4; ++x) {
                const auto* before = pixel(polygon, x, y);
                const auto* after = pixel(capture, x, y);
                if (std::abs(int(before[0]) - after[0]) > 20 || std::abs(int(before[1]) - after[1]) > 20)
                    ++diagonal;
            }
        check(diagonal > extent.width / 8, "explicit triangle edges restore the clipped polygon diagonal");
        frame.view.triangle_edges = false;
        floor.selected = false;
        for (const auto mode : {render::Wireframe::overlay, render::Wireframe::only}) {
            frame.view.wireframe = mode;
            frame.view.hidden_lines = true;
            draw();
            check(capture.depth == reference.depth, "explicit wire modes preserve the surface depth");
            for (unsigned y = extent.height * 3 / 5; y < extent.height * 9 / 10; ++y)
                for (unsigned x = extent.width * 3 / 8; x < extent.width * 5 / 8; ++x) {
                    const auto* after = pixel(capture, x, y);
                    const auto* before = pixel(reference, x, y);
                    for (unsigned c = 0; c < 3; ++c)
                        check(std::abs(int(after[c]) - (mode == render::Wireframe::only ? 0 : before[c])) <= 2,
                              "explicit wire display does not flood clipped polygon interiors");
                }
            if (mode == render::Wireframe::only) {
                const auto visible = capture;
                frame.view.hidden_lines = false;
                draw();
                check(capture.rgba == visible.rgba && capture.depth == visible.depth,
                      "visible/all wires agree for one unobstructed clipped polygon");
            }
        }
    };
    run_case("behind-eye", -1, render::Shading::smooth);
    run_case("on-eye", 0, render::Shading::smooth);
    run_case("near-clipped", .04f, render::Shading::smooth);
    run_case("near-front", .11f, render::Shading::smooth);
    run_case("flat", -1, render::Shading::flat);
    run_case("matcap", -1, render::Shading::matcap);
    run_case("lit-off", -1, render::Shading::scene_lit);
    run_case("lit-taa", -1, render::Shading::scene_lit, render::Antialiasing::taa);
    run_case("lit-smaa", -1, render::Shading::scene_lit, render::Antialiasing::smaa);
    run_case("lit-t2x", -1, render::Shading::scene_lit, render::Antialiasing::smaa_t2x);
    run_case("lit-on-eye", 0, render::Shading::scene_lit);
    run_case("small-scale", -1, render::Shading::smooth, render::Antialiasing::none, 1e-20f);
    run_case("large-scale", -1, render::Shading::smooth, render::Antialiasing::none, 1e20f);
    run_case("mirrored", -1, render::Shading::smooth, render::Antialiasing::none, 1.f, true);
    run_case("wide", -1, render::Shading::smooth, render::Antialiasing::none, 1.f, false, 8.f);
    check(interiors_preserved, "near/eye-plane clipping must not flood selected polygon interiors");
    // Normal metre-scale polygon crossing far=4 at x=0. A whole-mesh CPU
    // rejection or a missing raster clip would lose the left half or retain the right.
    floor.world = translation(0, 0, 0);
    floor.geometry = std::make_shared<const Mesh>(std::vector<Vertex>{{{-2, -1, 2}, {0, 0, -1}},
                                                                      {{-2, 1, 2}, {0, 0, -1}},
                                                                      {{2, 1, 6}, {0, 0, -1}},
                                                                      {{-2, -1, 2}, {0, 0, -1}},
                                                                      {{2, 1, 6}, {0, 0, -1}},
                                                                      {{2, -1, 6}, {0, 0, -1}}},
                                                  std::vector<std::uint8_t>{5, 3});
    for (const bool orthographic : {false, true}) {
        frame.camera = {};
        frame.camera.projection =
            orthographic ? std::array<float, 16>{.4f, 0, 0, 0, 0, .6f, 0, 0, 0, 0, -1 / 3.75f, 0, 0, 0, 4 / 3.75f, 1}
                         : std::array<float, 16>{1, 0, 0, 0, 0, 1.5f, 0, 0, 0, 0, 0, 1, 0, 0, .25f, 0};
        frame.camera.far_depth = orthographic ? 0 : .25f / 4;
        for (const auto shading : {render::Shading::flat, render::Shading::scene_lit}) {
            frame.view.shading = shading;
            frame.view.cavity.enabled = true;
            frame.view.lit.antialiasing = render::Antialiasing::none;
            frame.view.wire_width = 1.25f;
            frame.view.wireframe = render::Wireframe::off;
            floor.selected = false;
            frame.camera_cut = true;
            draw();
            frame.camera_cut = false;
            const auto surface = capture;
            check(std::ranges::count_if(surface.depth, [](float z) { return z > 0; }) > 1000,
                  "far clipping retains the visible part of crossing triangles");
            check(
                std::ranges::all_of(surface.depth,
                                    [&](float z) { return z == 0 || (z >= frame.camera.far_depth - 1e-6f && z <= 1); }),
                "far clipped surface depth remains in the original device interval");
            for (const auto mode : {render::Wireframe::off, render::Wireframe::overlay, render::Wireframe::only}) {
                frame.view.wireframe = mode;
                floor.selected = mode == render::Wireframe::off;
                for (const bool hidden : {false, true}) {
                    frame.view.hidden_lines = hidden;
                    draw();
                    check(capture.depth == surface.depth,
                          "far clipping differs between surface and editing depth/edges");
                    std::size_t edge_pixels{};
                    for (unsigned y = 0; y < extent.height; ++y)
                        for (unsigned x = 0; x < extent.width; ++x) {
                            const auto* p = pixel(capture, x, y);
                            if (x > extent.width / 2 + 3)
                                check(depth(capture, x, y) == 0 && p[0] == 0 && p[1] == 0 && p[2] == 0,
                                      "geometry, selected edges or hidden wires leaked beyond the far clipping plane");
                            if (p[0] > 225 && p[1] > 150 && p[2] < 80)
                                ++edge_pixels;
                        }
                    if (floor.selected)
                        check(edge_pixels > 20, "far clipping lost the surviving selected edges");
                }
            }
            const auto name = std::string{"selection-far-"} + (orthographic ? "orthographic-" : "perspective-") +
                              (shading == render::Shading::scene_lit ? "lit" : "studio") + ".ppm";
            frame.view.wireframe = render::Wireframe::off;
            floor.selected = true;
            draw();
            snapshot(capture, name.c_str());
        }
    }
    frame.camera = {.projection = {1, 0, 0, 0, 0, 1.5f, 0, 0, 0, 0, 0, 1, 0, 0, .25f, 0}, .far_depth = .0625f};
    floor.selected = false;
    floor.geometry = std::make_shared<const Mesh>(std::vector<Vertex>{{{-2, -1, 4}, {0, 0, -1}},
                                                                      {{-2, 1, 4}, {0, 0, -1}},
                                                                      {{2, 1, 4}, {0, 0, -1}},
                                                                      {{-2, -1, 4}, {0, 0, -1}},
                                                                      {{2, 1, 4}, {0, 0, -1}},
                                                                      {{2, -1, 4}, {0, 0, -1}}});
    frame.camera_cut = true;
    draw();
    frame.camera_cut = false;
    check(depth(capture, extent.width / 2, extent.height / 2) == .0625f &&
              pixel(capture, extent.width / 2, extent.height / 2)[0] > 15,
          "positive far-plane depth remains a visible Lit surface, not background");
    frame.meshes = {};
    frame.view.shading = render::Shading::flat;
    frame.view.reference_grid = frame.view.origin_axes = true;
    frame.view.grid_plane = render::GridPlane::xy;
    frame.camera.view[14] = 4;
    draw();
    const auto grid = capture;
    check(std::ranges::count_if(grid.rgba, [](auto c) { return c > 0 && c < 255; }) > 100,
          "reference grid remains visible at the positive far clipping boundary");
    frame.camera.far_depth = .25f / 3;
    draw();
    for (std::size_t i = 0; i < capture.depth.size(); ++i)
        check(capture.depth[i] == 0 && capture.rgba[i * 4] == 0 && capture.rgba[i * 4 + 1] == 0 &&
                  capture.rgba[i * 4 + 2] == 0,
              "reference guides beyond the finite far plane do not leak color or depth");
    frame.view.reference_grid = frame.view.origin_axes = false;
    frame.camera.view[14] = 0;
    frame.view.shading = render::Shading::scene_lit;
    auto& debug = frame.view.lit;
    debug.probe_visualization = render::ProbeVisualization::state;
    debug.probe_volume = debug.probe_xray = true;
    for (auto& volume : debug.probe_volumes)
        volume.enabled = false;
    debug.probe_volumes[0].enabled = true;
    debug.probe_volumes[0].minimum = {-1, -1, 7};
    debug.probe_volumes[0].maximum = {1, 1, 9};
    debug.probe_volumes[0].counts = {2, 2, 2};
    frame.camera.far_depth = .25f / 12;
    frame.camera_cut = true;
    draw();
    check(std::ranges::count_if(capture.rgba, [](auto c) { return c > 0 && c < 255; }) > 100,
          "DDGI diagnostic spheres and bounds have a visible far-clip control");
    frame.camera.far_depth = .25f / 4;
    draw();
    frame.camera_cut = false;
    for (std::size_t i = 0; i < capture.depth.size(); ++i)
        check(capture.depth[i] == 0 && capture.rgba[i * 4] == 0 && capture.rgba[i * 4 + 1] == 0 &&
                  capture.rgba[i * 4 + 2] == 0,
              "X-ray DDGI diagnostic geometry still obeys the main-camera far plane");
    std::puts(
        "Renderer selection clipping: PASS (eye/near/far clipping, original/triangle edges, bounded width, four shading and AA modes, FP32 depth, extreme scale, mirror, explicit wires, far surface/grid/DDGI boundaries)");
}
