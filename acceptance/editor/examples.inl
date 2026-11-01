void verify_example_identities(const std::filesystem::path& source) {
    namespace fs = std::filesystem;
    // Check before open: Project::refresh creates identities for newly discovered resources.
    for (const auto* folder : {"Content", "Source", "Config"})
        for (const auto& file : fs::recursive_directory_iterator(source / folder))
            if (file.is_regular_file() &&
                (assets::is_mesh_path(file.path()) || file.path().extension() == L".material" ||
                 assets::is_texture_path(file.path()) || assets::is_sky_path(file.path())))
                require(fs::is_regular_file(file.path().wstring() + L".asset.json"),
                        "An example resource has no authored identity.");
}

void verify_examples() {
    namespace fs = std::filesystem;
    const fs::path source = ATELIER_EXAMPLE_PROJECT;
    const auto output = fs::absolute("out/examples-document") / std::to_string(GetTickCount64());
    fs::create_directories(output);
    verify_example_identities(source);
    auto project = take(editor::Project::open(source));
    for (const auto* folder : {"Content/Sponza", "Content/Scene Lit Room", "Content/Physic", "Content/Dungeons"})
        require(std::ranges::any_of(
                    project.entries(),
                    [&](const auto& entry) { return entry.path == folder && entry.kind == assets::AssetKind::folder; }),
                "An example is absent from the ordinary Content catalog.");
    editor::Session resources;
    for (const auto& entry : project.entries())
        if (entry.key.starts_with("project/")) {
            for (const auto& binding : entry.materials) {
                const auto* listed = project.entry(binding.key);
                require(listed && listed->kind == assets::AssetKind::material,
                        "An example mesh names a missing default material.");
                const auto root = take(resources.material_resource(project.assets(), binding.key));
                const auto surface = take(material::capture_program(resources.world().runtime(), root.id()));
                require(surface->diagnostics().empty() && !surface->uses_default_material(),
                        "An example default material failed to resolve.");
                for (const auto& declaration : take(resources.inspect_material(root.id())).declarations)
                    if (const auto* texture = std::get_if<material::TextureBinding>(&declaration.value);
                        texture && !texture->asset_key.empty())
                        require(project.entry(texture->asset_key) &&
                                    project.entry(texture->asset_key)->kind == assets::AssetKind::texture,
                                "An example default material names a missing texture.");
            }
        }
    std::size_t scenes{};
    for (const auto& entry : project.entries())
        if (entry.kind == assets::AssetKind::scene) {
            const auto original = take(assets::read_file(source / entry.path));
            auto document = take(editor::Session::deserialize(original, project, {}, editor::LoadPolicy::strict));
            const auto meshes = take(document->presentation().render_items());
            require(!meshes.empty() && document->load_issues().empty() && !document->dirty() &&
                        document->undo_count() == 0,
                    "An example failed strict loading or opening modified its authored World.");
            const auto camera = take(document->presentation().main_camera());
            take(camera.matrices(16.f / 9));
            require(camera.entity != ObjectId{} && take(document->main_camera()) == camera.entity,
                    "An example has no usable authored main camera.");
            std::size_t saved_bindings{};
            std::size_t loaded_bindings{};
            for (const auto& entity : nlohmann::json::parse(original)["entities"])
                for (const auto& part : entity["components"])
                    if (const auto at = part.find("materials"); at != part.end())
                        saved_bindings += at->size();
            for (const auto& item : meshes) {
                const auto bindings =
                    take(presentation::read_material_bindings(document->world().runtime(), item.component));
                loaded_bindings += bindings.size();
                const auto state = take(presentation::read_static_mesh(document->world().runtime(), item.component));
                for (const auto& binding : state.mesh.defaults) {
                    std::size_t matches{};
                    for (std::size_t slot = 0; slot < item.geometry->materials().size(); ++slot)
                        matches += presentation::material_slot_name(*item.geometry, slot) == binding.slot;
                    require(matches == 1, "An example mesh default no longer names a unique real slot.");
                }
            }
            require(loaded_bindings == saved_bindings, "An example lost an explicit material binding.");
            const auto instance = take(document->create_scene_material(
                meshes.front().component, presentation::material_slot_name(*meshes.front().geometry, 0)));
            const auto fields = take(document->inspect_material(instance.id()));
            const auto number = std::ranges::find_if(
                fields.declarations, [](const auto& field) { return std::holds_alternative<float>(field.value); });
            require(number != fields.declarations.end(), "An example has no editable numeric material declaration.");
            take(document->begin_material_edit(instance.id(), 881001));
            take(document->preview_material_entry(881001, {number->path, .61f, true}));
            take(document->commit_material_edit(881001));
            const auto saved = take(document->serialize());
            const auto path = output / entry.path;
            take(editor::write_file(path, saved));
            auto reopened = take(
                editor::Session::deserialize(take(assets::read_file(path)), project, {}, editor::LoadPolicy::strict));
            require(atelier::acceptance::dungeons_roundtrip_equal(saved, take(reopened->serialize())) &&
                        take(reopened->presentation().main_camera()).position == camera.position &&
                        take(assets::read_file(source / entry.path)) == original,
                    "Example save/reopen lost authored data or modified the source project.");
            ++scenes;
        }
    require(scenes == 14, "The ordinary examples project must contain all fourteen scenes.");
    require(editor::Project::is_scene_path("Content/Another Scene/Custom.scene") &&
                !editor::Project::is_scene_path("Config/Custom.scene") &&
                !editor::Project::is_scene_path("Examples/Custom.scene"),
            "Scene storage no longer follows the ordinary Content boundary.");
    std::puts(
        "Examples document acceptance: PASS (14 strict scenes, authored cameras and identities, material slots, isolated save/reopen)");
}
