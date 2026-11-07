// Import consumers inspect author paths and the actual compiled input table,
// never StandardPBR slot numbers or geometry-owned material content.
template <class T>
const T& imported_material_value(const assets::MaterialInstanceAsset& author, std::string_view path) {
    const auto at = std::ranges::find(author.entries, path, &material::Override::path);
    require(at != author.entries.end() && at->enabled && std::holds_alternative<T>(at->value),
            "An imported MI value is missing, disabled or has the wrong type.");
    return std::get<T>(at->value);
}

const material::SurfaceInput& material_input(const material::ProgramSurface& surface, std::string_view path) {
    const auto inputs = surface.program().textures();
    const auto at = std::ranges::find(inputs, path, &source::MaterialTextureInput::key);
    require(at != inputs.end() && surface.inputs().size() == inputs.size(),
            "Resolved material input does not match its Source declaration.");
    return surface.inputs()[static_cast<std::size_t>(at - inputs.begin())];
}

template <class T>
T resolved_material_value(const material::ProgramSurface& surface, std::string_view path) {
    const auto parameters = surface.program().parameters();
    const auto at = std::ranges::find(parameters, path, &source::MaterialParameter::key);
    require(at != parameters.end() && at->components * sizeof(float) == sizeof(T) &&
                at->offset + sizeof(T) <= surface.parameters().size(),
            "Resolved material parameter does not match its Source declaration.");
    T result;
    std::memcpy(&result, surface.parameters().data() + at->offset, sizeof(T));
    return result;
}

void verify_material_picking() {
    namespace mat = atelier::material;
    const std::vector<atelier::geometry::Vertex> vertices{
        {{-1, -1, 0}, {0, 0, -1}, {0, 1}}, {{-1, 1, 0}, {0, 0, -1}, {0, 0}}, {{1, 1, 0}, {0, 0, -1}, {1, 0}},
        {{-1, -1, 0}, {0, 0, -1}, {0, 1}}, {{1, 1, 0}, {0, 0, -1}, {1, 0}},  {{1, -1, 0}, {0, 0, -1}, {1, 1}}};
    const auto opacity = std::make_shared<const mat::Image>(
        2, 2, std::vector<std::uint8_t>{0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255});
    const auto mesh = std::make_shared<const atelier::geometry::Mesh>(vertices, std::vector<std::uint8_t>{},
                                                                      std::vector<geometry::MaterialSlot>{{"surface"}});
    editor::Session document;
    assets::Reader installed;
    take(document.publish_texture("fixture/opacity", opacity));
    assets::MaterialAsset cutout{
        "fixture/cutout", L"Cutout",
        assets::MaterialInstanceAsset{"builtin/material/standard-mask-double",
                                      {{"opacity_texture", mat::TextureBinding{"fixture/opacity"}}}}};
    const auto material = take(document.material_resource(installed, cutout));
    changed(document.create_static_mesh({{}, L"Cutout", mesh}));
    const auto front = document.selection().entity;
    const auto front_component = component(document, front, document.presentation_types().static_mesh_component);
    const std::array bindings{presentation::MaterialBinding{"surface", material, true}};
    take(presentation::set_material_bindings(document.world().runtime(), front_component, bindings));
    changed(document.create_static_mesh({{}, L"Receiver", mesh}, {0, 0, .8f}));
    const auto back = document.selection().entity;
    editor::Camera camera;
    require(camera.look_at({0, 0, -5}, {0, 0, 0}), "Material picking camera is invalid.");
    auto view = editor::ViewportView::make(camera, {0, 0, 800, 600}, 1);
    application::SceneSnapshot snapshot;
    const auto publish = [&] {
        take(snapshot.refresh(document));
        view.scene = snapshot.picking();
    };
    publish();
    editor::ViewportTool tool;
    const auto pick = [&](math::Vector3 position) {
        const auto p = view.project(position);
        require(p.has_value(), "Material picking point is outside its viewport.");
        return tool.pick(document, view, p->x, p->y);
    };
    require(pick({-.5f, .5f, 0}) == back && pick({.5f, .5f, 0}) == front,
            "Point picking did not pass through transparent texels to the rear Entity.");
    auto& author = std::get<assets::MaterialInstanceAsset>(cutout.author);
    author.entries.push_back({"opacity_uv.scale", math::Vector2{-1, 1}});
    require(take(document.publish_material(installed, cutout)).id() == material.id(),
            "Changing sampling replaced the material identity.");
    publish();
    require(pick({-.5f, .5f, 0}) == front && pick({.5f, .5f, 0}) == back,
            "Point picking ignored mirrored negative UV repetition.");
    author.parent_key = "builtin/material/standard-opaque-single";
    require(take(document.publish_material(installed, cutout)).id() == material.id(),
            "Changing the policy Source replaced the material identity.");
    const auto surface = take(mat::capture_program(document.world().runtime(), material.id()));
    require(surface->policy().coverage == mat::Coverage::opaque && !surface->policy().double_sided,
            "Picking material did not resolve its new Source policy.");
    take(document.select(back));
    changed(document.erase_entities(document.selected_entities()));
    publish();
    require(pick({0, 0, 0}) == front, "Single-sided picking discarded the authored front side.");
    require(camera.look_at({0, 0, 5}, {0, 0, 0}), "Backface picking camera is invalid.");
    view = editor::ViewportView::make(camera, {0, 0, 800, 600}, 1);
    publish();
    require(!pick({0, 0, 0}), "Point picking selected an invisible single-sided backface.");
    std::puts("Material picking acceptance: PASS (transparent holes, UV sampling, source replacement, sidedness)");
}

void verify_textured_obj() {
    verify_material_picking();
    namespace fs = std::filesystem;
    const auto evidence = fs::current_path() / "project-document-evidence" /
                          std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const auto source = evidence / "source";
    fs::create_directories(source);
    // An actual 2x2 BGRA BMP travels through WIC and the real import transaction.
    std::string bmp(70, '\0');
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = 70;
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = 2;
    bmp[22] = 2;
    bmp[26] = 1;
    bmp[28] = 32;
    for (std::size_t i = 54; i < bmp.size(); i += 4) {
        bmp[i + 2] = char(255);
        bmp[i + 3] = char(255);
    }
    take(editor::write_file(source / "color.bmp", bmp));
    take(editor::write_file(source / "scene.mtl",
                            "newmtl red\nKd 1 0.5 0.25\nmap_Kd color.bmp\nnewmtl other\nKd 0 1 0\nmap_Kd color.bmp\n"));
    const std::string obj =
        "mtllib scene.mtl\nv 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\nvt -1 0\nvt 2 0\nvt 2 1\nvt 0.5 0.5\nvt 0 1\nusemtl red\nf 1/1 2/2 3/3 4/4 5/5\nusemtl other\nf -5/1 -4/2 -2/4\n";
    take(editor::write_file(source / "scene.obj", obj));
    auto project = take(editor::Project::create(evidence / "project"));
    const auto key = take(project.import_mesh(source / "scene.obj"));
    const auto asset = take(project.assets().mesh(key));
    const auto old = asset.mesh;
    editor::Session original;
    const auto original_surfaces = take(original.mesh_surfaces(project.assets(), asset));
    require(original_surfaces && original_surfaces->size() == 3, "OBJ resolved material table lost a geometry slot.");
    const auto& red = (*original_surfaces)[1];
    const auto& other = (*original_surfaces)[2];
    const auto pixels = material_input(*red, "base_color_texture").image;
    require(old->vertices().size() == 12 && old->parts().size() == 2 && old->materials().size() == 3 &&
                asset.materials.size() == 2 && asset.materials[0].slot == "red" && asset.materials[1].slot == "other" &&
                pixels && pixels == material_input(*other, "base_color_texture").image,
            "OBJ lost triangles, material parts, default bindings or shared images.");
    require((*original_surfaces)[0] == take(material::default_world_surface()) && pixels->pixels()[0] == 255,
            "Unbound OBJ slot lost the grid default or the decoded material pixel is wrong.");
    const auto red_asset = take(project.assets().material(asset.materials[0].key));
    const auto& red_author = std::get<assets::MaterialInstanceAsset>(red_asset.author);
    const auto texture_key =
        imported_material_value<material::TextureBinding>(red_author, "base_color_texture").asset_key;
    require(imported_material_value<math::Vector4>(red_author, "base_color") == math::Vector4{1, .5f, .25f, 1} &&
                take(red->evaluate({})).base_color == math::Vector3{1, 0, 0} &&
                take(material::capture_program(
                    original.world().runtime(),
                    take(original.material_resource(project.assets(), red_asset.key)).id())) == red,
            "OBJ MI author factors or its resolved StandardPBR result changed.");
    bool negative_uv = false;
    double area = 0;
    for (const auto& vertex : old->vertices())
        negative_uv |= vertex.uv[0] < 0;
    for (std::size_t i = 0; i < 9; i += 3) {
        const auto& a = old->vertices()[i].position;
        const auto& b = old->vertices()[i + 1].position;
        const auto& c = old->vertices()[i + 2].position;
        area += std::abs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) * .5;
    }
    require(negative_uv && area == 3, "OBJ changed wrap coordinates or filled a concave cutout.");
    const auto revision = project.revision();
    take(project.refresh());
    require(project.revision() == revision && take(project.assets().mesh(key)).mesh == old,
            "Unchanged refresh replaced a mesh resource.");
    const auto sibling = project.root() / "Content/Meshes/scene/sibling.obj";
    take(editor::write_file(sibling, obj));
    take(project.refresh());
    const auto sibling_entry =
        std::ranges::find(project.entries(), fs::path("Content/Meshes/scene/sibling.obj"), &assets::ProjectEntry::path);
    require(sibling_entry != project.entries().end(), "Shared-texture mesh is absent from the real project catalog.");
    const auto sibling_key = sibling_entry->key;
    // Geometry discovery does not synthesize a second material product path. This
    // sibling explicitly shares the original named defaults in its real sidecar.
    auto sibling_metadata = sibling;
    sibling_metadata += L".asset.json";
    json::Builder mapping{take(assets::json_format::parse(take(assets::read_file(sibling_metadata))))};
    const auto materials = mapping.object_validated(mapping.root(), "materials");
    for (const auto& binding : asset.materials)
        take(mapping.set(materials, binding.slot, binding.key));
    const auto mapping_bytes = json::write(mapping.root(), {.pretty = true});
    take(editor::write_file(sibling_metadata, {mapping_bytes.data(), mapping_bytes.size()}));
    take(project.refresh());
    const auto sibling_asset = take(project.assets().mesh(sibling_key));
    const auto sibling_mesh = sibling_asset.mesh;
    const auto sibling_surfaces = take(original.mesh_surfaces(project.assets(), sibling_asset));
    require(sibling_asset.materials == asset.materials && (*sibling_surfaces)[1] == red,
            "Sibling defaults did not resolve to the same material resource.");
    // import_mesh already owns its first bundle; reopen to exercise the project cache.
    auto shared_project = take(editor::Project::open(project.root()));
    const auto shared_a = take(shared_project.assets().mesh(key));
    const auto shared_b = take(shared_project.assets().mesh(sibling_key));
    editor::Session shared;
    const auto surfaces_a = take(shared.mesh_surfaces(shared_project.assets(), shared_a));
    const auto surfaces_b = take(shared.mesh_surfaces(shared_project.assets(), shared_b));
    require(shared_a.mesh != shared_b.mesh && material_input(*(*surfaces_a)[1], "base_color_texture").image ==
                                                  material_input(*(*surfaces_b)[1], "base_color_texture").image,
            "Different model assets decoded the same atlas more than once.");
    const auto texture = project.root() / "Content/Meshes/scene/color.bmp";
    for (std::size_t i = 54; i < bmp.size(); i += 4) {
        bmp[i] = char(255);
        bmp[i + 2] = 0;
    }
    take(editor::write_file(texture, bmp));
    fs::last_write_time(texture, fs::last_write_time(texture) + std::chrono::seconds(2));
    take(project.refresh());
    const auto changed_asset = take(project.assets().mesh(key));
    const auto sibling_changed = take(project.assets().mesh(sibling_key));
    editor::Session refreshed;
    const auto current = take(refreshed.mesh_surfaces(project.assets(), changed_asset));
    const auto current_sibling = take(refreshed.mesh_surfaces(project.assets(), sibling_changed));
    const auto changed_pixels = material_input(*(*current)[1], "base_color_texture").image;
    require(sibling_changed.mesh != sibling_mesh &&
                changed_pixels == material_input(*(*current_sibling)[1], "base_color_texture").image &&
                material_input(*(*sibling_surfaces)[1], "base_color_texture").image == pixels &&
                pixels->pixels()[0] == 255,
            "Shared atlas refresh lost sharing or retired pixels.");
    require(changed_asset.mesh != old && changed_pixels != pixels && changed_pixels->pixels()[2] == 255 &&
                pixels->pixels()[0] == 255 && project.entry(texture_key) && changed_asset.key == asset.key &&
                changed_asset.materials == asset.materials &&
                take(assets::mesh_content_sha256(changed_asset)) == take(assets::mesh_content_sha256(asset)),
            "Texture refresh did not publish a new resource while preserving its old owner.");
    take(editor::write_file(project.root() / "Content/Meshes/scene/scene.mtl",
                            "newmtl red\nKd 0 1 0\nmap_Kd color.bmp\nnewmtl other\nKd 1 1 1\n"));
    take(project.refresh());
    const auto edited = take(project.assets().mesh(key));
    const auto source_author = take(assets::load_obj(project.root() / "Content/Meshes/scene/scene.obj"));
    const auto current_material = assets::material_format::encode(take(project.assets().material(red_asset.key)));
    const auto original_material = assets::material_format::encode(red_asset);
    require(edited.mesh != changed_asset.mesh &&
                imported_material_value<math::Vector4>(*source_author.materials[1], "base_color") ==
                    math::Vector4{0, 1, 0, 1} &&
                current_material && original_material && *current_material == *original_material &&
                take(assets::mesh_content_sha256(edited)) == take(assets::mesh_content_sha256(asset)),
            "MTL refresh failed to invalidate its dependency or silently overwrote the independent MI author data.");
    take(editor::write_file(source / "bad.obj", "mtllib ../outside.mtl\nv 0 0 0\n"));
    static_cast<void>(project.import_mesh(source / "bad.obj"));
    require(!fs::exists(project.root() / "Content/Meshes/bad.obj"), "Unsafe dependency left a published import.");
    take(editor::write_file(source / "degenerate_uv.obj",
                            "v 0 0 0\nv 0 1 0\nv 0 0 1\nvt 0 0\nvn 1 0 0\nf 1/1/1 2/1/1 3/1/1\n"));
    const auto degenerate_key = take(project.import_mesh(source / "degenerate_uv.obj"));
    const auto degenerate = take(project.assets().mesh(degenerate_key)).mesh;
    for (const auto& vertex : degenerate->vertices()) {
        const auto& n = vertex.normal;
        const auto& t = vertex.tangent;
        require(n[0] == 1 && n[1] == 0 && n[2] == 0, "Tangent generation changed the authored OBJ normal.");
        require(std::abs(std::hypot(double(t[0]), double(t[1]), double(t[2])) - 1) < 1e-6 &&
                    std::abs(double(n[0]) * t[0] + double(n[1]) * t[1] + double(n[2]) * t[2]) < 1e-6 &&
                    std::abs(t[3]) == 1,
                "Degenerate OBJ UVs did not retain a finite orthonormal tangent frame.");
    }
    std::puts(
        "Textured OBJ acceptance: PASS (bundle import, concave polygon, UV, tangent fallback, shared texture, refresh, lifetime, failure/retry boundaries)");
}
