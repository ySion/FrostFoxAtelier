void verify_project_authoring_window(Application& app, const std::filesystem::path& output) {
    using Access = atelier::application::EditorAcceptanceAccess;
    namespace source = atelier::editor::source_control;
    const auto folder = output / (L"Project-" + std::to_wstring(GetTickCount64()));
    Access::mark_saved(app);
    model(app.open_project(folder, true));
    tick(app, 2);
    model(app.create_source("Source/Move.source"));
    const auto helper = folder / "Source/Math.source";
    const std::string math = "float travel(float speed,float dt) { return speed * dt; }";
    model(atelier::editor::write_file(helper, math));
    model(atelier::editor::write_file(folder / "Source/Move.source", R"(class project.Move : world.Component {
    float speed = 1.0f;
    bool moving = true;
    void tick(float dt) {
        if (self.moving) { self.translate_owner(travel(self.speed,dt),0.0f,0.0f); }
    }
}
)"));
    model(app.build_project(true));
    tick(app, 2);
    click(app, operator_item(app, "scene.add", {{"kind", "mesh"}}));
    const auto entity = app.document().selection().entity;
    click(app, operator_item(app, "layout.show", {{"tab", "parameters"}}));
    require(!app.presented()->find(control::add_component), "Parameters still exposes component creation.");
    click(app, operator_item(app, "layout.show", {{"tab", "structure"}}));
    click(app, control::add_component);
    const auto choice = Access::menu_item(app, L"project.Move");
    require(choice, "Project component was absent from Add Component.");
    click(app, choice);
    auto& runtime = app.document().world().runtime();
    const auto component_type = model(runtime.find_type("project.Move"));
    atelier::ObjectId component;
    for (const auto& item : model(atelier::world::components(runtime, entity)))
        if (model(runtime.type_of(item.id())) == component_type)
            component = item.id();
    require(component != atelier::ObjectId{}, "Native component menu did not create the Source type.");
    const std::array speed{model(runtime.find_field(component_type, "speed"))};
    require(app.document().selection() == atelier::editor::Selection{entity, component} &&
                app.presented()->find(app.field_control(component, speed)) &&
                !app.presented()->find(control::position_fold),
            "Source component inspection lost Entity context or included unrelated spatial properties.");
    const auto component_row = Access::structure_control(app, component);
    require(component_row, "Source component has no Entity Structure row.");
    click(app, component_row);
    key(app, VK_DELETE);
    require(contains(app, entity) && app.document().selection() == atelier::editor::Selection{entity, entity} &&
                !Access::structure_control(app, component),
            "Deleting a Source component deleted its Entity or kept a stale row.");
    key(app, 'Z', true);
    require(app.document().selection() == atelier::editor::Selection{entity, component} &&
                Access::structure_control(app, component),
            "Source component Undo changed object identity or failed to restore inspection.");
    const auto entity_row = Access::structure_control(app, entity);
    require(entity_row, "Source Entity has no structure row.");
    click(app, entity_row);
    require(app.document().selection() == atelier::editor::Selection{entity, entity} &&
                app.presented()->find(property_control(app, entity, 0)) &&
                app.presented()->find(app.field_control(component, speed)),
            "Entity Parameters did not expose root and Source properties together in its flat list.");
    click(app, Access::structure_control(app, component));
    const auto history = app.document().undo_count();
    click(app, app.field_control(component, speed));
    type(app, L"3.5");
    key(app, VK_RETURN);
    require(std::get<float>(model(app.document().read_property(component, speed))) == 3.5f &&
                app.document().undo_count() == history + 1,
            "Source number editor did not commit one typed scene transaction.");
    click(app, operator_item(app, "scene.save"));
    const auto saved = model(atelier::assets::read_file(folder / "Content/Scenes/Main.scene"));
    const auto saved_history = app.document().undo_count();
    capture(app, output / "project-components.png");

    const auto wait = [&](auto predicate, const char* message) {
        const auto deadline = GetTickCount64() + 15000;
        do {
            tick(app, 1);
            if (predicate())
                return;
        } while (GetTickCount64() < deadline);
        throw std::runtime_error(message);
    };
    model(app.open_source(folder / "Source/Move.source"));
    tick(app, 2);
    model(atelier::editor::write_file(helper, "float travel(float speed,float dt) { return missing; }"));
    wait([&] { return Access::project_build(app) && Access::project_build(app)->error.has_value(); },
         "Saved project error did not reach Code.");
    click(app, source::problem);
    require(app.presented()->find(source::buffer)->code->text().find(L"missing") != std::wstring::npos,
            "Cross-file diagnostic did not open Math.source.");
    capture(app, output / "project-diagnostic.png");
    click(app, source::buffer);
    key(app, 'A', true);
    {
        KeyStateScope modifiers;
        for (const auto c : atelier::platform::utf8_to_utf16_validated(math))
            SendMessageW(app.window(), WM_CHAR, c, 0);
        tick(app, 2);
    }
    key(app, 'S', true);
    wait([&] { return Access::project_build(app) && !Access::project_build(app)->error; },
         "Native project Ctrl+S did not rebuild the corrected source.");
    require(model(atelier::assets::read_file(helper)) == math, "Code save did not update the project source file.");
    model(app.create_source("Source/UI/Main.ui.source", true));
    model(app.build_project());
    tick(app, 2);
    key(app, VK_F8);
    wait([&] { return Access::output_contains(app, L"Project started:"); },
         "Native Run did not start the project process.");
    wait(
        [&] {
            return Access::output_contains(app, L"Main camera:") &&
                   Access::output_contains(app, L"Project UI published:");
        },
        "A camera-less Play did not diagnose its missing camera while publishing the project UI.");
    const auto until = GetTickCount64() + 400;
    while (GetTickCount64() < until)
        tick(app, 1);
    require(Access::project_running(app), "The native game window failed after starting.");
    app.stop_project();
    wait([&] { return !Access::project_running(app); }, "Native project Stop did not finish.");
    require(!app.document().dirty() && app.document().undo_count() == saved_history &&
                model(atelier::assets::read_file(folder / "Content/Scenes/Main.scene")) == saved,
            "Play modified the authored scene or history.");
    model(app.preview_project_ui());
    const auto preview_until = GetTickCount64() + 1200;
    while (GetTickCount64() < preview_until)
        tick(app, 1);
    require(Access::project_running(app), "The real project UI preview did not stay running.");
    app.stop_project();
    wait([&] { return !Access::project_running(app); }, "Project UI preview did not stop.");

    {
        auto replacement = atelier::control::Json::parse(saved);
        replacement.at("entities").front()["name"] = "Restart replacement";
        model(atelier::editor::write_file(folder / "Content/Scenes/RestartTarget.scene", replacement.dump(2)));
        const auto launches = [&] {
            return std::ranges::count_if(Access::output(app).records(), [](const auto& item) {
                return item.record.entry.event_code == "play.launched";
            });
        };
        const auto starts = [&] {
            return std::ranges::count_if(Access::output(app).records(), [](const auto& item) {
                return item.record.entry.event_code == "runner.started";
            });
        };
        std::array<bool, 3> passed{};
        for (std::size_t scenario = 0; scenario < passed.size(); ++scenario) {
            const auto before_start = starts();
            model(app.run_project());
            wait([&] { return starts() == before_start + 1 && Access::project_running(app); },
                 "Restart fixture did not start its real Play process.");
            const auto before = launches();
            // Use the operator dispatcher without an intervening app
            // step: Stop/open must be able to cancel a queued restart before poll.
            model(Access::invoke(app, "play.restart"));
            if (scenario == 1) {
                app.stop_project();
                app.stop_project();
            }
            if (scenario == 2)
                model(app.open_scene("Content/Scenes/RestartTarget.scene"));
            if (scenario == 0)
                wait([&] { return starts() == before_start + 2 && Access::project_running(app); },
                     "Ordinary Restart did not start its replacement process.");
            else
                wait([&] { return !Access::project_running(app) || launches() > before; },
                     "Cancelled Restart did not stop its original process.");
            const auto settled = GetTickCount64() + 400;
            while (GetTickCount64() < settled)
                tick(app, 1);
            passed[scenario] = scenario == 0 ? launches() == before + 1 && Access::project_running(app)
                                             : launches() == before && !Access::project_running(app);
            if (scenario == 2)
                passed[scenario] =
                    passed[scenario] && app.document().name(entities(app).front().id()) == L"Restart replacement";
            std::printf("Restart %s: %s; additional launches=%lld, running=%d\n",
                        scenario == 0   ? "ordinary"
                        : scenario == 1 ? "then explicit Stop"
                                        : "then scene switch",
                        passed[scenario] ? "PASS" : "FAIL", static_cast<long long>(launches() - before),
                        int(Access::project_running(app)));
            std::fflush(stdout);
            app.stop_project();
            wait([&] { return !Access::project_running(app); },
                 "Restart fixture could not clean up its real Play process.");
        }
        require(passed[0] && passed[1] && passed[2],
                "Restart intent escaped explicit Stop/document replacement, or ordinary Restart launched incorrectly.");
    }
    model(app.open_project(folder));
    tick(app, 2);
    const auto reopened = model(app.document().world().members()).front().id();
    const auto reopened_type = model(app.document().world().runtime().find_type("project.Move"));
    bool retained{};
    for (const auto& item : model(atelier::world::components(app.document().world().runtime(), reopened)))
        if (model(app.document().world().runtime().type_of(item.id())) == reopened_type) {
            const std::array field{model(app.document().world().runtime().find_field(reopened_type, "speed"))};
            retained = std::get<float>(model(app.document().read_property(item.id(), field))) == 3.5f;
        }
    require(retained, "Reopening lost the Source component or its authored property.");
    capture(app, output / "project-reopened.png");

    Access::mark_saved(app);
    std::puts("Editor project authoring window acceptance: PASS");
}
