// Real Project -> Workspace -> shared UI layout/drawing/hit data, without a
// native window or desktop input. The separate HWND acceptance remains required.
void verify_project_workspace(editor::Project& project) {
    namespace browser = editor::browser_control;
    editor::Session document;
    editor::Workspace workspace;
    workspace.project_open = true;
    workspace.assets.reset(true);
    const std::array fonts{(std::filesystem::current_path() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface(fonts);
    auto theme = ui::StyleSheet::parse(take(assets::read_file("editor.ss")));
    require(theme.has_value(), "Project UI theme failed to parse.");
    require(surface.set_style_sheet(*theme).has_value(), "Project UI theme failed to resolve.");
    ui::PixelExtent current_extent{};
    float current_scale{};
    std::uint64_t surface_revision{};
    const auto present = [&](ui::PixelExtent extent, float scale) {
        if (current_extent != extent || current_scale != scale)
            ++surface_revision;
        current_extent = extent;
        current_scale = scale;
        require(surface.set_surface({extent, scale, surface_revision}).has_value(), "Project UI rejected its surface.");
        const auto controls = workspace.describe(document, {extent.width / scale, extent.height / scale},
                                                 surface.presented().get(), &project);
        auto described = surface.set_controls(controls);
        if (!described)
            throw std::runtime_error("Project UI invalid control " + std::to_string(described.error().key));
        workspace.commit_ui_controls();
        auto frame = surface.prepare();
        if (!frame)
            throw std::runtime_error("Project UI layout failure " + std::to_string(frame.error().key) + " at " +
                                     std::to_string(extent.width) + "x" + std::to_string(extent.height) + " scale " +
                                     std::to_string(scale));
        require(surface.accept(*frame), "Project UI rejected the current prepared frame.");
    };
    const auto input = [&](const ui::Event& event) {
        auto reply = surface.input(event);
        require(!reply.error, "Project UI input failed.");
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect)) {
                const ui::Action routed{action->kind, action->target ? action->target : action->key, action->value};
                if (!workspace.assets.apply(routed) && !action->target)
                    require(workspace.apply_ui(*action).has_value(), "Project Source/JIT event failed.");
            }
    };
    const auto pointer_click = [&](float x, float y) {
        for (const auto kind : {ui::EventKind::pointer_down, ui::EventKind::pointer_up})
            input({.kind = kind, .x = x, .y = y, .button = ui::PointerButton::primary});
    };
    const auto resource_visible = [&](ui::ControlKey key) {
        const auto* table = surface.presented()->find_table(browser::results);
        return table && std::ranges::any_of(table->cells, [&](const auto& cell) {
                   return cell.row == key && cell.bounds.y >= cell.clip.top &&
                          cell.bounds.y + cell.bounds.height <= cell.clip.bottom;
               });
    };
    const auto press = [&](ui::Key key) {
        input({.kind = ui::EventKind::key_down, .key = key});
        input({.kind = ui::EventKind::key_up, .key = key});
        present(current_extent, current_scale);
    };
    const auto click = [&](ui::ControlKey key) {
        if (!surface.presented()->find(key)) {
            // Installed material Sources add real rows; a mesh need not be in
            // the first virtualized page of the accepted resource table.
            for (std::size_t attempt = 0; !resource_visible(key); ++attempt) {
                const auto* table = surface.presented()->find_table(browser::results);
                require(table && !table->cells.empty() && attempt <= table->data->rows.size(),
                        "Project resource cannot be revealed by scrolling.");
                const auto target = std::ranges::find(table->data->rows, key, &ui::TableRow::key);
                require(target != table->data->rows.end(), "Project resource is absent from the table model.");
                const auto first = std::ranges::find(table->data->rows, table->cells.front().row, &ui::TableRow::key);
                const auto step = table->row_stride / current_scale / 48;
                input({.kind = ui::EventKind::wheel,
                       .x = table->body.x + table->body.width / 2,
                       .y = table->body.y + table->body.height / 2,
                       .wheel = target < first ? step : -step});
                present(current_extent, current_scale);
            }
            const auto* table = surface.presented()->find_table(browser::results);
            require(table, "Project resource has no accepted table.");
            const auto cell = std::ranges::find(table->cells, key, &ui::TableCellGeometry::row);
            require(cell != table->cells.end(), "Project resource has no accepted visible cell.");
            const auto left = std::max(cell->bounds.x, static_cast<float>(cell->clip.left));
            const auto right = std::min(cell->bounds.x + cell->bounds.width, static_cast<float>(cell->clip.right));
            const auto top = std::max(cell->bounds.y, static_cast<float>(cell->clip.top));
            const auto bottom = std::min(cell->bounds.y + cell->bounds.height, static_cast<float>(cell->clip.bottom));
            require(right > left && bottom > top, "Project resource table cell is clipped.");
            pointer_click((left + right) / 2, (top + bottom) / 2);
            return;
        }
        const auto visible = [&](const ui::ControlBox& box) {
            const auto y = box.bounds.y + box.bounds.height / 2;
            return y >= box.clip.top && y < box.clip.bottom;
        };
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            const auto* target = surface.presented()->find(key);
            require(target != nullptr, "Project UI target is missing.");
            if (visible(*target))
                break;
            auto parent = target->parent;
            while (parent) {
                const auto found = std::ranges::find_if(surface.presented()->scrollbars,
                                                        [&](const auto& bar) { return bar.key == parent; });
                if (found != surface.presented()->scrollbars.end()) {
                    surface.input({.kind = ui::EventKind::wheel,
                                   .x = found->track.x + found->track.width / 2,
                                   .y = found->track.y + found->track.height / 2,
                                   .wheel = target->bounds.y < target->clip.top ? 3.f : -3.f});
                    break;
                }
                const auto* ancestor = surface.presented()->find(parent);
                parent = ancestor ? ancestor->parent : 0;
            }
            present(current_extent, current_scale);
        }
        const auto* box = surface.presented()->find(key);
        require(box && visible(*box), "Project UI target is not reachable through scrolling.");
        const auto x = box->bounds.x + box->bounds.width / 2;
        const auto y = box->bounds.y + box->bounds.height / 2;
        pointer_click(x, y);
    };
    present({1600, 1000}, 1);
    present({1600, 1000}, 1);
    const auto* details = surface.presented()->find(browser::results);
    require(details && details->kind == ui::ControlKind::table && details->parent == browser::content &&
                surface.presented()->find(browser::search)->kind == ui::ControlKind::text_edit &&
                surface.presented()->find(browser::directories)->kind == ui::ControlKind::tree,
            "Asset Source/JIT regions did not preserve their real shell aliases and Table content slot.");
    click(browser::engine);
    present({1600, 1000}, 1);
    click(workspace.assets.row_key("builtin/sphere"));
    workspace.dock.maximize(editor::control::assets_tab);
    workspace.assets.apply({ui::ActionKind::activate, browser::engine, {}});
    {
        present({240, 720}, 1);
        present({240, 720}, 1);
        const auto* path = surface.presented()->find(browser::path);
        const auto* search = surface.presented()->find(browser::search);
        require(path && search && std::abs(path->bounds.y - search->bounds.y) < .1f,
                "Narrow asset navigation split its path and search onto different rows.");
        for (const auto key : {browser::back, browser::up, browser::path, browser::search, browser::filter,
                               browser::view_menu, browser::actions}) {
            const auto* tool = surface.presented()->find(key);
            require(tool && tool->bounds.width > 0 && tool->bounds.x >= tool->clip.left &&
                        tool->bounds.x + tool->bounds.width <= tool->clip.right + .1f,
                    "The compact asset toolbar overflowed its narrow pane.");
        }
        const auto* tree = surface.presented()->find(browser::directories);
        const auto* table = surface.presented()->find_table(browser::results);
        require(tree && tree->bounds.width > 0 && table && table->data->columns.size() == 3 &&
                    resource_visible(workspace.assets.row_key("builtin/sphere")),
                "A narrow pane lost its directory tree or one of the actual table columns.");
        float content_width{};
        for (const auto& column : table->data->columns)
            content_width += column.width;
        if (content_width > table->body.width + .1f)
            require(std::ranges::any_of(surface.presented()->scrollbars,
                                        [](const auto& bar) {
                                            return bar.key == browser::results && bar.horizontal && bar.maximum > 0;
                                        }),
                    "Narrow Details hid columns instead of retaining a horizontal scroll range.");
        for (const auto& cell : table->cells) {
            const auto column = std::ranges::find(table->columns, cell.column, &ui::TableColumnGeometry::column);
            require(column != table->columns.end() && std::abs(column->bounds.x - cell.bounds.x) < .1f &&
                        std::abs(column->bounds.width - cell.bounds.width) < .1f,
                    "Table header and body did not share the accepted column geometry.");
        }
    }
    present({1000, 720}, 1);
    workspace.dock.maximize(editor::control::assets_tab);
    workspace.assets.apply({ui::ActionKind::activate, browser::project, {}});
    const std::wstring unicode_name = L"Entry00000_材质\U0001F98A.txt";
    const auto unicode_resource = "Source/" + assets::utf8(unicode_name);
    std::ofstream(project.root() / "Source" / unicode_name) << "// Unicode catalog fixture\n";
    for (unsigned i = 0; i < 64; ++i)
        std::ofstream(project.root() / "Source" / ("Entry" + std::to_string(10000 + i) + ".txt"))
            << "// Catalog acceptance fixture\n";
    take(project.refresh());
    workspace.assets.navigate(L"Content");
    workspace.assets.navigate(L"Source");
    present({1600, 1000}, 1);
    present({1600, 1000}, 1);
    const auto unicode_key = workspace.assets.row_key(unicode_resource);
    require(unicode_key && resource_visible(unicode_key), "Unicode resource key was not owned by the asset browser.");
    click(unicode_key);
    present({1600, 1000}, 1);
    press(ui::Key::end);
    const auto* end_table = surface.presented()->find_table(browser::results);
    const auto last = workspace.assets.row_key("Source/Entry10063.txt");
    require(end_table->selection.row == last && last && resource_visible(last),
            "Table End did not select and reveal the authored final file.");
    take(project.refresh());
    workspace.assets.navigate(L"Content");
    workspace.assets.navigate(L"Source");
    present({1600, 1000}, 1);
    present({1600, 1000}, 1);
    const auto& refreshed_rows = surface.presented()->find_table(browser::results)->data->rows;
    require(workspace.assets.row_key(unicode_resource) == unicode_key &&
                std::ranges::find(refreshed_rows, unicode_key, &ui::TableRow::key) != refreshed_rows.end(),
            "Refresh or temporary invisibility changed a resource's row identity.");
    workspace.assets.reset(true);
    workspace.assets.navigate(L"Source");
    present({1600, 1000}, 1);
    present({1600, 1000}, 1);
    require(workspace.assets.row_key(unicode_resource) > unicode_key, "Reset reused a previous resource identity.");

    // Path order differs from display-name order and from native-string order at prefix siblings.
    const std::array<std::filesystem::path, 3> directories{
        "Source/DirectoryLookup/Z-parent/child", "Source/DirectoryLookup/Z-parent-2", "Source/DirectoryLookup/a-last"};
    for (const auto& path : directories)
        std::filesystem::create_directories(project.root() / path);
    take(project.refresh());
    workspace.assets.navigate(directories[0]);
    present({1600, 1000}, 1);
    take(workspace.assets.set_expanded(directories, true));
    require(surface.presented()->find(workspace.assets.directory_control(directories[0])),
            "A newly discovered current directory did not expand its ancestors.");
    workspace.assets.navigate(directories[1]);
    workspace.assets.navigate(directories[2]);
    present({1600, 1000}, 1);
    std::filesystem::remove(project.root() / directories[1]);
    take(project.refresh());
    present({1600, 1000}, 1);
    require(workspace.assets.directory() == directories[2], "Refresh discarded a surviving current directory.");
    workspace.assets.apply({ui::ActionKind::activate, browser::back, {}});
    present({1600, 1000}, 1);
    require(workspace.assets.directory() == directories[0],
            "Back navigation retained a removed directory or lost a surviving one.");
    std::filesystem::remove_all(project.root() / "Source/DirectoryLookup");
    take(project.refresh());
    present({1600, 1000}, 1);
    static_cast<void>(workspace.assets.set_expanded(directories, true));
    require(!workspace.assets.engine() && workspace.assets.directory().empty(),
            "Removed current directories or expansion state survived refresh.");
    std::puts(
        "Project workspace acceptance: PASS (Source/JIT Table, narrow geometry, last row, UTF-8 identity, retired directory)");
}
