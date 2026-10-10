int verify_dungeons_window(Application& app,const char* output_directory) try {
    using Access=atelier::application::EditorAcceptanceAccess;
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(initialized)||initialized==RPC_E_CHANGED_MODE,"Cannot initialize dungeon captures.");
    struct Apartment {bool owns;~Apartment(){if(owns) CoUninitialize();}} apartment{SUCCEEDED(initialized)};
    const auto output=std::filesystem::absolute(output_directory);std::filesystem::create_directories(output);
    model(app.open_example(atelier::editor::Command::example_dungeons));
    if(const auto& build=Access::project_build(app);build && build->error) throw std::runtime_error(build->error->message);
    require(actors(app).size()==3617 && model(app.document().world().render_items()).size()==3425,
        "The official Dungeons entry did not load the complete demo.");
    require(Access::scene_lit_view(app).shading==atelier::renderer::Shading::smooth,"Dungeons did not select textured rendering.");
    tick(app,3);capture(app,output/"01-dungeon.png");
    const auto directory=Access::directory_key(app,"Examples/Dungeons");
    require(directory && app.presented()->find(directory) && app.presented()->find(directory)->selected,
        "Dungeons is absent from the official Project/Examples tree.");
    const auto saved=model(app.document().serialize());
    model(app.save_scene("Examples/Dungeons/Scenes/Acceptance.scene"));
    model(app.open_scene("Examples/Dungeons/Scenes/DungeonOverview.scene"));
    model(app.configure_view({45,42,-48},{0,0,0},atelier::renderer::Shading::smooth));
    require(model(app.document().world().render_items()).size()==626,"Dungeon overview lost model instances.");
    tick(app,2);capture(app,output/"02-dungeon-overview.png");
    model(app.open_scene("Examples/Dungeons/Scenes/GenericOverview.scene"));
    model(app.configure_view({125,95,-130},{0,0,0},atelier::renderer::Shading::smooth));
    require(model(app.document().world().render_items()).size()==380,"Generic overview lost model instances.");
    tick(app,2);capture(app,output/"03-generic-overview.png");
    model(app.open_scene("Examples/Dungeons/Scenes/Acceptance.scene"));
    require(atelier::acceptance::dungeons_roundtrip_equal(saved,model(app.document().serialize())),"Dungeons save/reopen changed scene data.");
    std::ofstream(output/"acceptance.txt")<<"Dungeons window acceptance: PASS (official entry, Project/Examples tree, 3425/626/380 model instances, textured D3D12 captures, isolated save/reopen). No native input or GPU validation.\n";
    app.close();std::puts("Dungeons window acceptance: PASS");return 0;
} catch(const std::exception& error) {std::fprintf(stderr,"Dungeons window acceptance: FAIL: %s\n",error.what());return 1;}
