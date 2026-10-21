// Narrow product-frame regressions for surface identity, editing work and
// viewport/Hi-Z boundaries. No private renderer state or synthetic GPU inputs.
void audit_numeric_inputs(Fixture& f) {
    const auto lights=f.lights;
    auto accepted=f.capture("audit-numeric-accepted",8);
    const auto accepted_id=accepted.frame_id;
    render::Light invalid_light;invalid_light.stable_key=99;invalid_light.intensity=1e38f;
    for(unsigned kind=0;kind<3;++kind) {
        invalid_light.kind=static_cast<render::LightKind>(kind);
        invalid_light.width=invalid_light.height=2;
        f.lights={invalid_light};f.edited();++f.drawing.id;
        const auto rejected=prepared_render(f.renderer,f.frame,&accepted);
        check(!rejected&&!rejected.error().terminal&&accepted.frame_id==accepted_id,
              "overflowing finite analytic light is rejected before GPU encoding without replacing accepted output");
    }
    invalid_light.kind=render::LightKind::rectangle;
    invalid_light.intensity=static_cast<float>(atelier::rendering::analytic_light_radiance_limit/4);
    invalid_light.width=std::nextafter(2.f,std::numeric_limits<float>::infinity());
    f.lights={invalid_light};f.edited();++f.drawing.id;
    const auto rejected=prepared_render(f.renderer,f.frame,&accepted);
    check(!rejected&&!rejected.error().terminal&&accepted.frame_id==accepted_id,
          "rectangle product overflow uses the same recoverable boundary as World authoring");
    f.lights=lights;f.edited();
    check(f.step().stats.scene_lit_history_resets==0,"numeric input rejection preserves the accepted lighting history domain");
}

void audit_selection_work(Fixture& f) {
    f.frame.view={};f.frame.view.shading=render::Shading::scene_lit;
    auto& options=f.frame.view.lit;
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    f.frame.environment={.05f,.05f,.05f};f.lights.clear();
    f.room.meshes.assign(5,{});
    for(unsigned i=0;i<5;++i) {
        auto& mesh=f.room.meshes[i];mesh.stable_key=100+i;
        mesh.geometry=atelier::geometry::cube_mesh();mesh.color={.2f,.2f,.2f,1};
        mesh.world=box(float(i)*1.5f-3,0,4,1,1,1);
    }
    f.frame.camera={};
    f.frame.camera.projection={.2f,0,0,0, 0,.4f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    ++f.frame.scene_identity;f.edited();
    std::ofstream evidence(f.output/"audit-selection-work.csv");
    evidence<<"aa,selection,captured,scene_draw_calls,orange_pixels\n";
    for(const auto aa:{render::Antialiasing::none,render::Antialiasing::taa}) {
        options.antialiasing=aa;f.frame.camera_cut=true;
        for(unsigned selection=0;selection<3;++selection) {
            for(unsigned i=0;i<5;++i)
                f.room.meshes[i].selected=selection==2 || (selection==1&&(i==1||i==2||i==4));
            f.edited();
            // Selected instances of one geometry share one editing-line draw. Temporal
            // AA needs an additional unjittered depth draw for editing/capture.
            const unsigned runs=selection==0?0:1;
            const auto without_capture=f.step().stats.scene_draw_calls;
            const unsigned editing_depth=aa==render::Antialiasing::taa&&selection!=0?1:0;
            check(without_capture==1+runs+editing_depth,
                  "SceneLit emits one GBuffer draw plus one selected-line draw and only the required editing depth");
            render::Capture image;const auto captured=f.step(&image).stats.scene_draw_calls;
            check(captured==1+runs+(aa==render::Antialiasing::taa?1:0),
                  "capture adds unjittered depth only for temporal AA and reports every actual scene draw");
            std::array<unsigned,5> orange{};
            for(unsigned y=0;y<image.extent.height;++y)for(unsigned x=0;x<image.extent.width;++x) {
                const auto* color=pixel(image,x,y);
                if(color[0]<=225||color[1]<=150||color[2]>=80)continue;
                for(unsigned i=0;i<5;++i) {
                    const auto center=project_region(f.frame.camera,f.frame.scene_viewport,image.extent,
                                                    {float(i)*1.5f-3,0,3.5f},1);
                    const float distance=std::abs(float(x)-float(center.left+1));
                    if(distance<f.frame.scene_viewport.width*.06f)++orange[i];
                }
            }
            unsigned total=0;
            for(unsigned i=0;i<5;++i) {
                total+=orange[i];
                check(f.room.meshes[i].selected?orange[i]>20:orange[i]==0,
                      "selected runs retain visible orange outlines without outlining neighboring unselected instances");
            }
            const std::string name="audit-selection-"+std::to_string(static_cast<unsigned>(aa))+"-"+std::to_string(selection);
            png(image,f.output/(name+".png"));
            evidence<<static_cast<unsigned>(aa)<<','<<selection<<",0,"<<without_capture<<",0\n"
                    <<static_cast<unsigned>(aa)<<','<<selection<<",1,"<<captured<<','<<total<<'\n';
        }
    }
    check(evidence.good(),"selected-run draw and pixel evidence written");
}

void audit_surface_history(Fixture& f) {
    const Room defaults;
    constexpr std::uint64_t front_key=0x100000007ull,back_key=0x200000007ull;
    f.frame.view={};f.frame.view.shading=render::Shading::scene_lit;
    auto& options=f.frame.view.lit;
    options.antialiasing=render::Antialiasing::none;
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    f.frame.environment={};f.lights.assign(1,{});
    f.lights[0].stable_key=100;f.lights[0].kind=render::LightKind::directional;
    f.lights[0].direction={0,0,1};f.lights[0].intensity=4;
    f.room.meshes={defaults.meshes[4],defaults.meshes[4]};
    for(unsigned i=0;i<2;++i) {
        auto& mesh=f.room.meshes[i];mesh.stable_key=i==0?front_key:back_key;
        mesh.world=box(0,0,4+float(i)*.001f,4,3,.1f);
        mesh.color=i==0?ui::Color{1,0,0,1}:ui::Color{0,0,1,1};
        mesh.material={.roughness=1};
    }
    f.frame.camera={};
    f.frame.camera.projection={.2f,0,0,0, 0,.4f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    ++f.frame.scene_identity;f.edited();f.frame.camera_cut=true;
    const auto before=f.capture("audit-di-high-key-front",48);
    // The hidden actor was submitted in every frame: its transform, geometry
    // and material correspondence remain valid when the nearer actor moves.
    f.room.meshes[0].world[12]=10000;f.edited();
    render::Capture revealed;
    check(f.step(&revealed).stats.scene_lit_history_resets==0,
          "revealing an existing stable actor preserves the scene history domain");
    png(revealed,f.output/"audit-di-high-key-revealed.png");
    const auto roi=f.region({0,0,3.951f});
    f.frame.camera_cut=true;
    const auto clean=f.capture("audit-di-high-key-clean");
    options.direct_reference=true;
    const auto raw=f.capture("audit-di-high-key-reference64");
    check(revealed.depth==clean.depth && clean.depth==raw.depth,
          "DI identity isolation and its independent raw reference observe identical geometry");
    const auto old_color=mean_rgb(before,roi),new_color=mean_rgb(revealed,roi);
    const auto clean_color=mean_rgb(clean,roi),raw_color=mean_rgb(raw,roi);
    std::printf("SceneLit high-key DI blue: revealed=%.3f clean=%.3f raw=%.3f; old red=%.3f\n",
                new_color[2],clean_color[2],raw_color[2],old_color[0]);
    check(old_color[0]>30 && raw_color[2]>30,
          "DI identity fixture contains a bright red foreground and independently lit blue revealed receiver");
    for(unsigned c=0;c<3;++c)
        check(std::abs(new_color[c]-clean_color[c])<=1 && std::abs(new_color[c]-raw_color[c])<=1,
              "DI immediately matches reset and raw lighting when only the high surface-key word differs");

    // Reflect a nearly coplanar pair of immutable red/blue emitters. Reusing
    // the red source's anchor through the blue actor leaves forbidden red on
    // the stationary metallic floor, even though both source motions are valid.
    options.direct_reference=false;options.sssr=true;f.lights.clear();
    f.room.meshes={defaults.meshes[0],defaults.meshes[7],defaults.meshes[7]};
    f.room.meshes[0].color={.7f,.7f,.7f,1};
    f.room.meshes[0].material={.roughness=.18f,.metallic=1};
    for(unsigned i=1;i<3;++i) {
        auto& source=f.room.meshes[i];source.stable_key=i==1?front_key:back_key;
        source.world=box(-1.4f,2.1f,6.2f+float(i-1)*.001f,1.6f,1.8f,.1f);
        source.color={0,0,0,1};source.material={.roughness=1};
        source.material.emissive=i==1?Vec3{12,0,0}:Vec3{0,0,12};
    }
    f.frame.camera=camera({static_cast<unsigned>(f.frame.scene_viewport.width),
                           static_cast<unsigned>(f.frame.scene_viewport.height)});
    ++f.frame.scene_identity;f.edited();f.frame.camera_cut=true;
    options.sssr=false;const auto off=f.capture("audit-sssr-high-key-off",2);
    options.sssr=true;const auto red=f.capture("audit-sssr-high-key-front",96);
    std::vector<std::size_t> footprint;
    for(std::size_t i=0;i<red.depth.size();++i)
        if(red.depth[i]>0 && red.depth[i]==off.depth[i] && int(red.rgba[i*4])-int(off.rgba[i*4])>=8)
            footprint.push_back(i);
    check(footprint.size()>=64,"SSSR identity fixture starts with a real red reflection footprint");
    f.room.meshes[1].world[12]=10000;f.edited();
    std::ofstream evidence(f.output/"audit-surface-history.csv");
    evidence<<"frame_after_reveal,reflection_pixels,maximum_red,mean_blue\n";
    for(unsigned frame=0;frame<=8;++frame) {
        render::Capture current;const auto stats=f.step(&current).stats;
        check(stats.scene_lit_history_resets==0,"source disocclusion does not globally reset SSSR histories");
        unsigned red_max=0;double blue=0;
        for(const auto i:footprint) {
            check(current.depth[i]==red.depth[i],"identity response is observed on the unchanged reflective receiver");
            red_max=std::max(red_max,unsigned(current.rgba[i*4]));blue+=current.rgba[i*4+2];
        }
        evidence<<frame<<','<<footprint.size()<<','<<red_max<<','<<blue/double(footprint.size())<<'\n';
        if(frame==0||frame==8)png(current,f.output/("audit-sssr-high-key-after-"+std::to_string(frame)+".png"));
        if(frame==0||frame==8||red_max>1)
            std::printf("SceneLit high-key SSSR frame=%u: pixels=%zu max red=%u mean blue=%.3f\n",
                        frame,footprint.size(),red_max,blue/double(footprint.size()));
        check(red_max<=1,"SSSR rejects the old red source immediately when the revealed source differs only in its high identity word");
    }
    f.frame.camera_cut=true;
    const auto blue=f.capture("audit-sssr-high-key-clean",96);
    double blue_sum=0;for(const auto i:footprint)blue_sum+=blue.rgba[i*4+2];
    check(blue_sum/double(footprint.size())>8,"independently reset SSSR converges to the newly visible blue emitter");
    check(evidence.good(),"surface identity trajectory evidence written");
}

void audit_viewport_depth(Fixture& f) {
    const Room defaults;
    f.window.resize(337,191);f.drawing.extent=take(f.renderer.sync_window_target()).extent;
    check(f.drawing.extent==ui::PixelExtent{337,191},"Hi-Z audit uses a non-power-of-two physical target");
    f.drawing.draws.clear();f.frame.scene_viewport={0,0,337,191};f.frame.camera=camera({337,191});
    f.frame.view={};f.frame.view.shading=render::Shading::scene_lit;
    auto& options=f.frame.view.lit;options.antialiasing=render::Antialiasing::none;
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    f.frame.environment={};f.lights.clear();f.room.meshes={defaults.meshes[0],defaults.meshes[7]};
    f.room.meshes[0].material={.roughness=.18f,.metallic=1};
    f.room.meshes[1].world=box(-1.4f,2.1f,6.2f,1.6f,1.8f,.1f);
    f.room.meshes[1].material.emissive={12,0,0};
    ++f.frame.scene_identity;f.edited();f.frame.camera_cut=true;
    const auto off=f.capture("audit-hiz-337x191-off",2);
    options.sssr=true;const auto reflected=f.capture("audit-hiz-337x191-on",96);
    check(off.depth==reflected.depth,"odd-size reflection leaves the complete public FP32 depth unchanged");
    check(f.compare("audit-hiz-337x191",off,reflected).changed>=16,
          "odd-size Hi-Z traversal produces a real reflection rather than only a fallback image");

    // A large front-facing receiver covers the entire viewport, including its
    // fractional edges. Move/resize it over previously illuminated pixels.
    f.room.meshes={defaults.meshes[4]};auto& wall=f.room.meshes[0];
    wall.world=box(0,0,4,30,30,.1f);wall.color={.4f,.4f,.4f,1};wall.material={.roughness=.18f};
    f.lights=room_lights();f.frame.environment={.1f,.1f,.1f};
    options.ddgi=options.gtao=true;
    options.probe_volumes[0].minimum={-4,-4,0};options.probe_volumes[0].maximum={4,4,8};
    options.probe_volumes[0].counts={3,3,3};options.probe_update_budget=27;options.probe_rays=16;
    f.frame.camera={};f.frame.camera.projection={.2f,0,0,0, 0,.4f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    f.edited();
    constexpr std::array<ui::Rect,4> viewports{{{.25f,.25f,336.5f,190.5f},
        {19.25f,23.75f,171.5f,101.5f},{157.75f,49.25f,131.5f,119.5f},{11.75f,9.25f,307.5f,173.5f}}};
    for(unsigned stage=0;stage<viewports.size();++stage) {
        f.frame.scene_viewport=viewports[stage];
        options.ddgi=options.gtao=options.sssr=stage!=2;
        const auto image=f.capture(("audit-viewport-"+std::to_string(stage)).c_str(),stage==0?16:2);
        const auto v=f.frame.scene_viewport;unsigned lit=0;
        for(unsigned y=0;y<image.extent.height;++y)for(unsigned x=0;x<image.extent.width;++x) {
            const bool inside=float(x)+.5f>=v.x&&float(x)+.5f<v.x+v.width&&
                              float(y)+.5f>=v.y&&float(y)+.5f<v.y+v.height;
            const auto* color=pixel(image,x,y);
            if(!inside)check(depth(image,x,y)==0 && color[0]==0&&color[1]==0&&color[2]==0,
                             "fractional viewport movement and effect toggles leave no old depth or lighting outside the viewport");
            else {
                if(depth(image,x,y)<=0)std::fprintf(stderr,"Missing viewport receiver stage=%u pixel=(%u,%u), rgba=(%u,%u,%u), rect=(%.3f,%.3f,%.3f,%.3f)\n",
                    stage,x,y,color[0],color[1],color[2],v.x,v.y,v.width,v.height);
                check(depth(image,x,y)>0,"fractional viewport includes every covered receiver pixel");lit+=color[0]>5;
            }
        }
        check(lit>1000,"moved viewport retains visible lighting through effect off/on transitions");
    }
    // Decorated windows have a desktop-dependent minimum client width. Remove
    // the frame for the smallest gfx window target, then restore the style.
    const auto window_style=GetWindowLongPtrW(f.window.hwnd,GWL_STYLE);
    check(SetWindowLongPtrW(f.window.hwnd,GWL_STYLE,WS_POPUP|WS_VISIBLE)!=0,
          "remove window decoration for the minimum physical target");
    f.window.resize(8,8);f.drawing.extent=take(f.renderer.sync_window_target()).extent;
    check(f.drawing.extent==ui::PixelExtent{8,8},"Hi-Z audit reaches gfx's minimum window target");
    f.frame.scene_viewport={0,0,1,1};options.ddgi=options.gtao=false;f.lights.clear();f.edited();
    options.sssr=false;const auto single_off=f.capture("audit-hiz-single-view-off",2);
    options.sssr=true;const auto single_on=f.capture("audit-hiz-single-view-on",2);
    check(single_off.depth==single_on.depth && single_on.depth.size()==64 && single_on.depth[0]>0 &&
          std::all_of(single_on.depth.begin()+1,single_on.depth.end(),[](float value){return value==0;}),
          "one-pixel viewport preserves FP32 receiver depth and leaves the remaining target clear");
    for(unsigned c=0;c<3;++c)
        check(std::abs(int(single_off.rgba[c])-int(single_on.rgba[c]))<=1,
              "single-pixel viewport reflection retains its finite environment fallback");
    check(SetWindowLongPtrW(f.window.hwnd,GWL_STYLE,window_style)!=0,"restore the ordinary fixture window style");
}

void quality_audit(Fixture& f) {
    const auto meshes=f.room.meshes;
    const auto lights=f.lights;
    const auto view=f.frame.view;const auto camera=f.frame.camera;const auto viewport=f.frame.scene_viewport;
    const auto environment=f.frame.environment;const auto extent=f.drawing.extent;const auto draws=f.drawing.draws;
    audit_numeric_inputs(f);
    audit_selection_work(f);
    audit_surface_history(f);
    audit_viewport_depth(f);
    f.window.resize(static_cast<int>(extent.width),static_cast<int>(extent.height));
    f.drawing.extent=take(f.renderer.sync_window_target()).extent;
    check(f.drawing.extent==extent,"audit restores the existing contracts target");
    f.room.meshes=meshes;f.lights=lights;f.frame.view=view;f.frame.camera=camera;
    f.frame.scene_viewport=viewport;f.frame.environment=environment;f.drawing.draws=draws;
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    std::puts("Renderer SceneLit audit: PASS (64-bit surface histories, selected runs, odd/single-pixel Hi-Z, fractional viewport lifecycle)");
}
