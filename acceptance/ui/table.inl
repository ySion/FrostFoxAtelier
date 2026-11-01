void shared_table_view() {
    for (float dpi : {1.f, 1.5f}) {
        Consumer c;
        accepted(c.ui.set_surface({{static_cast<unsigned>(640 * dpi), static_cast<unsigned>(480 * dpi)}, dpi, 2}));
        auto data = std::make_shared<ui::TableData>();
        data->columns = {{1, L"Name", 180, 64}, {2, L"Type", 90, 48}, {3, L"Size", 100, 48, ui::TextAlign::right}};
        for (ui::TableKey i = 0; i < 1500; ++i) {
            const auto row = 1000 + i;
            data->rows.push_back({row,
                                  {{row * 4, L"Resource " + std::to_wstring(i), ui::ControlIcon::mesh},
                                   {row * 4 + 1, L"Mesh"},
                                   {row * 4 + 2, std::to_wstring(i * 1024)}},
                                  i != 1});
        }
        auto table = leaf(10, ui::ControlKind::table, 45);
        table.layout = ui_layout::NodeInput::box(280, 210);
        table.layout.offset_x = 10;
        table.layout.offset_y = 45;
        table.table = data;
        table.table_selection = {1000, 1};
        table.action_target = 900;
        table.context_target = 901;
        table.style.scrollbar_width = 12;
        table.style.scrollbar_button_size = 12;
        c.controls = {leaf(9, ui::ControlKind::button, 5, L"Before"), table,
                      leaf(11, ui::ControlKind::button, 270, L"After")};
        c.prepare();
        const auto geometry = [&]() -> const ui::TableGeometry& {
            const auto* result = c.frame->find_table(10);
            require(result, "accepted table geometry missing");
            return *result;
        };
        const auto cell_point = [&](ui::TableKey row, ui::TableKey column) {
            const auto& cells = geometry().cells;
            const auto at = std::find_if(cells.begin(), cells.end(),
                                         [&](const auto& cell) { return cell.row == row && cell.column == column; });
            require(at != cells.end(), "requested table cell is not visible");
            return std::pair{(std::max(at->bounds.x, static_cast<float>(at->clip.left)) +
                              std::min(at->bounds.x + at->bounds.width, static_cast<float>(at->clip.right))) *
                                 .5f,
                             (std::max(at->bounds.y, static_cast<float>(at->clip.top)) +
                              std::min(at->bounds.y + at->bounds.height, static_cast<float>(at->clip.bottom))) *
                                 .5f};
        };
        const auto select = [&](const ui::Reply& reply) {
            const auto actions = actions_of(reply);
            require(actions.size() == 1 && actions[0].kind == ui::ActionKind::select && actions[0].key == 10 &&
                        !actions[0].target,
                    "table selection did not use its real control and typed payload");
            const auto snapshot = c.controls[1].table;
            c.controls[1].table_selection = std::get<ui::TableSelection>(actions[0].value);
            c.prepare();
            require(c.controls[1].table == snapshot && geometry().data == snapshot,
                    "selection copied the row snapshot");
        };
        const auto press_cell = [&](ui::TableKey row, ui::TableKey column, unsigned clicks = 1) {
            const auto [x, y] = cell_point(row, column);
            const auto down = c.ui.input({.kind = ui::EventKind::pointer_down, .x = x, .y = y, .clicks = clicks});
            require(down.capture == ui::Capture::acquire && actions_of(down).empty(),
                    "cell press committed before release");
            return std::pair{x, y};
        };
        require(c.frame->controls.size() == 3 && c.ui.statistics().layout_nodes_created == 3 &&
                    geometry().cells.size() < 40,
                "table created layout controls or geometry for offscreen rows");
        require(c.ui.statistics().rasterizations < 50 &&
                    std::any_of(
                        c.frame->drawing->draws.begin(), c.frame->drawing->draws.end(),
                        [](const auto& draw) { return draw.coverage && !draw.coverage->pixels.empty(); }),
                "table did not rasterize only its visible native text");
        const auto cached = c.ui.statistics().rasterizations;
        c.prepare();
        require(c.ui.statistics().rasterizations == cached, "unchanged visible table text was rasterized again");
        require(c.frame->scrollbars.size() == 2 && std::all_of(c.frame->scrollbars.begin(), c.frame->scrollbars.end(),
                                                               [](const auto& bar) {
                                                                   return bar.decrement.width > 0 &&
                                                                          bar.decrement.height > 0 &&
                                                                          bar.increment.width > 0 &&
                                                                          bar.increment.height > 0;
                                                               }),
                "table did not share both scrollbar step controls");
        c.ui.focus(9);
        key(c.ui, ui::Key::tab);
        require(c.ui.focused_control() == 10, "Tab did not enter the table collection");
        key(c.ui, ui::Key::tab);
        require(c.ui.focused_control() == 11, "table added a Tab stop per cell");
        key(c.ui, ui::Key::tab, false, true);
        require(c.ui.focused_control() == 10, "reverse Tab did not restore the table stop");
        c.controls[2].kind = ui::ControlKind::text_edit;
        c.prepare();
        c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
        key(c.ui, ui::Key::tab);
        require(c.ui.focused_control() == 11 && !c.ui.interaction_state().keyboard,
                "table Enter survived Tab into a text field");
        const auto transferred = c.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::enter});
        require(transferred.consumed && actions_of(transferred).empty() && !c.ui.interaction_state().active(),
                "text field swallowed table Enter release without retiring its activation");
        c.controls[2].kind = ui::ControlKind::button;
        c.prepare();
        const auto [x, y] = press_cell(1002, 1);
        select(c.ui.input({.kind = ui::EventKind::pointer_up, .x = x, .y = y}));
        require(geometry().selection == ui::TableSelection{1002, 1}, "accepted table selection missing");
        select(key(c.ui, ui::Key::home));
        select(key(c.ui, ui::Key::down));
        require(geometry().selection.row == 1002, "table keyboard navigation did not skip a disabled row");
        select(key(c.ui, ui::Key::right));
        require(geometry().selection.column == 2, "Right did not move the active column");
        select(key(c.ui, ui::Key::right));
        require(geometry().selection.column == 3, "Right did not reach the final column");
        const auto at_right = std::find_if(c.frame->scrollbars.begin(), c.frame->scrollbars.end(),
                                           [](const auto& bar) { return bar.horizontal; });
        require(at_right != c.frame->scrollbars.end() && at_right->value > 0, "active column did not scroll into view");
        const auto header_y = geometry().header.y;
        select(key(c.ui, ui::Key::page_down));
        require(geometry().selection.row > 1002 && geometry().header.y == header_y,
                "PageDown did not scroll rows below a fixed header");
        select(key(c.ui, ui::Key::end));
        require(geometry().selection.row == 2499 && std::any_of(geometry().cells.begin(), geometry().cells.end(),
                                                                [](const auto& cell) { return cell.row == 2499; }),
                "End did not select and reveal the final row");
        select(key(c.ui, ui::Key::page_up));
        require(geometry().selection.row < 2499, "PageUp did not move by the body viewport");
        select(key(c.ui, ui::Key::home));
        select(key(c.ui, ui::Key::left));
        select(key(c.ui, ui::Key::left));
        const auto before_enter = c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
        require(actions_of(before_enter).empty(), "Enter activated before release");
        const auto activated = actions_of(c.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::enter}));
        require(activated.size() == 1 && activated[0].kind == ui::ActionKind::activate && activated[0].key == 10 &&
                    activated[0].target == 900 &&
                    std::get<ui::TableSelection>(activated[0].value) == ui::TableSelection{1000, 1},
                "Enter lost its stable row or action route");
        const auto [dx, dy] = press_cell(1000, 1, 2);
        const auto doubled = actions_of(c.ui.input({.kind = ui::EventKind::pointer_up, .x = dx, .y = dy}));
        require(doubled.size() == 2 && doubled[0].kind == ui::ActionKind::select &&
                    doubled[1].kind == ui::ActionKind::activate && doubled[1].target == 900,
                "double click did not select then activate exactly once");
        const auto [rx, ry] = cell_point(1002, 1);
        const auto context = actions_of(c.ui.input(
            {.kind = ui::EventKind::pointer_down, .x = rx, .y = ry, .button = ui::PointerButton::secondary}));
        require(context.size() == 2 && context[0].kind == ui::ActionKind::select &&
                    context[1].kind == ui::ActionKind::context_menu,
                "right click did not select before the shared context request");
        const auto request = std::get<ui::ContextMenuRequest>(context[1].value);
        require(request.source == 10 && request.table == ui::TableSelection{1002, 1},
                "context request invented a row ControlKey");
        const auto header = geometry().columns.front();
        const auto hx = header.bounds.x + 20 * dpi;
        const auto hy = header.bounds.y + header.bounds.height * .5f;
        c.ui.input({.kind = ui::EventKind::pointer_down, .x = hx, .y = hy});
        const auto sorted = actions_of(c.ui.input({.kind = ui::EventKind::pointer_up, .x = hx, .y = hy}));
        require(sorted.size() == 1 && sorted[0].kind == ui::ActionKind::table_sort &&
                    std::get<ui::TableSort>(sorted[0].value) == ui::TableSort{1, ui::TableSortDirection::ascending},
                "table header did not request typed sorting");
        auto sorted_data = std::make_shared<ui::TableData>(*data);
        sorted_data->sort = {1, ui::TableSortDirection::ascending};
        c.controls[1].table = sorted_data;
        c.prepare();
        c.ui.input({.kind = ui::EventKind::pointer_down, .x = hx, .y = hy});
        const auto descending = actions_of(c.ui.input({.kind = ui::EventKind::pointer_up, .x = hx, .y = hy}));
        require(descending.size() == 1 &&
                    std::get<ui::TableSort>(descending[0].value).direction == ui::TableSortDirection::descending,
                "accepted sort state did not toggle the next requested direction");
        const auto resize_point = [&]() {
            const auto grip = geometry().columns.front().resize;
            return std::pair{grip.x + grip.width * .5f, grip.y + grip.height * .5f};
        };
        auto [gx, gy] = resize_point();
        require(c.ui.input({.kind = ui::EventKind::pointer_down, .x = gx, .y = gy}).capture == ui::Capture::acquire,
                "column resize did not capture");
        const auto preview = c.ui.input({.kind = ui::EventKind::pointer_move, .x = gx - 300 * dpi, .y = gy});
        c.prepare();
        require(actions_of(preview).empty() && c.controls[1].table->columns[0].width == 180 &&
                    std::abs(geometry().columns[0].bounds.width - 64 * dpi) < .01f,
                "column preview mutated the host or ignored min_width");
        const auto resized = actions_of(c.ui.input({.kind = ui::EventKind::pointer_up, .x = gx - 300 * dpi, .y = gy}));
        require(resized.size() == 1 && resized[0].kind == ui::ActionKind::table_column_resize &&
                    std::get<ui::TableColumnResize>(resized[0].value) == ui::TableColumnResize{1, 64},
                "column release did not emit one committed width");
        c.prepare();
        std::tie(gx, gy) = resize_point();
        c.ui.input({.kind = ui::EventKind::pointer_down, .x = gx, .y = gy});
        c.ui.input({.kind = ui::EventKind::pointer_move, .x = gx + 25 * dpi, .y = gy});
        const auto cancelled = c.ui.input({.kind = ui::EventKind::capture_lost});
        c.prepare();
        require(cancelled.capture == ui::Capture::release && actions_of(cancelled).empty() &&
                    std::abs(geometry().columns[0].bounds.width - 180 * dpi) < .01f,
                "capture loss committed or retained a column preview");
        c.ui.input({.kind = ui::EventKind::wheel,
                    .x = geometry().body.x + 8 * dpi,
                    .y = geometry().body.y + 8 * dpi,
                    .wheel = -5});
        c.prepare();
        const auto vertical_scroll = [&] {
            const auto bar = std::find_if(c.frame->scrollbars.begin(), c.frame->scrollbars.end(),
                                          [](const auto& value) { return !value.horizontal; });
            require(bar != c.frame->scrollbars.end(), "table vertical scrollbar disappeared");
            return bar->value;
        };
        const auto scrolled = vertical_scroll();
        auto width_only = std::make_shared<ui::TableData>(*sorted_data);
        width_only->columns[0].width = 185;
        c.controls[1].table = width_only;
        c.prepare();
        require(scrolled > 0 && vertical_scroll() == scrolled,
                "column-only snapshot update jumped back to an offscreen selection");
        c.controls[1].table = sorted_data;
        c.prepare();
        std::tie(gx, gy) = resize_point();
        c.ui.input({.kind = ui::EventKind::pointer_down, .x = gx, .y = gy});
        auto replaced = std::make_shared<ui::TableData>(*sorted_data);
        std::reverse(replaced->rows.begin(), replaced->rows.end());
        c.controls[1].table = replaced;
        const auto rebound = accepted(c.ui.set_controls(c.controls));
        require(rebound.capture == ui::Capture::release && actions_of(rebound).empty(),
                "snapshot reorder retained table capture");
        require(actions_of(c.ui.input({.kind = ui::EventKind::pointer_up, .x = gx, .y = gy})).empty() &&
                    !c.ui.input({.kind = ui::EventKind::pointer_down, .x = gx, .y = gy}).consumed,
                "old accepted table geometry routed a new snapshot");
        c.prepare();
        c.ui.focus(10);
        c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
        c.controls[1].table_selection = {1002, 1};
        accepted(c.ui.update_controls(std::span{&c.controls[1], 1}));
        c.prepare();
        c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
        require(actions_of(c.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::enter})).empty(),
                "selection replacement let a held Enter repeat activate the replacement row");
        c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
        c.controls[1].action_target = 902;
        accepted(c.ui.update_controls(std::span{&c.controls[1], 1}));
        c.prepare();
        c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
        require(actions_of(c.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::enter})).empty(),
                "retargeting rearmed held Enter");
        auto invalid = std::make_shared<ui::TableData>(*data);
        invalid->rows[1].key = invalid->rows[0].key;
        c.controls[1].table = invalid;
        require(!c.ui.set_controls(c.controls), "duplicate stable table row identity was accepted");
        auto empty = std::make_shared<ui::TableData>();
        empty->columns = data->columns;
        c.controls[1].table = empty;
        c.controls[1].table_selection = {};
        c.prepare();
        c.ui.focus(10);
        const auto empty_menu = actions_of(key(c.ui, ui::Key::menu));
        require(empty_menu.size() == 1 && empty_menu[0].kind == ui::ActionKind::context_menu &&
                    !std::get<ui::ContextMenuRequest>(empty_menu[0].value).table,
                "empty table keyboard menu invented a selected row");
        c.controls[1].table = data;
        c.controls[1].table_selection = {1000, 1};
        c.controls[1].layout.width = ui_layout::Axis::fixed(100);
        c.prepare();
        require(std::abs(geometry().columns.front().bounds.x - geometry().body.x) < .01f,
                "wide active column hid its leading text in a narrow table");
    }
}
