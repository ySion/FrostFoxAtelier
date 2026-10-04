void reference_guide_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    ui::Frame drawing{.id=10000,.extent=extent,.draws={}};
    drawing.draws.push_back({{4,4,12,12},{4,4,16,16},{0,1,0,1},{0,0,1,1},{}});
    render::Frame frame{drawing,{}, {24,32,float(extent.width-48),float(extent.height-64)}, {}};
    // Orthographic camera looking down from Y=5: reference plane depth is .5.
    frame.camera.view={1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,5,1};
    frame.camera.projection={.2f,0,0,0, 0,.2f,0,0, 0,0,.1f,0, 0,0,0,1};
    frame.view.background=render::Background::gradient;
    render::Capture capture;
    const auto draw=[&] {
        ++drawing.id;
        const auto result=take(renderer.render(frame,&capture));
        check(result.status==render::FrameStatus::accepted && capture.frame_id==drawing.id,
              "reference guides retain accepted frame identity");
        check(pixel(capture,8,8)[1]==255 && depth(capture,8,8)==1 && pixel(capture,20,20)[0]==0,
              "viewport guides do not escape their viewport or affect UI");
        return result;
    };
    const auto difference=[](const render::Capture& a,const render::Capture& b) {
        std::size_t changed=0;
        for (std::size_t i=0;i<a.rgba.size();i+=4)
            if (std::abs(int(a.rgba[i])-b.rgba[i])>3 || std::abs(int(a.rgba[i+1])-b.rgba[i+1])>3 ||
                std::abs(int(a.rgba[i+2])-b.rgba[i+2])>3) ++changed;
        return changed;
    };
    auto result=draw(); const auto background=capture;
    check(result.stats.scene_draw_calls==0 && result.stats.viewport_draw_calls==1 &&
          pixel(capture,extent.width/2,extent.height-48)[1]>pixel(capture,extent.width/2,48)[1]+25 &&
          std::ranges::all_of(capture.depth,[](float z){return z==1;}),
          "empty viewport has a gradient without geometry depth");
    frame.view.reference_grid=true; result=draw(); const auto grid=capture;
    check(result.stats.viewport_draw_calls==2 && result.stats.scene_draw_calls==0 &&
          capture.depth==background.depth && difference(capture,background)>100,
          "empty-world grid changes real pixels, not scene depth or draw counts");
    snapshot(capture,"viewport-empty-grid.ppm");
    frame.view.reference_grid=false; frame.view.origin_axes=true; draw();
    check(difference(capture,background)>30 && capture.depth==background.depth,
          "world origin axes are independently visible without grid lines");
    frame.view.reference_grid=true;
    std::array<render::MeshInstance,1> meshes{};
    meshes[0].geometry=atelier::geometry::cube_mesh(); frame.meshes=meshes;
    for (const auto wire:{render::Wireframe::off,render::Wireframe::overlay,render::Wireframe::only})
        for (const bool hidden:{true,false}) {
            frame.view.wireframe=wire; frame.view.hidden_lines=hidden;
            frame.view.reference_grid=frame.view.origin_axes=false; draw(); const auto bare=capture;
            frame.view.reference_grid=frame.view.origin_axes=true; result=draw();
            check(capture.depth==bare.depth && difference(capture,bare)>100 && result.stats.viewport_draw_calls==2,
                  "guides preserve full geometry depth in all shading and wire modes");
            std::size_t occluder_pixels=0;
            for (std::size_t i=0;i<capture.depth.size();++i) if (capture.depth[i]<.499f) {
                ++occluder_pixels;
                for (unsigned c=0;c<3;++c) check(capture.rgba[i*4+c]==bare.rgba[i*4+c],
                    "reference grid stays behind solid geometry and wireframe depth");
            }
            check(occluder_pixels>100,"guide fixture has a visible geometry occluder");
        }
    // A real floor on Y=0 takes priority over the coplanar reference guide.
    using atelier::geometry::Vertex;
    meshes[0].geometry=std::make_shared<const atelier::geometry::Mesh>(std::vector<Vertex>{
        {{-2,0,-2},{0,1,0}},{{-2,0,2},{0,1,0}},{{2,0,2},{0,1,0}},
        {{-2,0,-2},{0,1,0}},{{2,0,2},{0,1,0}},{{2,0,-2},{0,1,0}}});
    frame.view.wireframe=render::Wireframe::off;
    frame.view.reference_grid=frame.view.origin_axes=false; draw(); const auto floor=capture;
    frame.view.reference_grid=frame.view.origin_axes=true; draw();
    check(capture.depth==floor.depth,"coplanar guides do not change floor depth");
    for (std::size_t i=0;i<capture.depth.size();++i) if (capture.depth[i]<1)
        for (unsigned c=0;c<3;++c) check(capture.rgba[i*4+c]==floor.rgba[i*4+c],
            "coplanar geometry wins over reference grid and axes");
    // Deleting the final mesh must not expose its retained GPU depth.
    frame.meshes={}; frame.view.origin_axes=false; draw();
    check(capture.depth==background.depth && capture.rgba==grid.rgba,"empty world restores grid after geometry retirement");
    frame.camera.projection={}; result=draw();
    check(capture.rgba==background.rgba && result.stats.viewport_draw_calls==1,
          "singular finite camera skips only the optional grid");
    // A camera on the reference plane must not produce a full-screen axis.
    frame.camera={};
    frame.camera.projection={1,0,0,0, 0,1,0,0, 0,0,30000.f/29999.95f,1, 0,0,-.05f*30000.f/29999.95f,0};
    draw(); check(capture.rgba==background.rgba,"camera on grid plane has no spurious guide coverage");
    for (const float y:{-.5f,.5f,2000.f}) {
        frame.camera.view=translation(0,-y,0); draw();
        check(capture.depth==background.depth,"perspective and below-plane grid preserves geometry depth");
    }
    frame.scene_viewport={}; result=draw();
    check(result.stats.viewport_draw_calls==0 && pixel(capture,extent.width/2,extent.height/2)[0]==0 &&
          capture.depth==background.depth,"zero viewport suppresses all auxiliary draws");
    frame.view.background=static_cast<render::Background>(999);
    const auto previous=capture.frame_id;
    const auto invalid=renderer.render(frame,&capture);
    check(!invalid && !invalid.error().terminal && capture.frame_id==previous,"invalid background is rejected before encoding");
    frame.view.background=render::Background::none; draw();
    std::puts("Renderer reference guides: PASS (empty world, gradient, grid, axes, occlusion, wire modes, depth, camera, retirement)");
}
