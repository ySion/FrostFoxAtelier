void verify_parameter_search(Application& app, const std::filesystem::path& output) {
    const auto entity = app.document().selection().entity;
    const auto field = property_control(app, entity, 0);
    const auto incarnation = app.presented()->find(field)->incarnation;
    const auto history = app.document().undo_count();
    const auto closed_list = app.presented()->find(control::parameter_list)->bounds;
    const auto name = atelier::application::EditorAcceptanceAccess::name_control(app, entity);
    const auto name_bounds = app.presented()->find(name)->bounds;
    const auto button = app.presented()->find(control::parameter_search_toggle)->bounds;
    require(!app.presented()->find(control::parameter_search) &&
                !app.presented()->find(control::parameter_search_toggle)->selected &&
                std::abs(name_bounds.y + name_bounds.height * .5f - button.y - button.height * .5f) < .1f &&
                name_bounds.height == app.presented()->find(field)->bounds.height,
            "Parameter header did not align the shared single-line editor and collapsed search button.");
    capture(app, output / "parameters-search-closed.png");
    click(app, control::parameter_search_toggle);
    require(app.text_input() && app.text_input()->token.key == control::parameter_search &&
                app.presented()->find(control::parameter_search_toggle)->selected &&
                app.presented()->find(field)->incarnation == incarnation,
            "Opening parameter search did not focus the presented field or select its magnifier.");
    const auto search = app.presented()->find(control::parameter_search)->bounds;
    const auto header = app.presented()->find(51)->bounds;
    const auto list = app.presented()->find(control::parameter_list)->bounds;
    require(search.y >= header.y + header.height - .1f && search.y + search.height <= list.y + .1f,
            "Search did not open between the object header and flat property list.");
    type(app, L"no-such-parameter");
    require(!app.presented()->find(field), "Expanded search did not filter the real property.");
    capture(app, output / "parameters-search-open.png");
    click(app, control::parameter_search_toggle);
    require(!app.presented()->find(control::parameter_search) && app.presented()->find(field) &&
                property_control(app, entity, 0) == field && app.query() == L"no-such-parameter" &&
                app.presented()->find(control::parameter_list)->bounds == closed_list && !app.text_input(),
            "Closing search lost text or field identity, kept hidden filtering/focus, or retained row space.");
    key(app, VK_SPACE);
    require(app.text_input() && app.text_input()->token.key == control::parameter_search &&
                !app.presented()->find(field),
            "Keyboard reopening did not restore and focus the retained query.");
    type(app, L"");
    key(app, VK_RETURN);
    require(app.presented()->find(field) && app.document().undo_count() == history,
            "Search visibility or filtering changed scene history.");
}

void parameter_menu(Application& app, ControlKey source) {
    require_input_focus(app);
    KeyStateScope modifiers;
    const auto at = centre(app, source);
    SendMessageW(app.window(), WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(at.x, at.y));
    SendMessageW(app.window(), WM_RBUTTONUP, 0, MAKELPARAM(at.x, at.y));
    tick(app, 2);
    if (!app.menu_frame() || app.menu_frame()->popup != control::parameter_menu)
        throw std::runtime_error("Parameter menu did not enter the accepted frame (source " + std::to_string(source) +
                                 ", status: " + atelier::assets::utf8(app.status_text()) + ").");
}

bool tuple_reset_available(Application& app, ControlKey group) {
    parameter_menu(app, group);
    const bool enabled = app.menu_frame()->find(control::menu_group_reset)->enabled;
    key(app, VK_ESCAPE);
    return enabled;
}

void reset_tuple(Application& app, ControlKey group) {
    parameter_menu(app, group);
    click(app, control::menu_group_reset);
    require(!app.menu_window(), "Tuple reset left its popup open.");
}

void verify_parameter_slider(Application& app, const std::filesystem::path& output) {
    namespace sample = atelier::editor::workbench_control;
    click(app, sample::scalar);
    type(app, L"24");
    key(app, VK_RETURN);
    require(app.control_workbench().values().exposure == 24,
            "Number input was incorrectly limited to the slider's navigation range.");
    const auto history = app.control_workbench().undo_count();
    auto at = centre(app, sample::scalar_slider);
    mouse(app, WM_LBUTTONDOWN, at);
    const auto preview = app.control_workbench().values().exposure;
    require(preview > 6 && preview < 10, "Slider track click did not preview the value.");
    key(app, VK_ESCAPE);
    mouse(app, WM_LBUTTONUP, at);
    require(app.control_workbench().values().exposure == 24 && app.control_workbench().undo_count() == history,
            "Slider cancellation lost an original value outside the navigation range.");
    mouse(app, WM_LBUTTONDOWN, at);
    at.x += 20;
    mouse(app, WM_MOUSEMOVE, at);
    mouse(app, WM_LBUTTONUP, at);
    require(app.control_workbench().values().exposure > preview && app.control_workbench().undo_count() == history + 1,
            "Slider drag did not commit exactly one history entry.");
    key(app, 'Z', true);
    require(app.control_workbench().values().exposure == 24, "Slider undo did not restore the number.");
    key(app, VK_END);
    require(app.control_workbench().values().exposure == 16, "Slider End failed.");
    at = centre(app, sample::scalar_slider);
    mouse(app, WM_LBUTTONDOWN, at);
    SendMessageW(app.window(), WM_CANCELMODE, 0, 0);
    tick(app, 2);
    require(app.control_workbench().values().exposure == 16 && GetCapture() != app.window(),
            "Capture loss did not roll back the slider.");
    click(app, sample::disable);
    click(app, sample::scalar_slider);
    require(app.control_workbench().values().exposure == 16, "Disabled slider accepted input.");
    click(app, sample::disable);
    click(app, sample::scalar);
    type(app, L"1");
    key(app, VK_RETURN);
    capture(app, output / "controls-axis-slider.png");
}

void verify_basic_controls(Application& app, const std::filesystem::path& output, atelier::ObjectId entity) {
    const auto resize_panel = [&](float width) {
        require_input_focus(app);
        KeyStateScope modifiers;
        const auto from = centre(app, app.parameter_splitter());
        auto to = from;
        to.x += static_cast<LONG>(std::lround((app.parameter_width() - width) * app.presented()->drawing->dpi_scale));
        // This check observes the completed resize. Send the whole gesture
        // before polling unrelated native motion, just as click_at does.
        SendMessageW(app.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
        SendMessageW(app.window(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
        SendMessageW(app.window(), WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
        tick(app, 2);
    };
    click(app, control::parameter_search);
    type(app, L"");
    key(app, VK_RETURN);
    key(app, VK_ESCAPE);
    const auto target = entity;
    require(model(atelier::world::has_transform(app.document().world().runtime(), entity)),
            "Parameter-row acceptance needs Entity space.");
    const auto position = [&](std::size_t axis) {
        return std::get<float>(model(app.document().world().read(target, app.document().parameters()[axis].path)));
    };
    for (std::size_t axis = 0; axis < 3; ++axis) {
        click(app, property_control(app, entity, axis));
        type(app, std::to_wstring(axis + 1).c_str());
        key(app, VK_RETURN);
    }
    const auto count = app.document().undo_count();
    const auto reset = app.reset_control(target);
    require(reset && tuple_reset_available(app, reset), "Modified vector has no reset action.");
    capture(app, output / "controls-vector-modified.png");
    reset_tuple(app, reset);
    require(position(0) == 0 && position(1) == 0 && position(2) == 0 && app.document().undo_count() == count + 1,
            "Vector reset did not apply as one history entry.");
    click(app, operator_item(app, "edit.undo"));
    require(position(0) == 1 && position(1) == 2 && position(2) == 3, "Undo failed to restore the whole vector.");
    click(app, operator_item(app, "edit.redo"));
    require(position(0) == 0 && position(1) == 0 && position(2) == 0, "Redo failed to reset the whole vector.");

    click(app, property_control(app, entity, 1));
    type(app, L"5");
    key(app, VK_RETURN);
    click(app, control::parameter_search);
    type(app, L"x");
    key(app, VK_RETURN);
    require(!app.presented()->find(property_control(app, entity, 1)) && tuple_reset_available(app, reset),
            "Filtering out the only modified component disabled the whole-vector reset.");
    const auto filtered_history = app.document().undo_count();
    reset_tuple(app, reset);
    require(position(0) == 0 && position(1) == 0 && position(2) == 0 &&
                app.document().undo_count() == filtered_history + 1,
            "Filtered reset did not restore the hidden component in one transaction.");
    click(app, operator_item(app, "edit.undo"));
    require(position(1) == 5 && app.query() == L"x" && tuple_reset_available(app, reset),
            "Undo did not restore the hidden component and whole-group modified state.");
    capture(app, output / "controls-filtered-reset.png");
    click(app, operator_item(app, "edit.redo"));
    click(app, control::parameter_search);
    type(app, L"");
    key(app, VK_RETURN);
    key(app, VK_ESCAPE);

    const auto stable = property_control(app, entity, 0);
    click(app, control::position_fold);
    require(!app.presented()->find(stable) && !app.text_input(), "Collapsed group retained an editable target.");
    key(app, VK_SPACE);
    require(app.presented()->find(stable) && property_control(app, entity, 0) == stable,
            "Keyboard expansion changed field identity or failed to restore the group.");

    namespace sample = atelier::editor::workbench_control;
    const auto scene_history = app.document().undo_count();
    click(app, control::workbench);
    require(app.workbench_visible() && app.presented()->find(sample::position_x),
            "Control workbench is not reachable from the editor.");
    capture(app, output / "controls-workbench-initial.png");
    click(app, sample::position_x);
    type(app, L"12.5");
    key(app, VK_RETURN);
    require(app.control_workbench().values().position[0] == 12.5 && app.control_workbench().undo_count() == 1,
            "Workbench numeric field did not use the edit transaction.");
    key(app, 'Z', true);
    require(app.control_workbench().values().position[0] == 0,
            "Passive sample field did not route undo to sample history.");
    key(app, 'Y', true);
    require(app.control_workbench().values().position[0] == 12.5, "Sample redo did not restore the value.");
    click(app, sample::name);
    type(app, L"控件样本 · Unicode");
    key(app, VK_RETURN);
    require(app.control_workbench().values().name == L"控件样本 · Unicode",
            "Workbench text did not use the shared Unicode editor.");
    click(app, sample::enabled);
    require(!app.control_workbench().values().enabled, "Checkbox pointer activation did not publish a bool.");
    key(app, VK_SPACE);
    require(app.control_workbench().values().enabled, "Checkbox keyboard activation did not toggle once.");
    key(app, VK_DELETE);
    require(contains(app, entity), "Workbench keyboard command escaped into scene deletion.");
    click(app, sample::disable);
    require(!app.presented()->find(sample::position_x)->enabled, "Disabled group did not propagate to numeric fields.");
    click(app, sample::position_x);
    type(app, L"999");
    require(app.control_workbench().values().position[0] == 12.5, "Disabled sample accepted input.");
    click(app, sample::disable);
    click(app, sample::long_labels);
    const auto width = app.parameter_width();
    resize_panel(180); // Cross the lower bound to verify its clamp.
    // A pane's floor is its widest tab's declared minimum: World (280) shares this pane.
    if (std::abs(app.parameter_width() - 280) >= 1)
        throw std::runtime_error("Narrow workbench did not reach the declared minimum: width " +
                                 std::to_string(app.parameter_width()) + " from " + std::to_string(width) + ".");
    const auto* first = app.presented()->find(sample::position_x);
    for (const auto field : {sample::position_x, sample::position_y, sample::position_z}) {
        const auto* box = app.presented()->find(field);
        require(box && box->bounds.y == first->bounds.y && box->bounds.width >= 24 && box->bounds.x >= box->clip.left &&
                    box->bounds.x + box->bounds.width <= box->clip.right,
                "Wrapped vector lost a component or overflowed the narrow panel.");
    }
    capture(app, output / "controls-workbench-narrow.png");
    wheel(app, sample::list, -12);
    const auto* four = app.presented()->find(sample::vector4_x);
    for (const auto field : {sample::vector4_x, sample::vector4_y, sample::vector4_z, sample::vector4_w}) {
        const auto* box = app.presented()->find(field);
        require(box && box->bounds.y == four->bounds.y && box->bounds.width >= 24 && box->bounds.x >= box->clip.left &&
                    box->bounds.x + box->bounds.width <= box->clip.right && box->bounds.y >= box->clip.top &&
                    box->bounds.y + box->bounds.height <= box->clip.bottom,
                "Four-component row was clipped in the minimum-width panel.");
    }
    click(app, sample::vector4_w);
    type(app, L"2");
    key(app, VK_RETURN);
    require(app.control_workbench().values().vector4[3] == 2, "Narrow Vector4 last field was not editable.");
    capture(app, output / "controls-workbench-vector4.png");
    click(app, operator_item(app, "edit.undo"));
    require(app.control_workbench().values().vector4[3] == 1, "Vector4 editing did not share sample history.");
    wheel(app, sample::list, 12);
    resize_panel(width);
    click(app, sample::long_labels);
    // Real native messages, clipboard, accepted frames, history and renderer.
    parameter_menu(app, sample::position_x);
    capture(app, output / "controls-parameter-menu.png");
    click(app, control::menu_copy);
    require(!app.menu_window(), "Copy left its popup open.");
    parameter_menu(app, sample::position_y);
    click(app, control::menu_paste);
    require(app.control_workbench().values().position[1] == 12.5, "Paste Value did not commit the clipboard value.");
    click(app, operator_item(app, "edit.undo"));
    require(app.control_workbench().values().position[1] == 0, "Menu paste did not produce one undoable edit.");
    parameter_menu(app, sample::position_x);
    click(app, control::menu_reset);
    require(app.control_workbench().values().position[0] == 0, "Scalar default command did not restore its value.");
    click(app, operator_item(app, "edit.undo"));
    parameter_menu(app, sample::name);
    click(app, control::menu_copy);
    parameter_menu(app, sample::position_x);
    click(app, control::menu_paste);
    require(app.control_workbench().values().position[0] == 12.5 && app.text_input(),
            "Invalid numeric paste committed or discarded the editable draft.");
    key(app, VK_ESCAPE);
    parameter_menu(app, sample::position_x);
    key(app, VK_DELETE);
    require(app.menu_window() && contains(app, entity), "Popup leaked Delete to the scene.");
    const auto view = atelier::application::EditorAcceptanceAccess::camera_view(app);
    wheel(app, control::viewport, 3);
    require(atelier::application::EditorAcceptanceAccess::camera_view(app) == view,
            "Popup wheel escaped to the camera.");
    click(app, control::workbench);
    require(!app.menu_window() && app.workbench_visible(),
            "Outside click did not dismiss without activating the toolbar.");
    click(app, sample::position_x);
    key(app, VK_APPS);
    require(app.menu_window(), "Keyboard context menu did not open.");
    key(app, VK_ESCAPE);
    require(!app.menu_window(), "Escape failed to dismiss the menu.");
    key(app, VK_APPS);
    key(app, VK_ESCAPE); // Owner focus survived dismissal.
    {
        KeyStateScope modifiers(false, true);
        SendMessageW(app.window(), WM_SYSKEYDOWN, VK_F10, 0);
        SendMessageW(app.window(), WM_SYSKEYUP, VK_F10, 0);
        tick(app, 2);
    }
    require(app.menu_window(), "Native Shift+F10 did not open the parameter menu.");
    key(app, VK_ESCAPE);
    // A cancelled or completed ladder gesture owns one transaction.
    const auto drag = [&](bool cancel) {
        require_input_focus(app);
        KeyStateScope modifiers;
        const auto from = centre(app, sample::position_x);
        const LONG dx = static_cast<LONG>(std::lround(10 * app.presented()->drawing->dpi_scale));
        SendMessageW(app.window(), WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(from.x, from.y));
        SendMessageW(app.window(), WM_MOUSEMOVE, MK_MBUTTON, MAKELPARAM(from.x + dx, from.y));
        if (cancel)
            SendMessageW(app.window(), WM_KEYDOWN, VK_ESCAPE, 0);
        SendMessageW(app.window(), WM_MBUTTONUP, 0, MAKELPARAM(from.x + dx, from.y));
        if (cancel)
            SendMessageW(app.window(), WM_KEYUP, VK_ESCAPE, 0);
        tick(app, 2);
    };
    const auto history_before = app.control_workbench().undo_count();
    drag(true);
    require(app.control_workbench().values().position[0] == 12.5 &&
                app.control_workbench().undo_count() == history_before,
            "Esc failed to roll back the ladder gesture.");
    drag(false);
    require(std::abs(app.control_workbench().values().position[0] - 12.7) < 1e-6 &&
                app.control_workbench().undo_count() == history_before + 1,
            "Middle drag did not commit one stepped edit.");
    click(app, operator_item(app, "edit.undo"));
    {
        require_input_focus(app);
        KeyStateScope modifiers;
        const auto from = centre(app, sample::position_x);
        // This gesture stays captured across frames. Keep physical and synthetic
        // positions aligned so capture-generated WM_MOUSEMOVE is not a drag to the title bar.
        auto native_point = from;
        ClientToScreen(app.window(), &native_point);
        require(SetCursorPos(native_point.x, native_point.y) != FALSE, "Cannot align the held ladder pointer.");
        tick(app, 2);
        SendMessageW(app.window(), WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(from.x, from.y));
        tick(app, 2);
        capture(app, output / "controls-value-ladder.png");
        require(app.presented()->value_ladder.has_value(), "Value ladder did not publish its painted geometry.");
        const auto ladder = *app.presented()->value_ladder;
        require(ladder.selected_row == 3 && ladder.increment == .1 && ladder.bounds.y >= 0 &&
                    ladder.bounds.y + ladder.bounds.height <= app.presented()->drawing->extent.height,
                "Value ladder default increment or surface containment is incorrect.");
        const LONG dx = static_cast<LONG>(std::lround(10 * app.presented()->drawing->dpi_scale));
        const LONG row_y = static_cast<LONG>(std::lround(ladder.bounds.y + ladder.row_height * 2.5f));
        const auto select_history = app.control_workbench().undo_count();
        auto row_point = POINT{from.x, row_y};
        ClientToScreen(app.window(), &row_point);
        require(SetCursorPos(row_point.x, row_point.y) != FALSE, "Cannot align the ladder row pointer.");
        SendMessageW(app.window(), WM_MOUSEMOVE, MK_MBUTTON, MAKELPARAM(from.x, row_y));
        tick(app, 2);
        require(app.presented()->value_ladder->selected_row == 2 && app.presented()->value_ladder->increment == 1 &&
                    app.control_workbench().values().position[0] == 12.5 &&
                    app.control_workbench().undo_count() == select_history,
                "Selecting a visible ladder rung changed the value or used a different hit region.");
        SendMessageW(app.window(), WM_MOUSEMOVE, MK_MBUTTON, MAKELPARAM(from.x + dx, row_y));
        SendMessageW(app.window(), WM_MBUTTONUP, 0, MAKELPARAM(from.x + dx, row_y));
        tick(app, 2);
        require(std::abs(app.control_workbench().values().position[0] - 14.5) < 1e-6 && !app.presented()->value_ladder,
                "Coarse ladder edit or release cleanup failed.");
        RECT host{};
        GetWindowRect(app.window(), &host);
        require(SetCursorPos(host.left + 32, host.top + 12) != FALSE,
                "Cannot park the acceptance pointer after the ladder.");
        tick(app, 2);
    }
    click(app, operator_item(app, "edit.undo"));
    wheel(app, sample::list, -12);
    click(app, sample::mode);
    require(app.menu_window(), "Combo did not open its menu.");
    capture(app, output / "controls-combo.png");
    key(app, VK_DOWN);
    key(app, VK_RETURN);
    require(!app.menu_window() && app.control_workbench().values().mode == 1, "Combo keyboard selection failed.");
    key(app, VK_DOWN);
    key(app, VK_ESCAPE);
    require(app.control_workbench().values().mode == 1, "Combo cancellation changed the selection.");
    click(app, operator_item(app, "edit.undo"));
    require(app.control_workbench().values().mode == 0, "Combo selection bypassed sample history.");
    wheel(app, sample::list, 12);
    click(app, sample::fold);
    require(!app.presented()->find(sample::position_x), "Workbench foldout did not collapse its subtree.");
    key(app, VK_RETURN);
    require(app.presented()->find(sample::position_x), "Workbench foldout did not reopen from keyboard.");
    capture(app, output / "controls-workbench.png");
    click(app, sample::reset);
    require(app.control_workbench().values() == atelier::editor::ControlWorkbench::Values{},
            "Workbench reset did not restore sample defaults.");
    click(app, operator_item(app, "edit.undo"));
    require(app.control_workbench().values().position[0] == 12.5,
            "Sample reset did not have one reversible history entry.");
    verify_parameter_slider(app, output);
    click(app, sample::appearance_page);
    require(!app.presented()->find(sample::position_x) && app.presented()->find(sample::red),
            "Parameter page did not switch its content.");
    const auto color_before = app.control_workbench().values().color;
    click(app, sample::red);
    type(app, L"0.75");
    key(app, VK_RETURN);
    require(app.control_workbench().values().color[0] == .75 &&
                app.presented()->find(sample::swatch)->style->background->r == .75f,
            "Color tuple and its swatch diverged.");
    click(app, sample::rgb_page);
    key(app, VK_RIGHT);
    require(app.presented()->find(sample::channels_page)->selected && app.presented()->find(480),
            "Nested parameter tabs did not support arrow navigation.");
    const auto outer = app.presented()->find(sample::list)->bounds;
    const auto nested = app.presented()->find(sample::color_pages)->bounds;
    const auto nested_tab = app.presented()->find(sample::rgb_page)->bounds;
    const auto nested_fold = app.presented()->find(sample::color_fold)->bounds;
    // Section bars start flush with their list on every parameter page; only rows under a section are inset.
    require(std::abs(nested.x - outer.x) < .1f && std::abs(nested_tab.x - nested.x) < .1f &&
                std::abs(nested_fold.x - nested.x) < .1f && std::abs(nested_fold.width - nested.width) < .1f,
            "Nested page insets accumulated in tabs or its full-width fold header.");
    capture(app, output / "controls-color-channels.png");
    click(app, operator_item(app, "edit.undo"));
    require(app.control_workbench().values().color == color_before, "Color editing did not share sample history.");
    click(app, sample::color_fold);
    require(!app.presented()->find(sample::red), "Nested fold did not collapse the color controls.");
    capture(app, output / "controls-nested-collapsed.png");
    key(app, VK_RETURN);
    click(app, sample::rgb_page);
    capture(app, output / "controls-color-tuple.png");
    click(app, sample::general_page);
    click(app, sample::appearance_page);
    require(app.presented()->find(sample::rgb_page)->selected, "Independent tab groups lost their selected pages.");
    click(app, sample::general_page);
    require(app.document().undo_count() == scene_history && position(0) == 0,
            "Workbench values or history modified the actual scene.");
    click(app, control::parameters_tab);
    require(!app.workbench_visible() && app.presented()->find(property_control(app, entity, 0)),
            "Returning from the workbench did not restore scene controls.");
}
