// Native metadata -> real editor document -> editor.ui.source -> accepted UI
// geometry and delayed help. This path needs no GPU or desktop input injection.
void verify_property_tooltips() {
    editor::Session document;
    changed(document.create_static_mesh(assets::mesh_assets().front()));
    const auto mesh = document.selection().entity;
    const auto mesh_component = component(document, mesh, document.presentation_types().static_mesh_component);
    const auto material = take(document.create_scene_material(mesh_component, "slot/0"));
    take(document.set_material_override(material.id(), "cell_size", true));
    take(document.set_material_override(material.id(), "light", true));
    changed(document.create_light(presentation::LightKind::point));
    const auto light = component(document, document.selection().entity, document.presentation_types().light_component);
    take(document.select(mesh));
    document.mark_saved();
    const auto history = document.undo_count(), redo = document.redo_count();
    const auto lighting = take(document.presentation().environment());
    auto& runtime = document.world().runtime();
    const auto* mesh_type = take(runtime.describe(take(runtime.type_of(mesh))));
    const auto field = [&](TypeId type, std::string_view name) -> const FieldInfo& {
        const auto* info = take(runtime.describe(type));
        const auto at = std::ranges::find(info->fields, name, &FieldInfo::name);
        require(at != info->fields.end(), "Tooltip acceptance field is absent from Runtime.");
        return *at;
    };
    const auto& position = field(mesh_type->id, "position");
    const auto& rotation = field(mesh_type->id, "rotation");
    const auto& color = field(document.presentation_types().light_settings, "color");
    require(!position.description.empty() && !rotation.description.empty() && !color.description.empty(),
            "Real editor authoring fields have no native descriptions.");

    editor::Workspace workspace;
    const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface(fonts);
    const auto theme = ui::StyleSheet::parse(take(assets::read_file(editor::asset_directory() / "editor.ss")));
    require(theme && surface.set_style_sheet(*theme) && surface.set_surface({{1600, 1000}, 1, 1}),
            "Property tooltip editor surface failed.");
    std::vector<ui::Control> controls;
    const auto publish = [&] {
        controls = workspace.describe(document, {1600, 1000}, surface.presented().get());
        require(workspace.ui_error().empty(), "Property tooltip production DSL failed.");
        auto described = surface.set_controls(controls);
        require(described.has_value(), "Property tooltip descriptors were rejected.");
        for (const auto& effect : described->effects)
            if (const auto* action = std::get_if<ui::Action>(&effect)) {
                if (action->kind == ui::ActionKind::edit_cancel)
                    take(document.cancel_edit());
                else
                    take(workspace.apply_ui(*action));
            }
        workspace.commit_ui_controls();
        auto frame = surface.prepare();
        require(frame && surface.accept(*frame), "Property tooltip frame failed to prepare or accept.");
    };
    const auto label = [&](std::wstring_view text) -> ui::ControlKey {
        const auto at = std::ranges::find_if(controls, [&](const auto& control) {
            return control.kind == ui::ControlKind::label && control.text == text &&
                   surface.presented()->find(control.key);
        });
        require(at != controls.end(), "Property tooltip label is absent from accepted layout.");
        return at->key;
    };
    const auto help_for = [&](ui::ControlKey key) -> std::shared_ptr<const ui::Tooltip> {
        for (auto* box = surface.presented()->find(key); box;
             box = box->parent ? surface.presented()->find(box->parent) : nullptr)
            if (box->tooltip)
                return box->tooltip;
        throw std::runtime_error("Property tooltip did not reach its accepted label or ancestors.");
    };
    const auto hover = [&](ui::ControlKey key) {
        const auto* box = surface.presented()->find(key);
        require(box != nullptr, "Property tooltip hover target disappeared.");
        const auto left = std::max(box->bounds.x, float(box->clip.left));
        const auto right = std::min(box->bounds.x + box->bounds.width, float(box->clip.right));
        const auto top = std::max(box->bounds.y, float(box->clip.top));
        const auto bottom = std::min(box->bounds.y + box->bounds.height, float(box->clip.bottom));
        require(right > left && bottom > top, "Property tooltip label is clipped.");
        const auto expected = help_for(key);
        const auto reply =
            surface.input({.kind = ui::EventKind::pointer_move, .x = (left + right) * .5f, .y = (top + bottom) * .5f});
        require(!reply.error, "Property tooltip pointer input failed.");
        Sleep(520);
        publish();
        require(surface.presented()->tooltip && surface.presented()->tooltip->content == expected,
                "Hovering the real property label did not show its help after the delay.");
        return surface.presented();
    };
    publish();
    const auto position_key = label(L"Position (m)"), rotation_key = label(L"Rotation (°)");
    const auto position_help = help_for(position_key), rotation_help = help_for(rotation_key);
    require(position_help->title == L"Position (m)" &&
                position_help->description == assets::wide(position.description) &&
                position_help->identity == assets::wide(mesh_type->name) + L".position : " +
                                               assets::wide(take(runtime.describe(position.value.type))->name),
            "Default Transform Position help differs from its Runtime aggregate field.");
    require(rotation_help->description == assets::wide(rotation.description) &&
                rotation_help->identity == assets::wide(mesh_type->name) + L".rotation : " +
                                               assets::wide(take(runtime.describe(rotation.value.type))->name) +
                                               L" · Euler (°)",
            "Rotation help lost its Quaternion storage type or displayed Euler unit.");
    const auto type_help = help_for(52);
    require(type_help->identity == assets::wide(mesh_type->name) &&
                type_help->description == assets::wide(mesh_type->description),
            "Object header help differs from the actual Runtime type.");
    const auto position_frame = hover(position_key);
    publish();
    require(surface.presented()->tooltip && surface.presented()->tooltip->content == position_help,
            "Recomposing an unchanged document restarted or replaced its visible help.");

    take(document.select(light));
    publish();
    require(!surface.presented()->tooltip && !surface.accept(position_frame),
            "Switching inspected objects retained or accepted an old tooltip frame.");
    const auto color_help = help_for(label(L"Color"));
    const auto* light_type = take(runtime.describe(take(runtime.type_of(light))));
    require(color_help->description == assets::wide(color.description) + L"\n硬下限 0" &&
                color_help->identity == assets::wide(light_type->name) + L".settings.color : " +
                                            assets::wide(take(runtime.describe(color.value.type))->name),
            "Light Color help used a scalar lane identity or lost its aggregate description and hard-bound note.");

    // Reflected light ranges remain native metadata; material declarations no
    // longer inherit the removed fixed StandardPBR reflection bounds.
    const auto sliders_beside = [&](const ui::Control& number) {
        return std::ranges::count_if(controls, [&](const auto& control) {
            return control.kind == ui::ControlKind::slider && control.parent == number.parent;
        });
    };
    const auto* lanes = take(runtime.describe(document.types().vector3));
    for (const auto& lane : lanes->fields) {
        const world::FieldPath path{field(take(runtime.type_of(light)), "settings").id, color.id, lane.id};
        const auto at = std::ranges::find(controls, workspace.field_control(light, path), &ui::Control::key);
        require(at != controls.end() && at->kind == ui::ControlKind::number && at->minimum == 0 &&
                    sliders_beside(*at) == 0,
                "Light Color lanes lost their native hard lower bound or acquired an undeclared slider.");
    }
    take(document.select(mesh_component));
    publish();
    const auto number_at = [&](std::string_view path, std::size_t lane = 0) -> const ui::Control& {
        const auto at = std::ranges::find_if(controls, [&](const auto& control) {
            const auto* binding = workspace.binding(control.key);
            return control.kind == ui::ControlKind::number && binding && binding->object == material.id() &&
                   binding->kind == editor::Workspace::Binding::Kind::material_field &&
                   binding->material_path == path && binding->index == lane;
        });
        require(at != controls.end(), "A declared material parameter has no stable-path number control.");
        return *at;
    };
    const auto scalar_key = number_at("cell_size").key;
    require(number_at("cell_size").minimum == -std::numeric_limits<float>::max() &&
                number_at("cell_size").maximum == std::numeric_limits<float>::max() &&
                sliders_beside(number_at("cell_size")) == 0,
            "An arbitrary material scalar acquired a hard-coded StandardPBR range.");
    for (std::size_t lane = 0; lane < 3; ++lane)
        require(sliders_beside(number_at("light", lane)) == 0, "A material vector lane acquired an undeclared slider.");
    const auto material_help = help_for(label(L"cell_size"));
    require(material_help->identity == L"Material declaration · cell_size" && !material_help->description.empty(),
            "Material help did not describe its stable declaration path and override semantics.");
    publish();
    require(number_at("cell_size").key == scalar_key,
            "Material recomposition changed its declaration control identity.");
    take(document.select(ObjectId{}));
    publish();
    require(!surface.presented()->tooltip && !surface.accept(position_frame) &&
                std::ranges::none_of(surface.presented()->controls,
                                     [&](const auto& control) {
                                         return control.tooltip == position_help || control.tooltip == color_help;
                                     }),
            "Empty selection retained the previous object's property help.");

    workspace.dock.select(editor::world_control::tab);
    publish();
    const auto* exposure = editor::view_option(editor::ViewOption::exposure_ev);
    require(exposure != nullptr, "World exposure option metadata is missing.");
    const auto exposure_key = label(assets::wide(exposure->label));
    const auto exposure_help = help_for(exposure_key);
    require(exposure_help->title == assets::wide(exposure->label) &&
                exposure_help->description == assets::wide(exposure->description) &&
                exposure_help->identity.find(assets::wide(exposure->name)) != std::wstring::npos &&
                exposure_help->identity.starts_with(L"Scene · "),
            "World property help diverged from the shared ViewOption descriptor.");
    const auto world_frame = hover(exposure_key);
    workspace.dock.select(editor::control::parameters_tab);
    publish();
    require(!surface.presented()->tooltip && !surface.accept(world_frame),
            "Leaving World settings for an empty Parameters page retained its help.");
    require(!document.dirty() && document.undo_count() == history && document.redo_count() == redo &&
                take(document.presentation().environment()) == lighting && document.environment() == ObjectId{},
            "Reading parameter or World help changed the document or its history.");
    std::puts(
        "Editor property tooltip acceptance: PASS (real Runtime/Session/DSL layout, Transform/type and Light Color metadata, delayed label hover, World metadata, stale-target retirement and history isolation)");
}
