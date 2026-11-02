// Exercise the same document transactions used by the color popup, including
// real material assets, component instances and their existing save/history paths.
void verify_color_edits() {
    const auto root =
        std::filesystem::absolute("out/color-edits-" + std::to_string(GetCurrentProcessId()) + "-" +
                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto project = take(editor::Project::create(root / "Project"));
    editor::Session document;
    auto& runtime = document.world().runtime();
    const auto paths = [&](ObjectId object, std::string_view prefix) {
        std::vector<world::FieldPath> result;
        for (const auto& property : take(document.properties(object)))
            if (property.name.starts_with(prefix))
                result.push_back(property.path);
        require(result.size() == 3, "Color did not expose three channels.");
        return result;
    };
    const auto read = [&](ObjectId object, const auto& fields) {
        std::vector<Scalar> result;
        for (const auto& path : fields)
            result.push_back(take(document.read_property(object, path)));
        return result;
    };
    const std::array first{Scalar{.2f}, Scalar{.4f}, Scalar{.6f}};
    const std::array last{Scalar{.7f}, Scalar{.3f}, Scalar{.5f}};
    const std::array invalid_range{Scalar{.8f}, Scalar{1e30f}, Scalar{.1f}};

    changed(document.create_light(presentation::LightKind::point, {0, 2, 0}));
    const auto light = take(world::components(runtime, document.selection().entity)).front().id();
    const auto light_paths = paths(light, "settings.color.");
    const auto original = read(light, light_paths);
    const auto light_history = document.undo_count();
    document.mark_saved();
    take(document.begin_group_edit(light, light_paths, 9001));
    require(document.dirty() && document.editing(), "An active color group was omitted from scene dirty state.");
    take(document.preview_group_edit(9001, first));
    (void)document.preview_group_edit(9001, invalid_range);
    require(read(light, light_paths) == std::vector<Scalar>(first.begin(), first.end()),
            "A rejected group value changed an earlier channel.");
    take(document.cancel_edit());
    require(!document.dirty() && read(light, light_paths) == original && document.undo_count() == light_history,
            "Cancel did not restore the color and saved state.");
    take(document.begin_group_edit(light, light_paths, 9002));
    take(document.preview_group_edit(9002, first));
    take(document.preview_group_edit(9002, last));
    take(document.commit_edit(9002));
    require(document.undo_count() == light_history + 1, "Color preview created more than one history entry.");
    changed(document.undo());
    require(read(light, light_paths) == original && !document.dirty(), "Color undo missed its saved value.");
    changed(document.redo());
    require(read(light, light_paths) == std::vector<Scalar>(last.begin(), last.end()),
            "Color redo missed its final value.");

    const auto key = take(project.create_material("Content/Materials/Color.material"));
    const auto material = take(document.material_resource(project.assets(), key));
    const auto color = [](const editor::Session& session, ObjectId object) {
        const auto view = take(session.inspect_material(object));
        const auto at = std::ranges::find(view.declarations, "base_color", &editor::MaterialParameterView::path);
        require(at != view.declarations.end() && at->components == 4,
                "MS base_color is not an owning vector4 declaration.");
        return std::get<math::Vector4>(at->value);
    };
    const auto entries = [&](ObjectId object) {
        return assets::material_format::encode_entries(take(document.inspect_material(object)).entries);
    };
    const auto preview = [&](std::uint64_t control, math::Vector4 value) {
        return document.preview_material_entry(control, {"base_color", value, true});
    };
    const math::Vector4 first_color{.2f, .4f, .6f, .7f};
    const math::Vector4 last_color{.7f, .3f, .5f, .7f};
    const math::Vector4 non_finite{.8f, std::nanf(""), .1f, .7f};
    const auto material_before = color(document, material.id());
    document.mark_saved();
    const auto scene_history = document.undo_count();
    take(document.begin_material_edit(material.id(), 9010));
    require(document.material_dirty(key) && !document.dirty(), "Asset color edited the scene's dirty state.");
    take(preview(9010, first_color));
    (void)preview(9010, non_finite);
    require(color(document, material.id()) == first_color, "Rejected material color left a partial preview.");
    take(document.cancel_material_edit());
    require(entries(material.id()).empty() && color(document, material.id()) == material_before &&
                !document.material_dirty(key),
            "Cancelling a first asset color preview did not restore sparse absence.");
    take(document.begin_material_edit(material.id(), 9011));
    take(preview(9011, last_color));
    take(document.commit_material_edit(9011));
    require(document.undo_count() == scene_history && !document.dirty(), "Asset color created scene history.");
    take(document.material_undo(key));
    require(color(document, material.id()) == material_before && !document.material_dirty(key),
            "Asset color did not undo in one step.");
    take(document.material_undo(key, true));
    take(document.save_material(project, key));
    require(!document.material_dirty(key), "Asset color did not use the material save path.");
    auto opened = take(editor::Project::open(project.root()));
    editor::Session reloaded;
    const auto reloaded_material = take(reloaded.material_resource(opened.assets(), key));
    require(color(reloaded, reloaded_material.id()) == last_color,
            "Saved color or its untouched alpha did not survive material reopen.");

    changed(document.create_static_mesh(take(project.assets().mesh("builtin/cube"))));
    const auto entity = document.selection().entity;
    const auto mesh = component(document, entity, document.presentation_types().static_mesh_component);
    take(document.assign_material(project, mesh, "slot/0", key));
    const auto instance = take(document.create_scene_material(mesh, "slot/0"));
    take(document.set_material_override(instance.id(), "base_color", true));
    take(document.set_material_override(instance.id(), "base_color", false));
    const auto before = entries(instance.id());
    const auto instance_history = document.undo_count();
    document.mark_saved();
    take(document.begin_material_edit(instance.id(), 9020));
    (void)preview(9020, non_finite);
    require(entries(instance.id()) == before,
            "Rejected instance preview retained a color override or partial channels.");
    take(preview(9020, first_color));
    take(document.cancel_edit());
    require(entries(instance.id()) == before && !document.dirty(), "Cancel did not restore inherited instance color.");
    take(document.begin_material_edit(instance.id(), 9021));
    take(preview(9021, first_color));
    take(preview(9021, last_color));
    take(document.commit_material_edit(9021));
    require(document.undo_count() == instance_history + 1 && document.dirty() && !document.material_dirty(key),
            "Instance color did not use exactly one scene history entry.");
    changed(document.undo());
    require(entries(instance.id()) == before && !document.dirty(), "Instance undo did not restore inheritance.");
    changed(document.redo());
    auto scene = take(editor::Session::deserialize(take(document.serialize()), project));
    ObjectId saved_mesh;
    for (const auto& member : take(scene->world().members()))
        for (const auto& part : take(world::components(scene->world().runtime(), member.id())))
            if (take(scene->world().runtime().is_a(take(scene->world().runtime().type_of(part.id())),
                                                   scene->presentation_types().static_mesh_component)))
                saved_mesh = part.id();
    require(saved_mesh != ObjectId{} &&
                take(take(presentation::material_surfaces(scene->world().runtime(), saved_mesh))->front()->evaluate({}))
                        .base_color == math::Vector3{last_color.x, last_color.y, last_color.z},
            "Instance color did not survive scene roundtrip.");
    std::puts("Editor color transactions: PASS (dirty, reject rollback, cancel, asset/instance history and save)");
}
