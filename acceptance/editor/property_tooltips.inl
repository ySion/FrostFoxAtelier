// Native metadata -> real editor document -> editor.ui.source -> accepted UI
// geometry and delayed help. This path needs no GPU or desktop input injection.
void verify_property_tooltips() {
    editor::Session document;
    take(document.initialize());
    changed(document.create_static_mesh(assets::mesh_assets().front()));
    const auto mesh = document.selection().entity;
    changed(document.create_light(presentation::LightKind::point));
    const auto light = component(document, document.selection().entity, document.presentation_types().light_component);
    take(document.select(mesh));

    editor::Workspace workspace;
    const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface;
    take(surface.initialize(fonts));
    const auto theme = ui::StyleSheet::parse(take(assets::read_file(editor::asset_directory() / "editor.ss")));
    require(theme && surface.set_style_sheet(*theme) && surface.set_surface({{1600, 1000}, 1, 1}),
            "Property tooltip editor surface failed.");
    std::vector<ui::Control> controls;
    const auto publish = [&] {
        controls = take(workspace.describe(document, {1600, 1000}, surface.presented().get()));
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
    const auto position_key = label(L"Position (m)");
    const auto position_frame = hover(position_key);

    take(document.select(light));
    publish();
    require(!surface.presented()->tooltip && !surface.accept(position_frame),
            "Switching inspected objects retained or accepted an old tooltip frame.");
    std::puts("Editor property tooltip acceptance: PASS (inspected target retires its tooltip and accepted frame)");
}
