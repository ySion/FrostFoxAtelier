// Real file -> module/JIT -> repeated scopes -> Session input/layout. The clock
// argument advances only debounce time; every compile and draw is the real path.
void verify_composed_ui() {
    namespace language = ui_language;
    Runtime runtime;
    take(ui_language::register_types(runtime));
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("atelier-ui-authoring-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    const auto path = folder / "components.ui.source";
    const std::string source = R"(
ui Card(in Text title, in TextEditBinding edit) {
    state bool expanded = true;
    Column root {
        Foldout toggle {
            text: input.title;
            selected: state.expanded;
            on activate { state.expanded = event.checked; }
        }
        Column contents {
            collapsed: !state.expanded;
            TextField field { edit: input.edit; }
        }
    }
}
ui Board(in bool visible, in Items<Card> rows) {
    Column root {
        Text marker { text: "Version one"; height: 23; }
        Repeat entries { items: input.rows; mounted: input.visible; }
    }
}
)";
    take(editor::write_file(path, source));
    ui::ControlKey next_key = 10000;
    language::LiveDocument live(runtime, path, next_key);
    const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface(fonts);
    require(surface.set_surface({{640, 480}, 1, 1}).has_value(), "Composition surface failed.");
    std::vector<language::Item> rows{{1001, {std::wstring{L"A"}, language::TextEditBinding{6001, L"one"}}},
                                     {1002, {std::wstring{L"B"}, language::TextEditBinding{6002, L"two"}}}};
    bool visible = true;
    std::vector<ui::Control> controls;
    std::vector<ui::Action> actions;
    const auto route = [&](const ui::Reply& reply) {
        require(!reply.error, "Composition input failed.");
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect)) {
                take(live.event(*action));
                actions.push_back(*action);
            }
    };
    const auto inputs = [&] {
        return std::array<language::InputValue, 2>{
            visible, language::ItemList{std::make_shared<const std::vector<language::Item>>(rows)}};
    };
    const auto draw = [&] {
        controls.clear();
        const auto appended = live.append("Board", 0, {}, inputs(), controls);
        if (live.candidate_failed()) {
            live.reject_controls(std::string(live.diagnostic()));
            controls.clear();
            take(live.append("Board", 0, {}, inputs(), controls));
        } else if (!appended)
            throw std::runtime_error(appended.error().message);
        auto submitted = surface.set_controls(controls);
        if (!submitted && live.reject_controls("Session rejected candidate topology")) {
            controls.clear();
            take(live.append("Board", 0, {}, inputs(), controls));
            submitted = surface.set_controls(controls);
        }
        require(submitted.has_value(), "Composition descriptors failed.");
        route(*submitted);
        live.commit_controls();
        auto frame = surface.prepare();
        require(frame.has_value() && surface.accept(*frame), "Composition frame failed.");
    };
    auto now = language::LiveDocument::Clock::now();
    live.poll(now);
    draw();
    const auto toggle = [&](ui::ControlKey parent) {
        const auto at = std::ranges::find_if(
            controls, [&](const auto& item) { return item.parent == parent && item.kind == ui::ControlKind::foldout; });
        require(at != controls.end(), "Missing child Foldout.");
        return at->key;
    };
    const auto click = [&](ui::ControlKey key) {
        const auto* target = surface.presented()->find(key);
        require(target != nullptr, "Missing composition hit target.");
        const auto box = target->bounds;
        route(surface.input({.kind = ui::EventKind::pointer_down, .x = box.x + 8, .y = box.y + 8}));
        route(surface.input({.kind = ui::EventKind::pointer_up, .x = box.x + 8, .y = box.y + 8}));
    };
    const auto first_toggle = toggle(1001);
    const auto incarnation = surface.presented()->find(1001)->incarnation;
    click(first_toggle);
    draw();
    require(!surface.presented()->find(6001) && surface.presented()->find(6002),
            "Repeated child state leaked between instances.");
    std::reverse(rows.begin(), rows.end());
    draw();
    require(toggle(1001) == first_toggle && surface.presented()->find(1001)->incarnation == incarnation &&
                !surface.presented()->find(6001),
            "Repeat reorder changed identity or local state.");
    click(6002);
    route(surface.input({.kind = ui::EventKind::key_down, .key = ui::Key::a, .control = true}));
    route(surface.input({.kind = ui::EventKind::text, .data = std::wstring{L"draft 中文"}}));
    draw();
    require(surface.text_input().has_value(), "Typed text did not create the shared input session.");
    const auto edit_token = surface.text_input()->token;
    const auto save = [&](const std::string& text) {
        take(editor::write_file(path, text));
        now += std::chrono::milliseconds(350);
        live.poll(now);
        now += std::chrono::milliseconds(250);
        live.poll(now);
        draw();
    };
    auto updated = source;
    updated.replace(updated.find("Version one"), 11, "Version two");
    save(updated);
    require(live.revision() == 2 && live.diagnostic().empty() && surface.text_input()->token == edit_token &&
                !surface.presented()->find(6001) && toggle(1001) == first_toggle,
            "Hot reload lost compatible state, keys or edit token.");
    require(std::ranges::any_of(controls, [](const auto& item) { return item.text == L"Version two"; }),
            "Saved source did not change live controls.");
    save(updated + "\n?");
    require(live.revision() == 2 && !live.diagnostic().empty() && surface.text_input()->token == edit_token,
            "Syntax failure replaced the active document.");
    auto invalid = updated;
    invalid.replace(invalid.find("height: 23"), 10, "height: -1");
    save(invalid);
    require(live.revision() == 2 && !live.diagnostic().empty() && surface.text_input()->token == edit_token,
            "Invalid candidate properties replaced a live draft.");
    auto topology = updated;
    topology.insert(topology.find("        Repeat entries"),
                    "        Tree invalid { rows: 2; TreeItem early {} TreeItem later {} }\n");
    save(topology);
    require(live.revision() == 2 && surface.text_input()->token == edit_token,
            "Session rejection did not preserve the active component owner and input.");
    save(updated + "\n// recovered\n");
    require(live.revision() == 3 && live.diagnostic().empty(), "A corrected save failed to recover.");
    std::filesystem::remove(path);
    now += std::chrono::milliseconds(350);
    live.poll(now);
    now += std::chrono::milliseconds(250);
    live.poll(now);
    draw();
    require(live.revision() == 3 && !live.diagnostic().empty() && surface.text_input()->token == edit_token,
            "Removing the authoring file invalidated the running UI.");
    save(updated + "\n// recovered\n");
    require(live.revision() == 4 && live.diagnostic().empty(),
            "Restoring identical file content retained an obsolete IO error.");
    const auto before_rename = *live.statistics("Board");
    updated.replace(updated.find("Column root"), 11, "Column renamed_root");
    save(updated);
    const auto after_rename = *live.statistics("Board");
    require(after_rename.instance == before_rename.instance &&
                after_rename.scope_creations == before_rename.scope_creations &&
                after_rename.scope_retirements == before_rename.scope_retirements && toggle(1001) == first_toggle &&
                surface.presented()->find(1001)->incarnation == incarnation &&
                surface.text_input()->token == edit_token,
            "Repeated root rename recreated scopes, changed the host key or discarded input state.");
    route(surface.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter}));
    require(std::ranges::any_of(actions,
                                [](const auto& action) {
                                    return action.kind == ui::ActionKind::edit_commit && action.key == 6002 &&
                                           std::get<std::wstring>(action.value) == L"draft 中文";
                                }),
            "Saved code discarded the text draft before commit.");
    click(6002);
    route(surface.input({.kind = ui::EventKind::text, .data = std::wstring{L"abandoned"}}));
    draw();
    actions.clear();
    visible = false;
    draw();
    require(
        !surface.presented()->find(6002) &&
            std::ranges::any_of(actions, [](const auto& action) { return action.kind == ui::ActionKind::edit_cancel; }),
        "Conditional unmount did not settle the old text session.");
    visible = true;
    draw();
    require(surface.presented()->find(6001) && toggle(1001) != first_toggle &&
                surface.presented()->find(1001)->incarnation != incarnation,
            "Remount restored a retired child scope.");
    rows.erase(rows.begin());
    draw();
    require(!surface.presented()->find(1002), "Removed keyed row remained mounted.");
    rows.push_back({1002, {std::wstring{L"Restored"}, language::TextEditBinding{6002, L"fresh"}}});
    draw();
    require(surface.presented()->find(6002), "A keyed row could not be inserted after retirement.");
    const auto valid_controls = controls;
    rows.push_back(rows.front());
    controls.clear();
    static_cast<void>(live.append("Board", 0, {}, inputs(), controls));
    require(std::ranges::equal(controls, valid_controls), "Duplicate item identities corrupted the published view.");
    rows.pop_back();
    draw();
    auto removed = surface.set_controls({});
    require(removed.has_value(), "Composition unmount failed.");
    route(*removed);
    live.commit_controls();
    std::puts(
        "Composed UI acceptance: PASS (typed text, isolated state, keyed reorder/remount, save/recover, draft preservation)");
}
