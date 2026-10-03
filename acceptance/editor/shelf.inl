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
    const auto* tool=app.presented()->find(control::add_actor);
    require(std::abs(shelf.height/dpi-56)<.1f && std::abs(tool->bounds.width/dpi-64)<.1f &&
        tool->style->icon_placement==atelier::ui::IconPlacement::above && tool->style->font.size==10.5f,
        "Tool shelf did not use the compact icon-over-caption layout.");
    // The icon and caption must both activate the same command exactly once.
    for (bool icon:{true,false}) {
        const auto bounds=app.presented()->find(control::add_actor)->bounds;
        click_at(app,{static_cast<LONG>(bounds.x+bounds.width*.5f),
            static_cast<LONG>(icon?bounds.y+10*dpi:bounds.y+bounds.height-6*dpi)});
        require(actors(app).size()==members+1 && app.document().undo_count()==history+1,
            "Shelf icon/caption did not activate exactly one document command.");
        click(app,control::undo); click(app,app.object_control(selected));
    }
    click(app,control::shelf_view);
    require(app.presented()->find(control::focus) && !app.presented()->find(control::add_actor),"Shelf category did not select its own commands.");
    click(app,control::shelf_handle);
    verify_stow_alignment(app);
    require(std::abs(app.presented()->find(control::shelf)->bounds.height/dpi-6)<.1f &&
        !app.presented()->find(control::shelf_tabs) && !app.presented()->find(control::focus) &&
        app.presented()->find(control::file_menu) && app.presented()->find(control::shelf_handle),
        "Stowing the shelf hid its restore handle, retained hidden commands, or changed the menubar.");
    require(std::abs(app.presented()->find(control::viewport)->bounds.y-viewport.y+50*dpi)<.1f,
        "Stowing the shelf did not return its height to the workspace.");
    capture(app,output/"shelf-collapsed.png");
    key(app,VK_SPACE);
    verify_stow_alignment(app);
    require(app.presented()->find(control::shelf_view)->selected && app.presented()->find(control::focus) &&
        app.presented()->find(control::shelf)->bounds==shelf,"Keyboard expansion lost the shelf category or dimensions.");
    click(app,control::shelf_create);
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
