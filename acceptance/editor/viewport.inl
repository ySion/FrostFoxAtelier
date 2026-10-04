void verify_viewport_display(Application& app,const std::filesystem::path& output) {
    atelier::renderer::Capture empty;
    require(app.step(&empty) && std::ranges::all_of(empty.depth,[](float z){return z==1;}),
            "Empty viewport guides changed geometry depth.");
    png(empty,output/"empty-reference-grid.png");
    click(app,atelier::application::EditorAcceptanceAccess::asset_key(app,"builtin/sphere"));
    click(app,atelier::editor::browser_control::add);
    // Inspect actual faces at a useful scale, beyond the selected edge coverage.
    click(app,control::focus);
    require_input_focus(app);
    wheel(app,control::viewport,5);
    const auto actor=app.document().selected();
    const auto history=app.document().undo_count();
    const auto items=model(app.document().world().render_items());
    require(items.size()==1,"Viewport fixture did not create a real sphere.");
    atelier::renderer::Capture initial;
    require(app.step(&initial),"Initial MatCap frame failed.");
    png(initial,output/"matcap.png");
    const auto choose=[&](ControlKey source,ControlKey option) {
        click(app,source);
        require(app.menu_frame() && app.menu_frame()->find(option),"Viewport choice did not use the native shared menu.");
        click(app,option); tick(app);
        require(!app.menu_frame(),"Viewport choice left its menu open.");
    };
    const auto capture_mode=[&](const char* name) {
        atelier::renderer::Capture image;
        require(app.step(&image),"Viewport mode failed to render.");
        require(image.depth==initial.depth,"A display-only switch changed scene depth.");
        require(app.document().selected()==actor && app.document().undo_count()==history,
                "A viewport option changed selection or document history.");
        const auto current=model(app.document().world().render_items());
        require(current.size()==items.size() && current[0].geometry==items[0].geometry &&
                current[0].world.elements==items[0].world.elements &&
                current[0].material.base_color.x==items[0].material.base_color.x &&
                current[0].material.base_color.y==items[0].material.base_color.y &&
                current[0].material.base_color.z==items[0].material.base_color.z,
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
    choose(control::scene_guides,control::guide_grid);
    const auto no_grid=capture_mode("reference-grid-off.png");
    choose(control::scene_guides,control::guide_grid);
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
    choose(control::scene_edges,control::edges_first+1); capture_mode("matcap-edges.png");
    choose(control::scene_topology,control::topology_first+1); capture_mode("matcap-triangles.png");
    choose(control::scene_topology,control::topology_first);
    choose(control::scene_edges,control::edges_first+2); capture_mode("hidden-lines.png");
    choose(control::scene_edges,control::edges_first+3); capture_mode("all-lines.png");
    choose(control::scene_edges,control::edges_first); capture_mode("restored-matcap.png");
    verify_transform_projection(app,output,actor);
}
