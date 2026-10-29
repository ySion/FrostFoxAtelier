void verify_viewport_display(Application& app,const std::filesystem::path& output) {
    const auto* path=app.presented()->find(control::scene_path);
    const auto* shading=app.presented()->find(control::scene_shading);
    const auto* guides=app.presented()->find(control::scene_guides);
    require(path && shading && guides,"Viewport path lost its compact display menus.");
    const auto* path_bar=app.presented()->find(path->parent);
    const auto dpi=app.presented()->drawing->dpi_scale;
    // Blender header order: overlays dropdown before the shading segment, whose
    // trailing arrow opens the full display menu at the row's end.
    require(path_bar && shading->kind==atelier::ui::ControlKind::combo && guides->kind==atelier::ui::ControlKind::combo &&
        std::abs(shading->bounds.width/dpi-16)<.1f && std::abs(shading->bounds.height/dpi-24)<.1f &&
        std::abs(guides->bounds.width/dpi-34)<.1f && std::abs(guides->bounds.height/dpi-24)<.1f &&
        guides->bounds.x>=path->bounds.x+path->bounds.width-.1f && shading->bounds.x>=guides->bounds.x+guides->bounds.width-.1f &&
        shading->bounds.x+shading->bounds.width<=path_bar->bounds.x+path_bar->bounds.width+.1f &&
        std::abs(shading->bounds.y-path->bounds.y)<.1f && std::abs(guides->bounds.y-path->bounds.y)<.1f,
        "Viewport overlay and shading menus must stay compact header targets after the path.");
    atelier::renderer::Capture empty;
    require(app.step(&empty) && std::ranges::all_of(empty.depth,[](float z){return z==0;}),
            "Empty viewport guides changed geometry depth.");
    png(empty,output/"empty-reference-grid.png");
    const auto browser_history=app.document().undo_count();
    const auto resource=atelier::application::EditorAcceptanceAccess::asset_key(app,"builtin/sphere");
    click(app,resource);
    require(entities(app).empty() && app.document().undo_count()==browser_history,
        "Selecting a resource row created a scene object or changed document history.");
    double_click(app,resource);key(app,VK_RETURN);
    require(entities(app).empty() && app.document().undo_count()==browser_history,
        "Resource Open gestures implicitly created a scene object.");
    click(app,atelier::editor::browser_control::add);
    // Inspect actual faces at a useful scale, beyond the selected edge coverage.
    click(app,control::focus);
    require_input_focus(app);
    wheel(app,control::viewport,5);
    const auto entity=app.document().selection().entity;
    const auto history=app.document().undo_count();
    const auto items=model(app.document().presentation().render_items());
    require(items.size()==1,"Viewport fixture did not create a real sphere.");
    atelier::renderer::Capture initial;
    require(app.step(&initial),"Initial MatCap frame failed.");
    png(initial,output/"matcap.png");
    const auto choose=[&](ControlKey source,ControlKey option) {
        click(app,source);
        if(!app.menu_frame() || !app.menu_frame()->find(option)) {
            capture(app,output/"viewport-menu-failure.png");
            throw std::runtime_error("Viewport choice did not use the native shared menu: source="+
                std::to_string(source)+", option="+std::to_string(option));
        }
        const auto frame=app.menu_frame();
        if (source==control::scene_shading) {
            const std::array starts{control::shading_first,control::edges_first,control::topology_first};
            const std::array counts{3u,4u,2u};
            std::array<ControlKey,3> groups{};
            for (std::size_t group=0;group<starts.size();++group) {
                unsigned selected{};
                for (unsigned index=0;index<counts[group];++index) {
                    const auto* item=frame->find(starts[group]+index);
                    require(item && item->menu_behavior==atelier::ui::MenuBehavior::radio && item->radio_group,
                        "Display menu lost a radio option or its group identity.");
                    if (!index) groups[group]=item->radio_group;
                    require(item->radio_group==groups[group],"Related display options split across radio groups.");
                    selected+=item->selected?1u:0u;
                }
                require(selected==1,"A display radio group must show exactly one current option.");
            }
            require(groups[0]!=groups[1] && groups[0]!=groups[2] && groups[1]!=groups[2],
                "Shading, edges and topology shared one mutually exclusive radio group.");
        } else {
            for (const auto key:{control::guide_background,control::guide_grid,control::guide_axes}) {
                const auto* item=frame->find(key);
                require(item && item->menu_behavior==atelier::ui::MenuBehavior::checkbox && !item->radio_group,
                    "Viewport guides must expose independent checkbox options.");
            }
        }
        const bool was_selected=frame->find(option)->selected;
        click(app,option); tick(app);
        require(!app.menu_frame(),"Viewport choice left its menu open.");
        return was_selected;
    };
    const auto capture_mode=[&](const char* name) {
        atelier::renderer::Capture image;
        require(app.step(&image),"Viewport mode failed to render.");
        require(image.depth==initial.depth,"A display-only switch changed scene depth.");
        require(app.document().selection().entity==entity && app.document().undo_count()==history,
                "A viewport option changed selection or document history.");
        const auto current=model(app.document().presentation().render_items());
        require(current.size()==items.size() && current[0].geometry==items[0].geometry &&
                current[0].world.elements==items[0].world.elements &&
                current[0].surfaces==items[0].surfaces,
                "Display mode changed geometry, material or transforms.");
        png(image,output/name);
        return image;
    };
    choose(control::scene_shading,control::shading_first);
    const auto smooth=capture_mode("smooth.png");
    choose(control::scene_shading,control::shading_first+1);
    const auto flat=capture_mode("flat.png");
    const auto scene=app.presented()->find(control::viewport)->bounds;
    std::size_t changed=0;
    for (unsigned y=static_cast<unsigned>(scene.y);y<static_cast<unsigned>(scene.y+scene.height);++y)
        for (unsigned x=static_cast<unsigned>(scene.x);x<static_cast<unsigned>(scene.x+scene.width);++x) {
            const auto offset=(std::size_t(y)*flat.extent.width+x)*4;
            if (std::abs(int(flat.rgba[offset])-smooth.rgba[offset])>3 ||
                std::abs(int(flat.rgba[offset+1])-smooth.rgba[offset+1])>3 ||
                std::abs(int(flat.rgba[offset+2])-smooth.rgba[offset+2])>3) ++changed;
        }
    require(changed>100,"Flat/Smooth controls did not change the actual sphere shading.");
    const auto grid_selected=choose(control::scene_guides,control::guide_grid);
    const auto no_grid=capture_mode("reference-grid-off.png");
    require(choose(control::scene_guides,control::guide_grid)!=grid_selected,
        "Reopening Guides did not show the accepted checkbox change.");
    const auto with_grid=capture_mode("reference-grid-on.png");
    changed=0;
    for (unsigned y=static_cast<unsigned>(scene.y);y<static_cast<unsigned>(scene.y+scene.height);++y)
        for (unsigned x=static_cast<unsigned>(scene.x);x<static_cast<unsigned>(scene.x+scene.width);++x) {
            const auto offset=(std::size_t(y)*with_grid.extent.width+x)*4;
            if (std::abs(int(with_grid.rgba[offset])-no_grid.rgba[offset])>3 ||
                std::abs(int(with_grid.rgba[offset+1])-no_grid.rgba[offset+1])>3 ||
                std::abs(int(with_grid.rgba[offset+2])-no_grid.rgba[offset+2])>3) ++changed;
        }
    require(changed>100,"Reference Grid menu did not change actual viewport pixels.");
    choose(control::scene_guides,control::guide_axes); capture_mode("origin-axes-off.png");
    choose(control::scene_guides,control::guide_axes);
    choose(control::scene_guides,control::guide_background); capture_mode("gradient-off.png");
    choose(control::scene_guides,control::guide_background);
    choose(control::scene_shading,control::shading_first+2);
    choose(control::scene_shading,control::edges_first+1); capture_mode("matcap-edges.png");
    choose(control::scene_shading,control::topology_first+1); capture_mode("matcap-triangles.png");
    choose(control::scene_shading,control::topology_first);
    choose(control::scene_shading,control::edges_first+2); capture_mode("hidden-lines.png");
    choose(control::scene_shading,control::edges_first+3); capture_mode("all-lines.png");
    choose(control::scene_shading,control::edges_first); capture_mode("restored-matcap.png");
    verify_transform_projection(app,output,entity);
}
