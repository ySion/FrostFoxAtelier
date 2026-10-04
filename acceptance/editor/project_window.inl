void verify_project_window(Application& app,const std::filesystem::path& output) {
    namespace access=atelier::application;
    namespace browser=atelier::editor::browser_control;
    using atelier::editor::Command;
    const auto folder=std::filesystem::absolute(output/L"项目");
    access::EditorAcceptanceAccess::mark_saved(app);
    model(access::EditorAcceptanceAccess::command(app,Command::new_project,atelier::editor::utf8(folder.wstring())));tick(app,4);
    require(actors(app).empty() && !app.document().dirty(),"New project did not publish a clean empty scene.");
    verify_about(app,folder.wstring());
    click(app,control::add_static_mesh);
    const auto actor=app.document().selected();
    const auto field=property_control(app,actor,0);
    click(app,field);type(app,L"7");key(app,'S',true);tick(app,2);
    require(!app.document().dirty() && x(app,actor)==7,"Save did not commit the active field and mark the scene clean.");
    auto saved=app.document().serialize();require(saved.has_value(),"Cannot inspect the saved scene.");
    model(app.open_project(folder));tick(app,4);
    require(actors(app).size()==1 && !app.document().dirty() && model(app.document().serialize())==*saved,
        "Opening the saved project lost scene data.");
    const auto current=model(app.document().world().members()).front().id();
    click(app,app.object_control(current));
    click(app,browser::engine);
    const auto sphere=access::EditorAcceptanceAccess::asset_key(app,"builtin/sphere");
    click(app,sphere);
    require(app.document().selected()==current && actors(app).size()==1,"Resource selection changed the scene selection or created an Actor.");
    for (const auto view:{browser::tiles,browser::list,browser::details}) {
        click(app,view);require(app.presented()->find(sphere)->selected,"Switching asset views lost resource selection.");
    }
    click(app,browser::search);type(app,L"no such asset");key(app,VK_RETURN);
    require(!app.presented()->find(sphere),"Asset search retained an unmatched resource.");
    click(app,browser::clear);require(app.presented()->find(sphere),"Clearing asset search did not restore the catalog.");
    click(app,browser::filter);key(app,VK_HOME);key(app,VK_DOWN);key(app,VK_DOWN);key(app,VK_RETURN);
    require(!app.presented()->find(sphere),"Scene type filter retained mesh resources.");
    click(app,browser::clear);
    capture(app,output/"project-assets.png");
    const auto external=std::filesystem::absolute(output/"Imported.obj");
    model(atelier::editor::write_file(external,"v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n"));
    model(access::EditorAcceptanceAccess::command(app,Command::import_mesh,atelier::editor::utf8(external.wstring())));tick(app,3);
    require(app.presented()->find(browser::project)->selected,"Import did not reveal the project's mesh folder.");
    capture(app,output/"project-directory.png");
    model(access::EditorAcceptanceAccess::command(app,Command::save_scene));tick(app,2);
    model(atelier::editor::write_file(folder/"Content/Scenes/Broken.scene","{broken"));
    require(!app.open_scene("Content/Scenes/Broken.scene") && model(app.document().serialize())==*saved,
        "Failed scene open replaced the current document.");
    const auto metadata=folder/"Content/Meshes/Imported.obj.asset.json";
    const auto metadata_text=model(atelier::editor::read_file(metadata));
    model(atelier::editor::write_file(metadata,"{broken"));
    model(access::EditorAcceptanceAccess::command(app,Command::create_actor));
    saved=app.document().serialize();require(saved.has_value(),"Cannot inspect the modified scene.");
    model(access::EditorAcceptanceAccess::command(app,Command::save_scene));
    require(!app.document().dirty() && app.status_text().starts_with(L"Scene saved; asset refresh failed:") &&
        model(atelier::editor::read_file(folder/"Content/Scenes/Main.scene"))==*saved,
        "A failed catalog refresh misreported or undid the successful scene save.");
    model(atelier::editor::write_file(metadata,metadata_text));
    model(atelier::editor::write_file(folder/"Cache/disposable.txt","Derived data only"));
    std::filesystem::remove(folder/"Cache/disposable.txt");std::filesystem::remove(folder/"Cache");
    const auto moved=std::filesystem::absolute(output/L"工程搬移");std::filesystem::rename(folder,moved);
    model(app.open_project(moved));tick(app,4);
    require(model(app.document().serialize())==*saved,"Moved project depended on its old root or Cache.");
    capture(app,output/"project-reopened.png");
}
