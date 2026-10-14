void verify_project_window(Application& app,const std::filesystem::path& output,bool existing=false) {
    namespace access=atelier::application;
    namespace browser=atelier::editor::browser_control;
    using atelier::editor::Command;
    const auto has_resource=[&](ControlKey key) {return app.presented()->find(key) || row_table(app,key);};
    const auto selected_resource=[&](ControlKey key) {
        if (const auto* control=app.presented()->find(key)) return control->selected;
        const auto* table=row_table(app,key);return table && table->selection.row==key;
    };
    const auto folder=std::filesystem::absolute(output/L"项目");
    access::EditorAcceptanceAccess::mark_saved(app);
    model(access::EditorAcceptanceAccess::command(app,existing?Command::open_project:Command::new_project,atelier::editor::utf8(folder.wstring())));tick(app,4);
    const auto initial_count=entities(app).size();
    require((existing || initial_count==0) && !app.document().dirty(),"Project did not publish its clean initial scene.");
    verify_about(app,folder.wstring());
    click(app,control::add_static_mesh);
    const auto entity=app.document().selection().entity;
    const auto field=property_control(app,entity,0);
    click(app,field);type(app,L"7");key(app,'S',true);tick(app,2);
    require(!app.document().dirty() && x(app,entity)==7,"Save did not commit the active field and mark the scene clean.");
    auto saved=app.document().serialize();require(saved.has_value(),"Cannot inspect the saved scene.");
    model(app.open_project(folder));tick(app,4);
    require(entities(app).size()==initial_count+1 && !app.document().dirty() && model(app.document().serialize())==*saved,
        "Opening the saved project lost scene data.");
    const auto current=model(app.document().world().members())[initial_count].id();
    click(app,app.object_control(current));
    require(app.presented()->find(browser::project)->kind==atelier::ui::ControlKind::tree_item &&
        app.presented()->find(browser::engine)->parent==browser::directories &&
        app.presented()->find(browser::project)->parent==browser::directories &&
        app.presented()->find(browser::path)->kind==atelier::ui::ControlKind::combo,
        "Asset sources and interactive path did not use the common directory tree.");
    const auto action_enabled=[&](ControlKey command) {
        click(app,browser::actions);const auto* item=app.menu_frame()?app.menu_frame()->find(command):nullptr;
        require(item,"The resource actions menu omitted a real command.");const bool enabled=item->enabled;key(app,VK_ESCAPE);return enabled;
    };
    click(app,browser::project);key(app,VK_END);
    require(app.presented()->find(browser::engine)->selected,"Directory tree End did not select Engine.");
    key(app,VK_HOME);require(app.presented()->find(browser::project)->selected,"Directory tree Home did not select Project.");
    key(app,VK_RIGHT);require(!app.presented()->find(browser::project)->selected,"Directory tree Right did not enter a Project directory.");
    key(app,VK_LEFT);require(app.presented()->find(browser::project)->selected,"Directory tree Left did not return to Project.");
    key(app,VK_END);
    const auto sphere=access::EditorAcceptanceAccess::asset_key(app,"builtin/sphere");
    click(app,sphere);
    require(app.document().selection().entity==current && entities(app).size()==initial_count+1,"Resource selection changed the scene selection or created an Entity.");
    for (const auto view:{browser::tiles,browser::list,browser::details}) {
        click(app,view);require(selected_resource(sphere),"Switching asset views lost resource selection.");
        require(!app.presented()->find(view),"A view choice remained as a duplicate toolbar button.");
        click(app,browser::view_menu);
        require(app.menu_frame() && app.menu_frame()->find(view)->selected,"The view menu did not expose its selected mode.");
        key(app,VK_ESCAPE);
    }
    click(app,browser::view_menu);key(app,VK_HOME);key(app,VK_DOWN);key(app,VK_RETURN);
    require(selected_resource(sphere),"Keyboard view-menu selection lost the resource selection.");
    click(app,sphere);
    const auto table_data=app.presented()->find_table(browser::results)->data;
    const auto resource_history=app.document().undo_count();
    key(app,VK_RIGHT);
    require(app.presented()->find_table(browser::results)->selection==atelier::ui::TableSelection{sphere,browser::type_column} &&
        app.presented()->find_table(browser::results)->data==table_data,"Moving the current resource cell rebuilt its rows or changed selection.");
    key(app,VK_RETURN);double_click(app,sphere);
    require(entities(app).size()==initial_count+1 && app.document().selection().entity==current && app.document().undo_count()==resource_history,
        "Enter or double-click on a mesh created an Entity or changed the scene.");
    click_at(app,table_header_centre(app,browser::results,browser::name_column));
    require(app.presented()->find_table(browser::results)->data->sort==atelier::ui::TableSort{browser::name_column,atelier::ui::TableSortDirection::descending} &&
        selected_resource(sphere),"A native header click did not change ordering while preserving resource selection.");
    click_at(app,table_header_centre(app,browser::results,browser::name_column));
    click(app,browser::search);type(app,L"Sphere");key(app,VK_RETURN);
    require(selected_resource(sphere) && action_enabled(browser::add),
        "A matching search cleared the visible resource selection.");
    click(app,browser::search);type(app,L"no such asset");key(app,VK_RETURN);
    require(!has_resource(sphere) && !action_enabled(browser::add),
        "Asset search retained an executable hidden resource selection.");
    click(app,browser::add);key(app,VK_ESCAPE);
    require(entities(app).size()==initial_count+1,"A hidden search result could still be added to the scene.");
    click(app,browser::clear);
    require(has_resource(sphere) && !selected_resource(sphere) && !action_enabled(browser::add),
        "Clearing asset search failed to restore the catalog without its retired selection.");
    click(app,sphere);
    click(app,browser::filter);key(app,VK_HOME);key(app,VK_DOWN);key(app,VK_DOWN);key(app,VK_RETURN);
    require(!has_resource(sphere) && !action_enabled(browser::add),
        "Scene type filter retained an executable hidden mesh selection.");
    click(app,browser::clear);
    require(!selected_resource(sphere),"Clearing the type filter restored a hidden selection.");
    capture(app,output/"project-assets.png");
    const auto selection_folder=std::filesystem::path{"Content"}/("AcceptanceSelection-"+std::to_string(GetTickCount64()));
    std::filesystem::create_directories(folder/selection_folder);
    model(access::EditorAcceptanceAccess::command(app,Command::refresh_assets));tick(app,2);
    const auto content_tree=access::EditorAcceptanceAccess::directory_key(app,"Content");
    const auto selected_tree=access::EditorAcceptanceAccess::directory_key(app,selection_folder);
    click(app,content_tree);
    const auto directory=access::EditorAcceptanceAccess::asset_key(app,atelier::editor::utf8(selection_folder.generic_wstring()));
    click(app,directory);require(action_enabled(browser::open),"Selected project folder could not be opened.");
    click(app,browser::search);type(app,L"no such asset");key(app,VK_RETURN);
    require(!has_resource(directory) && !action_enabled(browser::open),
        "Asset search retained an executable hidden folder selection.");
    click(app,browser::open);key(app,VK_ESCAPE);click(app,browser::clear);
    require(has_resource(directory) && !selected_resource(directory),
        "Hidden folder Open changed the directory or clearing search restored its selection.");
    click(app,content_tree);key(app,VK_LEFT);
    click(app,directory);key(app,VK_RETURN);
    const auto* revealed=app.presented()->find(selected_tree);
    require(revealed && revealed->selected && revealed->bounds.y>=revealed->clip.top &&
        revealed->bounds.y+revealed->bounds.height<=revealed->clip.bottom,
        "Opening a folder did not expand and reveal its selected directory-tree row.");
    click(app,browser::up);require(app.presented()->find(content_tree)->selected,"Up did not select the parent directory.");
    click(app,browser::back);require(app.presented()->find(selected_tree)->selected,"Back did not restore the previous directory.");
    click(app,browser::path);key(app,VK_HOME);key(app,VK_DOWN);key(app,VK_RETURN);
    require(app.presented()->find(content_tree)->selected,"The interactive path menu did not navigate to its Content ancestor.");
    double_click(app,directory);
    require(app.presented()->find(selected_tree)->selected,"Double-click did not use the existing folder Open route.");
    click(app,browser::up);click(app,directory);
    const auto context_point=centre(app,directory);
    SendMessageW(app.window(),WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(context_point.x,context_point.y));
    SendMessageW(app.window(),WM_RBUTTONUP,0,MAKELPARAM(context_point.x,context_point.y));tick(app,2);
    require(app.menu_frame() && app.menu_frame()->find(browser::open)->enabled,"A real table-row context menu lost its folder target.");
    require(std::filesystem::remove(folder/selection_folder),"Cannot remove the context-menu directory fixture.");
    model(access::EditorAcceptanceAccess::command(app,Command::refresh_assets));tick(app,2);
    require(!app.menu_frame() && !has_resource(directory) && !action_enabled(browser::open) &&
        entities(app).size()==initial_count+1 && app.document().selection().entity==current,
        "A refreshed table retained a stale row menu or changed the scene.");
    std::filesystem::create_directories(folder/selection_folder);
    model(access::EditorAcceptanceAccess::command(app,Command::refresh_assets));tick(app,2);
    click(app,directory);click(app,browser::open);
    require(app.presented()->find(selected_tree) && app.presented()->find(selected_tree)->selected,
        "Open did not enter the recreated directory.");
    require(std::filesystem::remove(folder/selection_folder),"Cannot remove the empty directory acceptance fixture.");
    model(access::EditorAcceptanceAccess::command(app,Command::refresh_assets));tick(app,2);
    require(app.presented()->find(browser::project)->selected,"Refreshing a removed directory did not return to Project.");
    require(!app.presented()->find(selected_tree),"Refreshing retained a removed directory tree row.");
    require(!action_enabled(browser::open),"Refreshing retained an executable removed directory.");
    const auto external=std::filesystem::absolute(output/"Imported.obj");
    model(atelier::editor::write_file(external,"v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n"));
    model(access::EditorAcceptanceAccess::command(app,Command::import_mesh,atelier::editor::utf8(external.wstring())));tick(app,3);
    require(app.presented()->find(access::EditorAcceptanceAccess::directory_key(app,"Content/Meshes"))->selected,
        "Import did not reveal the project's mesh folder.");
    capture(app,output/"project-directory.png");
    const auto catalog=model(atelier::editor::Project::open(folder));
    const auto imported_entry=std::find_if(catalog.entries().begin(),catalog.entries().end(),[](const auto& entry) {
        return entry.path==L"Content/Meshes/Imported.obj" && entry.kind==atelier::editor::AssetKind::mesh;
    });
    require(imported_entry!=catalog.entries().end(),"Imported OBJ did not publish a project mesh entry.");
    click(app,access::EditorAcceptanceAccess::asset_key(app,imported_entry->key));
    click(app,browser::add);
    const auto imported_entity=app.document().selection().entity;
    require(entities(app).size()==initial_count+2 && imported_entity!=current,"Adding the imported resource did not create a distinct Entity.");
    click(app,app.object_control(current));click(app,app.object_control(imported_entity));
    require(app.document().selection().entity==imported_entity,"Scene Tree did not select the imported Entity.");
    const auto import_history=app.document().undo_count();
    click(app,property_control(app,imported_entity,0));type(app,L"2.5");key(app,VK_RETURN);
    require(x(app,imported_entity)==2.5f && app.document().undo_count()==import_history+1,
        "Imported Entity parameter edit did not create one document transaction.");
    click(app,app.object_control(imported_entity));click(app,control::undo);
    require(x(app,imported_entity)==0,"Undo did not restore the imported Entity's position.");
    click(app,control::redo);require(x(app,imported_entity)==2.5f,"Redo did not restore the imported Entity's edit.");
    click(app,control::focus);
    // Selected geometry keeps its orange edges even when the display overlay is off.
    click(app,control::tree_world);
    require(app.document().selection().entity==atelier::ObjectId{},"World selection did not clear the imported Entity's outline.");
    const auto display_document=model(app.document().serialize());
    const auto display_history=app.document().undo_count();
    const auto choose=[&](ControlKey source,ControlKey option) {click(app,source);click(app,option);tick(app);};
    choose(control::scene_shading,control::shading_first+2);
    choose(control::scene_shading,control::edges_first);
    atelier::renderer::Capture shaded;
    require(app.step(&shaded) && shaded.frame_id!=0 && std::any_of(shaded.depth.begin(),shaded.depth.end(),[](float value){return value>0;}),
        "Project Entity did not reach an accepted GPU scene frame.");
    png(shaded,output/"project-matcap.png");
    choose(control::scene_shading,control::edges_first+1);
    atelier::renderer::Capture outlined;
    require(app.step(&outlined) && outlined.frame_id!=0 && outlined.extent==shaded.extent,
        "Project wireframe did not produce an accepted GPU frame.");
    png(outlined,output/"project-wireframe.png");
    const auto viewport=app.presented()->find(control::viewport)->bounds;
    std::size_t changed{};
    for (unsigned row=static_cast<unsigned>(viewport.y);row<static_cast<unsigned>(viewport.y+viewport.height);++row)
        for (unsigned column=static_cast<unsigned>(viewport.x);column<static_cast<unsigned>(viewport.x+viewport.width);++column) {
            const auto offset=(std::size_t(row)*shaded.extent.width+column)*4;
            if (std::abs(int(shaded.rgba[offset])-int(outlined.rgba[offset]))>3 ||
                std::abs(int(shaded.rgba[offset+1])-int(outlined.rgba[offset+1]))>3 ||
                std::abs(int(shaded.rgba[offset+2])-int(outlined.rgba[offset+2]))>3) ++changed;
        }
    std::printf("Project overlay changed %zu viewport pixels.\n",changed);
    require(changed>8,"Wireframe did not change the unselected imported mesh's scene pixels.");
    require(outlined.depth==shaded.depth,"Project wireframe changed scene depth.");
    require(app.document().selection().entity==atelier::ObjectId{} && model(app.document().serialize())==display_document &&
        app.document().undo_count()==display_history,"Wireframe changed selection or persistent project state.");
    choose(control::scene_shading,control::edges_first);
    model(access::EditorAcceptanceAccess::command(app,Command::save_scene));tick(app,2);
    saved=app.document().serialize();require(saved.has_value(),"Cannot inspect the saved imported scene.");
    model(app.open_project(folder));tick(app,4);
    require(entities(app).size()==initial_count+2 && model(app.document().serialize())==*saved,
        "Save/reopen lost the imported mesh, Entity or parameter values.");
    capture(app,output/"project-import-reopened.png");
    model(atelier::editor::write_file(folder/"Content/Scenes/Broken.scene","{broken"));
    require(!app.open_scene("Content/Scenes/Broken.scene") && model(app.document().serialize())==*saved,
        "Failed scene open replaced the current document.");
    const auto metadata=folder/"Content/Meshes/Imported.obj.asset.json";
    const auto metadata_text=model(atelier::editor::read_file(metadata));
    model(atelier::editor::write_file(metadata,"{broken"));
    model(access::EditorAcceptanceAccess::command(app,Command::create_entity));
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
