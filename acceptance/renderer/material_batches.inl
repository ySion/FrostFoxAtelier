// Native public consumer only. This does not exercise Editor binding/history
// or ViewportTool clicks. No StandardPBR fixture adapter participates here.
namespace material_batch_e2e {
namespace render = atelier::renderer;

namespace material = atelier::material;

namespace geometry = atelier::geometry;

namespace source = atelier::source;

template <class T, class E>
T need(std::expected<T, E> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

void wait(render::Renderer& renderer, render::MaterialBatch& batch) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
    for (;;) {
        const auto status = take(renderer.preparation(batch));
        if (status.ready) {
            std::printf("Native batch ready: %u/%u material pipelines\n", status.done, status.total);
            return;
        }
        check(std::chrono::steady_clock::now() < deadline, "native batch preparation deadline");
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::shared_ptr<const std::vector<std::uint32_t>> slots(std::initializer_list<std::uint32_t> values) {
    return std::make_shared<const std::vector<std::uint32_t>>(values);
}

material::ProgramSurfaceRef texture_override(material::ProgramSurfaceRef parent, const char* path,
                                             material::ImageRef image,
                                             material::Address address = material::Address::repeat) {
    return need(material::resolve_instance(
        parent,
        std::array{material::Override{path, material::TextureBinding{image ? "native/image" : "", address}, true}},
        std::array{image}));
}

std::vector<material::ProgramSurfaceRef> surfaces(const std::filesystem::path& output) {
    atelier::Runtime compiler;
    need(material::register_program_types(compiler));
    const auto compile = [&](const char* name, bool masked) {
        // Distinct Params layouts and source bodies, no hand-authored GPU schema.
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
            "if (p.mode>1.5) { uv=vector2(c.position_world.x,c.position_world.y); }\n"
            "if (p.mode>2.5) { uv=vector2(c.normal_world.x,c.normal_world.y); }\n"
            "if (p.mode>3.5) { uv=vector2(c.tangent_world.x,c.tangent_world.y); }\n"
            "if (p.mode>4.5) { vector4 driver=sample(t.mData,c.uv); uv=vector2(driver.x,driver.y); }\n"
            "vector4 color=sample(t.aColor,uv*p.frequency); vector4 data=sample(t.mData,c.uv);\n"
            "o.base_color=vector3(p.color.x*color.x,p.color.y*color.y,p.color.z*color.z);\n"
            "o.emissive=vector3(p.emission.x*color.x,p.emission.y*color.y,p.emission.z*color.z); o.roughness=0.8; o.metallic=0.0;\n"
            "if (p.mode<0.0) { o.base_color=vector3(data.x,color.x,0.0); }\n"
            "if (p.mode<-1.5) { vector2 oriented=uv*p.frequency; if (p.mode<-2.5) { oriented=vector2(1.0,2.0); }\n"
            "vector2 sign=sample_orientation(t.aColor,oriented); o.base_color=vector3(sign.x*0.5+0.5,sign.y*0.5+0.5,0.0); }\n"
            "if (p.mode<-3.5) { o.base_color=vector3(); o.emissive=p.emission*(4.0*color.x*color.z); }\n" +
            (masked
                 ? "vector4 mask=sample(t.zCoverage,uv*p.frequency); o.coverage=p.cutoff-mask.x+c.position_world.y*0.125;\n"
                 : "o.coverage=-1.0;\n") +
            "o.normal_world=c.normal_world; return o; }";
        std::ofstream file(output / (std::string(name) + ".source"));
        file << text;
        check(file.good(), "write native source evidence");
        return need(source::compile_material(compiler, need(compiler.native_type<material::Context>()),
                                             need(compiler.native_type<material::Output>()),
                                             need(compiler.native_type<material::TextureInput>()), name, text));
    };
    const auto mask = compile("NativeMask", true);
    const auto opaque = compile("NativeOpaque", false);
    const auto white = std::make_shared<const material::Image>(1, 1, std::vector<std::uint8_t>{255, 255, 255, 255});
    std::vector<std::uint8_t> ramp(256 * 4);
    for (unsigned i = 0; i < 256; ++i) {
        ramp[i * 4] = ramp[i * 4 + 1] = ramp[i * 4 + 2] = static_cast<std::uint8_t>(i);
        ramp[i * 4 + 3] = 255;
    }
    const auto coverage_image = std::make_shared<const material::Image>(256, 1, std::move(ramp));
    const auto source_surface = [&](auto program, material::Policy policy) {
        // Author, sorted descriptor, and compiler-local slot orders differ.
        const auto definition = need(material::make_definition(
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
        return need(material::resolve_source(definition, images));
    };
    const auto masked = source_surface(mask, {material::Coverage::mask, true, true});
    const auto solid = source_surface(opaque, {material::Coverage::opaque, true, true});
    const auto override = [&](auto parent, std::vector<material::Override> entries) {
        return need(material::resolve_instance(parent, entries, std::vector<material::ImageRef>(entries.size())));
    };
    // Actual explicit-parent MS -> MI -> MI and sibling resolutions; the GPU
    // owns these captures after this compiling Runtime and author arrays die.
    const auto parent = override(masked, {{"cutoff", .5f, true}});
    const auto child = override(parent, {{"color", atelier::math::Vector3{1, 0, 0}, true}});
    const auto sibling = override(parent, {{"color", atelier::math::Vector3{0, 0, 1}, true}, {"cutoff", .85f, true}});
    const std::string broken_text =
        "struct Params { float value; };\n"
        "Params defaults() { Params p; p.value=1.0; return p; }\n"
        "material.Output shade(material.Context c, Params p) { material.Output o; o.base_color=missing_color; return o; }\n";
    const auto broken = source::compile_material(
        compiler, need(compiler.native_type<material::Context>()), need(compiler.native_type<material::Output>()),
        need(compiler.native_type<material::TextureInput>()), "Broken.source", broken_text);
    check(!broken, "unknown author symbol must fail the real material compiler");
    std::ofstream failed_source(output / "Broken.source");
    failed_source << broken_text;
    std::ofstream diagnostic(output / "Broken.diagnostic.txt");
    diagnostic << broken.error().message;
    check(failed_source.good() && diagnostic.good(), "preserve failed source and original compiler diagnostic");
    const auto fallback = need(material::resolve_source({}, {}));
    check(fallback && fallback->uses_default_material() && fallback->diagnostics().size() == 1,
          "failed definition resolves an engine default with diagnostics");
    const auto emitter = override(masked, {{"cutoff", 2.f, true},
                                           {"color", atelier::math::Vector3{}, true},
                                           {"emission", atelier::math::Vector3{10, 10, 10}, true}});
    return {
        child,
        override(solid, {{"color", atelier::math::Vector3{0, 1, 0}, true}}),
        source_surface(mask, {material::Coverage::opaque, true, true}),
        override(solid, {{"color", atelier::math::Vector3{.65f, .65f, .65f}, true}}),
        texture_override(emitter, "aColor",
                         std::make_shared<const material::Image>(1, 1, std::vector<std::uint8_t>{255, 40, 25, 255})),
        texture_override(emitter, "aColor",
                         std::make_shared<const material::Image>(1, 1, std::vector<std::uint8_t>{25, 40, 255, 255})),
        texture_override(override(emitter, {{"cutoff", 0.f, true}}), "zCoverage", white),
        sibling,
        fallback};
}

void run(HWND window, const std::filesystem::path& output) {
    auto table = surfaces(output);
    auto renderer = take(render::Renderer::open(
        window,
        {.validation = atelier::rhi::Validation::gpu, .vsync = false, .shader_cache = shader_cache_directory()}));
    auto batch = take(renderer.prepare_materials(table));
    check(batch.surfaces().size() == table.size(), "batch retains exact frozen surface order");
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
    meshes[0].geometry = std::make_shared<const geometry::Mesh>(
        vertices, std::vector<std::uint8_t>{}, std::vector<geometry::MaterialSlot>{{"front"}, {"back"}},
        std::vector<geometry::MeshPart>{{0, 6, 0}, {6, 6, 1}});
    meshes[0].stable_key = 1;
    meshes[0].material_slots = slots({0, 1});
    render::Frame frame{drawing, {}, {0, 0, float(drawing.extent.width), float(drawing.extent.height)}, meshes};
    frame.materials = &batch;
    frame.scene_identity = 30000;
    frame.view.cavity.enabled = false;
    render::Capture capture;
    capture.frame_id = 19;
    const auto pending = take(renderer.render(frame, &capture));
    check(pending.status == render::FrameStatus::not_ready && pending.accepted_id == 0 && capture.frame_id == 19 &&
              pending.stats.scene_draw_calls == 0,
          "new candidate cannot encode/capture before explicit completion, even if workers finish early");
    const auto busy = renderer.close();
    check(!busy && busy.error().cause.code == atelier::rhi::ErrorCode::busy && !busy.error().terminal,
          "live batch close is nonterminal and checked before destructive cleanup");
    wait(renderer, batch);
    const auto x = drawing.extent.width / 4;
    const auto y = drawing.extent.height / 4;
    std::ofstream log(output / "native-outputs.csv");
    log << "stage,left_r,left_g,left_b,left_depth,right_r,right_g,right_b,right_depth\n";
    const auto draw = [&](const char* name, bool save = false) {
        pump();
        ++drawing.id;
        const auto result = take(prepared_render(renderer, frame, &capture));
        check(result.status == render::FrameStatus::accepted, "native candidate frame accepted");
        const auto* a = pixel(capture, x, y);
        const auto* b = pixel(capture, x * 3, y);
        log << name << ',' << unsigned(a[0]) << ',' << unsigned(a[1]) << ',' << unsigned(a[2]) << ','
            << depth(capture, x, y) << ',' << unsigned(b[0]) << ',' << unsigned(b[1]) << ',' << unsigned(b[2]) << ','
            << depth(capture, x * 3, y) << '\n';
        log.flush();
        check(log.good(), "write native numeric evidence");
        if (save)
            scene_lit_e2e::png(capture, output / (std::string(name) + ".png"));
        return result;
    };
    const auto coverage = [&] {
        check(std::abs(depth(capture, x, y) - .8f) < 1e-5f && std::abs(depth(capture, x * 3, y) - .4f) < 1e-5f,
              "one complete group supplies front coverage and exposed rear depth");
    };
    draw("native-smooth", true);
    coverage();
    check(pixel(capture, x, y)[0] > 150 && pixel(capture, x * 3, y)[1] > 150,
          "both source bodies and mixed parameter layouts reach studio shading");
    const auto original = capture;
    const auto default_coverage = [&] {
        check(std::abs(depth(capture, x, y) - .8f) < 1e-5f && std::abs(depth(capture, x * 3, y) - .8f) < 1e-5f,
              "default material replaces the old Mask hole with complete opaque coverage");
        for (const auto px : {x, x * 3}) {
            const auto* color = pixel(capture, px, y);
            check(color[0] > color[1] + 60 && color[2] > color[1] + 60,
                  "engine default is recognizable magenta, not the last good material");
        }
    };
    meshes[0].material_slots = slots({8, 1});
    draw("native-default-smooth", true);
    default_coverage();
    meshes[0].material_slots = slots({0, 1});
    draw("native-repaired-smooth");
    check(capture.rgba == original.rgba && capture.depth == original.depth,
          "repaired content restores normal shading and Mask coverage");
    const auto restored = capture;
    for (const auto& invalid_slots :
         {std::shared_ptr<const std::vector<std::uint32_t>>{}, slots({0}), slots({0, 99})}) {
        meshes[0].material_slots = invalid_slots;
        const auto invalid = renderer.render(frame, &capture);
        check(!invalid && !invalid.error().terminal && capture.frame_id == restored.frame_id,
              "missing/partial/out-of-range batch mapping rejects without fallback or capture");
    }
    meshes[0].material_slots = slots({0, 1});
    frame.materials = nullptr;
    const auto missing = renderer.render(frame, &capture);
    check(!missing && !missing.error().terminal && capture.frame_id == restored.frame_id,
          "a mesh frame without a batch rejects instead of falling back to a fixed material");
    frame.materials = &batch;
    material::Context point;
    point.uv = {.75f, .25f};
    point.position_world = {.5f, .5f, .2f};
    point.normal_world = {0, 0, -1};
    point.geometric_normal_world = {0, 0, -1};
    point.tangent_world = {1, 0, 0, 1};
    check(need(batch.surfaces()[0]->evaluate(point)).coverage < 0 &&
              need(batch.surfaces()[7]->evaluate(point)).coverage > 0,
          "native CPU query on retained exact captures agrees with the two GPU coverage candidates");
    auto next_table = table;
    next_table[0] = table[7];
    auto next = take(renderer.prepare_materials(next_table));
    frame.materials = &next;
    const auto not_ready = take(renderer.render(frame, &capture));
    check(not_ready.status == render::FrameStatus::not_ready && capture.frame_id == restored.frame_id,
          "pending replacement preserves accepted capture");
    frame.materials = &batch;
    draw("native-pending-old");
    check(capture.rgba == original.rgba && capture.depth == original.depth,
          "preparing a candidate does not change the complete old group");
    auto abandoned = take(renderer.prepare_materials(next_table));
    abandoned = {};
    const auto rejected = renderer.prepare_materials({});
    check(!rejected && !rejected.error().terminal, "empty candidate rejects without poisoning the current batch");
    draw("native-rejected-old");
    check(capture.rgba == original.rgba, "rejected/cancelled candidates preserve old rendering");
    wait(renderer, next);
    frame.materials = &next;
    draw("native-adopted", true);
    check(pixel(capture, x * 3, y)[2] > 150 && std::abs(depth(capture, x * 3, y) - .8f) < 1e-5f,
          "color and coverage change together on selecting a ready candidate");
    frame.materials = &batch;
    {
        const auto original_mesh = meshes[0];
        const auto parameters = [](material::ProgramSurfaceRef parent, std::vector<material::Override> entries) {
            return need(material::resolve_instance(parent, entries, std::vector<material::ImageRef>(entries.size())));
        };
        const auto white = parameters(table[2], {{"color", atelier::math::Vector3{1, 1, 1}, true}});
        std::vector<std::uint8_t> checks(256 * 256 * 4);
        std::vector<std::uint8_t> coordinates(checks.size());
        for (unsigned v = 0; v < 256; ++v)
            for (unsigned u = 0; u < 256; ++u) {
                const auto at = (v * 256 + u) * 4;
                const bool red = ((u / 4 + v / 4) & 1) == 0;
                checks[at] = red ? 255 : 0;
                checks[at + 2] = red ? 0 : 255;
                checks[at + 3] = 255;
                coordinates[at] = static_cast<std::uint8_t>(u);
                coordinates[at + 1] = static_cast<std::uint8_t>(v);
                coordinates[at + 3] = 255;
            }
        const auto checker = std::make_shared<const material::Image>(256, 256, std::move(checks));
        const auto driver = std::make_shared<const material::Image>(256, 256, std::move(coordinates));
        const auto textured =
            texture_override(texture_override(white, "aColor", checker), "mData", driver, material::Address::clamp);
        render::MaterialBatch variant;
        const auto select = [&](material::ProgramSurfaceRef surface) {
            auto candidates = table;
            candidates[0] = std::move(surface);
            auto prepared = take(renderer.prepare_materials(candidates));
            // This call must only upload tables/images, not resubmit 17 PSOs.
            check(take(renderer.preparation(prepared)).ready,
                  "ordinary input/parameter edits reuse the live exact program-set owner immediately");
            variant = std::move(prepared);
            frame.materials = &variant;
            meshes[0].material_slots = slots({0, 1});
        };
        const auto channel_difference = [&] {
            unsigned maximum = 0;
            for (unsigned py = drawing.extent.height / 3; py < drawing.extent.height * 2 / 3; py += 11)
                for (unsigned px = drawing.extent.width / 3; px < drawing.extent.width * 2 / 3; px += 13) {
                    const auto* color = pixel(capture, px, py);
                    maximum = std::max(maximum, static_cast<unsigned>(std::abs(int(color[0]) - int(color[2]))));
                }
            return maximum;
        };
        select(parameters(textured, {{"mode", 1.f, true}, {"frequency", 1.f, true}}));
        draw("native-mip-control", true);
        check(channel_difference() > 60, "LOD0 control really resolves red/blue texels rather than a constant fixture");
        select(parameters(textured, {{"mode", 1.f, true}, {"frequency", 64.f, true}}));
        // Prepared, unadopted image ownership must survive unrelated old frames.
        frame.materials = &batch;
        meshes[0] = original_mesh;
        for (unsigned i = 0; i < 6; ++i)
            draw("native-texture-pending-old");
        frame.materials = &variant;
        draw("native-final-uv-mips", true);
        check(channel_difference() < 8,
              "final scaled UV footprint filters checker detail, not original mesh UV or LOD0");
        auto filtered_mask = texture_override(texture_override(table[0], "aColor", checker), "zCoverage", checker);
        select(parameters(filtered_mask, {{"color", atelier::math::Vector3{1, 1, 1}, true},
                                          {"cutoff", .6f, true},
                                          {"mode", 1.f, true},
                                          {"frequency", 64.f, true}}));
        const auto filtered_depth = [&] {
            for (unsigned py = drawing.extent.height / 3; py < drawing.extent.height * 2 / 3; py += 11)
                for (unsigned px = drawing.extent.width / 3; px < drawing.extent.width * 2 / 3; px += 13)
                    check(std::abs(depth(capture, px, py) - .8f) < 1e-5f,
                          "primary coverage uses filtered final UV, not LOD0 checker holes");
        };
        draw("native-filtered-coverage", true);
        filtered_depth();
        const auto studio_view = frame.view;
        frame.view.shading = render::Shading::scene_lit;
        frame.view.lit.ddgi = frame.view.lit.gtao = frame.view.lit.sssr = frame.view.lit.bloom =
            frame.view.lit.shadows = false;
        for (auto aa : {render::Antialiasing::none, render::Antialiasing::taa}) {
            frame.view.lit.antialiasing = aa;
            frame.camera_cut = true;
            draw(aa == render::Antialiasing::none ? "native-filtered-gbuffer" : "native-filtered-edit-depth", true);
            filtered_depth();
        }
        frame.view = studio_view;
        frame.camera_cut = false;
        for (float mode : {2.f, 3.f, 4.f, 5.f}) {
            auto modified = vertices;
            for (auto& v : modified) {
                if (v.position[1] > 0)
                    v.position[0] *= .7f; // Asymmetric triangle charts.
                if (mode == 2)
                    v.uv[0] = v.uv[1] = .125f; // World-space derivatives survive a collapsed UV chart.
                if (mode == 3) {
                    v.normal[0] = v.position[0];
                    v.normal[1] = v.position[1];
                    v.normal[2] = -1;
                }
                if (mode == 4) {
                    v.tangent[0] = v.position[0];
                    v.tangent[1] = v.position[1];
                    v.tangent[2] = 0;
                    v.tangent[3] = -1;
                }
            }
            meshes[0].geometry = std::make_shared<const geometry::Mesh>(
                modified, std::vector<std::uint8_t>{}, std::vector<geometry::MaterialSlot>{{"front"}, {"back"}},
                std::vector<geometry::MeshPart>{{0, 6, 0}, {6, 6, 1}});
            meshes[0].world[0] = -.8f;
            meshes[0].world[5] = 1.3f; // Mirrored, nonuniform world transform.
            select(parameters(textured, {{"mode", mode, true}, {"frequency", 64.f, true}}));
            const auto name = "native-context-mips-" + std::to_string(int(mode));
            draw(name.c_str(), true);
            check(
                channel_difference() < 8,
                "world/normalized-normal/raw-tangent/texture-driven final UV footprints remain filtered under mirrors");
        }
        meshes[0] = original_mesh;
        const auto gray = std::make_shared<const material::Image>(1, 1, std::vector<std::uint8_t>{128, 128, 128, 128});
        select(parameters(texture_override(texture_override(white, "aColor", gray), "mData", gray),
                          {{"mode", -1.f, true}}));
        draw("native-srgb-linear", true);
        check(pixel(capture, x, y)[0] > pixel(capture, x, y)[1] + 35,
              "one exact image has independent linear and sRGB cache interpretations");
        select(texture_override(white, "aColor", {}));
        draw("native-explicit-clear", true);
        check(pixel(capture, x, y)[2] > pixel(capture, x, y)[1] + 30 &&
                  pixel(capture, x, y)[1] > pixel(capture, x, y)[0] + 25,
              "explicit empty input samples its linear float4 contract, not white or inherited pixels");
        // Input-local addressing remains distinct even for the same exact image.
        const auto stripe =
            std::make_shared<const material::Image>(2, 1, std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});
        std::array<unsigned, 3> red{};
        for (unsigned address = 0; address < 3; ++address) {
            select(parameters(texture_override(white, "aColor", stripe, static_cast<material::Address>(address)),
                              {{"frequency", 3.f, true}}));
            draw("native-address");
            red[address] = pixel(capture, x * 3, y)[0];
        }
        check(red[0] > red[1] + 100 && red[2] > red[1] + 100,
              "repeat/mirror sample the tiled red interval while clamp remains at blue");
        for (unsigned address = 0; address < 3; ++address) {
            select(parameters(texture_override(white, "aColor", stripe, static_cast<material::Address>(address)),
                              {{"frequency", 5.f / 3.f, true}}));
            draw("native-address-odd-tile");
            red[address] = pixel(capture, x * 3, y)[0];
        }
        check(red[0] > red[2] + 100, "mirror reverses odd tiles instead of aliasing repeat");
        for (unsigned address = 0; address < 3; ++address) {
            select(parameters(texture_override(white, "aColor", stripe, static_cast<material::Address>(address)),
                              {{"frequency", 5.f / 3.f, true}, {"mode", -2.f, true}}));
            draw("native-input-orientation");
            const auto* color = pixel(capture, x * 3, y);
            check(color[1] > 150 && (address == 2 ? color[0] < 5 : color[0] > 150),
                  "orientation follows the exact input address, with Mirror reversing only the odd U interval");
        }
        select(parameters(texture_override(white, "aColor", {}, material::Address::mirror), {{"mode", -3.f, true}}));
        draw("native-orientation-folds");
        check(pixel(capture, x, y)[0] > 150 && pixel(capture, x, y)[1] > 150,
              "cleared Mirror input preserves addressing and exact integer folds use positive orientation");
        frame.materials = &batch;
        meshes[0] = original_mesh;
    }
    for (auto shading : {render::Shading::flat, render::Shading::matcap}) {
        frame.view.shading = shading;
        frame.view.cavity.enabled = true;
        draw(shading == render::Shading::flat ? "native-flat-cavity" : "native-matcap-cavity", true);
        coverage();
    }
    frame.view.cavity.enabled = false;
    frame.view.wireframe = render::Wireframe::only;
    for (bool hidden : {true, false}) {
        frame.view.hidden_lines = hidden;
        draw(hidden ? "native-visible-lines" : "native-all-lines");
        coverage();
    }
    frame.view.wireframe = render::Wireframe::off;
    frame.view.shading = render::Shading::scene_lit;
    frame.environment = {};
    auto& lit = frame.view.lit;
    lit.ddgi = lit.gtao = lit.sssr = lit.bloom = lit.shadows = false;
    std::array<render::Light, 1> sunlight{};
    sunlight[0].kind = render::LightKind::directional;
    sunlight[0].intensity = 4;
    frame.lights = sunlight;
    for (auto aa : {render::Antialiasing::none, render::Antialiasing::taa}) {
        lit.antialiasing = aa;
        frame.camera_cut = true;
        draw(aa == render::Antialiasing::none ? "native-gbuffer" : "native-edit-depth", true);
        coverage();
        check(pixel(capture, x, y)[0] > pixel(capture, x, y)[1] + 20 &&
                  pixel(capture, x * 3, y)[1] > pixel(capture, x * 3, y)[0] + 20,
              "GBuffer reads actual program colors");
    }
    meshes[0].material_slots = slots({8, 1});
    draw("native-default-gbuffer", true);
    default_coverage();
    meshes[0].material_slots = slots({0, 1});
    draw("native-repaired-gbuffer");
    coverage();
    frame.camera_cut = false;
    lit.antialiasing = render::Antialiasing::none;
    sunlight[0].direction = {.89442719f, 0, .4472136f};
    draw("native-shadow-off");
    const auto unshadowed = pixel(capture, x * 3, y)[1];
    lit.shadows = true;
    draw("native-shadow-blocked", true);
    check(pixel(capture, x * 3, y)[1] < unshadowed * .3f,
          "atlas shadow pass evaluates the procedural cutout at caster coordinates");
    sunlight[0].direction = {0, 0, 1};
    draw("native-shadow-hole", true);
    check(pixel(capture, x * 3, y)[1] > unshadowed * .7f, "procedural shadow hole exposes the rear receiver");
    {
        // Exactly two program identities, unlike the full table with its error
        // program. The unused first NativeMask definition stays Opaque while
        // the actual caster changes policy. A stale Opaque PSO still opens the
        // camera hole but its constant coverage entry shadows the green rear.
        check(&table[2]->program() == &table[0]->program() &&
                  std::ranges::equal(table[2]->parameters(), table[0]->parameters()),
              "policy transition keeps exact program identity and parameter bytes");
        check(std::ranges::equal(table[2]->inputs(), table[0]->inputs(),
                                 [](const auto& a, const auto& b) {
                                     return a.image == b.image && a.address == b.address && a.color == b.color &&
                                            a.empty_value.x == b.empty_value.x && a.empty_value.y == b.empty_value.y &&
                                            a.empty_value.z == b.empty_value.z && a.empty_value.w == b.empty_value.w;
                                 }) &&
                  table[2]->policy().coverage == material::Coverage::opaque &&
                  table[0]->policy().coverage == material::Coverage::mask &&
                  table[2]->policy().double_sided == table[0]->policy().double_sided &&
                  table[2]->policy().emission_two_sided == table[0]->policy().emission_two_sided,
              "coverage policy is the only changed material input");
        std::array policy_table{table[2], table[1], table[2]};
        auto opaque_policy = take(renderer.prepare_materials(policy_table));
        wait(renderer, opaque_policy);
        meshes[0].material_slots = slots({2, 1});
        frame.materials = &opaque_policy;
        ++frame.scene_identity;
        draw("native-policy-opaque", true);
        check(std::abs(depth(capture, x, y) - .8f) < 1e-5f && std::abs(depth(capture, x * 3, y) - .8f) < 1e-5f &&
                  pixel(capture, x * 3, y)[0] > pixel(capture, x * 3, y)[1] + 20,
              "Opaque caster hides the green rear plane");

        policy_table[2] = table[0];
        auto mask_policy = take(renderer.prepare_materials(policy_table));
        wait(renderer, mask_policy);
        frame.materials = &mask_policy;
        ++frame.scene_identity;
        draw("native-policy-mask", true);
        coverage();
        check(
            pixel(capture, x * 3, y)[1] > unshadowed * .7f &&
                pixel(capture, x * 3, y)[1] > pixel(capture, x * 3, y)[0] + 20,
            "Opaque-to-Mask invalidates shadow coverage PSOs despite identical programs and an Opaque first definition");
        std::swap(policy_table[0], policy_table[2]);
        auto reordered = take(renderer.prepare_materials(policy_table));
        check(take(renderer.preparation(reordered)).ready,
              "coverage classification is independent of first definition and surface order");
        frame.materials = &reordered;
        meshes[0].material_slots = slots({0, 1});
        ++frame.scene_identity;
        draw("native-policy-mask-reordered", true);
        coverage();
        check(pixel(capture, x * 3, y)[1] > unshadowed * .7f &&
                  pixel(capture, x * 3, y)[1] > pixel(capture, x * 3, y)[0] + 20,
              "Mask-first table preserves the same camera and direct-shadow hole");

        policy_table = {table[2], table[1], table[2]};
        auto opaque_again = take(renderer.prepare_materials(policy_table));
        check(take(renderer.preparation(opaque_again)).ready,
              "returning to Opaque reuses only the matching retained classification");
        frame.materials = &opaque_again;
        meshes[0].material_slots = slots({2, 1});
        ++frame.scene_identity;
        draw("native-policy-opaque-restored", true);
        check(std::abs(depth(capture, x, y) - .8f) < 1e-5f && std::abs(depth(capture, x * 3, y) - .8f) < 1e-5f &&
                  pixel(capture, x * 3, y)[0] > pixel(capture, x * 3, y)[1] + 20,
              "Mask-to-Opaque restores the solid caster with the same programs");
        meshes[0].material_slots = slots({0, 1});
        frame.materials = &batch;
        ++frame.scene_identity;
    }
    const auto ready_busy = renderer.close();
    check(!ready_busy && ready_busy.error().cause.code == atelier::rhi::ErrorCode::busy && !ready_busy.error().terminal,
          "close checks live batches before destroying already-used Scene Lit resources");
    std::array<render::MeshInstance, 2> siblings{meshes[0], meshes[0]};
    siblings[1].stable_key = 2;
    siblings[1].world[12] = 4;
    siblings[1].material_slots = slots({2, 1});
    frame.meshes = siblings;
    for (unsigned order = 0; order < 2; ++order) {
        draw("native-shared-geometry");
        coverage();
        check(pixel(capture, x * 3, y)[1] > unshadowed * .7f, "opaque sibling cannot suppress Mask coverage");
        std::swap(siblings[0], siblings[1]);
    }
    siblings[1].world[12] = 0;
    frame.meshes = std::span(siblings).subspan(1);
    draw("native-opaque-only");
    check(std::abs(depth(capture, x * 3, y) - .8f) < 1e-5f, "Opaque policy ignores a negative shader coverage result");
    frame.meshes = meshes;
    draw("native-mask-restored");
    coverage();

    // Reuse the real room/camera/DDGI fixture. Every slot is explicitly mapped
    // to this batch, not converted by production code from old fixed materials.
    struct {
        std::vector<render::MeshInstance> meshes;
    } room;
    for (const auto& mesh : scene_lit_e2e::Room{}.meshes)
        room.meshes.push_back({mesh.world, mesh.selected, mesh.geometry, mesh.stable_key, slots({3})});
    room.meshes[7].geometry =
        std::make_shared<const geometry::Mesh>(std::vector<geometry::Vertex>{{{-2, 6, -2}, {0, -1, 0}, {0, 0}},
                                                                             {{2, 6, -2}, {0, -1, 0}, {1, 0}},
                                                                             {{2, 6, 2}, {0, -1, 0}, {1, 1}},
                                                                             {{-2, 6, -2}, {0, -1, 0}, {0, 0}},
                                                                             {{2, 6, 2}, {0, -1, 0}, {1, 1}},
                                                                             {{-2, 6, 2}, {0, -1, 0}, {0, 1}}});
    room.meshes[7].world = scene_lit_e2e::box(0, 0, 0, 1, 1, 1);
    room.meshes[7].material_slots = slots({4});
    frame.meshes = room.meshes;
    frame.lights = {};
    frame.camera = scene_lit_e2e::camera(drawing.extent);
    ++frame.scene_identity;
    const auto above_view = [&](const geometry::Vertex& vertex) {
        std::array<float, 4> v{vertex.position[0], vertex.position[1], vertex.position[2], 1};
        for (const auto& m : {frame.camera.view, frame.camera.projection}) {
            std::array<float, 4> next{};
            for (unsigned c = 0; c < 4; ++c)
                for (unsigned r = 0; r < 4; ++r)
                    next[c] += v[r] * m[r * 4 + c];
            v = next;
        }
        return v[1] > v[3];
    };
    check(std::ranges::all_of(room.meshes[7].geometry->vertices(), above_view),
          "emitter is entirely outside the primary camera");
    lit.probe_volumes[0].minimum = {-5, -1, -1};
    lit.probe_volumes[0].maximum = {5, 5, 8};
    lit.probe_volumes[0].counts = {11, 7, 10};
    lit.probe_update_budget = 96;
    lit.probe_rays = 64;
    const auto region =
        scene_lit_e2e::project_region(frame.camera, frame.scene_viewport, drawing.extent, {-3.15f, .01f, 1.f});
    const auto transport = [&](const char* name) {
        render::FrameResult result;
        for (unsigned i = 0; i < scene_lit_e2e::warmup_frames; ++i) {
            pump();
            ++drawing.id;
            result = take(prepared_render(renderer, frame, i + 1 == scene_lit_e2e::warmup_frames ? &capture : nullptr));
        }
        scene_lit_e2e::png(capture, output / (std::string(name) + ".png"));
        const auto rgb = scene_lit_e2e::mean_rgb(capture, region);
        std::printf("Native DDGI %s RGB=(%.5f,%.5f,%.5f) rays=%llu\n", name, rgb[0], rgb[1], rgb[2],
                    static_cast<unsigned long long>(result.stats.scene_lit_probe_rays));
        log << name << ',' << rgb[0] << ',' << rgb[1] << ',' << rgb[2] << ",0,0,0,0,0\n";
        log.flush();
        if (lit.ddgi)
            check(result.stats.scene_lit_probe_rays > 0, "native path records actual inline probe ray queries");
        return rgb;
    };
    lit.ddgi = false;
    const auto dark = transport("native-ddgi-off");
    check(dark[0] < 2 && dark[1] < 2 && dark[2] < 2, "offscreen emission has no direct-light shortcut");
    lit.ddgi = true;
    const auto warm = transport("native-ddgi-warm");
    room.meshes[7].material_slots = slots({5});
    const auto cool = transport("native-ddgi-cool");
    check(warm[0] > warm[2] + 20 && cool[2] > cool[0] + 20,
          "arbitrary program emission and exact texture replacement reach DDGI receivers");
    material::Context emitter_point;
    emitter_point.position_world = {0, 6, 0};
    const auto rejected_point = need(table[6]->evaluate(emitter_point));
    check(rejected_point.coverage < 0 && rejected_point.emissive.x > 0,
          "the GI approximation fixture really has negative point coverage and nonzero emission");
    room.meshes[7].material_slots = slots({6});
    ++frame.scene_identity;
    const auto masked = transport("native-ddgi-masked");
    check(masked[0] > 10 && masked[1] > 10 && masked[2] > 10,
          "GI-only opaque traversal keeps full emission from a camera-discarded Mask surface");
    {
        std::vector<std::uint8_t> pixels(256 * 256 * 4);
        for (unsigned v = 0; v < 256; ++v)
            for (unsigned u = 0; u < 256; ++u) {
                const auto at = (v * 256 + u) * 4;
                const bool red = ((u / 4 + v / 4) & 1) == 0;
                pixels[at] = red ? 255 : 0;
                pixels[at + 2] = red ? 0 : 255;
                pixels[at + 3] = 255;
            }
        const auto image = std::make_shared<const material::Image>(256, 256, std::move(pixels));
        auto emitter = texture_override(texture_override(table[4], "aColor", image), "zCoverage", image);
        emitter = need(material::resolve_instance(emitter,
                                                  std::array{material::Override{"frequency", 64.f, true},
                                                             material::Override{"cutoff", -.35f, true},
                                                             material::Override{"mode", -4.f, true}},
                                                  std::array<material::ImageRef, 3>{}));
        material::Context c;
        c.position_world = {0, 6, 0};
        c.normal_world = c.geometric_normal_world = {0, -1, 0};
        c.uv = {4.5f / (256 * 64), .5f / (256 * 64)};
        const auto blue = need(emitter->evaluate(c));
        check(blue.coverage > 0 && blue.emissive.x == 0,
              "LOD0 blue texel passes coverage but its red-blue product emits zero");
        c.uv.x = .5f / (256 * 64);
        const auto red = need(emitter->evaluate(c));
        check(red.coverage < 0 && red.emissive.x == 0,
              "LOD0 red texel fails coverage and its red-blue product emits zero");
        auto filtered_table = table;
        filtered_table[4] = emitter;
        auto filtered = take(renderer.prepare_materials(filtered_table));
        check(take(renderer.preparation(filtered)).ready, "DDGI texture edits reuse the same program set");
        frame.materials = &filtered;
        room.meshes[7].material_slots = slots({4});
        ++frame.scene_identity;
        const auto mixed = transport("native-ddgi-footprint");
        // Filtering before the nonlinear red-blue product yields white emission
        // like the uniform reference. LOD0 is mostly black, even without Mask
        // candidate rejection; rechecking negative filtered coverage erases it.
        for (unsigned channel = 0; channel < 3; ++channel)
            check(mixed[channel] > masked[channel] * .8f,
                  "GI-opaque hits still filter final UV before material math and never recheck filtered coverage");
        check(std::abs(mixed[0] - mixed[2]) < 8, "filtered red-blue product produces neutral emission");
        frame.materials = &batch;
        room.meshes[7].material_slots = slots({6});
    }
    // GI's solid approximation must not depend on which instance first uses a
    // shared BLAS. Direct-shadow Mask behavior is checked above, separately.
    auto emitter = room.meshes[7];
    emitter.stable_key = 900;
    emitter.world[12] = 1000;
    emitter.material_slots = slots({2});
    room.meshes.push_back(emitter);
    frame.meshes = room.meshes;
    for (unsigned order = 0; order < 2; ++order) {
        ++frame.scene_identity;
        const auto value = transport("native-ddgi-shared-mask");
        check(value[0] > 10 && value[1] > 10 && value[2] > 10,
              "GI keeps the masked emitter's full shading with a shared Opaque sibling in either instance order");
        std::swap(room.meshes[7], room.meshes.back());
    }
    // Last use is deliberately NOT captured/finished. Releasing the batch must
    // enqueue retirement, not invalidate the work already submitted to the GPU.
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
    check(take(renderer.preparation(reused)).ready,
          "the last rendered program-set owner remains reusable after batch and image retirement");
    reused = {};
    take(renderer.close());
    std::puts(
        "Native material batches: PASS (pending/old group, cancellation, full slot maps, studio/cavity/GBuffer/edit depth/shadow/inline DDGI, Opaque-Mask-Opaque shadow invalidation, GI-only opaque approximation with footprint shading, shared geometry, in-flight retirement; no Editor binding/history or natural backend rejection proof)");
}
} // namespace material_batch_e2e
