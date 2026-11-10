void verify_table_views() {
    namespace language = atelier::ui_language;
    Runtime runtime;
    take(language::register_types(runtime));
    const std::string source = R"(
ui Results(in TableBinding model, in Text view, in double width) {
    Table results {
        model: input.model; view: input.view; context: node.results;
        width: input.width; height: 200; tile_width: 72; tile_height: 48; item_gap: 2;
        empty_text: "No items";
    }
})";
    auto data = std::make_shared<ui::TableData>();
    data->columns = {{11, L"Name", 160, 40}, {12, L"Type", 100, 40}};
    for (ui::TableKey i = 1; i <= 65; ++i)
        data->rows.push_back(
            {i, {{i * 2, L"Item " + std::to_wstring(i), ui::ControlIcon::mesh}, {i * 2 + 1, L"Mesh"}}});
    const std::shared_ptr<const ui::TableData> snapshot = std::move(data);
    std::array<language::InputValue, 3> inputs{language::TableBinding{snapshot, {40, 12}}, std::wstring{L"tiles"},
                                               360.0};
    auto module = take(language::Module::compile(runtime, "table-views.ui.source", source));
    ui::ControlKey next = 10000;
    const language::KeyBinding alias{"results", 41};
    auto instance = take(language::Instance::create(module->find("Results"), 0, next, std::span{&alias, 1}, inputs));
    ui::Session surface;
    take(surface.initialize());
    constexpr float scale = 1.5f;
    const auto present = [&] {
        require(surface
                    .set_surface({{static_cast<unsigned>(400 * scale), static_cast<unsigned>(240 * scale)},
                                  scale,
                                  static_cast<std::uint64_t>(scale * 2)})
                    .has_value(),
                "Table views surface failed.");
        take(instance->update(inputs));
        require(surface.set_controls(instance->controls()).has_value(), "Table views controls failed.");
        instance->commit_controls();
        auto frame = surface.prepare();
        require(frame && surface.accept(*frame), "Table views present failed.");
    };
    const auto press = [&](ui::Key key) {
        auto reply = surface.input({.kind = ui::EventKind::key_down, .key = key});
        require(!reply.error, "Table view keyboard failed.");
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect); action && action->kind == ui::ActionKind::select)
                std::get<language::TableBinding>(inputs[0]).selection = std::get<ui::TableSelection>(action->value);
        surface.input({.kind = ui::EventKind::key_up, .key = key});
        present();
    };
    present();
    surface.focus(41);
    press(ui::Key::end);
    const auto* geometry = surface.presented()->find_table(41);
    require(geometry && geometry->selection.row == 65 &&
                std::ranges::any_of(geometry->cells,
                                    [](const auto& cell) {
                                        return cell.row == 65 && cell.bounds.y >= cell.clip.top &&
                                               cell.bounds.y + cell.bounds.height <= cell.clip.bottom + .1f;
                                    }),
            "The final partial row was not fully revealed.");
    inputs[2] = 44.0;
    present();
    geometry = surface.presented()->find_table(41);
    require(geometry->columns_per_row == 1 && geometry->selection.row == 65 && !geometry->cells.empty(),
            "Narrow view reflow lost its current item.");
    for (const auto& cell : geometry->cells)
        require(cell.bounds.width <= 44 * scale + .1f, "A narrow tile exceeded its authored body width.");
    inputs[2] = 360.0;
    present();
    auto cell = surface.presented()->find_table(41)->cells.back();
    const float x = cell.bounds.x + 4;
    const float y = cell.bounds.y + 4;
    auto held = surface.input({.kind = ui::EventKind::pointer_down, .x = x, .y = y, .clicks = 2});
    require(held.capture == ui::Capture::acquire, "View switch fixture did not hold an item.");
    inputs[1] = std::wstring{L"list"};
    take(instance->update(inputs));
    auto changed = surface.set_controls(instance->controls());
    require(changed && changed->capture == ui::Capture::release, "View replacement retained the old item press.");
    instance->commit_controls();
    require(surface.input({.kind = ui::EventKind::pointer_up, .x = x, .y = y}).effects.empty(),
            "Old view pixels activated an item after a presentation change.");
    present();
    const auto accepted = std::vector<ui::Control>(instance->controls().begin(), instance->controls().end());
    inputs[1] = std::wstring{L"unknown"};
    const auto rejected = instance->update(inputs);
    require(!rejected && rejected.error().code == ErrorCode::invalid_definition,
            "Invalid view did not return a recoverable binding error.");
    require(std::ranges::equal(instance->controls(), accepted), "Invalid view replaced accepted controls.");
    inputs[1] = std::wstring{L"tiles"};
    present();
    require(instance->controls().front().table_layout.view == ui::TableView::tiles &&
                surface.presented()->find_table(41)->data == snapshot &&
                surface.presented()->find_table(41)->selection.row == 65,
            "Corrected view did not recover the retained Table model and selection.");
    auto empty = std::make_shared<ui::TableData>();
    empty->columns = snapshot->columns;
    inputs[0] = language::TableBinding{empty, {}};
    present();
    require(surface.presented()->find_table(41)->cells.empty() && surface.presented()->scrollbars.empty(),
            "Empty view retained cells or a false scrollbar.");
    require(surface.set_controls({}).has_value(), "Table views unmount failed.");
    instance->close();
    std::puts(
        "Table views DSL acceptance: PASS (tile reflow, last row, view-change cancellation, accepted controls, empty view)");
}

void verify_table_dsl() {
    namespace language = ui_language;
    Runtime runtime;
    take(ui_language::register_types(runtime));
    std::string source = R"(
ui Grid(in TableBinding model, in ActionBinding open, in Key context, in bool available, in double preferred) {
    Table grid {
        model: input.model; action: input.open; context: input.context;
        enabled: input.available; shrink_width: input.preferred; min_width: 96; height: 180;
        classes: "table-before";
    }
}
ui Copy(in TableBinding model, in ActionBinding open) {
    Table grid { model: input.model; action: input.open; context: node.grid; width: 240; height: 100; }
}
ui Board(in TableBinding model, in ActionBinding open, in Key context,
         in Items<Copy> copies, in bool available, in double preferred) {
    Column body {
        Row layout {
            width: 240; height: 180;
            Grid primary { model: input.model; open: input.open; context: input.context; available: input.available; preferred: input.preferred; }
            Text sibling { text: "Sibling"; width: 144; height: 24; }
        }
        Repeat copies { items: input.copies; }
    }
}
)";
    auto writable = std::make_shared<ui::TableData>();
    writable->columns = {{11, L"Name", 64, 40}, {12, L"Type", 64, 40}};
    writable->rows = {{101, {{1001, L"First"}, {1002, L"Mesh"}}}, {102, {{1003, L"Second"}, {1004, L"Scene"}}}};
    const std::shared_ptr<const ui::TableData> snapshot = std::move(writable);
    constexpr ui::ControlKey table_key = 701;
    constexpr ui::ControlKey open = 9001;
    constexpr ui::ControlKey other_open = 9002;
    constexpr ui::ControlKey context = 9003;
    const language::TableBinding original{snapshot, {101, 11}, true};
    const auto copies = [&](ui::TableSelection selection) {
        return language::ItemList{std::make_shared<const std::vector<language::Item>>(std::vector<language::Item>{
            {7101, {language::TableBinding{snapshot, selection}, language::ActionBinding{open}}},
            {7102, {original, language::ActionBinding{open}}}})};
    };
    std::array<language::InputValue, 6> inputs{
        original, language::ActionBinding{open}, context, copies({101, 11}), true, 144.0};
    auto module = take(language::Module::compile(runtime, "table-dsl.ui.source", source));
    ui::ControlKey next = 20000;
    const std::array<language::KeyBinding, 2> keys{{{"primary.grid", table_key}, {"layout", 702}}};
    auto instance = take(language::Instance::create(module->find("Board"), 0, next, keys, inputs));
    ui::Session surface;
    take(surface.initialize());
    require(surface.set_surface({{640, 480}, 1, 1}).has_value(), "Table DSL surface failed.");
    std::vector<ui::Action> actions;
    const auto route = [&](const ui::Reply& reply) {
        require(!reply.error, "Table DSL input failed.");
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect))
                actions.push_back(*action);
    };
    const auto present = [&] {
        auto frame = surface.prepare();
        require(frame && surface.accept(*frame), "Table DSL frame was refused.");
    };
    const auto submit = [&] {
        auto updated = surface.set_controls(instance->controls());
        require(updated.has_value(), "Table DSL controls were refused.");
        route(*updated);
        instance->commit_controls();
    };
    const auto draw = [&] {
        take(instance->update(inputs));
        submit();
        present();
    };
    const auto cell = [&](ui::ControlKey owner, ui::TableKey row) {
        const auto* table = surface.presented()->find_table(owner);
        require(table, "Missing accepted DSL Table.");
        const auto found = std::ranges::find(table->cells, row, &ui::TableCellGeometry::row);
        require(found != table->cells.end(), "Missing visible DSL Table cell.");
        return *found;
    };
    const auto pointer = [&](ui::EventKind kind, ui::ControlKey owner, ui::TableKey row, unsigned clicks = 1,
                             ui::PointerButton button = ui::PointerButton::primary) {
        const auto target = cell(owner, row);
        ui::Event event;
        event.kind = kind;
        event.x = target.bounds.x + 8;
        event.y = target.bounds.y + 8;
        event.clicks = clicks;
        event.button = button;
        route(surface.input(event));
    };
    const auto click = [&](ui::ControlKey owner, ui::TableKey row, unsigned clicks = 1) {
        pointer(ui::EventKind::pointer_down, owner, row, clicks);
        pointer(ui::EventKind::pointer_up, owner, row);
    };
    draw();
    const auto identity = instance->statistics();
    const auto incarnation = surface.presented()->find(table_key)->incarnation;
    for (const auto key : {table_key, ui::ControlKey{7101}, ui::ControlKey{7102}})
        require(surface.presented()->find_table(key) && surface.presented()->find_table(key)->data == snapshot,
                "Nested or repeated TableBinding copied or lost the immutable snapshot.");
    require(surface.presented()->find(table_key)->bounds.width >= 95.9f &&
                surface.presented()->find(table_key)->bounds.width <= 96.1f,
            "shrink_width did not let the real narrow row reach its minimum width.");
    inputs[5] = 200.0;
    draw();
    require(surface.presented()->find(table_key)->bounds.width >= 95.9f &&
                surface.presented()->find(table_key)->bounds.width <= 96.1f,
            "Updating shrink_width discarded the unchanged min_width constraint.");

    actions.clear();
    click(table_key, 102);
    require(actions.size() == 1 && actions[0].kind == ui::ActionKind::select && actions[0].key == table_key &&
                !actions[0].target && std::get<ui::TableSelection>(actions[0].value) == ui::TableSelection{102, 11},
            "Table single click did not retain its native selection route.");
    std::get<language::TableBinding>(inputs[0]).selection = {102, 11};
    inputs[3] = copies({102, 12});
    draw();
    require(surface.presented()->find(table_key)->incarnation == incarnation &&
                surface.presented()->find_table(table_key)->data == snapshot &&
                surface.presented()->find_table(table_key)->selection == ui::TableSelection{102, 11} &&
                surface.presented()->find_table(7101)->selection == ui::TableSelection{102, 12},
            "A lightweight selection update copied data, recreated the Table or failed to pass through Repeat.");
    actions.clear();
    pointer(ui::EventKind::pointer_down, table_key, 101, 1, ui::PointerButton::secondary);
    require(actions.size() == 2 && actions[0].kind == ui::ActionKind::select &&
                actions[1].kind == ui::ActionKind::context_menu && actions[1].key == context && !actions[1].target,
            "Table context request bypassed the shared selection/menu route.");
    const auto request = std::get<ui::ContextMenuRequest>(actions[1].value);
    require(request.source == table_key && request.table == ui::TableSelection{101, 11},
            "A Table row became a synthetic context-menu source Control.");
    actions.clear();
    pointer(ui::EventKind::pointer_down, 7101, 101, 1, ui::PointerButton::secondary);
    require(actions.size() == 2 && actions[1].key == 7101 &&
                std::get<ui::ContextMenuRequest>(actions[1].value).source == 7101,
            "node.grid did not resolve the repeated Table's actual host key.");
    actions.clear();
    click(table_key, 102, 2);
    require(actions.size() == 2 && actions[0].kind == ui::ActionKind::select && !actions[0].target &&
                actions[1].kind == ui::ActionKind::activate && actions[1].key == table_key &&
                actions[1].target == open &&
                std::get<ui::TableSelection>(actions[1].value) == ui::TableSelection{102, 11},
            "Table double click lost its source, typed row or activation target.");
    actions.clear();
    route(surface.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter}));
    route(surface.input({.kind = ui::EventKind::key_up, .key = ui::Key::enter}));
    require(actions.size() == 1 && actions[0].kind == ui::ActionKind::activate && actions[0].target == open,
            "Table Enter did not use the existing activation route.");
    require(!take(instance->event(actions[0])), "A host Table activation also entered a local DSL handler.");
    const auto header = surface.presented()->find_table(table_key)->columns.front();
    actions.clear();
    route(surface.input({.kind = ui::EventKind::pointer_down, .x = header.bounds.x + 8, .y = header.bounds.y + 8}));
    route(surface.input({.kind = ui::EventKind::pointer_up, .x = header.bounds.x + 8, .y = header.bounds.y + 8}));
    require(actions.size() == 1 && actions[0].kind == ui::ActionKind::table_sort && actions[0].key == table_key &&
                !actions[0].target,
            "Table sort was redirected through its activation binding.");
    actions.clear();
    const auto edge = header.resize.x + header.resize.width * .5f;
    const auto y = header.resize.y + 8;
    route(surface.input({.kind = ui::EventKind::pointer_down, .x = edge, .y = y}));
    route(surface.input({.kind = ui::EventKind::pointer_move, .x = edge + 8, .y = y}));
    route(surface.input({.kind = ui::EventKind::pointer_up, .x = edge + 8, .y = y}));
    require(actions.size() == 1 && actions[0].kind == ui::ActionKind::table_column_resize &&
                actions[0].key == table_key && !actions[0].target,
            "Table column resize was redirected through its activation binding.");

    const auto mark = source.find("table-before");
    require(mark != source.npos, "Table patch marker missing.");
    source.replace(mark, std::string_view("table-before").size(), "table-after");
    auto patched = take(language::Module::compile(runtime, "table-dsl.ui.source", source, module.get()));
    take(instance->update(inputs, patched->find("Board")));
    submit();
    present();
    module = std::move(patched);
    const auto after_patch = instance->statistics();
    require(after_patch.instance == identity.instance && after_patch.scope_creations == identity.scope_creations &&
                after_patch.scope_retirements == identity.scope_retirements &&
                surface.presented()->find(table_key)->incarnation == incarnation &&
                surface.presented()->find_table(table_key)->data == snapshot,
            "Source patch replaced a Table scope, its shared snapshot or its accepted control identity.");
    const auto accepted = std::vector<ui::Control>(instance->controls().begin(), instance->controls().end());
    auto bad_copies = std::make_shared<std::vector<language::Item>>(*std::get<language::ItemList>(inputs[3]).values);
    bad_copies->back().inputs[0] = false;
    auto bad_repeat = inputs;
    bad_repeat[3] = language::ItemList{std::move(bad_copies)};
    static_cast<void>(instance->update(bad_repeat));
    require(std::ranges::equal(instance->controls(), accepted),
            "A wrongly typed repeated Table argument partially replaced the composed tree.");
    auto bad_data = std::make_shared<ui::TableData>(*snapshot);
    bad_data->rows[1].cells[0].key = bad_data->rows[0].cells[0].key;
    auto candidate = inputs;
    candidate[0] = language::TableBinding{std::move(bad_data), {102, 11}};
    const auto old_frame = surface.presented();
    take(instance->update(candidate));
    static_cast<void>(surface.set_controls(instance->controls()));
    require(surface.presented() == old_frame,
            "Session accepted duplicate Table cell identities or replaced its presented frame on refusal.");
    instance->reject_controls();
    require(std::ranges::equal(instance->controls(), accepted),
            "Rejected native Table data did not roll back the prepared DSL tree.");
    draw();
    actions.clear();
    click(table_key, 102, 2);
    require(actions.size() == 2 && actions.back().target == open,
            "The accepted Table stopped working after a refused snapshot.");
    auto renamed = source;
    const auto name = renamed.find("Table grid");
    renamed.replace(name, std::string_view("Table grid").size(), "Table renamed");
    auto bad_patch = take(language::Module::compile(runtime, "renamed-table.ui.source", renamed, module.get()));
    static_cast<void>(instance->update(inputs, bad_patch->find("Board")));
    require(std::ranges::equal(instance->controls(), accepted),
            "A source patch that broke the concrete Table alias replaced the live tree.");

    actions.clear();
    pointer(ui::EventKind::pointer_down, table_key, 102, 2);
    inputs[1] = language::ActionBinding{other_open};
    take(instance->update(inputs));
    auto retarget = surface.set_controls(instance->controls());
    require(retarget && retarget->capture == ui::Capture::release,
            "Retargeting a held Table activation did not release capture.");
    route(*retarget);
    instance->commit_controls();
    pointer(ui::EventKind::pointer_up, table_key, 102);
    require(actions.empty(), "Old accepted Table pixels dispatched a newly bound target.");
    present();
    pointer(ui::EventKind::pointer_up, table_key, 102);
    require(actions.empty(), "Table retarget retained its old press.");
    click(table_key, 102, 2);
    require(actions.size() == 2 && actions.back().target == other_open,
            "A new Table gesture lost the new activation target.");
    actions.clear();
    inputs[2] = ui::ControlKey{9004};
    take(instance->update(inputs));
    submit();
    pointer(ui::EventKind::pointer_down, table_key, 102, 1, ui::PointerButton::secondary);
    require(actions.empty(), "An old accepted Table used a newly bound context target.");
    present();
    pointer(ui::EventKind::pointer_down, table_key, 102, 1, ui::PointerButton::secondary);
    require(actions.size() == 2 && actions.back().key == 9004 && !actions.back().target &&
                std::get<ui::ContextMenuRequest>(actions.back().value).source == table_key,
            "Accepted context retargeting lost the actual Table source identity.");
    require(surface.set_controls({}).has_value(), "Table DSL unmount failed.");
    instance->close();
    std::puts(
        "Table DSL acceptance: PASS (Source/JIT, immutable nested/repeated model, native actions, context/source identity, shrink layout, patch/rollback, retarget cancellation)");
}
