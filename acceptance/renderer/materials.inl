void material_cases(render::Renderer& renderer) {
    namespace geometry = atelier::geometry;
    namespace material = standard_fixture;
    const auto surface = [](std::array<float, 4> color, material::ImageRef image = {},
                            material::ImageRef opacity = {}) {
        material::Description d;
        d.parameters.base_color = color;
        d.parameters.coverage = material::Coverage::mask;
        d.texture(material::Input::base_color).image = std::move(image);
        d.texture(material::Input::opacity).image = std::move(opacity);
        return std::make_shared<const material::Surface>(std::move(d));
    };
    ui::Frame drawing{.id = 9000, .extent = take(renderer.target()).extent, .draws = {}};
    std::vector<geometry::Vertex> vertices;
    for (float z : {.2f, .6f})
        vertices.insert(vertices.end(), {{{-1, -1, z}, {0, 0, -1}, {-1, 1}},
                                         {{-1, 1, z}, {0, 0, -1}, {-1, 0}},
                                         {{1, 1, z}, {0, 0, -1}, {0, 0}},
                                         {{-1, -1, z}, {0, 0, -1}, {-1, 1}},
                                         {{1, 1, z}, {0, 0, -1}, {0, 0}},
                                         {{1, -1, z}, {0, 0, -1}, {0, 1}}});
    auto image = take(material::Image::create(
        2, 2, std::vector<std::uint8_t>{255, 0, 0, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 0, 255}));
    std::weak_ptr<const material::Image> observer = image;
    auto base_surfaces = std::make_shared<const std::vector<material::SurfaceRef>>(
        std::vector{surface({1, 1, 1, 1}, image), surface({0, 1, 0, 1})});
    std::array<render::MeshInstance, 1> meshes{};
    meshes[0].color = {1, 1, 1, 1};
    meshes[0].surfaces = base_surfaces;
    meshes[0].geometry = take(geometry::Mesh::create(vertices, std::vector<std::uint8_t>{},
                                                     std::vector<geometry::MaterialSlot>{{"cutout"}, {"background"}},
                                                     std::vector<geometry::MeshPart>{{0, 6, 0}, {6, 6, 1}}));
    image.reset();
    render::Frame frame{drawing, {}, {0, 0, float(drawing.extent.width), float(drawing.extent.height)}, meshes};
    take(prepared_render(renderer, frame));
    auto opaque = take(material::Image::create(
        2, 2, std::vector<std::uint8_t>{255, 0, 0, 255, 255, 255, 255, 255, 0, 0, 255, 255, 255, 255, 0, 255}));
    auto opacity = take(material::Image::create(
        2, 2, std::vector<std::uint8_t>{255, 255, 255, 255, 0, 0, 0, 255, 160, 160, 160, 255, 255, 255, 255, 255}));
    std::weak_ptr<const material::Image> opacity_observer = opacity;
    base_surfaces = std::make_shared<const std::vector<material::SurfaceRef>>(
        std::vector{surface({1, 1, 1, 1}, opaque, opacity), surface({0, 1, 0, 1})});
    meshes[0].surfaces = base_surfaces;
    opaque.reset();
    opacity.reset();
    ++drawing.id;
    render::Capture capture;
    take(prepared_render(renderer, frame, &capture));
    check(observer.expired(), "replaced material image retires after its last frame");
    const auto x = drawing.extent.width / 4;
    const auto y = drawing.extent.height / 4;
    check(pixel(capture, x * 3, y)[1] > 150 && pixel(capture, x * 3, y)[0] < 10 &&
              std::abs(depth(capture, x * 3, y) - .4f) < 1e-5f,
          "replacement opacity map still cuts out opaque diffuse pixels");
    meshes[0].surfaces.reset();
    base_surfaces.reset();
    frame.meshes = {};
    meshes[0].geometry.reset();
    ++drawing.id;
    take(prepared_render(renderer, frame));
    check(opacity_observer.expired(), "unused geometry and material images retire together");
    std::puts("Material image ownership: PASS (replacement, final owner retirement)");
}
