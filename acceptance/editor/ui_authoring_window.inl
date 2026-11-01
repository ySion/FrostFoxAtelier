void verify_asset_ui_authoring_window(Application& app, const std::filesystem::path& output,
                                      const std::filesystem::path& path) {
    using Access = atelier::application::EditorAcceptanceAccess;
    namespace browser = atelier::editor::browser_control;
    click(app, browser::engine);
    click(app, browser::clear);
    click(app, browser::details);
    const auto sphere = Access::asset_key(app, "builtin/sphere");
    click(app, sphere);
    key(app, VK_RIGHT);
    click_at(app, table_header_centre(app, browser::results, browser::name_column));
    const auto* initial = app.presented()->find_table(browser::results);
    require(initial && initial->selection == atelier::ui::TableSelection{sphere, browser::type_column},
            "Asset authoring fixture lost its real selected resource cell.");
    const auto column =
        std::ranges::find(initial->columns, browser::name_column, &atelier::ui::TableColumnGeometry::column);
    require(column != initial->columns.end(), "Asset authoring fixture has no accepted Name header.");
    const auto grip = column->resize;
    const POINT start{static_cast<LONG>(grip.x + grip.width * .5f), static_cast<LONG>(grip.y + grip.height * .5f)};
    require(start.x >= column->clip.left && start.x < column->clip.right && start.y >= column->clip.top &&
                start.y < column->clip.bottom,
            "Asset authoring fixture cannot reach the real column resize grip.");
    const auto width_before = initial->data->columns.front().width;
    const auto dpi = app.presented()->drawing->dpi_scale;
    const POINT end{start.x + static_cast<LONG>(std::round(16 * dpi)), start.y};
    require_input_focus(app);
    {
        KeyStateScope modifiers;
        // One complete native drag sequence precedes queued physical movement.
        SendMessageW(app.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start.x, start.y));
        SendMessageW(app.window(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(end.x, end.y));
        SendMessageW(app.window(), WM_LBUTTONUP, 0, MAKELPARAM(end.x, end.y));
        tick(app, 2);
    }
    require(GetCapture() != app.window() &&
                std::abs(app.presented()->find_table(browser::results)->data->columns.front().width - width_before -
                         static_cast<float>(end.x - start.x) / dpi) < .1f,
            "Asset authoring fixture did not commit a real native column resize.");
    click(app, browser::search);
    type(app, L"Sphere");
    tick(app, 2);
    require(app.text_input() && app.text_input()->token.key == browser::search,
            "Asset authoring fixture has no native query draft.");
    auto token = app.text_input()->token;
    const auto* table = app.presented()->find_table(browser::results);
    require(table && table->selection == atelier::ui::TableSelection{sphere, browser::type_column},
            "Matching asset query lost the current cell.");
    auto snapshot = table->data;
    const auto selection = table->selection;
    const auto table_box = *app.presented()->find(browser::results);
    const auto body = table->body;
    const auto header = table->header;
    const auto status = std::ranges::find_if(app.presented()->controls, [](const auto& item) {
        return item.parent == control::assets_content && item.kind == atelier::ui::ControlKind::label;
    });
    require(status != app.presented()->controls.end() &&
                std::ranges::count_if(app.presented()->controls,
                                      [](const auto& item) {
                                          return item.parent == control::assets_content &&
                                                 item.kind == atelier::ui::ControlKind::label;
                                      }) == 1,
            "Asset shell has no unique accepted status row.");
    const auto status_key = status->key;
    const auto status_incarnation = status->incarnation;
    const auto status_height = status->bounds.height / dpi;
    std::vector<std::pair<ControlKey, std::uint64_t>> identities;
    for (const auto key : {browser::path, browser::search, browser::directories, browser::content, browser::results}) {
        const auto* box = app.presented()->find(key);
        require(box, "Declarative asset shell omitted a stable host alias.");
        identities.emplace_back(key, box->incarnation);
    }
    require(table_box.parent == browser::content,
            "Details was not mounted in the declarative Asset Browser content slot.");
    const auto replace_component = [](std::string& text, std::string_view component, std::string_view before,
                                      std::string_view after) {
        const auto start = text.find(std::string("ui ") + std::string(component) + "(");
        const auto end = start == text.npos ? text.npos : text.find("\nui ", start + 3);
        const auto offset = start == text.npos ? text.npos : text.find(before, start);
        require(offset != text.npos && (end == text.npos || offset < end),
                "Asset authoring fixture could not find its real Source property.");
        text.replace(offset, before.size(), after);
    };
    auto changed = model(atelier::assets::read_file(path));
    replace_component(changed, "AssetResults", "padding_left: 0;", "padding_left: 4;");
    replace_component(changed, "AssetBrowserShell", "height: 20;", "height: 22;");
    const auto verify_retained = [&] {
        const auto* current = app.presented()->find_table(browser::results);
        const auto* box = app.presented()->find(browser::results);
        const auto* footer = app.presented()->find(status_key);
        require(current && box && current->data == snapshot && current->selection == selection &&
                    current->data->sort == snapshot->sort && current->data->columns == snapshot->columns &&
                    box->key == table_box.key && box->incarnation == table_box.incarnation &&
                    box->parent == browser::content,
                "Source replacement changed the Table identity, model snapshot, current cell, sort or column widths.");
        require(app.text_input() && app.text_input()->token == token,
                "Asset shell hot reload replaced or committed the existing query edit token.");
        for (const auto [key, incarnation] : identities)
            require(app.presented()->find(key) && app.presented()->find(key)->incarnation == incarnation,
                    "Asset shell hot reload recreated a stable host control.");
        require(footer && footer->incarnation == status_incarnation &&
                    std::abs(footer->bounds.height / dpi - status_height - 2) < .1f &&
                    std::abs((current->body.x - body.x) / dpi - 4) < .1f &&
                    std::abs((current->header.x - header.x) / dpi - 4) < .1f &&
                    std::abs((body.width - current->body.width) / dpi - 4) < .1f,
                "Accepted shell/Table geometry did not follow the saved Source layout properties.");
    };
    std::ofstream timings(output / "asset-ui-authoring.csv");
    timings << "save,save_to_accepted_ms,revision\n";
    const auto save = [&](const char* label, const std::string& text, bool valid) {
        require_input_focus(app);
        require(valid || Access::ui_error(app).empty(),
                "Asset authoring rejection check started from an unrecovered failed candidate.");
        const auto revision = Access::ui_revision(app);
        const auto start = GetTickCount64();
        model(atelier::editor::write_file(path, text));
        while (GetTickCount64() - start < 10000) {
            tick(app, 1);
            if (valid ? Access::ui_revision(app) > revision : !Access::ui_error(app).empty())
                break;
        }
        require(valid ? (Access::ui_revision(app) == revision + 1 && Access::ui_error(app).empty())
                      : (Access::ui_revision(app) == revision && !Access::ui_error(app).empty()),
                "Asset Source save did not accept or reject the complete real UI version.");
        verify_retained();
        timings << label << ',' << GetTickCount64() - start << ',' << Access::ui_revision(app) << '\n';
    };
    save("layout", changed, true);
    capture(app, output / "asset-ui-live-updated.png");
    save("syntax_error", changed + "\n?", false);
    capture(app, output / "asset-ui-live-error.png");
    save("syntax_recovered", changed + "\n// asset syntax recovered\n", true);
    auto invalid_binding = changed;
    replace_component(invalid_binding, "AssetResults", "in TableBinding model, in ActionBinding open",
                      "in ActionBinding open, in TableBinding model");
    save("binding_error", invalid_binding, false);
    save("binding_recovered", changed + "\n// asset binding recovered\n", true);
    key(app, VK_RETURN);
    require(app.presented()->find_table(browser::results)->data == snapshot &&
                app.presented()->find_table(browser::results)->selection == selection,
            "Committing the retained query changed the resource model.");
    capture(app, output / "asset-ui-live-recovered.png");
    for (const auto view : {browser::list, browser::tiles}) {
        click(app, view);
        const auto* current = app.presented()->find_table(browser::results);
        require(current && current->data == snapshot && current->selection == selection && current->columns.empty() &&
                    app.presented()->find(browser::results)->incarnation == table_box.incarnation &&
                    !app.presented()->find(sphere),
                "Native view switching lost shared row storage/selection or retained a synthetic asset button.");
        capture(app, output / (view == browser::list ? "asset-list-dsl.png" : "asset-tiles-dsl.png"));
    }
    click(app, browser::search);
    type(app, L"Sphere");
    require(static_cast<bool>(app.text_input()), "Tile authoring fixture lost its query editor.");
    token = app.text_input()->token;
    snapshot = app.presented()->find_table(browser::results)->data;
    auto tiles = changed;
    replace_component(tiles, "AssetResults", "tile_width: 90;", "tile_width: 76;");
    replace_component(tiles, "AssetResults", "tile_height: 66;", "tile_height: 54;");
    save("tiles_layout", tiles, true);
    const auto tile_size = [&] {
        const auto* current = app.presented()->find_table(browser::results);
        require(current && current->cells.size() == 1 &&
                    std::abs(current->cells.front().bounds.width / dpi - 76) < .1f &&
                    std::abs(current->cells.front().bounds.height / dpi - 54) < .1f,
                "Saved tile dimensions did not reach accepted native item geometry.");
    };
    tile_size();
    capture(app, output / "asset-tiles-live-updated.png");
    auto invalid_tiles = tiles;
    replace_component(invalid_tiles, "AssetResults", "tile_width: 76;", "tile_width: 0;");
    save("tiles_invalid", invalid_tiles, false);
    tile_size();
    save("tiles_recovered", tiles + "\n// tile layout recovered\n", true);
    tile_size();
    key(app, VK_RETURN);
    require(timings.good(), "Cannot write asset UI authoring evidence.");
}

void verify_ui_authoring_window(Application& app, const std::filesystem::path& output,
                                const std::filesystem::path& path) {
    using Access = atelier::application::EditorAcceptanceAccess;
    model(Access::invoke(app, "scene.add", {{"kind", "mesh"}}));
    tick(app, 3);
    if (!app.presented()->find(control::parameter_search))
        click(app, control::parameter_search_toggle);
    const auto entity = app.document().selection().entity;
    const auto field = property_control(app, entity, 0);
    const auto field_incarnation = app.presented()->find(field)->incarnation;
    const auto path_bar = app.presented()->find(control::parameter_path)->parent;
    const auto path_height = app.presented()->find(path_bar)->bounds.height / app.presented()->drawing->dpi_scale;
    const auto filter_incarnation = app.presented()->find(control::parameter_search)->incarnation;
    const auto verify_hierarchy = [&] {
        const auto* filter = app.presented()->find(control::parameter_search);
        const auto* header = app.presented()->find(51);
        const auto* list = app.presented()->find(control::parameter_list);
        require(filter && header && list && filter->incarnation == filter_incarnation &&
                    filter->bounds.y >= header->bounds.y + header->bounds.height - .1f &&
                    filter->bounds.y + filter->bounds.height <= list->bounds.y + .1f,
                "Native source update recreated Filter or moved it into the property list.");
    };
    verify_hierarchy();
    click(app, control::position_fold);
    require(!app.presented()->find(field), "Authoring fixture did not collapse the real Position component.");
    click(app, control::parameter_search);
    type(app, L"position");
    require(app.text_input().has_value(), "Declarative query did not open a native text session.");
    const auto token = app.text_input()->token;
    const auto source = model(atelier::assets::read_file(path));
    auto changed = source;
    const auto section = changed.find("Foldout section {");
    const auto offset = section == changed.npos ? changed.npos : changed.find("text: \"", section);
    const auto end = offset == changed.npos ? changed.npos : changed.find('"', offset + 7);
    require(end != changed.npos, "Authoring fixture could not locate the real foldout text.");
    changed.replace(offset, end - offset + 1, "text: \"Live scene root\"");
    const auto* initial_tool = command_tool(app, Access::operator_item(app, "scene.add", {{"kind", "mesh"}}));
    require(initial_tool, "Authoring fixture has no declarative command tool.");
    const auto tool_key = initial_tool->key;
    const auto tool_incarnation = initial_tool->incarnation;
    const auto shelf = changed.find("ui ShelfTool(");
    const auto width = shelf == changed.npos ? changed.npos : changed.find("width: 64;", shelf);
    require(width != changed.npos, "Authoring fixture could not locate the declarative tool size.");
    changed.replace(width, std::string_view("width: 64;").size(), "width: 68;");
    const auto shell = changed.find("ui PanePath(");
    const auto height = shell == changed.npos ? changed.npos : changed.find("height: 32;", shell);
    require(height != changed.npos, "Authoring fixture could not locate the Parameters path-bar height.");
    changed.replace(height, std::string_view("height: 32;").size(), "height: 36;");
    std::ofstream timings(output / "ui-authoring.csv");
    timings << "save,save_to_accepted_ms,revision\n";
    const auto save = [&](const char* label, const std::string& text, bool valid, float expected_height,
                          const auto& verify_edit) {
        require_input_focus(app);
        const auto previous = Access::ui_revision(app);
        const auto start = GetTickCount64();
        model(atelier::editor::write_file(path, text));
        while (GetTickCount64() - start < 10000) {
            tick(app, 1);
            if (valid ? Access::ui_revision(app) > previous : !Access::ui_error(app).empty())
                break;
        }
        require(valid ? (Access::ui_revision(app) == previous + 1 && Access::ui_error(app).empty())
                      : (Access::ui_revision(app) == previous && !Access::ui_error(app).empty()),
                "Native file save did not publish or retain the correct version.");
        verify_edit();
        verify_hierarchy();
        const auto* path_control = app.presented()->find(control::parameter_path);
        const auto* bar = app.presented()->find(path_bar);
        require(path_control && bar && path_control->parent == path_bar &&
                    std::abs(bar->bounds.height / app.presented()->drawing->dpi_scale - expected_height) < .1f,
                "Saved Parameters shell geometry did not update through the retained path aliases.");
        const auto* tool = command_tool(app, Access::operator_item(app, "scene.add", {{"kind", "mesh"}}));
        require(tool && tool->key == tool_key && tool->incarnation == tool_incarnation &&
                    std::abs(tool->bounds.width / app.presented()->drawing->dpi_scale - 68) < .1f,
                "Saved tool layout lost its stable action identity or failed to retain the accepted layout.");
        timings << label << ',' << GetTickCount64() - start << ',' << Access::ui_revision(app) << '\n';
    };
    const auto verify_query = [&] {
        require(app.text_input() && app.text_input()->token == token && app.query() == L"position" &&
                    !app.presented()->find(control::position_fold)->selected && !app.presented()->find(field) &&
                    property_control(app, entity, 0) == field,
                "Native source update lost the shared query draft, local foldout state or World field key.");
    };
    save("valid", changed, true, path_height + 4, verify_query);
    capture(app, output / "ui-live-updated.png");
    save("syntax_error", changed + "\n?", false, path_height + 4, verify_query);
    capture(app, output / "ui-live-error.png");
    save("recovered", changed + "\n// recovered\n", true, path_height + 4, verify_query);
    // Both values are legal DSL properties, but this height cannot contain the
    // path bar's existing padding. Only the real layout preparation rejects it.
    auto invalid_layout = changed;
    invalid_layout.replace(height, std::string_view("height: 36;").size(), "height: 1;");
    save("layout_error", invalid_layout, false, path_height + 4, verify_query);
    require(Access::ui_error(app).find("layout candidate rejected") != std::string_view::npos,
            "Saved layout fixture failed before reaching the real frame preparation boundary.");
    capture(app, output / "ui-live-layout-rejected.png");
    save("layout_recovered", changed + "\n// layout recovered\n", true, path_height + 4, verify_query);
    key(app, VK_RETURN);
    click(app, control::position_fold);
    click(app, field);
    const auto field_history = app.document().undo_count();
    type(app, L"4.25");
    require(app.text_input() && app.document().editing(), "Authoring fixture did not start a real numeric draft.");
    const auto field_token = app.text_input()->token;
    auto draft_source = changed;
    draft_source.replace(height, std::string_view("height: 36;").size(), "height: 40;");
    save("field_draft", draft_source, true, path_height + 8, [&] {
        require(app.text_input() && app.text_input()->token == field_token && app.document().editing() &&
                    property_control(app, entity, 0) == field &&
                    app.presented()->find(field)->incarnation == field_incarnation && x(app, entity) == 4.25f &&
                    app.document().undo_count() == field_history,
                "A live Parameters shell resize replaced or committed the active numeric draft.");
    });
    key(app, VK_RETURN);
    require(x(app, entity) == 4.25f && !app.document().editing() && app.document().undo_count() == field_history + 1,
            "World field editing after live Source replacement did not commit exactly once.");
    auto minimized_source = draft_source;
    minimized_source.replace(height, std::string_view("height: 40;").size(), "height: 44;");
    require_input_focus(app);
    const auto minimized_revision = Access::ui_revision(app);
    ShowWindow(app.window(), SW_MINIMIZE);
    require(app.step() && IsIconic(app.window()), "Could not minimize the live authoring window.");
    const auto minimized_frames = app.accepted_frames();
    model(atelier::editor::write_file(path, minimized_source));
    const auto minimized_start = GetTickCount64();
    do {
        require(app.step(), "Editor closed while a saved UI awaited a drawable surface.");
    } while (GetTickCount64() - minimized_start < 400);
    require(Access::ui_revision(app) == minimized_revision && app.accepted_frames() == minimized_frames,
            "Minimized authoring published a UI candidate without preparing its layout.");
    ShowWindow(app.window(), SW_RESTORE);
    const auto restored_start = GetTickCount64();
    do {
        tick(app, 1);
        if (Access::ui_revision(app) > minimized_revision)
            break;
    } while (GetTickCount64() - restored_start < 10000);
    require(Access::ui_revision(app) == minimized_revision + 1 && Access::ui_error(app).empty() &&
                std::abs(app.presented()->find(path_bar)->bounds.height / app.presented()->drawing->dpi_scale -
                         path_height - 12) < .1f &&
                x(app, entity) == 4.25f && app.document().undo_count() == field_history + 1,
            "Restoring the surface did not prepare and publish the saved layout once while retaining scene values.");
    verify_hierarchy();
    timings << "minimized_restore," << GetTickCount64() - restored_start << ',' << Access::ui_revision(app) << '\n';
    capture(app, output / "ui-live-minimized-recovered.png");
    const auto count = entities(app).size();
    const auto history = app.document().undo_count();
    click(app, operator_item(app, "scene.add", {{"kind", "mesh"}}));
    require(entities(app).size() == count + 1 && app.document().undo_count() == history + 1,
            "A hot-updated tool did not dispatch exactly one real command.");
    click(app, operator_item(app, "edit.undo"));
    require(entities(app).size() == count && x(app, entity) == 4.25f,
            "Hot-updated tool command did not retain normal undo behavior.");
    capture(app, output / "ui-live-recovered.png");
    require(timings.good(), "Cannot write live authoring evidence.");
    verify_asset_ui_authoring_window(app, output, path);
}
