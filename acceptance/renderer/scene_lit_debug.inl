// Real GPU coverage and overlay checks through the ordinary Renderer contract.
void probe_debug_cases(Fixture& f) {
    f.room.meshes.resize(1);
    auto& receiver=f.room.meshes.front();
    receiver.world=box(0,1,4.1f,10,8,.2f);receiver.color={.4f,.4f,.4f,1};
    receiver.material={.roughness=.5f,.metallic=1};
    f.lights.clear();f.frame.environment={0,0,0};f.edited();
    auto& options=f.frame.view.lit;
    options={};options.gtao=options.sssr=options.bloom=false;
    options.antialiasing=render::Antialiasing::smaa;
    options.probe_min={-2,-1,0};options.probe_max={2,3,6};options.probe_counts={3,3,3};
    options.probe_update_budget=1;options.probe_rays=64;options.probe_debug=true;
    options.probe_volume=true;options.probe_visualization=render::ProbeVisualization::state;
    f.frame.meshes={};
    const auto initial_empty=f.capture("debug-00-empty-initial",1);
    check(std::ranges::all_of(initial_empty.depth,[](float value){return value==0;}),"fresh empty diagnostics have no scene picking depth");
    options.probe_volume=false;options.probe_visualization=render::ProbeVisualization::off;f.edited();
    const auto dark_region=f.region({0,1,4}),outside_region=f.region({3,1,4});
    auto pending=f.capture("debug-01-pending",1);
    const auto pending_color=mean_rgb(pending,dark_region);
    check(pending_color[0]>pending_color[2]+80,"unvisited actual receiver support is pending, not reported as dark light");
    options.probe_update_budget=27;
    const auto coverage=f.capture("debug-02-coverage-dark-outside",32);
    const auto dark=mean_rgb(coverage,dark_region),outside=mean_rgb(coverage,outside_region);
    check(dark[1]>dark[0]+70 && dark[2]>dark[0]+70,"valid zero-irradiance receiver support is cyan");
    check(outside[2]>outside[1]+90 && outside[2]>outside[0]+120,"receiver outside the real volume is blue");
    check(pending.depth==coverage.depth,"receiver diagnostics preserve FP32 geometry depth");
    options.probe_debug=false;
    const auto plain=f.capture("debug-03-clean",1);
    check(mean_rgb(plain,dark_region)[1]<2,"disabling coverage immediately restores lighting without diagnostic history");
    options.probe_visualization=render::ProbeVisualization::state;options.probe_volume=true;
    const auto state=f.capture("debug-04-state-depth-tested",1);
    check(difference(plain,state).changed>150,"actual probe spheres and volume bounds are visible");
    check(plain.depth==state.depth,"probe geometry does not alter picking or capture depth");
    options.probe_xray=true;
    const auto xray=f.capture("debug-05-xray",1);
    check(difference(state,xray).changed>50,"X-Ray reveals probes behind the receiver");
    check(plain.depth==xray.depth,"X-Ray uses private depth only");
    options.probe_visualization=render::ProbeVisualization::update_age;
    const auto fresh=f.capture("debug-06-update-all",1);
    options.probe_update_budget=3;
    const auto aged=f.capture("debug-07-update-budget",6);
    check(difference(fresh,aged).changed>100,"actual budgeted evaluation ages change probe colors");
    options.probe_visualization=render::ProbeVisualization::distance;
    f.capture("debug-08-directional-distance",1);
    f.frame.environment={.15f,.06f,.025f};
    options.probe_update_budget=27;options.probe_visualization=render::ProbeVisualization::irradiance;
    f.capture("debug-09-irradiance",48);
    options.probe_relocation=true;
    f.capture("debug-10-relocation",1);
    const auto stable=f.step();
    check(stable.stats.scene_lit_history_resets==0,"display diagnostics do not reset the probe or surface histories");
    options.ddgi=false;options.probe_debug=true;
    const auto unavailable=f.capture("debug-11-ddgi-disabled",1);
    const auto unavailable_color=mean_rgb(unavailable,dark_region);
    check(std::abs(unavailable_color[0]-unavailable_color[1])<2 && std::abs(unavailable_color[1]-unavailable_color[2])<2,
          "disabled DDGI reports unavailable, not stale probe illumination");

    // A new field with a closed solid containing its center exposes real inside classification.
    options.ddgi=true;options.probe_debug=false;options.probe_visualization=render::ProbeVisualization::state;
    auto solid=receiver;solid.stable_key=99;solid.world=box(0,1,3,3.8f,3.8f,3.8f);
    f.room.meshes.push_back(solid);f.edited();
    auto inside=f.capture("debug-12-inside",40);
    std::size_t inside_pixels=0;
    for(std::size_t i=0;i<inside.depth.size();++i)
        inside_pixels+=inside.rgba[i*4]>220 && inside.rgba[i*4+1]<80 && inside.rgba[i*4+2]<80;
    check(inside_pixels>100,"relocated probes that remain inside the closed solid are red");

    // Orthographic projection and jittered modes retain the public editing camera/depth.
    f.frame.camera=camera({static_cast<unsigned>(f.frame.scene_viewport.width),static_cast<unsigned>(f.frame.scene_viewport.height)},true);
    for(auto aa:{render::Antialiasing::taa,render::Antialiasing::smaa_t2x}) {
        options.antialiasing=aa;
        const auto with=f.capture(aa==render::Antialiasing::taa?"debug-13-ortho-taa":"debug-14-ortho-t2x",4);
        options.probe_visualization=render::ProbeVisualization::off;options.probe_volume=options.probe_relocation=false;
        const auto without=f.capture("debug-15-ortho-clean",1);
        check(with.depth==without.depth,"jittered AA and probe overlays share unjittered capture depth");
        check(f.step().stats.scene_lit_history_resets==0,"disabling debug retains temporal history");
        options.probe_visualization=render::ProbeVisualization::state;options.probe_volume=options.probe_relocation=true;
    }
    f.frame.view.shading=render::Shading::matcap;
    const auto studio=f.capture("debug-16-studio",1);
    options.probe_visualization=render::ProbeVisualization::off;options.probe_volume=options.probe_relocation=false;
    const auto studio_clean=f.capture("debug-17-studio-clean",1);
    check(studio.rgba==studio_clean.rgba,"DDGI diagnostics are exclusive to Scene Lit");
    f.frame.view.shading=render::Shading::scene_lit;options.probe_visualization=render::ProbeVisualization::state;options.probe_volume=true;
    f.room.meshes.clear();f.edited();
    f.capture("debug-18-empty-configured-volume",1);
    const auto empty=f.capture("debug-19-empty-repeat",1);
    check(std::ranges::all_of(empty.depth,[](float value){return value==0;}),"empty configured volume has no scene picking depth");
    options.ddgi=false;options.probe_counts={32,8,16};
    f.capture("debug-20-maximum-grid",1);
    SetWindowPos(f.window.hwnd,nullptr,0,0,800,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    f.drawing.extent=take(f.renderer.sync_window_target()).extent;
    f.frame.scene_viewport={24,32,float(f.drawing.extent.width)-48,float(f.drawing.extent.height)-56};
    f.frame.camera=camera({static_cast<unsigned>(f.frame.scene_viewport.width),static_cast<unsigned>(f.frame.scene_viewport.height)},true);
    f.capture("debug-21-resized-empty",1);
    auto invalid=f.frame;invalid.view.lit.probe_radius=0;
    check(!f.renderer.render(invalid),"invalid diagnostic radius is rejected before GPU encoding");
    f.step();
    std::puts("Renderer DDGI diagnostics: PASS (actual coverage, pending/dark/outside/inside states, spheres, X-Ray, ages, irradiance/distance, relocation, AA/orthographic, clean history, empty scene, unchanged FP32 picking depth)");
}
