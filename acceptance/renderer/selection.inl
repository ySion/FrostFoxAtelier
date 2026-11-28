void selection_clipping_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    ui::Frame drawing{.id = 12000, .extent = extent, .draws = {}};
    render::MeshInstance floor;
    floor.color = {.2f, .2f, .2f, 1};
    floor.material.emissive = {.04f, .05f, .06f};
    floor.stable_key = 1;
    floor.selected = true;
    // The authored polygon crosses far=4 at x=0. A whole-mesh rejection
    // loses the left half; missing clipping leaks color/depth into the right.
    floor.geometry = take(Mesh::create(atelier::Array<Vertex>{{{-2, -1, 2}, {0, 0, -1}},
                                                              {{-2, 1, 2}, {0, 0, -1}},
                                                              {{2, 1, 6}, {0, 0, -1}},
                                                              {{-2, -1, 2}, {0, 0, -1}},
                                                              {{2, 1, 6}, {0, 0, -1}},
                                                              {{2, -1, 6}, {0, 0, -1}}},
                                       atelier::Array<std::uint8_t>{5, 3}));
    render::Frame frame{drawing, {}, {0, 0, float(extent.width), float(extent.height)}, {&floor, 1}};
    frame.camera.projection = {{1, 0, 0, 0, 0, 1.5f, 0, 0, 0, 0, 0, 1, 0, 0, .25f, 0}};
    frame.camera.far_depth = .25f / 4;
    frame.view.shading = render::Shading::flat;
    frame.view.wire_width = 1.25f;
    render::Capture capture;
    accepted_render(renderer, frame, &capture);
    check(std::ranges::count_if(capture.depth, [](float z) { return z > 0; }) > 1000,
          "far clipping retains the visible part of crossing triangles");
    check(std::ranges::all_of(capture.depth,
                              [&](float z) { return z == 0 || (z >= frame.camera.far_depth - 1e-6f && z <= 1); }),
          "far clipped surface depth remains in the original device interval");
    std::size_t edge_pixels{};
    for (unsigned y = 0; y < extent.height; ++y)
        for (unsigned x = 0; x < extent.width; ++x) {
            const auto* p = pixel(capture, x, y);
            if (x > extent.width / 2 + 3)
                check(depth(capture, x, y) == 0 && p[0] == 0 && p[1] == 0 && p[2] == 0,
                      "selected geometry leaked beyond the far clipping plane");
            if (p[0] > 225 && p[1] > 150 && p[2] < 80)
                ++edge_pixels;
        }
    check(edge_pixels > 20, "far clipping lost the surviving selected edges");
    snapshot(capture, "selection-far-perspective-studio.ppm");
    std::puts("Renderer selection clipping: PASS (perspective far clipping, selected edges)");
}
