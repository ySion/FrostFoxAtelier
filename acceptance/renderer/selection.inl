void selection_clipping_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    ui::Frame drawing{.id=12000,.extent=extent,.draws={}};
    render::MeshInstance floor;
    floor.color={.2f,.2f,.2f,1};
    floor.material.emissive={.04f,.05f,.06f};
    floor.stable_key=1;
    render::Frame frame{drawing,{}, {0,0,float(extent.width),float(extent.height)}, {&floor,1}};
    constexpr float near_plane=.1f,far_plane=100.f;
    const render::Camera perspective{.projection={1.5f*float(extent.height)/float(extent.width),0,0,0,
        0,1.5f,0,0, 0,0,-near_plane/(far_plane-near_plane),1,
        0,0,near_plane*far_plane/(far_plane-near_plane),0}};
    frame.view.lit.shadows=frame.view.lit.ddgi=frame.view.lit.gtao=false;
    frame.view.lit.sssr=frame.view.lit.ssil=frame.view.lit.bloom=false;
    render::Capture capture;
    const auto draw=[&] {
        ++drawing.id;
        check(take(renderer.render(frame,&capture)).status==render::FrameStatus::accepted,
              "selection clipping frame is accepted");
    };
    bool interiors_preserved=true;
    const auto run_case=[&](const char* name,float front_z,render::Shading shading,
                            render::Antialiasing aa=render::Antialiasing::none,float scale=1.f,bool mirror=false,
                            float wire_width=1.25f) {
        frame.camera=perspective;
        frame.camera.projection[14]*=scale;
        floor.world=translation(0,0,0);
        floor.world[0]=mirror?-scale:scale;floor.world[5]=floor.world[10]=scale;
        frame.view.shading=shading;frame.view.lit.antialiasing=aa;
        frame.view.wireframe=render::Wireframe::off;frame.view.triangle_edges=false;
        frame.view.wire_width=wire_width;
        // One polygon crosses the eye/near plane. Its diagonal is not an authored
        // edge, and the centre-lower viewport is far from every true boundary.
        floor.geometry=std::make_shared<const Mesh>(std::vector<Vertex>{
            {{-8,-.4f,front_z},{0,1,0}},{{-8,-.4f,8},{0,1,0}},{{8,-.4f,8},{0,1,0}},
            {{-8,-.4f,front_z},{0,1,0}},{{8,-.4f,8},{0,1,0}},{{8,-.4f,front_z},{0,1,0}}},
            std::vector<std::uint8_t>{5,3});
        floor.selected=false;draw();const auto reference=capture;
        snapshot(reference, (std::string{"selection-"}+name+"-base.ppm").c_str());
        floor.selected=true;draw();
        snapshot(capture, (std::string{"selection-"}+name+".ppm").c_str());
        std::size_t samples=0,changed=0,yellow=0;
        for (unsigned y=extent.height*3/5;y<extent.height*9/10;++y)
            for (unsigned x=extent.width*3/8;x<extent.width*5/8;++x) {
                if (depth(reference,x,y)==0) continue;
                ++samples;
                const auto* before=pixel(reference,x,y);const auto* after=pixel(capture,x,y);
                if (std::abs(int(before[0])-after[0])>2 || std::abs(int(before[1])-after[1])>2 ||
                    std::abs(int(before[2])-after[2])>2) ++changed;
                if (after[0]>220 && after[1]>160 && after[2]<50) ++yellow;
            }
        std::printf("Selection %s: interior=%zu changed=%zu yellow=%zu\n",
                    name,samples,changed,yellow);
        check(samples>1000,"clipped selection fixture covers a substantial interior");
        check(capture.depth==reference.depth,"selection does not change FP32 surface depth");
        interiors_preserved&=changed==0;
        // The original far edge stays visible even with an original vertex at w=0.
        const int edge_y=static_cast<int>((.5f+.3f/8.f)*float(extent.height));
        unsigned edge_pixels=0;
        for (int y=edge_y-6;y<=edge_y+6;++y) {
            const auto* before=pixel(reference,extent.width/2,static_cast<unsigned>(y));
            const auto* after=pixel(capture,extent.width/2,static_cast<unsigned>(y));
            if (std::abs(int(after[0])-before[0])>2 || std::abs(int(after[1])-before[1])>2 ||
                std::abs(int(after[2])-before[2])>2) ++edge_pixels;
        }
        std::printf("Selection %s: original-edge pixels=%u\n",name,edge_pixels);
        check(edge_pixels>=(wire_width>=8?3u:1u) && edge_pixels<=static_cast<unsigned>(std::ceil(std::max(wire_width,2.f)*.5f))+1,
              "original edge retains a bounded pixel width after clipping");
        const auto polygon=capture;
        frame.view.triangle_edges=true;draw();
        std::size_t diagonal=0;
        for (unsigned y=extent.height*11/20;y<extent.height*9/10;++y)
            for (unsigned x=extent.width/4;x<extent.width*3/4;++x) {
                const auto* before=pixel(polygon,x,y);const auto* after=pixel(capture,x,y);
                if (std::abs(int(before[0])-after[0])>20 || std::abs(int(before[1])-after[1])>20) ++diagonal;
            }
        check(diagonal>extent.width/8,"explicit triangle edges restore the clipped polygon diagonal");
        frame.view.triangle_edges=false;floor.selected=false;
        for (const auto mode : {render::Wireframe::overlay,render::Wireframe::only}) {
            frame.view.wireframe=mode;frame.view.hidden_lines=true;draw();
            check(capture.depth==reference.depth,"explicit wire modes preserve the surface depth");
            for (unsigned y=extent.height*3/5;y<extent.height*9/10;++y)
                for (unsigned x=extent.width*3/8;x<extent.width*5/8;++x) {
                    const auto* after=pixel(capture,x,y);const auto* before=pixel(reference,x,y);
                    for (unsigned c=0;c<3;++c)
                        check(std::abs(int(after[c])-(mode==render::Wireframe::only?0:before[c]))<=2,
                              "explicit wire display does not flood clipped polygon interiors");
                }
            if (mode==render::Wireframe::only) {
                const auto visible=capture;frame.view.hidden_lines=false;draw();
                check(capture.rgba==visible.rgba && capture.depth==visible.depth,
                      "visible/all wires agree for one unobstructed clipped polygon");
            }
        }
    };
    run_case("behind-eye",-1,render::Shading::smooth);
    run_case("on-eye",0,render::Shading::smooth);
    run_case("near-clipped",.04f,render::Shading::smooth);
    run_case("near-front",.11f,render::Shading::smooth);
    run_case("flat",-1,render::Shading::flat);
    run_case("matcap",-1,render::Shading::matcap);
    run_case("lit-off",-1,render::Shading::scene_lit);
    run_case("lit-taa",-1,render::Shading::scene_lit,render::Antialiasing::taa);
    run_case("lit-smaa",-1,render::Shading::scene_lit,render::Antialiasing::smaa);
    run_case("lit-t2x",-1,render::Shading::scene_lit,render::Antialiasing::smaa_t2x);
    run_case("lit-on-eye",0,render::Shading::scene_lit);
    run_case("small-scale",-1,render::Shading::smooth,render::Antialiasing::none,1e-20f);
    run_case("large-scale",-1,render::Shading::smooth,render::Antialiasing::none,1e20f);
    run_case("mirrored",-1,render::Shading::smooth,render::Antialiasing::none,1.f,true);
    run_case("wide",-1,render::Shading::smooth,render::Antialiasing::none,1.f,false,8.f);
    check(interiors_preserved,"near/eye-plane clipping must not flood selected polygon interiors");
    std::puts("Renderer selection clipping: PASS (eye/near clipping, original/triangle edges, bounded width, four shading and AA modes, FP32 depth, extreme scale, mirror, explicit wires)");
}
