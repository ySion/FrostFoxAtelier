void verify_project_window(Application& app,const std::filesystem::path& output,bool existing=false) {
    namespace access=atelier::application;
    namespace browser=atelier::editor::browser_control;
    using atelier::editor::Command;
    const auto folder=std::filesystem::absolute(output/L"项目");
    access::EditorAcceptanceAccess::mark_saved(app);
    model(access::EditorAcceptanceAccess::command(app,existing?Command::open_project:Command::new_project,atelier::editor::utf8(folder.wstring())));tick(app,4);
    const auto initial_count=actors(app).size();
    require((existing || initial_count==0) && !app.document().dirty(),"Project did not publish its clean initial scene.");
    verify_about(app,folder.wstring());
    click(app,control::add_static_mesh);
    const auto actor=app.document().selected();
    const auto field=property_control(app,actor,0);
    click(app,field);type(app,L"7");key(app,'S',true);tick(app,2);
    require(!app.document().dirty() && x(app,actor)==7,"Save did not commit the active field and mark the scene clean.");
    auto saved=app.document().serialize();require(saved.has_value(),"Cannot inspect the saved scene.");
    model(app.open_project(folder));tick(app,4);
    require(actors(app).size()==initial_count+1 && !app.document().dirty() && model(app.document().serialize())==*saved,
        "Opening the saved project lost scene data.");
    const auto current=model(app.document().world().members())[initial_count].id();
    click(app,app.object_control(current));
    click(app,browser::engine);
    const auto sphere=access::EditorAcceptanceAccess::asset_key(app,"builtin/sphere");
    click(app,sphere);
    require(app.document().selected()==current && actors(app).size()==initial_count+1,"Resource selection changed the scene selection or created an Actor.");
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
    const auto catalog=model(atelier::editor::Project::open(folder));
    const auto imported_entry=std::find_if(catalog.entries().begin(),catalog.entries().end(),[](const auto& entry) {
        return entry.path==L"Content/Meshes/Imported.obj" && entry.kind==atelier::editor::AssetKind::mesh;
    });
    require(imported_entry!=catalog.entries().end(),"Imported OBJ did not publish a project mesh entry.");
    click(app,access::EditorAcceptanceAccess::asset_key(app,imported_entry->key));
    click(app,browser::add);
    const auto imported_actor=app.document().selected();
    require(actors(app).size()==initial_count+2 && imported_actor!=current,"Adding the imported resource did not create a distinct Actor.");
    click(app,app.object_control(current));click(app,app.object_control(imported_actor));
    require(app.document().selected()==imported_actor,"Scene Tree did not select the imported Actor.");
    const auto import_history=app.document().undo_count();
    click(app,property_control(app,imported_actor,0));type(app,L"2.5");key(app,VK_RETURN);
    require(x(app,imported_actor)==2.5f && app.document().undo_count()==import_history+1,
        "Imported Actor parameter edit did not create one document transaction.");
    click(app,app.object_control(imported_actor));click(app,control::undo);
    require(x(app,imported_actor)==0,"Undo did not restore the imported Actor's position.");
    click(app,control::redo);require(x(app,imported_actor)==2.5f,"Redo did not restore the imported Actor's edit.");
    click(app,control::focus);
    const auto display_document=model(app.document().serialize());
    const auto display_history=app.document().undo_count();
    const auto choose=[&](ControlKey source,ControlKey option) {click(app,source);click(app,option);tick(app);};
    choose(control::scene_shading,control::shading_first+2);
    choose(control::scene_edges,control::edges_first);
    atelier::renderer::Capture shaded;
    require(app.step(&shaded) && shaded.frame_id!=0 && std::any_of(shaded.depth.begin(),shaded.depth.end(),[](float value){return value<1;}),
        "Project Actor did not reach an accepted GPU scene frame.");
    png(shaded,output/"project-matcap.png");
    choose(control::scene_edges,control::edges_first+1);
    atelier::renderer::Capture outlined;
    require(app.step(&outlined) && outlined.frame_id!=0 && outlined.extent==shaded.extent,
        "Project wireframe did not produce an accepted GPU frame.");
    const auto viewport=app.presented()->find(control::viewport)->bounds;
    std::size_t changed{};
    for (unsigned row=static_cast<unsigned>(viewport.y);row<static_cast<unsigned>(viewport.y+viewport.height);++row)
        for (unsigned column=static_cast<unsigned>(viewport.x);column<static_cast<unsigned>(viewport.x+viewport.width);++column) {
            const auto offset=(std::size_t(row)*shaded.extent.width+column)*4;
            if (std::abs(int(shaded.rgba[offset])-int(outlined.rgba[offset]))>3 ||
                std::abs(int(shaded.rgba[offset+1])-int(outlined.rgba[offset+1]))>3 ||
                std::abs(int(shaded.rgba[offset+2])-int(outlined.rgba[offset+2]))>3) ++changed;
        }
    require(changed>8 && model(app.document().serialize())==display_document && app.document().undo_count()==display_history,
        "Wireframe did not change scene pixels or modified persistent project state.");
    png(outlined,output/"project-wireframe.png");
    choose(control::scene_edges,control::edges_first);
    model(access::EditorAcceptanceAccess::command(app,Command::save_scene));tick(app,2);
    saved=app.document().serialize();require(saved.has_value(),"Cannot inspect the saved imported scene.");
    model(app.open_project(folder));tick(app,4);
    require(actors(app).size()==initial_count+2 && model(app.document().serialize())==*saved,
        "Save/reopen lost the imported mesh, Actor or parameter values.");
    capture(app,output/"project-import-reopened.png");
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
    std::filesystem::create_directories(folder/"Cache");
    model(atelier::editor::write_file(folder/"Cache/disposable.txt","Derived data only"));
    std::filesystem::remove(folder/"Cache/disposable.txt");std::filesystem::remove(folder/"Cache");
    const auto moved=std::filesystem::absolute(output/L"工程搬移");std::filesystem::rename(folder,moved);
    model(app.open_project(moved));tick(app,4);
    require(model(app.document().serialize())==*saved,"Moved project depended on its old root or Cache.");
    verify_about(app,moved.wstring());
    capture(app,output/"project-reopened.png");
}
