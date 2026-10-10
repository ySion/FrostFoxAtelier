int verify_examples_window(Application& app,const char* output_directory,bool snapshot_only) try {
    using Access=atelier::application::EditorAcceptanceAccess;
    using atelier::editor::Command;
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(initialized) || initialized==RPC_E_CHANGED_MODE,"Cannot initialize examples capture.");
    struct Apartment {bool owns;~Apartment(){if(owns) CoUninitialize();}} apartment{SUCCEEDED(initialized)};
    const auto output=std::filesystem::absolute(output_directory);std::filesystem::create_directories(output);
    if(!snapshot_only) await_acceptance_activation(app,output,"Official examples");
    require(actors(app).size()==4 && model(app.document().world().render_items()).front().geometry->vertices().size()==786801 &&
        Access::scene_lit_view(app).shading==atelier::renderer::Shading::scene_lit && Access::scene_lit_view(app).lit.ddgi,
        "Editor default startup did not load the complete Sponza Scene Lit/DDGI scene.");
    tick(app,128);capture(app,output/"01-default-sponza.png");
    for(const auto* folder:{"Examples","Examples/Sponza","Examples/Scene Lit Room"}) {
        const auto key=Access::directory_key(app,folder);
        require(key && app.presented()->find(key),"Project tree omitted an official example directory.");
    }
    require(app.presented()->find(Access::directory_key(app,"Examples/Sponza"))->selected,
        "Default startup did not navigate to Project/Examples/Sponza.");
    const auto choose=[&](Command command) {
        if(snapshot_only) {model(Access::command(app,command));tick(app,2);return;}
        click(app,atelier::editor::menu_key(atelier::editor::CommandGroup::project));
        require(app.menu_frame() && app.menu_frame()->find(control::examples_menu),"Project has no Examples submenu.");
        click(app,control::examples_menu);
        require(app.menu_frame() && app.menu_frame()->find(static_cast<ControlKey>(Command::example_sponza)) &&
            app.menu_frame()->find(static_cast<ControlKey>(Command::example_scene_lit)),"Official example entries are missing.");
        atelier::renderer::Capture menu;app.capture_menu(menu);png(menu,output/"02-examples-menu.png");
        click(app,static_cast<ControlKey>(command));tick(app,2);
        require(!app.menu_frame() && Access::scene_lit_view(app).shading==atelier::renderer::Shading::scene_lit,
            "Example selection did not close the menu and configure Scene Lit.");
    };
    choose(Command::example_scene_lit);
    require(actors(app).size()==14 && model(app.document().world().light_items()).size()==4,"Examples menu did not open Scene Lit Room.");
    tick(app,96);capture(app,output/"03-scene-lit-room.png");
    const auto shipped=atelier::editor::asset_directory()/"examples/Examples/Scene Lit Room/Scenes/Main.scene";
    const auto original=model(atelier::editor::read_file(shipped));
    const auto mesh=model(app.document().world().render_items()).front().component;
    const auto fields=model(app.document().properties(mesh));
    const auto roughness=std::ranges::find(fields,"material.roughness",&atelier::editor::Property::name);
    require(roughness!=fields.end(),"Scene Lit Room has no editable roughness.");
    model(Access::edit_field(app,mesh,roughness->path,.61f));model(app.save_scene());
    const auto saved=model(app.document().serialize());
    choose(Command::example_sponza);require(actors(app).size()==4,"Examples menu did not return to Sponza.");
    choose(Command::example_scene_lit);
    require(model(app.document().serialize())==saved && model(atelier::editor::read_file(shipped))==original,
        "Example switching lost the saved copy or modified the bundled template.");
    model(app.save_scene("Examples/Another Scene/Custom.scene"));
    choose(Command::example_sponza);model(app.open_scene("Examples/Another Scene/Custom.scene"));
    require(model(app.document().serialize())==saved,"An additional Examples scene did not save and reopen.");
    std::ofstream report(output/"acceptance.txt");
    report<<"Official examples acceptance: PASS\nDefault Sponza + Scene Lit/DDGI; visible Project/Examples/Sponza and Scene Lit Room tree; repeated switching; new example scene save/reopen; bundled template unchanged.\n"
        <<(snapshot_only?"D3D12 snapshots and application commands; no native input or GPU validation.\n":"Native Project > Examples menus with D3D12 GPU validation.\n");
    require(report.good(),"Cannot write examples acceptance report.");app.close();
    std::puts(snapshot_only?"Official examples snapshot acceptance: PASS":"Official examples window acceptance: PASS");return 0;
} catch(const std::exception& error) {std::fprintf(stderr,"Official examples window acceptance: FAIL: %s\n",error.what());return 1;}
