// Native public consumer; Editor binding/history are owned by their acceptances.
namespace material_batch_e2e {
namespace render = atelier::renderer;

namespace material = atelier::material;

namespace geometry = atelier::geometry;

namespace source = atelier::source;

void wait(render::Renderer& renderer, render::MaterialBatch& batch) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
    while (!take(renderer.preparation(batch)).ready) {
        check(std::chrono::steady_clock::now() < deadline, "native batch preparation deadline");
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::shared_ptr<const std::vector<std::uint32_t>> slots(std::initializer_list<std::uint32_t> values) {
    return std::make_shared<const std::vector<std::uint32_t>>(values);
}

material::ProgramSurfaceRef texture_override(material::ProgramSurfaceRef parent, const char* path,
                                             material::ImageRef image) {
    return take(material::resolve_instance(
        parent, std::array{material::Override{path, material::TextureBinding{"native/image"}, true}},
        std::array{image}));
}

std::vector<material::ProgramSurfaceRef> surfaces(const std::filesystem::path& output) {
    atelier::Runtime compiler;
    take(material::register_program_types(compiler));
    const auto compile = [&](const char* name, bool masked) {
        const std::string text =
            std::string(
                masked
                    ? "struct Params { float cutoff; vector3 color; vector3 emission; float frequency; float mode; };\n"
                    : "struct Params { vector3 emission; vector3 color; float cutoff; float mode; float frequency; };\n") +
            "struct Textures { material.TextureInput zCoverage; material.TextureInput aColor; material.TextureInput mData; };\n"
            "Params defaults() { Params p; p.cutoff=0.5; p.color=vector3(1.0,0.0,0.0); p.emission=vector3(0.0,0.0,0.0); p.frequency=1.0; p.mode=0.0; return p; }\n"
            "material.Output shade(material.Context c, Params p, Textures t) { material.Output o;\n"
            "vector2 uv=c.uv+vector2();\n"
            "if (p.mode>0.5 && p.mode<1.5 && c.uv.x>0.5) { uv=uv*2.0; }\n"
            "vector4 color=sample(t.aColor,uv*p.frequency);\n"
            "o.base_color=vector3(p.color.x*color.x,p.color.y*color.y,p.color.z*color.z);\n"
            "o.emissive=vector3(p.emission.x*color.x,p.emission.y*color.y,p.emission.z*color.z); o.roughness=0.8; o.metallic=0.0;\n" +
            (masked
                 ? "vector4 mask=sample(t.zCoverage,uv*p.frequency); o.coverage=p.cutoff-mask.x+c.position_world.y*0.125;\n"
                 : "o.coverage=-1.0;\n") +
            "o.normal_world=c.normal_world; return o; }";
        std::ofstream file(output / (std::string(name) + ".source"));
        file << text;
        check(file.good(), "write native source evidence");
        return take(source::compile_material(compiler, take(compiler.native_type<material::Context>()),
                                             take(compiler.native_type<material::Output>()),
                                             take(compiler.native_type<material::TextureInput>()), name, text));
    };
    const auto mask = compile("NativeMask", true);
    const auto opaque = compile("NativeOpaque", false);
    const auto white = take(material::Image::create(1, 1, std::vector<std::uint8_t>{255, 255, 255, 255}));
    std::vector<std::uint8_t> ramp(256 * 4);
    for (unsigned i = 0; i < 256; ++i) {
        ramp[i * 4] = ramp[i * 4 + 1] = ramp[i * 4 + 2] = static_cast<std::uint8_t>(i);
        ramp[i * 4 + 3] = 255;
    }
    const auto coverage_image = take(material::Image::create(256, 1, std::move(ramp)));
    const auto source_surface = [&](auto program, material::Policy policy) {
        const auto definition = take(material::make_definition(
            program, policy,
            std::array{material::TextureDefault{
                           "mData", {"native/white"}, material::TextureColor::linear, {.25f, .5f, .75f, 1}},
                       material::TextureDefault{"zCoverage",
                                                {"native/ramp", material::Address::clamp},
                                                material::TextureColor::linear,
                                                {0, 0, 0, 1}},
                       material::TextureDefault{
                           "aColor", {"native/white"}, material::TextureColor::srgb, {.2f, .4f, .8f, 1}}}));
        std::vector<material::ImageRef> images;
        for (const auto& input : definition->textures())
            images.push_back(input.path == "zCoverage" ? coverage_image : white);
        return take(material::resolve_source(definition, images));
    };
    const auto masked = source_surface(mask, {material::Coverage::mask, true, true});
    const auto solid = source_surface(opaque, {material::Coverage::opaque, true, true});
    const auto override = [&](auto parent, std::vector<material::Override> entries) {
        return take(material::resolve_instance(parent, entries, std::vector<material::ImageRef>(entries.size())));
    };
    const auto emitter = override(masked, {{"cutoff", 0.f, true},
                                           {"color", atelier::math::Vector3{}, true},
                                           {"emission", atelier::math::Vector3{10, 10, 10}, true}});
    return {masked,
            override(solid, {{"color", atelier::math::Vector3{0, 1, 0}, true}}),
            source_surface(mask, {material::Coverage::opaque, true, true}),
            override(solid, {{"color", atelier::math::Vector3{.65f, .65f, .65f}, true}}),
            texture_override(emitter, "zCoverage", white),
            take(material::resolve_source({}, {}))};
}

void run(HWND window, const std::filesystem::path& output) {
    auto table = surfaces(output);
    auto renderer = take(render::Renderer::open(
        window,
        {.validation = atelier::rhi::Validation::gpu, .vsync = false, .shader_cache = shader_cache_directory()}));
    auto batch = take(renderer.prepare_materials(table));
    wait(renderer, batch);
    ui::Frame drawing{.id = 30000, .extent = take(renderer.target()).extent, .draws = {}};
    std::vector<geometry::Vertex> vertices;
    for (float z : {.2f, .6f})
        vertices.insert(vertices.end(), {{{-1, -1, z}, {0, 0, -1}, {0, 1}, {1, 0, 0, 1}},
                                         {{-1, 1, z}, {0, 0, -1}, {0, 0}, {1, 0, 0, 1}},
                                         {{1, 1, z}, {0, 0, -1}, {1, 0}, {1, 0, 0, 1}},
                                         {{-1, -1, z}, {0, 0, -1}, {0, 1}, {1, 0, 0, 1}},
                                         {{1, 1, z}, {0, 0, -1}, {1, 0}, {1, 0, 0, 1}},
                                         {{1, -1, z}, {0, 0, -1}, {1, 1}, {1, 0, 0, 1}}});
    std::array<render::MeshInstance, 1> meshes{};
    meshes[0].geometry = take(geometry::Mesh::create(vertices, std::vector<std::uint8_t>{},
                                                     std::vector<geometry::MaterialSlot>{{"front"}, {"back"}},
                                                     std::vector<geometry::MeshPart>{{0, 6, 0}, {6, 6, 1}}));
    meshes[0].stable_key = 1;
    meshes[0].material_slots = slots({0, 1});
    render::Frame frame{drawing, {}, {0, 0, float(drawing.extent.width), float(drawing.extent.height)}, meshes};
    frame.materials = &batch;
    frame.scene_identity = 30000;
    frame.view.cavity.enabled = false;
    render::Capture capture;
    const auto draw = [&](const char* name) {
        pump();
        ++drawing.id;
        check(take(prepared_render(renderer, frame, &capture)).status == render::FrameStatus::accepted,
              "native candidate frame accepted");
        scene_lit_e2e::png(capture, output / (std::string(name) + ".png"));
    };
    const auto x = drawing.extent.width / 4;
    const auto y = drawing.extent.height / 4;
    draw("native-smooth");
    check(std::abs(depth(capture, x, y) - .8f) < 1e-5f && std::abs(depth(capture, x * 3, y) - .4f) < 1e-5f &&
              pixel(capture, x, y)[0] > 150 && pixel(capture, x * 3, y)[1] > 150,
          "authored red Mask and green rear surface reach studio color and depth");
    meshes[0].material_slots = slots({5, 1});
    draw("native-default-smooth");
    for (const auto px : {x, x * 3}) {
        const auto* color = pixel(capture, px, y);
        check(std::abs(depth(capture, px, y) - .8f) < 1e-5f && color[0] > color[1] + 60 && color[2] > color[1] + 60,
              "engine default is opaque magenta");
    }
    meshes[0].material_slots = slots({0, 1});
    std::vector<std::uint8_t> checks(256 * 256 * 4);
    for (unsigned v = 0; v < 256; ++v)
        for (unsigned u = 0; u < 256; ++u) {
            const auto at = (v * 256 + u) * 4;
            const bool red = ((u / 4 + v / 4) & 1) == 0;
            checks[at] = red ? 255 : 0;
            checks[at + 2] = red ? 0 : 255;
            checks[at + 3] = 255;
        }
    auto next_table = table;
    next_table[0] = texture_override(table[2], "aColor", take(material::Image::create(256, 256, std::move(checks))));
    next_table[0] = take(material::resolve_instance(
        next_table[0],
        std::array{material::Override{"color", atelier::math::Vector3{1, 1, 1}, true},
                   material::Override{"mode", 1.f, true}, material::Override{"frequency", 64.f, true}},
        std::array<material::ImageRef, 3>{}));
    auto next = take(renderer.prepare_materials(next_table));
    wait(renderer, next);
    // Prepared but unadopted image ownership survives unrelated old frames.
    for (unsigned i = 0; i < 6; ++i) {
        ++drawing.id;
        take(prepared_render(renderer, frame));
    }
    frame.materials = &next;
    draw("native-final-uv-mips");
    for (unsigned py = drawing.extent.height / 3; py < drawing.extent.height * 2 / 3; py += 11)
        for (unsigned px = drawing.extent.width / 3; px < drawing.extent.width * 2 / 3; px += 13) {
            const auto* color = pixel(capture, px, py);
            check(color[0] > 10 && color[2] > 10 && std::abs(int(color[0]) - int(color[2])) < 8 &&
                      std::abs(depth(capture, px, py) - .8f) < 1e-5f,
                  "equal-area red/blue checker filters to a visible mean at the authored front depth");
        }
    frame.materials = &batch;
    std::vector<render::MeshInstance> room;
    for (const auto& mesh : scene_lit_e2e::Room{}.meshes)
        room.push_back({mesh.world, mesh.selected, mesh.geometry, mesh.stable_key, slots({3})});
    room[7].geometry = take(geometry::Mesh::create(std::vector<geometry::Vertex>{{{-2, 6, -2}, {0, -1, 0}, {0, 0}},
                                                                                 {{2, 6, -2}, {0, -1, 0}, {1, 0}},
                                                                                 {{2, 6, 2}, {0, -1, 0}, {1, 1}},
                                                                                 {{-2, 6, -2}, {0, -1, 0}, {0, 0}},
                                                                                 {{2, 6, 2}, {0, -1, 0}, {1, 1}},
                                                                                 {{-2, 6, 2}, {0, -1, 0}, {0, 1}}}));
    room[7].world = scene_lit_e2e::box(0, 0, 0, 1, 1, 1);
    room[7].material_slots = slots({4});
    frame.meshes = room;
    frame.lights = {};
    frame.environment = {};
    frame.camera = scene_lit_e2e::camera(drawing.extent);
    ++frame.scene_identity;
    frame.view.shading = render::Shading::scene_lit;
    auto& lit = frame.view.lit;
    lit.ssilvb = lit.reflections = lit.bloom = lit.shadows = false;
    lit.antialiasing = render::Antialiasing::none;
    lit.indirect_lighting = true;
    lit.gi_quality = atelier::rendering::GiQuality::ultra;
    for (unsigned i = 1; i < scene_lit_e2e::warmup_frames; ++i) {
        pump();
        ++drawing.id;
        take(prepared_render(renderer, frame));
    }
    draw("native-gi-masked");
    const auto region =
        scene_lit_e2e::project_region(frame.camera, frame.scene_viewport, drawing.extent, {-3.15f, .01f, 1.f});
    const auto rgb = scene_lit_e2e::mean_rgb(capture, region);
    check(rgb[0] > 10 && rgb[1] > 10 && rgb[2] > 10,
          "GI-only opaque traversal keeps emission from the offscreen negative-coverage Mask surface");

    // Verify Masked RayQuery AnyHit shadow coverage (`ray_shadow_masked_pso`): opaque half casts shadow, cutout half transmits!
    std::array<render::MeshInstance, 2> shadow_pair{};
    shadow_pair[0] = {scene_lit_e2e::box(0, -.15f, 3.5f, 8, .3f, 7), false, geometry::cube_mesh(), 101, slots({3})};
    shadow_pair[1].geometry =
        take(geometry::Mesh::create(std::vector<geometry::Vertex>{{{-2, 1.6f, .5f}, {0, 1, 0}, {0, 0}},
                                                                  {{-2, 1.6f, 2.5f}, {0, 1, 0}, {0, 1}},
                                                                  {{2, 1.6f, 2.5f}, {0, 1, 0}, {1, 1}},
                                                                  {{-2, 1.6f, .5f}, {0, 1, 0}, {0, 0}},
                                                                  {{2, 1.6f, 2.5f}, {0, 1, 0}, {1, 1}},
                                                                  {{2, 1.6f, .5f}, {0, 1, 0}, {1, 0}}}));
    shadow_pair[1].world = scene_lit_e2e::box(0, 0, 0, 1, 1, 1);
    shadow_pair[1].stable_key = 102;
    shadow_pair[1].material_slots = slots({0});
    std::array<render::Light, 1> mask_sun{};
    mask_sun[0].stable_key = 500;
    mask_sun[0].kind = render::LightKind::directional;
    mask_sun[0].direction = {0, -1, 0};
    mask_sun[0].intensity = 4.f;
    mask_sun[0].casts_shadow = true;
    frame.meshes = shadow_pair;
    frame.lights = mask_sun;
    ++frame.scene_identity;
    frame.camera_cut = true;
    lit.indirect_lighting = false;
    lit.shadows = true;
    draw("native-masked-ray-shadow");
    const auto mask_shadowed =
        scene_lit_e2e::project_region(frame.camera, frame.scene_viewport, drawing.extent, {-1.f, 0.f, 1.5f}, 2);
    const auto mask_lit =
        scene_lit_e2e::project_region(frame.camera, frame.scene_viewport, drawing.extent, {1.5f, 0.f, 1.5f}, 2);
    check(
        scene_lit_e2e::mean_rgb(capture, mask_shadowed)[0] < 2.0 &&
            scene_lit_e2e::mean_rgb(capture, mask_lit)[0] > 40.0,
        "Masked RayQuery AnyHit casts shadow on positive-coverage half and transmits light through negative-coverage cutout");

    frame.meshes = room;
    frame.lights = {};
    ++frame.scene_identity;
    frame.camera_cut = true;
    // Last use is not captured/finished: retirement must preserve in-flight work.
    ++drawing.id;
    take(prepared_render(renderer, frame));
    batch = {};
    frame.materials = &next;
    for (unsigned i = 0; i < 6; ++i) {
        ++drawing.id;
        take(prepared_render(renderer, frame));
    }
    next = {};
    frame.materials = nullptr;
    frame.meshes = {};
    frame.lights = {};
    ++drawing.id;
    take(prepared_render(renderer, frame));
    auto reused = take(renderer.prepare_materials(table));
    wait(renderer, reused);
    reused = {};
    take(renderer.close());
    std::puts("Native material batches: PASS (four authored images and in-flight ownership sequence)");
}
} // namespace material_batch_e2e
