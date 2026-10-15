// Real Renderer snapshots exercise the shared source table and current/previous
// identity mapping. The reference is independent RIS with the same estimator.
void restir_cases(Fixture& f) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    std::ofstream evidence(f.output/"restir-energy.csv");
    evidence<<"case,realtime_r,realtime_g,realtime_b,reference_r,reference_g,reference_b\n";
    check(evidence.good(),"ReSTIR energy evidence opened");
    std::ofstream spatial_evidence(f.output/"restir-spatial.csv");
    spatial_evidence<<"case,frame_id,retained_sources,nonempty_cells,logical_bytes,uploaded,history_resets\n";
    check(spatial_evidence.good(),"ReSTIR spatial lifecycle evidence opened");
    f.frame.scene_viewport={24,32,320,180};
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.projection={1/(2.5f*aspect),0,0,0, 0,1/2.5f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    f.frame.environment={};f.frame.view.shading=render::Shading::scene_lit;
    f.frame.lighting_focus={};
    auto& options=f.frame.view.lit;options={};
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    options.shadows=true;options.antialiasing=render::Antialiasing::none;
    f.room.meshes.assign(1,{});
    auto& plane=f.room.meshes.front();plane.stable_key=1;plane.world=box(0,0,0,1,1,1);
    plane.geometry=std::make_shared<const Mesh>(std::vector<Vertex>{
        {{-6,-4,4},{0,0,-1}},{{6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},
        {{-6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},{{-6,4,4},{0,0,-1}}});
    plane.color={.5f,.5f,.5f,1};plane.material={.roughness=1};
    f.lights.assign(272,{});
    for(std::size_t i=0;i<f.lights.size();++i) {
        auto& light=f.lights[i];light.stable_key=100+i;
        light.position={-2.f+.04f*float(i%8),2.f+.04f*float((i/8)%8),.5f};
        light.color={1.f,.7f+.025f*float(i%8),.6f+.03f*float(i%8)};
        light.intensity=90.f/272;light.range=30;light.source_radius=0;light.casts_shadow=true;
    }
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    const auto sources=[&](std::size_t expected) {
        const auto stats=f.step().stats;
        check(stats.scene_lit_light_sources==expected && stats.scene_lit_shadow_sources==expected &&
              stats.scene_lit_di_candidates==options.direct_candidates,
              "all sources beyond 256 and all shadow casters beyond four reach the ordinary frame without fallback");
        return stats;
    };
    const auto spatial=[&](const char* name,const render::FrameStats& stats,unsigned retained,unsigned cells) {
        spatial_evidence<<name<<','<<f.drawing.id<<','<<stats.scene_lit_spatial_sources<<','
            <<stats.scene_lit_spatial_cells<<','<<stats.scene_lit_spatial_bytes<<','
            <<stats.scene_lit_spatial_changed<<','<<stats.scene_lit_history_resets<<'\n';
        spatial_evidence.flush();
        check(stats.scene_lit_spatial_sources==retained && stats.scene_lit_spatial_cells==cells,
              "spatial proposal publishes the expected retained sources and nonempty bins");
        check(retained ? stats.scene_lit_spatial_bytes>=32768u+16u*retained &&
                        stats.scene_lit_spatial_bytes<=32768ull+16ull*65536+4ull*stats.scene_lit_light_sources
                       : stats.scene_lit_spatial_bytes==(f.room.meshes.empty()?0u:36u),
              "spatial GPU tables remain bounded, empty proposals use sentinels, and empty scenes retire them");
    };
    const auto compare_endpoint=[&](const char* name,Vec3 point) {
        const auto region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,point,10);
        options.direct_reference=false;
        const auto realtime=lighting_mean(f,(std::string(name)+"-realtime").c_str(),region,96,24);
        options.direct_reference=true;
        const auto reference=lighting_mean(f,(std::string(name)+"-reference64").c_str(),region,1,24);
        evidence<<name;
        for(auto value:realtime)evidence<<','<<value;
        for(auto value:reference)evidence<<','<<value;
        evidence<<'\n';evidence.flush();
        check(reference[0]>10,"ReSTIR energy comparison samples a lit receiver");
        for(unsigned channel=0;channel<3;++channel)
            check(std::abs(realtime[channel]-reference[channel])<=std::max(3.,reference[channel]*.06),
                  "settled realtime mean agrees with independent reference64 at the current scene endpoint");
        options.direct_reference=false;
        f.step(); // Accept the diagnostic transition before the next scene edit.
        return realtime;
    };
    spatial("top32-overflow",sources(272),32,1);
    const auto initial=compare_endpoint("restir-272-sources",{0,0,4});
    options.spatial_light_sampling=false;
    const auto global_only=compare_endpoint("restir-272-global-only",{0,0,4});
    for(unsigned channel=0;channel<3;++channel)
        check(std::abs(initial[channel]-global_only[channel])<=std::max(3.,global_only[channel]*.06),
              "ordinary DI with top32 spatial proposals agrees with ordinary global-only DI");
    options.spatial_light_sampling=true;
    auto stable=f.step().stats;spatial("stable",stable,32,1);
    check(!stable.scene_lit_spatial_changed,"unchanged source grid does not upload again after the diagnostic toggle");
    const auto stable_bytes=stable.scene_lit_spatial_bytes;
    f.frame.lighting_focus={.5f,.5f,.5f};
    stable=f.step().stats;spatial("same-cell-focus",stable,32,1);
    check(!stable.scene_lit_spatial_changed && stable.scene_lit_spatial_bytes==stable_bytes &&
          stable.scene_lit_history_resets==0,"focus movement within the snapped cell preserves spatial tables and history");
    f.frame.lighting_focus={4096,4096,4096};
    const auto away=f.step().stats;spatial("focus-away",away,0,0);
    check(away.scene_lit_spatial_changed && away.scene_lit_history_resets==0,
          "rolling focus outside all sources uploads an empty proposal without resetting the scene domain");
    const auto outside=compare_endpoint("restir-focus-away-global-fallback",{0,0,4});
    for(unsigned channel=0;channel<3;++channel)
        check(std::abs(initial[channel]-outside[channel])<=std::max(3.,initial[channel]*.06),
              "receiver outside the local grid retains its global-supported illumination");
    f.frame.lighting_focus={};
    const auto returned=f.step().stats;spatial("focus-return",returned,32,1);
    check(returned.scene_lit_spatial_changed && returned.scene_lit_history_resets==0,
          "returning focus rebuilds spatial tables without resetting the scene domain");
    const auto original_position=f.lights.back().position;
    f.lights.back().position={9,2,.5f};f.edited();
    const auto moved=f.step().stats;spatial("light-cross-cell",moved,33,2);
    check(moved.scene_lit_spatial_changed && moved.scene_lit_history_resets==0,
          "moving one light between bins updates the proposal while preserving the scene domain");
    compare_endpoint("restir-light-cross-cell",{0,0,4});
    f.lights.back().position=original_position;f.edited();
    spatial("light-return",f.step().stats,32,1);
    std::ranges::reverse(f.lights);f.edited();sources(272);
    const auto reordered=compare_endpoint("restir-reordered",{0,0,4});
    for(unsigned channel=0;channel<3;++channel)
        check(std::abs(initial[channel]-reordered[channel])<=3,"source reorder preserves long-term receiver energy");
    const auto removed=f.lights.back();f.lights.pop_back();
    auto inserted=removed;inserted.stable_key=9000;inserted.position={1,-1,0};inserted.color={.2f,.4f,1};inserted.intensity=40;
    f.lights.insert(f.lights.begin()+37,inserted);f.edited();sources(272);
    compare_endpoint("restir-delete-insert",{0,0,4});
    for(unsigned i=0;i<16;++i) {
        f.lights[37].position[0]+=.1f;f.room.meshes[0].world[14]+=.0625f;f.edited();
        check(f.step().stats.scene_lit_history_resets==0,"light and receiver motion keep their established scene domain");
    }
    compare_endpoint("restir-light-receiver-motion",{0,0,5});
    // Every local sample must consult visibility. A screen-offset blocker casts
    // a shadow onto a still-visible receiver, including sources above the old cap.
    render::MeshInstance blocker;blocker.stable_key=2;blocker.geometry=atelier::geometry::cube_mesh();
    blocker.world=box(-1,1,2,1.2f,1.2f,.4f);blocker.color={.5f,.5f,.5f,1};
    f.room.meshes.push_back(blocker);f.room.meshes[0].world[14]=0;
    f.lights.erase(f.lights.begin()+37);f.edited();sources(271);
    const auto shadow_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},3);
    options.direct_reference=true;options.shadows=false;
    const auto unshadowed=lighting_mean(f,"restir-all-sources-no-shadow",shadow_region,1,16);
    options.shadows=true;
    const auto shadowed=lighting_mean(f,"restir-all-sources-shadow",shadow_region,1,16);
    check(unshadowed[0]>10 && shadowed[0]<unshadowed[0]*.2,
          "more than 256 local sources retain geometric shadows instead of contributing unshadowed fallback");
    f.lights.clear();f.edited();options.direct_reference=false;spatial("zero-sources",sources(0),0,0);
    const auto dark=lighting_mean(f,"restir-all-sources-deleted",shadow_region,96,8);
    check(*std::max_element(dark.begin(),dark.end())<=1,"deleting the complete source set removes stale reservoir radiance");

    // A source whose anchor lies outside the grid still illuminates the receiver.
    // Empty local support must select the global proposal, never discard it.
    f.room.meshes.resize(1);
    render::Light distant;distant.stable_key=12000;distant.position={0,0,-80};
    distant.intensity=6000;distant.range=200;distant.source_radius=0;distant.casts_shadow=true;
    f.lights={distant};f.edited();spatial("far-source",sources(1),0,0);
    const auto far_only=compare_endpoint("restir-far-source-global-fallback",{0,0,4});
    // This nearby source occupies the receiver's proposal neighborhood but its
    // finite range cannot reach the plane. All measured energy must therefore
    // come from the remote source through the retained global support.
    render::Light nearby;nearby.stable_key=12001;nearby.position={1,1,.5f};
    nearby.intensity=90;nearby.range=1;nearby.source_radius=0;nearby.casts_shadow=true;
    f.lights.push_back(nearby);f.edited();spatial("near-zero-far-main",sources(2),1,1);
    const auto mixed=compare_endpoint("restir-near-zero-far-main-spatial",{0,0,4});
    options.spatial_light_sampling=false;
    const auto mixed_global=compare_endpoint("restir-near-zero-far-main-global",{0,0,4});
    options.spatial_light_sampling=true;
    const auto candidate_count=options.direct_candidates;options.direct_candidates=1;
    const auto single=compare_endpoint("restir-near-zero-far-main-single-candidate",{0,0,4});
    options.direct_candidates=candidate_count;
    for(unsigned channel=0;channel<3;++channel) {
        const auto tolerance=std::max(3.,far_only[channel]*.06);
        check(std::abs(mixed[channel]-far_only[channel])<=tolerance &&
              std::abs(mixed_global[channel]-far_only[channel])<=tolerance &&
              std::abs(single[channel]-far_only[channel])<=tolerance,
              "zero-contribution local proposals preserve remote-light energy with spatial, global and one-candidate DI");
    }
    const auto receiver_copy=f.room.meshes.front();
    f.room.meshes.clear();f.lights.clear();f.edited();render::Capture empty;
    spatial("scene-release",f.step(&empty).stats,0,0);
    check(std::ranges::all_of(empty.depth,[](float value){return value==0;}),
          "empty scene retires spatial resources and captures background depth through Renderer");
    f.room.meshes={receiver_copy};distant.position={0,0,.5f};distant.intensity=90;
    f.lights={distant};f.edited();spatial("scene-rebuild",sources(1),1,1);
    compare_endpoint("restir-spatial-scene-rebuild",{0,0,4});

    // Sunlight is evaluated separately at DDGI hits. Adding an irrelevant local
    // pool must not amplify the sun by that pool's inverse selection probability.
    f.room=Room{};f.lights={room_lights()[0]};f.lights[0].source_radius=0;
    f.room.meshes[0].material={.roughness=1};f.room.meshes[7].material.emissive={};
    f.frame.camera=camera({320,180});options={};
    options.gtao=options.sssr=options.ssil=options.bloom=false;
    options.antialiasing=render::Antialiasing::none;
    options.probe_volumes[0].minimum={-5,-1,-1};options.probe_volumes[0].maximum={5,5,8};
    options.probe_volumes[0].counts={8,5,8};options.probe_update_budget=320;options.probe_rays=64;
    f.frame.environment={};++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    const auto bounce_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{-3.15f,.01f,1.f},2);
    options.ddgi=false;
    const auto direct=lighting_mean(f,"restir-ddgi-sun-direct",bounce_region,32,16);
    options.ddgi=true;
    const auto sun=lighting_mean(f,"restir-ddgi-sun-only",bounce_region,192,24);
    for(unsigned i=0;i<96;++i) {
        render::Light irrelevant;irrelevant.stable_key=1000+i;irrelevant.position={1000.f+float(i),1000,1000};
        irrelevant.range=1;irrelevant.intensity=100;irrelevant.source_radius=0;irrelevant.casts_shadow=true;
        f.lights.push_back(irrelevant);
    }
    ++f.frame.scene_identity;f.edited();sources(97);
    const auto with_locals=lighting_mean(f,"restir-ddgi-sun-96-irrelevant-locals",bounce_region,192,24);
    evidence<<"ddgi-sun-with-96-irrelevant-locals";
    for(auto value:with_locals)evidence<<','<<value;
    for(auto value:sun)evidence<<','<<value;
    evidence<<'\n';
    check(sun[0]-direct[0]>2,"sun-only fixture contains measurable DDGI bounce energy");
    for(unsigned channel=0;channel<3;++channel)
        check(std::abs(with_locals[channel]-sun[channel])<=std::max(3.,std::abs(sun[channel]-direct[channel])*.2),
              "96 noncontributing local lights preserve the sun's converged DDGI energy");
    check(evidence.good(),"ReSTIR lifecycle and DDGI energy evidence written");
    check(spatial_evidence.good(),"ReSTIR spatial proposal lifecycle evidence written");
    std::puts("Renderer ReSTIR DI: PASS (272 shadow sources, spatial top32/global fallback/rolling/lifetime, independent reference means, reorder/insert/delete/motion, sun-only DDGI energy)");
}
