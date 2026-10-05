void verify_stow_alignment(Application& app) {
    const auto* menu=app.presented()->find(control::menu_bar_handle);
    const auto* shelf=app.presented()->find(control::shelf_handle);
    require(menu && shelf && menu->bounds.x==shelf->bounds.x && menu->bounds.width==shelf->bounds.width,
        "Menubar and shelf stow rails do not share the same physical edges.");
    const auto& a=*menu->style; const auto& b=*shelf->style;
    require(a.indicator_size==b.indicator_size && a.border_width==b.border_width &&
        a.border==b.border && a.border_inner==b.border_inner && a.border_style==b.border_style,
        "Stow rails resolved different arrow or border metrics.");
    for (const auto body:{control::menu_bar_body,control::shelf_contents})
        if (const auto* box=app.presented()->find(body))
            require(box->bounds.x==menu->bounds.x+menu->bounds.width,
                "Expanded bar content does not start at the common rail edge.");
}
void verify_tool_shelf(Application& app,const std::filesystem::path& output) {
    verify_stow_alignment(app);
    for (const auto rail:{control::menu_bar_handle,control::shelf_handle}) {
        mouse(app,WM_MOUSEMOVE,centre(app,rail));
        verify_stow_alignment(app);
    }
    const auto dpi=app.presented()->drawing->dpi_scale;
    const auto selected=app.document().selected();
    const auto history=app.document().undo_count();
    const auto members=actors(app).size();
    const auto shelf=app.presented()->find(control::shelf)->bounds;
    const auto viewport=app.presented()->find(control::viewport)->bounds;
    std::vector<std::pair<ControlKey,ControlKey>> tool_keys;
    const auto verify_commands=[&](atelier::editor::CommandGroup group) {
        for (const auto& definition:atelier::editor::command_definitions) {
            if (definition.group!=atelier::editor::CommandGroup::create && definition.group!=atelier::editor::CommandGroup::view) continue;
            const auto target=static_cast<ControlKey>(definition.id);
            const auto* current=command_tool(app,target);
            require((current!=nullptr)==(definition.group==group),"The presented shelf included a hidden category or omitted a tool.");
            if (!current) continue;
            require(current->key!=target && std::ranges::count(app.presented()->controls,target,&atelier::ui::ControlBox::action_target)==1,
                "A shelf command lost its distinct visual identity or appeared more than once.");
            const auto previous=std::ranges::find(tool_keys,target,&std::pair<ControlKey,ControlKey>::first);
            if (previous==tool_keys.end()) tool_keys.emplace_back(target,current->key);
            else require(previous->second==current->key,"Changing shelf categories recreated a command control.");
        }
    };
    verify_commands(atelier::editor::CommandGroup::create);
    const auto* tool=command_tool(app,control::add_actor);
    require(tool && tool->key!=control::add_actor,"DSL shelf command retained its command ID as the visual identity.");
    require(std::abs(shelf.height/dpi-56)<.1f && std::abs(tool->bounds.width/dpi-64)<.1f &&
        tool->style->icon_placement==atelier::ui::IconPlacement::above && tool->style->font.size==10.5f,
        "Tool shelf did not use the compact icon-over-caption layout.");
    // The icon and caption must both activate the same command exactly once.
    for (bool icon:{true,false}) {
        const auto bounds=command_tool(app,control::add_actor)->bounds;
        click_at(app,{static_cast<LONG>(bounds.x+bounds.width*.5f),
            static_cast<LONG>(icon?bounds.y+10*dpi:bounds.y+bounds.height-6*dpi)});
        require(actors(app).size()==members+1 && app.document().undo_count()==history+1,
            "Shelf icon/caption did not activate exactly one document command.");
        require(!app.presented()->find(control::parameter_transform) &&
            app.presented()->find(control::parameter_components) && app.presented()->find(control::parameter_components)->selected,
            "An Actor without a scene root must expose only its Components page.");
        click(app,control::undo); click(app,app.object_control(selected));
        require(app.presented()->find(control::parameter_transform) && app.presented()->find(control::parameter_transform)->selected,
            "The temporary Components fallback overwrote the preferred Transform page.");
    }
    click(app,control::shelf_create);key(app,VK_TAB);key(app,VK_TAB,false,true);key(app,VK_END);
    require(app.presented()->find(control::shelf_view)->selected,"Shelf Tab/End did not enter and switch the category group.");
    verify_commands(atelier::editor::CommandGroup::view);
    key(app,VK_HOME);require(app.presented()->find(control::shelf_create)->selected,"Shelf Home did not select Create.");
    verify_commands(atelier::editor::CommandGroup::create);
    key(app,VK_RIGHT);require(app.presented()->find(control::shelf_view)->selected,"Shelf Right did not select View.");
    key(app,VK_LEFT);require(app.presented()->find(control::shelf_create)->selected,"Shelf Left did not select Create.");
    key(app,VK_END);verify_commands(atelier::editor::CommandGroup::view);
    click(app,control::shelf_handle);
    verify_stow_alignment(app);
    require(std::abs(app.presented()->find(control::shelf)->bounds.height/dpi-6)<.1f &&
        !app.presented()->find(control::shelf_tabs) && !command_tool(app,control::focus) &&
        app.presented()->find(control::file_menu) && app.presented()->find(control::shelf_handle),
        "Stowing the shelf hid its restore handle, retained hidden commands, or changed the menubar.");
    require(std::abs(app.presented()->find(control::viewport)->bounds.y-viewport.y+50*dpi)<.1f,
        "Stowing the shelf did not return its height to the workspace.");
    capture(app,output/"shelf-collapsed.png");
    key(app,VK_SPACE);
    verify_stow_alignment(app);
    require(app.presented()->find(control::shelf_view)->selected && command_tool(app,control::focus) &&
        app.presented()->find(control::shelf)->bounds==shelf,"Keyboard expansion lost the shelf category or dimensions.");
    verify_commands(atelier::editor::CommandGroup::view);
    click(app,control::shelf_create);
    verify_commands(atelier::editor::CommandGroup::create);
    // Content-page choices use the same keyboard group and retain hidden field identities.
    const auto field=property_control(app,selected,0);
    const auto incarnation=app.presented()->find(field)->incarnation;
    const auto original=x(app,selected);
    const auto filter_incarnation=app.presented()->find(control::parameter_search)->incarnation;
    const auto filter_bounds=app.presented()->find(control::parameter_search)->bounds;
    click(app,control::parameter_search);key(app,VK_TAB);key(app,VK_END);
    require(app.presented()->find(control::parameter_components)->selected && !app.presented()->find(field),
        "Parameter Tab/End did not select Components and hide the Transform field.");
    require(app.presented()->find(control::parameter_search) && app.presented()->find(control::parameter_search)->incarnation==filter_incarnation &&
        app.presented()->find(control::parameter_search)->bounds==filter_bounds,
        "Changing property pages hid, moved or recreated the shared Filter.");
    key(app,VK_HOME);
    require(app.presented()->find(control::parameter_transform)->selected && app.presented()->find(field)->incarnation==incarnation,
        "Parameter Home did not restore Transform with its existing field identity.");
    key(app,VK_RIGHT);require(app.presented()->find(control::parameter_components)->selected,"Parameter Right did not select Components.");
    key(app,VK_LEFT);require(app.presented()->find(control::parameter_transform)->selected,"Parameter Left did not select Transform.");
    click(app,field);type(app,(std::to_wstring(original+1)+L"x").c_str());key(app,VK_RETURN);
    require(app.document().editing() && x(app,selected)!=original && app.document().undo_count()==history,
        "Parameter page fixture did not retain an invalid draft after a real field preview.");
    click(app,control::parameter_components);
    require(!app.document().editing() && x(app,selected)==original && app.document().undo_count()==history &&
        !app.presented()->find(field) && !app.text_input(),"Changing pages failed to cancel the invalid draft and its preview.");
    click(app,control::parameter_transform);
    require(property_control(app,selected,0)==field && app.presented()->find(field)->incarnation==incarnation && x(app,selected)==original,
        "Returning to Transform lost the field identity or restored a cancelled draft.");
    const auto root=*model(atelier::world::root_component(app.document().world().runtime(),selected));
    const auto root_row=app.object_control(root);
    click(app,control::parameter_components);click(app,control::parameter_search);type(app,L"staticmesh");key(app,VK_RETURN);
    require(root_row && app.presented()->find(root_row),"Shared Filter did not match the selected Actor's real component type.");
    type(app,L"no-such-component");key(app,VK_RETURN);
    require(!app.presented()->find(root_row) && app.presented()->find(control::parameter_search),
        "Component filtering retained unmatched rows or hid its own Filter.");
    type(app,L"");key(app,VK_RETURN);
    require(app.presented()->find(root_row),"Clearing the shared Filter did not restore component rows.");
    click(app,control::parameter_transform);
    require(property_control(app,selected,0)==field && app.presented()->find(field) && app.document().undo_count()==history,
        "Filtering another page replaced the World field key or changed document history.");
    capture(app,output/"shelf-expanded.png");
    click(app,control::menu_bar_handle); click(app,control::shelf_handle);
    verify_stow_alignment(app);
    require(app.presented()->find(control::menu_bar_handle)->bounds.height==
        app.presented()->find(control::shelf_handle)->bounds.height,
        "Collapsed bars retained different restoration strip heights.");
    capture(app,output/"bars-collapsed.png");
    // Expand in the opposite order; F10 must still reveal the menu and open File.
    key(app,VK_SPACE); verify_stow_alignment(app);
    key(app,VK_F10);
    require(app.menu_frame() && app.presented()->find(control::file_menu),
        "F10 failed to restore the menubar after independently stowing both bars.");
    key(app,VK_ESCAPE); verify_stow_alignment(app);
    require(app.presented()->find(control::shelf)->bounds==shelf &&
        app.presented()->find(control::viewport)->bounds==viewport,
        "Restoring both bars changed the workspace geometry.");
    require(app.document().selected()==selected && app.document().undo_count()==history,
        "Shelf layout operations changed document selection or history.");
}
