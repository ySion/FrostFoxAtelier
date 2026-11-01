int verify_editor_dialogs(Application& app, const char* output_directory) {
    using namespace atelier;
    namespace control = atelier::editor::control;
    using Access = application::EditorAcceptanceAccess;
    const auto output = std::filesystem::absolute(assets::wide(output_directory));
    std::filesystem::create_directories(output);
    tick(app);
    require(bool(Access::invoke(app, "scene.add", {{"kind", "mesh"}})), "Dialog scene creation failed.");
    Access::mark_saved(app);
    tick(app);
    const auto owner = app.window();
    const auto thread = GetWindowThreadProcessId(owner, nullptr);
    std::ofstream report(output / "dialogs.txt");
    const ExpectedNativeDialogs expected;
    for (const std::string_view command : {"app.about", "source.open", "project.open", "app.quit"}) {
        if (command == "app.quit") {
            require(bool(Access::invoke(app, "scene.add", {{"kind", "entity"}})),
                    "Unsaved dialog scene creation failed.");
            tick(app);
        }
        const auto history = app.document().undo_count();
        const auto entity_count = entities(app).size();
        const auto pointer = [&](ui::EventKind kind, POINT point) {
            Access::input(app, {.kind = kind,
                                .x = static_cast<float>(point.x),
                                .y = static_cast<float>(point.y),
                                .button = ui::PointerButton::primary});
        };
        const auto definition =
            std::ranges::find_if(editor::menu_bindings, [&](const auto& row) { return row.call.id == command; });
        const auto menu = centre(app, editor::menu_key(definition->group));
        pointer(ui::EventKind::pointer_move, menu);
        pointer(ui::EventKind::pointer_down, menu);
        pointer(ui::EventKind::pointer_up, menu);
        tick(app, 1);
        require(app.menu_frame() != nullptr, "Native dialog menu did not open.");
        const auto* item = app.menu_frame()->find(Access::operator_item(app, std::string(command)));
        require(item != nullptr, "Native dialog menu item is absent.");
        POINT point{static_cast<LONG>(item->bounds.x + item->bounds.width * .5f),
                    static_cast<LONG>(item->bounds.y + item->bounds.height * .5f)};
        MapWindowPoints(app.menu_window(), owner, &point, 1);
        bool found{};
        bool responsive{};
        bool closed{};
        bool captured{};
        std::jthread probe([&](std::stop_token stop) {
            struct Lookup {
                HWND owner{};

                HWND dialog{};
            } lookup{owner};
            const auto deadline = GetTickCount64() + 10000;
            while (!stop.stop_requested() && GetTickCount64() < deadline && !lookup.dialog) {
                EnumThreadWindows(
                    thread,
                    [](HWND window, LPARAM state) -> BOOL {
                        auto& lookup = *reinterpret_cast<Lookup*>(state);
                        wchar_t name[32]{};
                        GetClassNameW(window, name, 32);
                        if (GetWindow(window, GW_OWNER) == lookup.owner && IsWindowVisible(window) &&
                            std::wstring_view(name) == L"#32770") {
                            lookup.dialog = window;
                            return FALSE;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&lookup));
                if (!lookup.dialog)
                    Sleep(10);
            }
            found = lookup.dialog != nullptr;
            if (found) {
                responsive = true;
                for (unsigned sample = 0; sample < 3; ++sample) {
                    Sleep(100);
                    GUITHREADINFO state{};
                    state.cbSize = sizeof(state);
                    const bool inspected = GetGUIThreadInfo(thread, &state) != FALSE;
                    responsive = responsive && inspected;
                    if (inspected && state.hwndCapture == owner)
                        captured = true;
                    InvalidateRect(owner, nullptr, FALSE);
                    DWORD_PTR ignored{};
                    for (const auto window : {owner, lookup.dialog})
                        responsive = SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000,
                                                         &ignored) != 0 &&
                                     responsive;
                }
                PostMessageW(lookup.dialog, WM_CLOSE, 0, 0);
                const auto close_deadline = GetTickCount64() + 5000;
                while (IsWindow(lookup.dialog) && GetTickCount64() < close_deadline && !stop.stop_requested())
                    Sleep(10);
                closed = !IsWindow(lookup.dialog);
            }
            if (!stop.stop_requested() && (!found || !responsive || !closed)) {
                std::fprintf(stderr,
                             "Editor native dialogs: FAIL command=%s found=%d responsive=%d closed=%d captured=%d\n",
                             command.data(), found, responsive, closed, captured);
                std::fflush(stderr);
                // A deadlocked UI cannot unwind; keep this E2E failure bounded.
                TerminateProcess(GetCurrentProcess(), 1);
            }
        });
        std::printf("Opening native dialog command %s\n", command.data());
        std::fflush(stdout);
        pointer(ui::EventKind::pointer_move, point);
        pointer(ui::EventKind::pointer_down, point);
        pointer(ui::EventKind::pointer_up, point);
        if (command == "app.quit")
            tick(app, 1); // Exit posts WM_CLOSE; cancelling its unsaved prompt keeps the editor alive.
        probe.request_stop();
        probe.join();
        require(found && responsive && closed, "Native dialog did not respond and close.");
        require(!captured, "Native dialog left mouse capture on its disabled editor owner.");
        require(IsWindowEnabled(owner) != FALSE, "Native dialog left its owner disabled.");
        tick(app);
        require(app.document().undo_count() == history && entities(app).size() == entity_count,
                "Cancelling a native dialog modified the scene or its undo history.");
        require(!app.menu_window() && GetCapture() != owner, "Native dialog dismissal retained menu input ownership.");
        report << command << ": responsive, cancelled, editor resumed\n";
        report.flush();
    }
    report.close();
    Access::mark_saved(app);
    app.close();
    std::puts("Editor native dialogs acceptance: PASS");
    std::fflush(stdout);
    return 0;
}
