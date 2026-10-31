// The production ProjectUI transaction, using real style/layout and isolated
// runners. Include inside project_execution.cpp's acceptance namespace.
void verify_project_ui_reload(const std::filesystem::path& evidence) {
    const auto root = evidence / "ui-reload";
    auto project = take(editor::Project::create(root));
    const auto path = root / "Source/Main.ui.source";
    const auto source = [](unsigned height, std::string_view extra = std::string_view{}) {
        return std::string{R"(ui ProjectUI(in Text title, in double elapsed, in double entities) {
    state bool checked = false;
    Column root {
        ToggleButton toggle {
            text: "Retained toggle"; selected: state.checked;
            width: 180; height: )"} +
               std::to_string(height) + R"(;
            on activate { state.checked = event.checked; }
        }
)" + std::string(extra) +
               "    }\n}\n";
    };
    const auto configure = [](ui::Session& session) {
        auto style = session.style();
        style.font.family = L"Sarasa UI SC";
        style.font.size = 12.5f;
        style.caption_scale = 1;
        display(session.set_style(style));
        display(session.set_style_sheet(application::editor_styles()));
    };
    take(editor::write_file(path, source(23)));
    take(project.set_ui_source("Source/Main.ui.source"));
    editor::ProjectUI project_ui;
    project_ui.sync(&project);
    ui::Session session(application::editor_fonts());
    configure(session);
    const auto effects = [&](const ui::Reply& reply) {
        require(!reply.error, "Project UI reload input failed.");
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect))
                take(project_ui.event(*action));
    };
    ui::Surface surface{{640, 480}, 1, 1};
    const auto draw = [&] {
        auto frame = take(project_ui.prepare(session, surface, 1, 0, effects));
        require(frame && session.accept(frame), "Project UI reload could not accept its restored frame.");
        return frame;
    };
    auto frame = draw();
    const auto toggle = std::ranges::find_if(
        frame->controls, [](const auto& control) { return control.button_behavior == ui::ButtonBehavior::toggle; });
    require(toggle != frame->controls.end(), "Project UI reload has no toggle.");
    const auto key = toggle->key;
    const auto click = [&] {
        const auto* control = session.presented()->find(key);
        require(control, "Project UI reload lost its toggle key.");
        const auto bounds = control->bounds;
        effects(session.input({.kind = ui::EventKind::pointer_down, .x = bounds.x + 8, .y = bounds.y + 8}));
        effects(session.input({.kind = ui::EventKind::pointer_up, .x = bounds.x + 8, .y = bounds.y + 8}));
    };
    click();
    frame = draw();
    require(frame->find(key)->selected, "Project UI reload did not retain the initial event.");
    const auto revision = project_ui.revision();
    const auto wait_frame = [&](auto condition, const char* message) {
        const auto deadline = GetTickCount64() + 15000;
        do {
            frame = draw();
            if (condition())
                return;
            Sleep(10);
        } while (GetTickCount64() < deadline);
        throw std::runtime_error(message);
    };
    take(editor::write_file(path, source(1)));
    wait_frame([&] { return project_ui.diagnostic().find("layout") != std::string_view::npos; },
               "Bad UI layout was not rejected.");
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        frame = draw();
        const auto* retained = frame->find(key);
        require(retained && retained->selected && retained->bounds.height == 23 && project_ui.revision() == revision,
                "Rejected layout changed the accepted UI definition, state, or geometry.");
    }
    take(editor::write_file(path, source(23, "        Tree tree { rows: 2; TreeItem early {} TreeItem later {} }\n")));
    wait_frame([&] { return project_ui.diagnostic().find("controls") != std::string_view::npos; },
               "Bad UI topology was not rejected.");
    require(frame->find(key) && frame->find(key)->selected && project_ui.revision() == revision,
            "Rejected topology removed the accepted UI state.");
    click();
    frame = draw();
    require(!frame->find(key)->selected, "Rejected source disabled the accepted event handler.");
    take(editor::write_file(path, source(29)));
    wait_frame([&] { return project_ui.revision() == revision + 1; },
               "Fixed UI source did not publish after rejection.");
    require(project_ui.diagnostic().empty() && frame->find(key) && !frame->find(key)->selected &&
                frame->find(key)->bounds.height == 29,
            "Fixed UI source lost retained state or layout.");

    surface = {{}, 1, 2};
    take(editor::write_file(path, source(1)));
    require(!take(project_ui.prepare(session, surface, 1, 0, effects)) && project_ui.revision() == revision + 1,
            "A suspended surface published unvalidated UI.");
    surface = {{640, 480}, 1, 3};
    wait_frame([&] { return project_ui.diagnostic().find("layout") != std::string_view::npos; },
               "Restoring the surface did not reject its bad pending UI.");
    require(frame->find(key) && frame->find(key)->bounds.height == 29 && project_ui.revision() == revision + 1,
            "Surface restoration lost the last valid layout.");
    editor::ProjectUI cold;
    cold.sync(&project);
    ui::Session cold_session(application::editor_fonts());
    configure(cold_session);
    require(!cold.prepare(cold_session, {{640, 480}, 1, 1}, 0, 0, [](const ui::Reply&) {}) && cold.revision() == 0,
            "An invalid cold UI was treated as an accepted empty page.");

    for (const bool preview : {false, true}) {
        take(editor::write_file(path, source(23)));
        application::PlayProcess process;
        take(process.start(root, {}, true, 0, {}, preview));
        RunLog output(process);
        output.await("runner.ui_published",
                     [](const auto& record) { return RunLog::integer(record, "ui_revision", 1); });
        take(editor::write_file(path, source(1)));
        output.await("runner.ui_failed", [](const auto& record) {
            return record.entry.message.find("layout failed") != std::string::npos;
        });
        require(process.running(), "Bad UI hot reload terminated the run.");
        take(editor::write_file(path, source(31)));
        output.await("runner.ui_published",
                     [](const auto& record) { return RunLog::integer(record, "ui_revision", 2); });
        require(process.running(), "Repaired UI did not keep the run alive.");
        process.stop();
        wait(process, std::chrono::seconds(5));
        if (preview)
            output.finish(true, {{"runner", "runner.ui_failed", logging::Severity::error, 1}});
        else
            output.finish(true, {{"runner", "runner.ui_failed", logging::Severity::error, 1},
                                 {"runner", "runner.camera_unavailable", logging::Severity::warning, 1}});
        require(process.exit_code() == 0 && !process.forced(), "UI reload left the run unable to stop normally.");
    }
    std::puts(
        "Project UI reload: PASS (layout/topology rejection, state, suspended surface, cold failure, game and preview recovery)");
}
