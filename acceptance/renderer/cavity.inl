void cavity_cases(render::Renderer& renderer, Window& window, const std::filesystem::path& output = {}) {
    namespace geometry = atelier::geometry;
    ui::Frame drawing{.id = 20000, .extent = take(renderer.target()).extent, .draws = {}};
    drawing.draws.push_back({{6, 6, 16, 16}, {0, 0, 32, 32}, {0, 1, 0, 1}, {0, 0, 1, 1}, {}});
    std::vector<geometry::Vertex> vertices;
    const auto fold = [&](float center, float sign) {
        for (float side : {-1.f, 1.f}) {
            const float x0 = center + (side < 0 ? -.6f : 0);
            const float x1 = center + (side < 0 ? 0 : .6f);
            const auto z = [&](float x) { return 3 + sign * std::abs(x - center) * .7f; };
            const std::array<float, 3> n{sign * side * .7f, 0, -1};
            for (unsigned corner : std::array<unsigned, 6>{0, 1, 2, 0, 2, 3}) {
                const float x = (corner == 1 || corner == 2) ? x1 : x0;
                const float y = corner >= 2 ? .8f : -.8f;
                vertices.push_back({{x, y, z(x)}, {n[0], n[1], n[2]}});
            }
        }
    };
    fold(-.7f, 1);
    fold(.7f, -1);
    std::array<render::MeshInstance, 1> meshes{};
    meshes[0].geometry = std::make_shared<const geometry::Mesh>(std::move(vertices));
    meshes[0].color = {.5f, .5f, .5f, 1};
    render::Frame frame{drawing, {}, {}, meshes};
    frame.view.shading = render::Shading::matcap;
    frame.view.background = render::Background::gradient;
    frame.camera.projection = {.65f, 0, 0, 0, 0, .8f, 0, 0, 0, 0, -.1f, 0, 0, 0, 1, 1};
    const auto viewport = [&] {
        frame.scene_viewport = {32, 24, float(drawing.extent.width - 64), float(drawing.extent.height - 48)};
    };
    viewport();
    render::Capture capture;
    const auto draw = [&] {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            pump();
            ++drawing.id;
            const auto result = take(prepared_render(renderer, frame, &capture));
            if (result.status == render::FrameStatus::accepted && result.next_extent == drawing.extent)
                return result;
            drawing.extent = result.next_extent;
            viewport();
        }
        throw std::runtime_error("Cavity capture did not settle at the resized target.");
    };
    const auto changes = [&](const render::Capture& baseline) {
        check(capture.depth == baseline.depth, "cavity preserves every FP32 depth value");
        std::array<std::size_t, 2> result{};
        for (std::size_t i = 0; i < capture.depth.size(); ++i) {
            if (capture.depth[i] == 0) {
                for (unsigned c = 0; c < 4; ++c)
                    if (capture.rgba[i * 4 + c] != baseline.rgba[i * 4 + c]) {
                        std::printf("Cavity background mismatch pixel=(%zu,%zu) channel=%u off=%u on=%u\n",
                                    i % drawing.extent.width, i / drawing.extent.width, c, baseline.rgba[i * 4 + c],
                                    capture.rgba[i * 4 + c]);
                        check(false, "cavity does not modify background, viewport margins or UI");
                    }
                continue;
            }
            const int delta = int(capture.rgba[i * 4]) - int(baseline.rgba[i * 4]);
            const bool left = i % drawing.extent.width < drawing.extent.width / 2;
            if (left && delta > 2)
                ++result[0];
            if (!left && delta < -2)
                ++result[1];
            check(left ? delta >= 0 : delta <= 0,
                  "cavity ridge and valley do not exchange signs or darken convex edges");
        }
        return result;
    };
    for (const auto shading : {render::Shading::matcap, render::Shading::smooth, render::Shading::flat}) {
        frame.view.shading = shading;
        frame.view.cavity.enabled = false;
        draw();
        const auto baseline = capture;
        frame.view.cavity.enabled = true;
        const auto active = draw();
        if (shading == render::Shading::matcap) {
            scene_lit_e2e::png(baseline, output / "cavity-off.png");
            scene_lit_e2e::png(capture, output / "cavity-on.png");
        }
        const auto count = changes(baseline);
        check(count[0] > 40 && count[1] > 40, "screen cavity makes both convex and concave geometric folds visible");
        check(active.stats.viewport_draw_calls == 2, "cavity adds one screen pass to the studio background");
        if (shading == render::Shading::matcap) {
            scene_lit_e2e::png(baseline, output / "cavity-off.png");
            scene_lit_e2e::png(capture, output / "cavity-on.png");
            const auto stable = capture;
            draw();
            check(capture.rgba == stable.rgba, "static cavity has no random or temporal noise");
            frame.view.cavity.ridge = 0;
            draw();
            const auto valley = changes(baseline);
            check(valley[0] == 0 && valley[1] > 40, "ridge control is independent from valley");
            frame.view.cavity.ridge = 1;
            frame.view.cavity.valley = 0;
            draw();
            const auto ridge = changes(baseline);
            check(ridge[0] > 40 && ridge[1] == 0, "valley control is independent from ridge");
            frame.view.cavity.ridge = frame.view.cavity.valley = 0;
            const auto zero = draw();
            check(capture.rgba == baseline.rgba && zero.stats.viewport_draw_calls == 1,
                  "zero strengths are an exact no-op without a cavity pass");
            frame.view.cavity = {};
            frame.view.cavity.radius = 4;
            draw();
            const auto wide = changes(baseline);
            check(wide[0] > count[0] * 2 && wide[1] > count[1] * 2,
                  "cavity radius widens its physical-pixel neighborhood");
            scene_lit_e2e::png(capture, output / "cavity-radius4.png");
            frame.view.cavity.radius = .5f;
            draw();
            const auto narrow = changes(baseline);
            check(narrow[0] > 0 && narrow[1] > 0, "subpixel cavity radius retains a bounded enhancement");
            frame.view.cavity = {};
        }
        frame.view.wireframe = render::Wireframe::only;
        frame.view.cavity.enabled = false;
        draw();
        const auto wires = capture;
        frame.view.cavity.enabled = true;
        draw();
        check(capture.rgba == wires.rgba && capture.depth == wires.depth, "pure wireframe ignores cavity");
        frame.view.wireframe = render::Wireframe::off;
    }
    // A real window resize reallocates the normal attachment at the accepted extent.
    window.resize(509, 317);
    drawing.extent = take(renderer.target()).extent;
    viewport();
    frame.view.shading = render::Shading::matcap;
    frame.view.cavity.enabled = false;
    draw();
    const auto resized = capture;
    frame.view.cavity.enabled = true;
    draw();
    const auto resize_count = changes(resized);
    check(resize_count[0] > 30 && resize_count[1] > 30, "resized cavity keeps its geometric support");
    // Perspective uses the same view-space reconstruction and signed curvature.
    frame.camera.projection = {2, 0, 0, 0, 0, 2.5f, 0, 0, 0, 0, -1.f / 99, 1, 0, 0, 10.f / 99, 0};
    frame.view.cavity.enabled = false;
    draw();
    const auto perspective = capture;
    frame.view.cavity.enabled = true;
    draw();
    const auto perspective_count = changes(perspective);
    check(perspective_count[0] > 30 && perspective_count[1] > 30, "perspective cavity preserves ridge/valley signs");
    scene_lit_e2e::png(capture, output / "cavity-perspective.png");
    frame.view.cavity.radius = 0;
    const auto rejected = prepared_render(renderer, frame);
    check(!rejected && !rejected.error().terminal, "invalid cavity settings reject before GPU encoding");
    frame.view.cavity = {};
    // A compact real 3D scene complements the signed-fold measurements.
    std::vector<geometry::Vertex> sphere;
    constexpr unsigned rings = 16;
    constexpr unsigned slices = 32;
    constexpr float pi = 3.141592653589793f;
    const auto vertex = [&](unsigned row, unsigned column) {
        const float latitude = pi * (float(row) / rings - .5f);
        const float longitude = 2 * pi * column / slices;
        const float radial = (row == 0 || row == rings) ? 0 : std::cos(latitude);
        const float x = radial * std::cos(longitude);
        const float y = std::sin(latitude);
        const float z = radial * std::sin(longitude);
        return geometry::Vertex{{x * .5f, y * .5f, z * .5f}, {x, y, z}, {float(column) / slices, float(row) / rings}};
    };
    for (unsigned row = 0; row < rings; ++row)
        for (unsigned column = 0; column < slices; ++column) {
            const std::array quad{vertex(row, column), vertex(row, column + 1), vertex(row + 1, column + 1),
                                  vertex(row + 1, column)};
            if (row > 0)
                for (unsigned i : {0u, 1u, 2u})
                    sphere.push_back(quad[i]);
            if (row + 1 < rings)
                for (unsigned i : {0u, 2u, 3u})
                    sphere.push_back(quad[i]);
        }
    std::array<render::MeshInstance, 3> primitives{};
    primitives[0].geometry = geometry::cube_mesh();
    primitives[0].world = scene_lit_e2e::box(0, -.1f, 2, 6, .2f, 6);
    primitives[1].geometry = geometry::cube_mesh();
    primitives[1].world = scene_lit_e2e::box(-1.15f, 1, 1.8f, 1.6f, 2, 1.6f);
    primitives[2].geometry = std::make_shared<const geometry::Mesh>(std::move(sphere));
    primitives[2].world = scene_lit_e2e::box(1.2f, 1, 2.4f, 2, 2, 2);
    frame.meshes = primitives;
    window.resize(960, 640);
    frame.camera = scene_lit_e2e::camera({960, 640});
    frame.view.cavity.enabled = false;
    draw();
    const auto primitives_off = capture;
    frame.view.cavity.enabled = true;
    draw();
    check(capture.depth == primitives_off.depth, "primitive cavity preserves depth");
    std::size_t enhanced{};
    for (std::size_t i = 0; i < capture.rgba.size(); i += 4)
        enhanced += std::abs(int(capture.rgba[i]) - primitives_off.rgba[i]) > 2;
    check(enhanced > 100, "cavity enhances real primitive boundaries and curved geometry");
    scene_lit_e2e::png(primitives_off, output / "cavity-primitives-off.png");
    scene_lit_e2e::png(capture, output / "cavity-primitives-on.png");
    frame.meshes = {};
    draw();
    window.resize(641, 385);
    draw();
    std::puts(
        "Studio cavity acceptance: PASS (ridge/valley, controls, all studio modes, depth/UI/background, static stability, wire-only, resize, perspective/orthographic, invalid input)");
}
