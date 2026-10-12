// Real GPU coverage and overlay checks through the ordinary Renderer contract.
void probe_volume_cases(Fixture& f) {
    f.room.meshes.resize(1);
    auto& receiver=f.room.meshes.front();
    receiver.world=box(0,1,4.1f,10,8,.2f);receiver.color={.4f,.4f,.4f,1};
    receiver.material={.roughness=.8f,.metallic=0};
    f.lights.clear();f.frame.environment={.15f,.2f,.3f};f.edited();
    auto& s=f.frame.view.lit;s={};s.gtao=s.sssr=s.bloom=false;
    s.antialiasing=render::Antialiasing::none;s.probe_bounce=0;s.probe_debug=true;
    s.probe_update_budget=128;s.probe_rays=32;
    auto& fine=s.probe_volumes[0];fine.minimum={-2,-1,0};fine.maximum={2,3,6};fine.counts={5,5,7};
    const auto near_region=f.region({0,1,4}),far_region=f.region({3,1,4});
    const auto single=f.capture("volumes-00-single",32);
    check(mean_rgb(single,far_region)[2]>mean_rgb(single,far_region)[1]+90,"single-volume missing receiver support is explicit");
    auto& coarse=s.probe_volumes[1];coarse=fine;coarse.minimum={-5,-1,0};coarse.maximum={5,3,6};coarse.counts={11,5,7};
    const auto multiple=f.capture("volumes-01-covered",32);
    check(mean_rgb(multiple,far_region)[1]>mean_rgb(multiple,far_region)[2]+40,"second volume supplies actual visible receiver support");
    s.probe_coverage=render::ProbeCoverage::volume;
    const auto sources=f.capture("volumes-02-sources");
    const auto inner=mean_rgb(sources,near_region),outer=mean_rgb(sources,far_region);
    check(inner[2]>inner[0]+80 && outer[0]>outer[2]+80,"stable slot priority selects near_region and fallback volumes");
    s.probe_coverage=render::ProbeCoverage::contributors;f.capture("volumes-03-contributors");
    s.probe_coverage=render::ProbeCoverage::dominance;f.capture("volumes-04-dominance");
    auto state=f.step().stats;
    check(state.scene_lit_probe_volumes[0].resident==175 && state.scene_lit_probe_volumes[1].resident==385,
          "frame statistics describe actual per-volume residency");
    check(state.scene_lit_probe_volumes[0].updated+state.scene_lit_probe_volumes[1].updated==128,
          "volumes share one update budget");

    fine.placement=render::ProbePlacement::follow_focus;fine.blend_cells=1;
    f.capture("volumes-05-follow-warm",32);
    state=f.step().stats;const auto origin=state.scene_lit_probe_volumes[0].minimum;
    f.frame.lighting_focus={.4f,0,0};state=f.step().stats;
    check(state.scene_lit_probe_volumes[0].minimum==origin && state.scene_lit_probe_volumes[0].reset_probes==0,
          "subcell focus motion preserves world grid and all history identities");
    f.frame.lighting_focus={1.2f,0,0};state=f.step().stats;
    check(state.scene_lit_probe_volumes[0].reset_probes==35 && state.scene_lit_probe_volumes[1].reset_probes==0,
          "one-cell scroll invalidates only the entering slice");
    check(f.step().stats.scene_lit_probe_volumes[0].reset_probes==0,"repeated focus is idempotent");
    f.frame.lighting_focus={-1.1f,-1.1f,1.2f};state=f.step().stats;
    check(state.scene_lit_probe_volumes[0].reset_probes==139,"negative multi-axis scroll clears the entering union exactly once");
    f.frame.lighting_focus={100,0,0};state=f.step().stats;
    check(state.scene_lit_probe_volumes[0].reset_probes==175,"teleport invalidates the entire reused volume");
    s.probe_coverage=render::ProbeCoverage::volume;
    const auto fallback=f.capture("volumes-06-teleport-fallback",4);
    check(mean_rgb(fallback,near_region)[0]>mean_rgb(fallback,near_region)[2]+80,"remote fine window leaves supported coarse lighting");
    f.frame.lighting_focus={0,0,0};f.capture("volumes-07-return",32);

    // Dark samples own their layer. An unchanged scene with no incident light
    // must remain supported rather than exposing a brightness-based fallback.
    f.frame.environment={0,0,0};++f.frame.scene_identity;s.probe_coverage=render::ProbeCoverage::status;
    const auto dark=f.capture("volumes-08-valid-dark",32);
    check(mean_rgb(dark,near_region)[1]>mean_rgb(dark,near_region)[0]+70 && mean_rgb(dark,near_region)[2]>mean_rgb(dark,near_region)[0]+70,
          "multivolume valid darkness remains distinct from missing support");
    f.frame.environment={.15f,.2f,.3f};++f.frame.scene_identity;s.probe_debug=false;
    const auto lit=f.capture("volumes-09-gain-one",32);
    s.indirect_gain=0;const auto zero=f.capture("volumes-10-gain-zero",2);
    check(mean_rgb(lit,near_region)[0]>mean_rgb(zero,near_region)[0]+20,"indirect gain controls only final composition");
    state=f.step().stats;
    check(state.scene_lit_probe_volumes[0].reset_probes==0 && state.scene_lit_probe_volumes[1].reset_probes==0,
          "artistic gain does not recreate probe histories");
    s.indirect_gain=1;s.probe_bounce=.85f;
    state=f.step().stats;
    check(state.scene_lit_probe_volumes[0].reset_probes==175 && state.scene_lit_probe_volumes[1].reset_probes==385,
          "transport changes invalidate the previous transport solution");
    s.probe_feedback_visibility=false;state=f.step().stats;
    check(state.scene_lit_probe_volumes[0].reset_probes==175,"feedback visibility comparison starts a clean field");
    f.capture("volumes-11-feedback-cached",48);
    s.probe_feedback_visibility=true;f.capture("volumes-12-feedback-visible",48);

    auto invalid=f.frame;invalid.view.lit.probe_volumes[2].enabled=true;invalid.view.lit.probe_volumes[2].counts={32,32,32};
    const auto rejected=f.renderer.render(invalid);
    check(!rejected && !rejected.error().terminal,"shared resident limit rejects atomically before encoding");
    invalid.view=f.frame.view;invalid.view.lit.probe_volumes[0].counts[1]=4;
    check(!f.renderer.render(invalid),"following volume requires an interior beyond its transition border");
    invalid.view=f.frame.view;invalid.view.lit.probe_volumes[0].minimum={-.004f,-.004f,-.004f};
    invalid.view.lit.probe_volumes[0].maximum={.004f,.004f,.004f};invalid.view.lit.probe_volumes[0].counts={5,5,5};
    invalid.lighting_focus={100000,0,0};
    check(!f.renderer.render(invalid),"following grid cannot collapse below actual world float precision");
    f.step();
    s.probe_debug=true;s.probe_volume=true;s.probe_visualization=render::ProbeVisualization::state;
    s.probe_debug_volume=1;f.capture("volumes-13-selected-overlay",2);
    for(auto& v:s.probe_volumes)v.enabled=false;
    const auto disabled=f.capture("volumes-14-no-volumes",2);
    check(mean_rgb(disabled,near_region)[0]>70 && mean_rgb(disabled,near_region)[0]<150,"empty volume set is explicit unavailable support");
    fine.enabled=true;fine.placement=render::ProbePlacement::fixed;fine.blend_cells=0;s.probe_debug_volume=-1;
    s.probe_volume=false;s.probe_visualization=render::ProbeVisualization::off;
    f.capture("volumes-15-reenabled",32);
    check(f.step().stats.scene_lit_probe_volumes[0].reset_probes==0,"disable and reenable completes a fresh stable lifetime");
    // The local cache is much smaller than the enclosing room. A ray that
    // leaves the cache still has to hit the distant enclosure before sky.
    for(unsigned axis=0;axis<3;++axis)for(float side:{-1.f,1.f}) {
        render::MeshInstance wall;wall.geometry=atelier::geometry::cube_mesh();
        std::array<float,3> center{},size{42,42,42};center[axis]=side*20;size[axis]=1;
        wall.world=box(center[0],center[1],center[2],size[0],size[1],size[2]);
        wall.color={.4f,.4f,.4f,1};wall.stable_key=1000+f.room.meshes.size();
        f.room.meshes.push_back(wall);
    }
    s.probe_bounce=0;s.probe_coverage=render::ProbeCoverage::status;
    ++f.frame.scene_identity;f.edited();
    const auto enclosed=f.capture("volumes-16-distant-enclosure",40);
    check(mean_rgb(enclosed,near_region)[1]>mean_rgb(enclosed,near_region)[0]+70 &&
          mean_rgb(enclosed,near_region)[2]>mean_rgb(enclosed,near_region)[0]+70,
          "small cache cannot treat distant scene occluders as a sky miss");
    std::puts("Renderer DDGI volumes: PASS (shared budget, real coverage, source/contributor/dominance diagnostics, rolling retention, negative/multiaxis/teleport, valid dark, transport/gain separation, feedback policy, resource lifecycle)");
}

void probe_debug_cases(Fixture& f) {
    f.room.meshes.resize(1);
    auto& receiver=f.room.meshes.front();
    receiver.world=box(0,1,4.1f,10,8,.2f);receiver.color={.4f,.4f,.4f,1};
    receiver.material={.roughness=.5f,.metallic=1};
    f.lights.clear();f.frame.environment={0,0,0};f.edited();
    auto& options=f.frame.view.lit;
    options={};options.gtao=options.sssr=options.bloom=false;
    options.antialiasing=render::Antialiasing::smaa;
    options.probe_volumes[0].minimum={-2,-1,0};options.probe_volumes[0].maximum={2,3,6};options.probe_volumes[0].counts={3,3,3};
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
    options.ddgi=false;options.probe_volumes[0].counts={32,8,16};
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
