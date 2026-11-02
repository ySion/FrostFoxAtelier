void verify_shortcut_input_boundaries(Application& app) {
    using namespace atelier;
    namespace control = atelier::editor::control;
    using Access = application::EditorAcceptanceAccess;
    const auto native_key = [&](WPARAM code, bool up = false, bool repeat = false, bool control = false,
                                bool alt = false, bool system = false) {
        KeyStateScope modifiers(control, false, alt);
        const auto message = system ? (up ? WM_SYSKEYUP : WM_SYSKEYDOWN) : (up ? WM_KEYUP : WM_KEYDOWN);
        const LPARAM flags =
            1 | (alt ? LPARAM{1} << 29 : 0) | (up || repeat ? LPARAM{1} << 30 : 0) | (up ? LPARAM{1} << 31 : 0);
        SendMessageW(app.window(), message, code, flags);
        tick(app, 1);
    };
    const auto tap = [&](WPARAM code, bool control = false, bool alt = false, bool system = false) {
        native_key(code, false, false, control, alt, system);
        native_key(code, true, false, control, alt, system);
    };
    const auto click_control = [&](ui::ControlKey key) {
        const auto point = centre(app, key);
        Access::input(app, {.kind = ui::EventKind::pointer_down, .x = float(point.x), .y = float(point.y)});
        Access::input(app, {.kind = ui::EventKind::pointer_up, .x = float(point.x), .y = float(point.y)});
        tick(app, 1);
    };
    const auto pane_count = [&] {
        std::size_t count{};
        for (const auto& pane : Access::dock_presentation(app).panes)
            count += pane.tabs.size();
        return count;
    };
    const auto maximized = [&] {
        return std::ranges::any_of(Access::dock_presentation(app).panes,
                                   [](const auto& pane) { return pane.maximized; });
    };
    Access::mark_saved(app);
    model(Access::invoke(app, "layout.reset"));
    model(Access::invoke(app, "layout.show", {{"tab", "viewport"}}));
    tick(app);

    // Exercise the actual HWND -> owned native queue -> Application path, with
    // thread-local modifiers only. No desktop keys or focus changes are injected.
    native_key(VK_F10, false, false, false, false, true);
    require(Access::menu_open(app), "Initial native F10 did not open the File menu.");
    native_key(VK_F10, false, true, false, false, true);
    require(Access::menu_open(app), "Repeated F10 toggled the already-open menu.");
    tap(VK_ESCAPE);
    native_key(VK_F10, false, true, false, false, true);
    require(!Access::menu_open(app), "Held F10 reopened a dismissed menu before release.");
    native_key(VK_F10, true, false, false, false, true);
    tap(VK_F10, false, false, true);
    require(Access::menu_open(app), "A fresh F10 press failed after the previous held sequence.");
    tap(VK_ESCAPE);

    const auto parent = app.document().selection().entity;
    require(parent != ObjectId{}, "Shortcut parent fixture needs a selected Entity.");
    model(Access::invoke(app, "scene.add", {{"kind", "entity"}}));
    const auto child = app.document().selection().entity;
    auto& document = Access::physics_document(app);
    model(document.reparent(child, parent));
    model(Access::invoke(app, "layout.show", {{"tab", "scene_tree"}}));
    tick(app);
    click_control(control::tree_world);
    model(Access::select_entity(app, child));
    tick(app);
    const auto history = app.document().undo_count();
    tap('P', false, true, true);
    require(!Access::menu_open(app) && model(world::parent_entity(document.world().runtime(), child)) == ObjectId{} &&
                app.document().undo_count() == history + 1,
            "Scene Tree Alt+P opened Project or failed to clear the selected parent.");
    model(document.undo());
    tick(app);
    require(model(world::parent_entity(document.world().runtime(), child)) == parent,
            "Clear Parent shortcut did not form one undo step.");
    click_control(control::tree_filter);
    tap('P', false, true, true);
    require(Access::menu_open(app) && model(world::parent_entity(document.world().runtime(), child)) == parent,
            "Text focus in Scene Tree dispatched Clear Parent instead of the Project mnemonic.");
    tap(VK_ESCAPE);
    require(app.text_input() && app.text_input()->token.key == control::tree_filter,
            "Closing Project lost the prior text input binding.");
    tap('P', true, true, true);
    require(!Access::menu_open(app) && model(world::parent_entity(document.world().runtime(), child)) == parent,
            "Ctrl+Alt text chord was dispatched as a menu mnemonic or scene command.");

    model(Access::select_entity(app, child));
    model(Access::invoke(app, "layout.show", {{"tab", "parameters"}}));
    tick(app);
    const auto name_key = Access::name_control(app, child);
    const std::wstring name_before{app.document().name(child)};
    const auto name_history = app.document().undo_count();
    click_control(name_key);
    require(Access::focused(app) == name_key && app.text_input() && app.text_input()->token.key == name_key,
            "Text-repeat fixture did not focus the selected Entity's name field.");
    tap('A', true);
    {
        KeyStateScope modifiers;
        for (const auto character : std::wstring_view{L"ABCD"})
            SendMessageW(app.window(), WM_CHAR, character, 1);
        tick(app, 1);
    }
    native_key(VK_BACK);
    native_key(VK_BACK, false, true);
    native_key(VK_BACK, true);
    // Name previews live in the UI edit buffer. The document receives one
    // rename only when the real Enter path commits the completed draft.
    require(app.document().name(child) == name_before && app.document().undo_count() == name_history,
            "Name draft changed the document before its commit.");
    tap(VK_RETURN);
    const bool renamed = app.document().name(child) == L"AB" && app.document().undo_count() == name_history + 1;
    if (!renamed)
        std::fprintf(stderr, "Text-repeat commit: name='%ls', focus=%llu, field=%llu, history=%zu/%zu\n",
                     std::wstring(app.document().name(child)).c_str(),
                     static_cast<unsigned long long>(Access::focused(app)), static_cast<unsigned long long>(name_key),
                     app.document().undo_count(), name_history + 1);
    require(renamed, "One-shot shortcut filtering swallowed repeated text deletion or its single name commit.");
    tap(VK_ESCAPE);

    model(Access::invoke(app, "layout.show", {{"tab", "viewport"}}));
    tick(app);
    native_key('B', false, false, true);
    require(maximized(), "Ctrl+B did not maximize the focused pane.");
    native_key('B', false, true, true);
    require(maximized(), "Repeated Ctrl+B restored the pane before key release.");
    native_key('B', true, false, true);
    tap('B', true);
    require(!maximized(), "A fresh Ctrl+B press did not restore the pane.");
    const auto tabs = pane_count();
    require(tabs > 2, "Shortcut close fixture needs multiple open tabs.");
    native_key('W', false, false, true);
    require(pane_count() == tabs - 1, "Ctrl+W did not close exactly one pane tab.");
    native_key('W', false, true, true);
    require(pane_count() == tabs - 1, "Held Ctrl+W closed additional tabs.");
    native_key('W', true, false, true);
    tap('W', true);
    require(pane_count() == tabs - 2, "Fresh Ctrl+W failed after the preceding held sequence.");
    model(Access::invoke(app, "layout.reset"));
    model(Access::invoke(app, "layout.show", {{"tab", "viewport"}}));
    tick(app);

    const auto view = app.presented()->find(control::viewport)->bounds;
    const math::Vector2 origin{view.x + view.width * .5f, view.y + view.height * .5f};
    Access::input(app, {.kind = ui::EventKind::pointer_move, .x = origin.x, .y = origin.y});
    Access::input(
        app, {.kind = ui::EventKind::pointer_down, .x = origin.x, .y = origin.y, .button = ui::PointerButton::middle});
    require(GetCapture() == app.window(), "Camera shortcut fixture did not acquire capture.");
    Access::input(app, {.kind = ui::EventKind::pointer_move, .x = origin.x + 20, .y = origin.y + 10});
    tap('B', true);
    require(GetCapture() != app.window(), "Global pane command retained the old camera capture.");
    const auto camera = Access::camera_view(app);
    const auto updated = app.presented()->find(control::viewport)->bounds;
    const math::Vector2 hover{updated.x + updated.width * .5f, updated.y + updated.height * .5f};
    Access::input(app, {.kind = ui::EventKind::pointer_move, .x = hover.x, .y = hover.y});
    require(Access::camera_view(app) == camera,
            "Global command released capture but left camera drag responding to hover.");
    Access::input(app,
                  {.kind = ui::EventKind::pointer_up, .x = hover.x, .y = hover.y, .button = ui::PointerButton::middle});
    require(Access::camera_view(app) == camera && GetCapture() != app.window(),
            "Late camera release changed the completed command state.");
    model(Access::invoke(app, "layout.reset"));
    tick(app);
    std::puts("Native shortcut boundaries: PASS (context precedence, repeat, text, camera retirement)");
}
