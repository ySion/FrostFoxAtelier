int verify_dungeons_window(Application& app, const char* output_directory) try {
    using Access = atelier::application::EditorAcceptanceAccess;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "Cannot initialize dungeon captures.");
    struct Apartment {
        bool owns;
        ~Apartment() {
            if (owns)
                CoUninitialize();
        }
    } apartment{SUCCEEDED(initialized)};
    const auto output = std::filesystem::absolute(output_directory);
    std::filesystem::create_directories(output);
    const auto project = output / ("Project-" + std::to_string(GetTickCount64()));
    model(atelier::editor::Project::create(project));
    std::filesystem::create_directories(project / "Content/Dungeons");
    for (const auto* directory : {"Models", "Scenes"})
        std::filesystem::copy(std::filesystem::path{ATELIER_EXAMPLE_PROJECT} / "Content/Dungeons" / directory,
                              project / "Content/Dungeons" / directory, std::filesystem::copy_options::recursive);
    model(app.open_project(project));
    model(app.open_scene("Content/Dungeons/Scenes/Dungeon.scene"));
    model(app.configure_view({6.927793f, -.362655f, 24.410025f}, {3.6086f, -1.3809f, 33.7881f},
                             atelier::renderer::Shading::smooth));
    if (const auto& build = Access::project_build(app); build && build->error)
        throw std::runtime_error(build->error->message);
    require(entities(app).size() == 3619 && model(app.document().presentation().render_items()).size() == 3425,
            "The ordinary Dungeons project did not load the complete demo.");
    require(Access::scene_lit_view(app).shading == atelier::renderer::Shading::smooth,
            "Dungeons did not select textured rendering.");
    tick(app, 3);
    capture(app, output / "01-dungeon.png");
    const auto directory = Access::directory_key(app, "Content/Dungeons");
    require(directory && app.presented()->find(directory), "Dungeons is absent from the ordinary Content tree.");
    const auto saved = model(app.document().serialize());
    model(app.save_scene("Content/Dungeons/Scenes/Acceptance.scene"));
    model(app.open_scene("Content/Dungeons/Scenes/DungeonOverview.scene"));
    model(app.configure_view({45, 42, -48}, {0, 0, 0}, atelier::renderer::Shading::smooth));
    require(model(app.document().presentation().render_items()).size() == 626,
            "Dungeon overview lost model instances.");
    tick(app, 2);
    capture(app, output / "02-dungeon-overview.png");
    model(app.open_scene("Content/Dungeons/Scenes/GenericOverview.scene"));
    model(app.configure_view({125, 95, -130}, {0, 0, 0}, atelier::renderer::Shading::smooth));
    require(model(app.document().presentation().render_items()).size() == 380,
            "Generic overview lost model instances.");
    tick(app, 2);
    capture(app, output / "03-generic-overview.png");
    model(app.open_scene("Content/Dungeons/Scenes/Acceptance.scene"));
    require(atelier::acceptance::dungeons_roundtrip_equal(saved, model(app.document().serialize())),
            "Dungeons save/reopen changed scene data.");
    std::ofstream(output / "acceptance.txt")
        << "Dungeons window acceptance: PASS (ordinary project, Content tree, 3425/626/380 model instances, textured D3D12 captures, isolated save/reopen). No native input or GPU validation.\n";
    app.close();
    std::puts("Dungeons window acceptance: PASS");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "Dungeons window acceptance: FAIL: %s\n", error.what());
    return 1;
}
