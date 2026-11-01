void verify_pane_handles(Application& app, const std::filesystem::path& output, ControlKey field) {
    using atelier::ui::DockSpace;
    using atelier::ui::DragAxis;
    const auto scene = app.docking().pane(control::scene_tab);
    const auto parameters = app.docking().pane(control::parameters_tab);
    const auto vertical = app.docking().divider(control::scene_tab, DragAxis::horizontal);
    const auto horizontal = app.docking().divider(control::parameters_tab, DragAxis::vertical);
    const auto scene_bounds = app.presented()->find(scene)->bounds;
    const auto parameter_bounds = app.presented()->find(parameters)->bounds;
    // Exercise the expanded hit area beyond the arrow, on both rail orientations.
    const auto click_outer = [&](ControlKey key, bool trailing = false) {
        const auto bounds = app.presented()->find(key)->bounds;
        const auto dpi = app.presented()->drawing->dpi_scale;
        auto at = centre(app, key);
        const auto shift = static_cast<LONG>((trailing ? 14.f : -14.f) * dpi);
        (bounds.height > bounds.width ? at.y : at.x) += shift;
        click_at(app, at);
    };
    click_outer(DockSpace::collapse_first_button(vertical));
    require(!app.presented()->find(control::viewport) && app.presented()->find(field) &&
                app.docking().pane(control::scene_tab) == scene,
            "Collapsing Scene lost its identity or left content visible.");
    require(app.presented()->find(vertical)->bounds.width <= 8.1f * app.presented()->drawing->dpi_scale,
            "Collapsed pane did not leave a narrow restore strip.");
    capture(app, output / "workspace-collapsed.png");
    click_outer(DockSpace::collapse_first_button(vertical));
    require(app.presented()->find(scene)->bounds == scene_bounds, "Expanding Scene lost its previous size.");
    click_outer(DockSpace::collapse_first_button(horizontal));
    require(!app.presented()->find(field) && app.presented()->find(control::viewport),
            "Divider collapse did not hide Parameters.");
    click(app, DockSpace::collapse_first_button(horizontal));
    require(app.presented()->find(parameters)->bounds == parameter_bounds,
            "Nested pane restore lost its previous size.");
    click_outer(DockSpace::collapse_second_button(vertical), true);
    require(!app.presented()->find(field) && app.presented()->find(control::viewport), "Collapse second side failed.");
    // Showing a registered view must reveal all its collapsed ancestors.
    click(app, operator_item(app, "layout.show", {{"tab", "parameters"}}));
    require(app.presented()->find(field) && app.presented()->find(parameters)->bounds == parameter_bounds,
            "Show Parameters did not reveal a collapsed ancestor.");
    click_outer(DockSpace::divider_grip(horizontal), true);
    require(app.presented()->find(parameters)->bounds.y > parameter_bounds.y,
            "Central grip click did not exchange the panes.");
    capture(app, output / "workspace-swapped.png");
    click(app, DockSpace::divider_grip(horizontal));
    require(app.presented()->find(parameters)->bounds == parameter_bounds, "Exchanging twice changed the layout.");
    const auto rotate = [&] {
        require_input_focus(app);
        KeyStateScope modifiers(false, false, true);
        const auto at = centre(app, DockSpace::divider_grip(horizontal));
        SendMessageW(app.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(at.x, at.y));
        SendMessageW(app.window(), WM_LBUTTONUP, 0, MAKELPARAM(at.x, at.y));
        tick(app, 2);
    };
    rotate();
    const auto rotated = app.presented()->find(horizontal)->bounds;
    require(rotated.height > rotated.width, "Alt+grip did not rotate the split.");
    rotate();
    require(app.presented()->find(parameters)->bounds == parameter_bounds, "Rotating twice changed the saved ratio.");
}

void verify_docking(Application& app, const std::filesystem::path& output, atelier::ObjectId entity) {
    using atelier::ui::DockSpace;
    using atelier::ui::DragAxis;
    namespace sample = atelier::editor::workbench_control;
    const auto dpi = app.presented()->drawing->dpi_scale;
    SetWindowPos(app.window(), nullptr, 0, 0, static_cast<int>(1280 * dpi), static_cast<int>(840 * dpi),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    tick(app, 3);
    click(app, operator_item(app, "layout.reset"));
    const auto field = property_control(app, entity, 0);
    const auto value = x(app, entity);
    const auto history = app.document().undo_count();
    const auto parameter_pane = app.docking().pane(control::parameters_tab);
    click(app, control::workbench);
    click(app, sample::position_x);
    type(app, L"2");
    key(app, VK_RETURN);
    const auto sample_history = app.control_workbench().undo_count();
    const auto drag_to = [&](ControlKey source, POINT target) {
        mouse(app, WM_LBUTTONDOWN, centre(app, source));
        mouse(app, WM_MOUSEMOVE, target);
        mouse(app, WM_LBUTTONUP, target);
    };
    const auto scene = app.docking().pane(control::scene_tab);
    const auto* scene_box = app.presented()->find(scene);
    POINT target{static_cast<LONG>(scene_box->bounds.x + scene_box->bounds.width * .5f),
                 static_cast<LONG>(scene_box->bounds.y + scene_box->bounds.height - 8 * dpi)};
    mouse(app, WM_LBUTTONDOWN, centre(app, control::workbench));
    mouse(app, WM_MOUSEMOVE, target);
    require(app.docking().dragging(), "Tab drag did not start through UI capture.");
    capture(app, output / "workspace-dock-preview.png");
    mouse(app, WM_LBUTTONUP, target);
    require(!app.docking().dragging() && app.docking().pane(control::workbench) != parameter_pane &&
                app.presented()->find(control::viewport) && app.presented()->find(sample::position_x),
            "Edge drop did not split the pane.");
    require(property_control(app, entity, 0) == field && x(app, entity) == value &&
                app.document().undo_count() == history && app.control_workbench().values().position[0] == 2,
            "Docking changed document state or field binding.");
    const auto new_pane = app.docking().pane(control::workbench);
    auto divider = centre(app, app.docking().divider(control::workbench, DragAxis::vertical));
    const auto old_height = app.presented()->find(new_pane)->bounds.height;
    {
        require_input_focus(app);
        KeyStateScope modifiers;
        SendMessageW(app.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(divider.x, divider.y));
        divider.y -= 35;
        SendMessageW(app.window(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(divider.x, divider.y));
        SendMessageW(app.window(), WM_LBUTTONUP, 0, MAKELPARAM(divider.x, divider.y));
        tick(app, 2);
    }
    require(app.presented()->find(new_pane)->bounds.height > old_height + 20, "Horizontal pane divider failed.");
    const auto before = app.presented()->find(new_pane)->bounds;
    click(app, DockSpace::maximize_button(new_pane));
    require(!app.presented()->find(control::viewport) && app.presented()->find(sample::position_x),
            "Maximize left another pane visible.");
    capture(app, output / "workspace-maximized.png");
    key(app, 'B', true);
    require(app.presented()->find(control::viewport) && app.presented()->find(new_pane)->bounds == before,
            "Restore lost pane geometry.");
    const auto* scene_tab = app.presented()->find(control::scene_tab);
    target = {static_cast<LONG>(scene_tab->bounds.x + scene_tab->bounds.width * .5f),
              static_cast<LONG>(scene_tab->bounds.y + 8 * dpi)};
    mouse(app, WM_LBUTTONDOWN, centre(app, control::workbench));
    mouse(app, WM_MOUSEMOVE, target);
    key(app, VK_ESCAPE);
    mouse(app, WM_LBUTTONUP, target);
    require(app.docking().pane(control::workbench) == new_pane && !app.docking().dragging() &&
                GetCapture() != app.window(),
            "Escape did not cancel tab docking.");
    drag_to(control::workbench, target);
    require(app.docking().pane(control::workbench) == app.docking().pane(control::scene_tab) &&
                !app.presented()->find(control::viewport),
            "Dropping on tab strip did not merge/select the tab.");
    const auto* first_tab = app.presented()->find(control::scene_tab);
    target = {static_cast<LONG>(first_tab->bounds.x + 2 * dpi), static_cast<LONG>(first_tab->bounds.y + 8 * dpi)};
    drag_to(control::workbench, target);
    require(app.presented()->find(control::workbench)->bounds.x < app.presented()->find(control::scene_tab)->bounds.x,
            "Tab strip drop did not reorder before the target tab.");
    key(app, VK_TAB, true); // Cycle back to Scene, keeping both view instances.
    require(app.presented()->find(control::viewport), "Ctrl+Tab did not cycle pane tabs.");
    click(app, control::workbench);
    key(app, 'W', true);
    require(!app.docking().pane(control::workbench) && app.presented()->find(control::viewport),
            "Close did not normalize the tab group.");
    click(app, DockSpace::add_tab_button(app.docking().pane(control::scene_tab)));
    key(app, VK_HOME);
    key(app, VK_DOWN);
    key(app, VK_DOWN);
    key(app, VK_RETURN);
    require(app.workbench_visible() && app.control_workbench().values().position[0] == 2 &&
                app.control_workbench().undo_count() == sample_history,
            "Reopening lost the view's state or history.");
    capture(app, output / "workspace-tabs.png");
    const auto group = app.docking().pane(control::workbench);
    const auto* add = app.presented()->find(DockSpace::add_tab_button(group));
    const auto* tabs = app.presented()->find(group + 9);
    const auto* first = app.presented()->find(control::scene_tab);
    const auto* last = app.presented()->find(control::workbench);
    require(first->bounds.x < tabs->bounds.x + 3 * dpi && add && add->kind == atelier::ui::ControlKind::combo &&
                add->bounds.x > last->bounds.x + last->bounds.width &&
                add->bounds.x < last->bounds.x + last->bounds.width + 24 * dpi,
            "Tab strip has a leading stow button or its add button is detached from the tabs.");
    require(std::abs(add->bounds.height / dpi - 22) < .1f, "Tab strip height differs from the calibrated chrome.");
    click(app, DockSpace::add_tab_button(group));
    require(app.menu_window() && GetWindow(app.menu_window(), GW_OWNER) == app.window(),
            "Add Tab did not use the native popup host.");
    capture(app, output / "workspace-add-tab.png");
    key(app, VK_HOME);
    key(app, VK_DOWN);
    key(app, VK_RETURN); // Move the existing Parameters view here.
    require(app.docking().pane(control::parameters_tab) == group && app.presented()->find(field) &&
                x(app, entity) == value && app.document().undo_count() == history,
            "Add Tab copied or lost the existing view's document state.");
    click(app, control::workbench);
    require(app.presented()->find(control::workbench)->style->border_width == 0,
            "Focused dock tab unexpectedly draws a focus rectangle.");
    click(app, sample::position_x);
    key(app, VK_ESCAPE);
    key(app, 'Z', true);
    require(app.control_workbench().values().position[0] == 0, "Reopened sample could not undo its edit.");
    click(app, operator_item(app, "layout.reset"));
    require(app.presented()->find(control::viewport) && app.presented()->find(field) && !app.workbench_visible() &&
                app.document().undo_count() == history,
            "Default layout restoration changed document state.");
    capture(app, output / "workspace-default.png");
    verify_pane_handles(app, output, field);
    for (auto view : {control::workbench, control::parameters_tab, control::entities_tab, control::scene_tab}) {
        click(app, view);
        key(app, 'W', true);
    }
    require(!app.presented()->find(control::viewport) && !app.presented()->find(field),
            "Closing the final pane left stale content.");
    click(app, operator_item(app, "layout.reset"));
    require(app.presented()->find(field) && app.presented()->find(control::viewport),
            "Empty workspace could not restore its views.");
}
