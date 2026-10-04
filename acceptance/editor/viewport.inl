void verify_viewport_display(Application& app,const std::filesystem::path& output) {
    click(app,atelier::application::EditorAcceptanceAccess::asset_key(app,"builtin/sphere"));
    click(app,atelier::editor::browser_control::add);
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
            if (std::abs(int(flat.rgba[offset])-smooth.rgba[offset])>3) ++changed;
        }
    require(changed>100,"Flat/Smooth controls did not change the actual sphere shading.");
    choose(control::scene_shading,control::shading_first+2);
    choose(control::scene_edges,control::edges_first+1); capture_mode("matcap-edges.png");
    choose(control::scene_topology,control::topology_first+1); capture_mode("matcap-triangles.png");
    choose(control::scene_topology,control::topology_first);
    choose(control::scene_edges,control::edges_first+2); capture_mode("hidden-lines.png");
    choose(control::scene_edges,control::edges_first+3); capture_mode("all-lines.png");
    choose(control::scene_edges,control::edges_first); capture_mode("restored-matcap.png");
    verify_transform_projection(app,output,actor);
}
