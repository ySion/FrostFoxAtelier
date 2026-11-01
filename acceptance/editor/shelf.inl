void verify_stow_alignment(Application& app) {
    const auto* menu = app.presented()->find(control::menu_bar_handle);
    const auto* shelf = app.presented()->find(control::shelf_handle);
    require(menu && shelf && menu->bounds.x == shelf->bounds.x,
            "Menubar and shelf stow rails do not share the same leading edge.");
    const auto dpi = app.presented()->drawing->dpi_scale;
    for (const auto* handle : {menu, shelf}) {
        const auto* bar = app.presented()->find(handle->parent);
        require(bar && std::abs(handle->bounds.width - (handle->selected ? bar->bounds.width : 12 * dpi)) < .1f &&
                    (!handle->selected || std::abs(handle->bounds.height - 12 * dpi) < .1f) &&
                    handle->style->icon_align == atelier::ui::TextAlign::left,
                "Stow handle must be a 12 DIP rail or a full-width 12 DIP restore strip with a leading arrow.");
    }
    const auto& a = *menu->style;
    const auto& b = *shelf->style;
    require(a.indicator_size == b.indicator_size && a.border_width == b.border_width && a.border == b.border &&
                a.border_inner == b.border_inner && a.border_style == b.border_style,
            "Stow rails resolved different arrow or border metrics.");
    for (const auto* handle : {menu, shelf})
        if (const auto* box = app.presented()->find(handle == menu ? control::menu_bar_body : control::shelf_contents))
            require(box->bounds.x == handle->bounds.x + handle->bounds.width,
                    "Expanded bar content does not start at the common rail edge.");
}

void verify_tool_shelf(Application& app, const std::filesystem::path& output) {
    verify_stow_alignment(app);
    for (const auto rail : {control::menu_bar_handle, control::shelf_handle}) {
        mouse(app, WM_MOUSEMOVE, centre(app, rail));
        verify_stow_alignment(app);
    }
    const auto dpi = app.presented()->drawing->dpi_scale;
    const auto selected = app.document().selection().entity;
    const auto history = app.document().undo_count();
    const auto members = entities(app).size();
    const auto shelf = app.presented()->find(control::shelf)->bounds;
    const auto viewport = app.presented()->find(control::viewport)->bounds;
    std::vector<std::pair<ControlKey, ControlKey>> tool_keys;
    // Viewport rail and header buttons also target View commands; only the shelf subtree counts here.
    const auto shelf_tool = [&](ControlKey target) -> const atelier::ui::ControlBox* {
        for (const auto& item : app.presented()->controls) {
            if (item.kind != atelier::ui::ControlKind::button || item.action_target != target)
                continue;
            for (auto parent = item.parent; parent;) {
                if (parent == control::shelf_body)
                    return &item;
                const auto* box = app.presented()->find(parent);
                parent = box ? box->parent : ControlKey{};
            }
        }
        return nullptr;
    };
    const auto verify_commands = [&](atelier::editor::MenuGroup group) {
        for (const auto& definition : atelier::editor::menu_bindings) {
            // The shelf lists every Create command and the View commands that declare a shelf title.
            if (definition.group != atelier::editor::MenuGroup::create &&
                (definition.group != atelier::editor::MenuGroup::view || definition.shelf_title.empty()))
                continue;
            const auto target = Access::operator_item(app, definition.call.id, definition.call.args);
            const auto* current = shelf_tool(target);
            if ((current != nullptr) != (definition.group == group))
                throw std::runtime_error("The presented shelf included a hidden category or omitted a tool: " +
                                         atelier::assets::utf8(definition.title));
            if (!current)
                continue;
            require(current->key != target && std::ranges::count(app.presented()->controls, target,
                                                                 &atelier::ui::ControlBox::action_target) == 1,
                    "A shelf command lost its distinct visual identity or appeared more than once.");
            const auto previous = std::ranges::find(tool_keys, target, &std::pair<ControlKey, ControlKey>::first);
            if (previous == tool_keys.end())
                tool_keys.emplace_back(target, current->key);
            else
                require(previous->second == current->key, "Changing shelf categories recreated a command control.");
        }
    };
    verify_commands(atelier::editor::MenuGroup::create);
    const auto add_entity = Access::operator_item(app, "scene.add", {{"kind", "entity"}});
    const auto* tool = command_tool(app, add_entity);
    require(tool && tool->key != add_entity, "DSL shelf command retained its command ID as the visual identity.");
    require(std::abs(shelf.height / dpi - 56) < .1f && std::abs(tool->bounds.width / dpi - 64) < .1f &&
                tool->style->icon_placement == atelier::ui::IconPlacement::above && tool->style->font.size == 10.5f,
            "Tool shelf did not use the compact icon-over-caption layout.");
    // The icon and caption must both activate the same command exactly once.
    for (bool icon : {true, false}) {
        const auto bounds = command_tool(app, add_entity)->bounds;
        click_at(app, {static_cast<LONG>(bounds.x + bounds.width * .5f),
                       static_cast<LONG>(icon ? bounds.y + 10 * dpi : bounds.y + bounds.height - 6 * dpi)});
        require(entities(app).size() == members + 1 && app.document().undo_count() == history + 1,
                "Shelf icon/caption did not activate exactly one document command.");
        require(app.presented()->find(control::parameter_list) && !app.presented()->find(control::position_fold) &&
                    !app.presented()->find(control::add_component),
                "An empty Entity acquired spatial controls or parameter-side component commands.");
        click(app, operator_item(app, "edit.undo"));
        click(app, app.object_control(selected));
        require(app.presented()->find(control::position_fold) &&
                    app.presented()->find(property_control(app, selected, 0)),
                "Returning to the spatial Entity did not restore its actual root properties.");
    }
    click(app, control::shelf_create);
    key(app, VK_TAB);
    key(app, VK_TAB, false, true);
    key(app, VK_END);
    require(app.presented()->find(control::shelf_view)->selected,
            "Shelf Tab/End did not enter and switch the category group.");
    verify_commands(atelier::editor::MenuGroup::view);
    key(app, VK_HOME);
    require(app.presented()->find(control::shelf_create)->selected, "Shelf Home did not select Create.");
    verify_commands(atelier::editor::MenuGroup::create);
    key(app, VK_RIGHT);
    require(app.presented()->find(control::shelf_view)->selected, "Shelf Right did not select View.");
    key(app, VK_LEFT);
    require(app.presented()->find(control::shelf_create)->selected, "Shelf Left did not select Create.");
    key(app, VK_END);
    verify_commands(atelier::editor::MenuGroup::view);
    const auto focus = Access::operator_item(app, "view.frame_selected");
    click(app, control::shelf_handle);
    verify_stow_alignment(app);
    require(std::abs(app.presented()->find(control::shelf)->bounds.height / dpi - 12) < .1f &&
                !app.presented()->find(control::shelf_tabs) && !command_tool(app, focus) &&
                app.presented()->find(control::file_menu) && app.presented()->find(control::shelf_handle),
            "Stowing the shelf hid its restore handle, retained hidden commands, or changed the menubar.");
    require(std::abs(app.presented()->find(control::viewport)->bounds.y - viewport.y + 44 * dpi) < .1f,
            "Stowing the shelf did not return its height to the workspace.");
    capture(app, output / "shelf-collapsed.png");
    key(app, VK_SPACE);
    verify_stow_alignment(app);
    require(app.presented()->find(control::shelf_view)->selected && command_tool(app, focus) &&
                app.presented()->find(control::shelf)->bounds == shelf,
            "Keyboard expansion lost the shelf category or dimensions.");
    verify_commands(atelier::editor::MenuGroup::view);
    click(app, control::shelf_create);
    verify_commands(atelier::editor::MenuGroup::create);
    // Properties share one scroll area. Selection and filtering retain the
    // document field identity without inventing fixed content-page choices.
    const auto field = property_control(app, selected, 0);
    const auto original = x(app, selected);
    const auto filter_incarnation = app.presented()->find(control::parameter_search)->incarnation;
    const auto filter_bounds = app.presented()->find(control::parameter_search)->bounds;
    const auto property_group = [&] {
        return atelier::application::EditorAcceptanceAccess::property_group(
            app, component(app, selected, app.document().presentation_types().static_mesh_component));
    };
    require(app.presented()->find(field) && property_group() && !app.presented()->find(control::add_component),
            "Entity properties were split into fixed pages or retained component commands in Parameters.");
    click(app, field);
    type(app, (std::to_wstring(original + 1) + L"x").c_str());
    key(app, VK_RETURN);
    require(app.document().editing() && x(app, selected) != original && app.document().undo_count() == history,
            "Parameter fixture did not retain an invalid draft after a real field preview.");
    click(app, control::tree_world);
    require(!app.document().editing() && x(app, selected) == original && app.document().undo_count() == history &&
                !app.presented()->find(field) && !app.text_input(),
            "Changing inspected objects failed to cancel the invalid draft and its preview.");
    click(app, app.object_control(selected));
    require(property_control(app, selected, 0) == field && app.presented()->find(field) && x(app, selected) == original,
            "Returning to the Entity lost field identity or restored a cancelled draft.");
    click(app, control::parameter_search);
    type(app, L"staticmesh");
    key(app, VK_RETURN);
    require(property_group() && !app.presented()->find(field),
            "Shared Filter did not match the real component group and exclude unrelated Position.");
    require(app.presented()->find(control::parameter_search)->incarnation == filter_incarnation &&
                app.presented()->find(control::parameter_search)->bounds == filter_bounds,
            "Property filtering moved or recreated the shared Filter.");
    click(app, control::parameter_search);
    type(app, L"no-such-component");
    key(app, VK_RETURN);
    require(!property_group() && app.presented()->find(control::parameter_search),
            "Component filtering retained unmatched rows or hid its own Filter.");
    click(app, control::parameter_search);
    type(app, L"");
    key(app, VK_RETURN);
    require(property_group(), "Clearing the shared Filter did not restore component properties.");
    require(property_control(app, selected, 0) == field && app.presented()->find(field) &&
                app.document().undo_count() == history,
            "Filtering properties replaced the World field key or changed document history.");
    capture(app, output / "shelf-expanded.png");
    click(app, control::menu_bar_handle);
    click(app, control::shelf_handle);
    verify_stow_alignment(app);
    require(app.presented()->find(control::menu_bar_handle)->bounds.height ==
                app.presented()->find(control::shelf_handle)->bounds.height,
            "Collapsed bars retained different restoration strip heights.");
    capture(app, output / "bars-collapsed.png");
    // The accepted button itself owns the complete strip; no auxiliary hit area.
    for (const auto handle : {control::menu_bar_handle, control::shelf_handle}) {
        const auto incarnation = app.presented()->find(handle)->incarnation;
        for (const auto fraction : {.5f, .99f}) {
            const auto bounds = app.presented()->find(handle)->bounds;
            click_at(app, {static_cast<LONG>(bounds.x + bounds.width * fraction),
                           static_cast<LONG>(bounds.y + bounds.height * .5f)});
            require(!app.presented()->find(handle)->selected &&
                        app.presented()->find(handle)->incarnation == incarnation,
                    "The middle or far end of a restore strip failed to expand the same bar control.");
            verify_stow_alignment(app);
            click(app, handle);
            verify_stow_alignment(app);
        }
    }
    // Expand in the opposite order; F10 must still reveal the menu and open File.
    key(app, VK_SPACE);
    verify_stow_alignment(app);
    key(app, VK_F10);
    require(app.menu_frame() && app.presented()->find(control::file_menu),
            "F10 failed to restore the menubar after independently stowing both bars.");
    key(app, VK_ESCAPE);
    verify_stow_alignment(app);
    require(app.presented()->find(control::shelf)->bounds == shelf &&
                app.presented()->find(control::viewport)->bounds == viewport,
            "Restoring both bars changed the workspace geometry.");
    require(app.document().selection().entity == selected && app.document().undo_count() == history,
            "Shelf layout operations changed document selection or history.");
}
