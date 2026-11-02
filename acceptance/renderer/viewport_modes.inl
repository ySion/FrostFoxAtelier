void viewport_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    using atelier::geometry::Vertex;
    auto quad = std::make_shared<const atelier::geometry::Mesh>(std::vector<Vertex>{{{-.6f, -.6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{.6f, -.6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{.6f, .6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{-.6f, -.6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{.6f, .6f, .2f}, {.8f, 0, -.6f}},
                                                                                    {{-.6f, .6f, .2f}, {.8f, 0, -.6f}}},
                                                                std::vector<std::uint8_t>{5, 3});
    std::array<render::MeshInstance, 1> meshes{};
    meshes[0].geometry = quad;
    meshes[0].color = {1, 0, 0, 1};
    ui::Frame drawing{.id = 9000, .extent = extent, .draws = {}};
    render::Frame frame{drawing, {}, {0, 0, float(extent.width), float(extent.height)}, meshes};
    frame.view.shading = render::Shading::matcap;
    frame.view.wireframe = render::Wireframe::only;
    // First interior sample: d <= 1 gives coverage >= .5, so R >= sRGB(.24), about 134.
    frame.view.wire_width = 2;
    render::Capture capture;
    take(prepared_render(renderer, frame, &capture));
    const unsigned cx = extent.width / 2;
    const unsigned cy = extent.height / 2;
    check(pixel(capture, cx, cy)[0] == 0 && std::abs(depth(capture, cx, cy) - .8f) < .0001f,
          "hidden-line depth prepass preserves background and full surface depth");
    // The authored quad projects its left boundary to x=.2*width. A depth-only
    // implementation must not satisfy the wireframe representative.
    unsigned edge_pixels{};
    const int edge = static_cast<int>(.2f * extent.width);
    for (int x = edge - 2; x <= edge + 2; ++x)
        if (pixel(capture, static_cast<unsigned>(x), cy)[0] > 80)
            ++edge_pixels;
    check(edge_pixels >= 1, "hidden-line rendering retains the authored quad boundary");
    snapshot(capture, "viewport-hidden-lines.ppm");
    std::puts("Renderer viewport: PASS (hidden-line polygon boundary and surface depth)");
}
