void viewport_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    using atelier::geometry::Vertex;
    auto quad = std::make_shared<const atelier::geometry::Mesh>(std::vector<Vertex>{{{-.6f, -.6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{.6f, -.6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{.6f, .6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{-.6f, -.6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{.6f, .6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{-.6f, .6f, .2f}, {.8f, 0, -.6f}}},
                                                                std::vector<std::uint8_t>{5, 3});
    std::array<render::MeshInstance, 2> meshes{};
    meshes[0].geometry = quad;
    meshes[0].color = {1, 0, 0, 1};
    meshes[1].geometry = quad;
    meshes[1].world[0] = meshes[1].world[5] = .4f;
    meshes[1].world[14] = .4f;
    ui::Frame drawing{.id = 9000, .extent = extent, .draws = {}};
    drawing.draws.push_back({{4, 4, 12, 12}, {4, 4, 16, 16}, {0, 1, 0, 1}, {0, 0, 1, 1}, {}});
    render::Frame frame{drawing, {}, {0, 0, float(extent.width), float(extent.height)}, {meshes.data(), 1}};
    render::Capture capture;
    const unsigned cx = extent.width / 2;
    const unsigned cy = extent.height / 2;
    const auto render_frame = [&] {
        ++drawing.id;
        const auto result = take(prepared_render(renderer, frame, &capture));
        check(result.status == render::FrameStatus::accepted && result.accepted_id == drawing.id,
              "display modes use the accepted frame contract");
        check(pixel(capture, 8, 8)[1] == 255 && depth(capture, 8, 8) == 0,
              "display policy does not affect UI or its depth");
        return result;
    };
    render_frame();
    const auto smooth = capture;
    frame.view.shading = render::Shading::flat;
    render_frame();
    check(std::abs(int(pixel(capture, cx, cy)[0]) - pixel(smooth, cx, cy)[0]) > 3 && capture.depth == smooth.depth,
          "flat geometric normals differ from authored smooth normals");
    frame.view.shading = render::Shading::matcap;
    render_frame();
    const auto clay = capture;
    snapshot(capture, "viewport-matcap.ppm");
    meshes[0].color = {0, 0, 1, 1};
    render_frame();
    check(capture.rgba == clay.rgba && capture.depth == clay.depth,
          "MatCap overrides base color without mutating scene geometry");
    frame.camera.view[0] = frame.camera.view[5] = 0;
    frame.camera.view[1] = 1;
    frame.camera.view[4] = -1;
    render_frame();
    check(std::abs(int(pixel(capture, cx, cy)[0]) - pixel(clay, cx, cy)[0]) > 15, "MatCap follows view-space normals");
    frame.camera = {};
    frame.view.wireframe = render::Wireframe::overlay;
    render_frame();
    const auto polygon = capture;
    snapshot(capture, "viewport-matcap-edges.ppm");
    frame.view.triangle_edges = true;
    render_frame();
    check(pixel(capture, cx, cy)[0] + 35 < pixel(polygon, cx, cy)[0],
          "polygon masks suppress the triangulation diagonal; triangle display restores it");
    frame.view.triangle_edges = false;
    frame.view.wireframe = render::Wireframe::only;
    render_frame();
    const auto hidden = capture;
    snapshot(capture, "viewport-hidden-lines.ppm");
    check(pixel(capture, cx, cy)[0] == 0 && std::abs(depth(capture, cx, cy) - .8f) < .0001f,
          "hidden-line depth prepass preserves background and full surface depth");
    frame.meshes = meshes;
    const auto hidden_result = render_frame();
    check(capture.rgba == hidden.rgba && hidden_result.stats.scene_draw_calls == 2,
          "all occluders contribute depth before batched visible edges");
    frame.view.hidden_lines = false;
    const auto all_result = render_frame();
    const auto lit = [](const render::Capture& image) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < image.rgba.size(); i += 4)
            if (image.rgba[i] > 80)
                ++count;
        return count;
    };
    check(lit(capture) > lit(hidden) + extent.width / 2 && capture.depth == hidden.depth &&
              all_result.stats.scene_draw_calls == 2,
          "all-edge wireframe reveals occluded geometry and preserves depth");
    snapshot(capture, "viewport-all-lines.ppm");
    frame.meshes = {meshes.data(), 1};
    frame.view.hidden_lines = true;
    frame.view.wire_width = 1;
    render_frame();
    const auto thin = lit(capture);
    frame.view.wire_width = 4;
    render_frame();
    check(lit(capture) > thin * 2, "wire width changes actual pixel coverage");
    const auto line_width = [&](float z) {
        meshes[0].world[14] = z - .2f;
        frame.camera.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -.1f / 9.9f, 1, 0, 0, 1.f / 9.9f, 0};
        render_frame();
        const int centre = static_cast<int>((.5f - .3f / z) * extent.width);
        unsigned pixels = 0;
        for (int x = centre - 6; x <= centre + 6; ++x)
            if (pixel(capture, static_cast<unsigned>(x), cy)[0] > 80)
                ++pixels;
        return pixels;
    };
    const auto near_width = line_width(2);
    const auto far_width = line_width(4);
    check(near_width >= 1 && far_width >= 1 && std::abs(int(near_width) - int(far_width)) <= 1,
          "perspective distance does not scale wire pixel width");
    frame.camera = {};
    meshes[0].world[14] = 0;
    frame.view.wire_width = std::numeric_limits<float>::quiet_NaN();
    const auto previous = capture.frame_id;
    const auto invalid = prepared_render(renderer, frame, &capture);
    check(!invalid && !invalid.error().terminal && capture.frame_id == previous,
          "invalid display settings reject before encoding without replacing capture");
    frame.view.wire_width = 1.25f;
    for (unsigned i = 0; i < 12; ++i) {
        frame.view.wireframe = static_cast<render::Wireframe>(i % 3);
        frame.view.shading = static_cast<render::Shading>(i % 3);
        ++drawing.id;
        check(take(prepared_render(renderer, frame)).status == render::FrameStatus::accepted,
              "ordinary asynchronous frames switch modes through the same caches");
    }
    std::puts(
        "Renderer viewport: PASS (MatCap, flat/smooth, polygon/triangle edges, hidden/all lines, selection, width, retirement)");
}
