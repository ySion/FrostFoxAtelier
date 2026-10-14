// Ordinary Renderer frames verify skipped work through completed GPU queries.
void quality_work_elision(Fixture& f) {
    using atelier::geometry::Vertex;
    using atelier::geometry::Mesh;
    f.frame.scene_viewport={24,32,320,180};
    std::ofstream evidence(f.output / "work-elision-gpu.csv");
    evidence << "case,submission,case_first_frame,completed_sequence,completed_frame,current_logical_resource_bytes,visibility_queries,probe_queries,ddgi_hit_visibility,ddgi_gather_visibility\n";
    check(evidence.good(), "work-elision GPU evidence opened");
    std::uint64_t previous_sequence=0;
    const auto completed=[&](const char* name,std::uint64_t first_frame) {
        std::array<render::FrameStats,3> samples{};
        std::size_t count=0;
        for(unsigned attempt=0;attempt<80 && count<samples.size();++attempt) {
            const auto stats=f.step().stats;
            if(!stats.scene_lit_gpu_timing_valid || stats.scene_lit_gpu_sample_sequence<=previous_sequence ||
               stats.scene_lit_gpu_sample_frame_id<first_frame) continue;
            check(stats.scene_lit_gpu_sample_frame_id<=f.drawing.id,
                  "work-elision completion belongs to an already submitted case frame");
            const auto& options=f.frame.view.lit;
            const auto active=[&](unsigned bit){return (stats.scene_lit_gpu_pass_mask&(1u<<bit))!=0;};
            check(active(10)==!options.direct_reference && active(11)==!options.direct_reference && active(12)&&active(13),
                  "completed direct scope mask distinguishes reference bypass from its visibility and resolve work");
            const bool probes=options.ddgi&&std::ranges::any_of(options.probe_volumes,[](const auto& v){return v.enabled;});
            check(active(3)==options.ddgi && active(8)==probes && active(9)==options.ddgi && active(14)==probes && active(15)==probes,
                  "completed DDGI scope mask follows this case instead of delayed previous settings");
            for(float value:stats.scene_lit_direct_gpu_ms)check(std::isfinite(value)&&value>=0,"direct subpass duration is finite");
            for(float value:stats.scene_lit_ddgi_update_gpu_ms)check(std::isfinite(value)&&value>=0,"DDGI update subpass duration is finite");
            previous_sequence=stats.scene_lit_gpu_sample_sequence;samples[count++]=stats;
            evidence << name << ',' << f.drawing.id << ',' << first_frame << ',' << previous_sequence << ','
                     << stats.scene_lit_gpu_sample_frame_id << ',' << stats.scene_lit_resource_bytes << ','
                     << stats.scene_lit_shadow_rays << ',' << stats.scene_lit_probe_rays << ','
                     << stats.scene_lit_ddgi_visibility_rays[0] << ',' << stats.scene_lit_ddgi_visibility_rays[1] << '\n';
        }
        check(count==samples.size(), "work-elision receives three distinct completed samples from the current case");
        evidence.flush();check(evidence.good(), "work-elision GPU evidence written");
        return samples;
    };

    f.room.meshes.assign(1,{});
    auto& receiver=f.room.meshes.front();
    receiver.stable_key=1;receiver.world=box(0,0,0,1,1,1);
    receiver.geometry=std::make_shared<const Mesh>(std::vector<Vertex>{
        {{-6,-4,4},{0,0,-1}},{{6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},
        {{-6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},{{-6,4,4},{0,0,-1}}});
    receiver.color={.4f,.4f,.4f,1};receiver.material={.roughness=1};
    f.lights.assign(1,{});auto& light=f.lights.front();
    light.stable_key=100;light.position={0,0,.5f};light.range=30;light.casts_shadow=true;
    f.frame.environment={};f.frame.view.shading=render::Shading::scene_lit;
    auto& options=f.frame.view.lit;options={};
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    options.shadows=true;options.antialiasing=render::Antialiasing::none;
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.projection={1/(2.5f*aspect),0,0,0, 0,1/2.5f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    ++f.frame.scene_identity;f.edited();f.frame.camera_cut=true;
    const auto region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},12);

    for(auto kind:{render::LightKind::point,render::LightKind::directional}) {
        light.kind=kind;light.source_radius=0;light.intensity=kind==render::LightKind::point?30.f:2.f;
        const std::string prefix=kind==render::LightKind::point?"work-point-":"work-directional-";
        std::uint64_t reference_queries=0;
        std::array<double,3> reference_color{};
        std::vector<float> reference_depth;
        for(auto candidate_count:{1u,8u,32u}) {
            options.direct_candidates=candidate_count;options.direct_reference=false;f.edited();
            const auto first_frame=f.drawing.id+1;
            const auto name=prefix+std::to_string(candidate_count);
            const auto image=f.capture(name.c_str(),16);
            const auto color=mean_rgb(image,region);
            const auto samples=completed(name.c_str(),first_frame);
            if(candidate_count==1) {
                reference_queries=samples.front().scene_lit_shadow_rays;
                reference_color=color;reference_depth=image.depth;
                check(reference_queries>0 && color[0]>5,"delta-light receiver is lit and actually traces visibility");
            }
            for(const auto& stats:samples)
                check(stats.scene_lit_shadow_rays==reference_queries && stats.scene_lit_probe_rays==0,
                      "delta-light 1/8/32 candidates trace one selected visibility ray per receiver");
            check(image.depth==reference_depth,"delta-light work reduction preserves captured geometry depth");
            for(std::size_t channel=0;channel<3;++channel)
                check(std::abs(color[channel]-reference_color[channel])<=2,
                      "delta-light candidate settings preserve settled receiver brightness within two RGB8 levels");
        }
        options.direct_reference=true;f.edited();
        auto first_frame=f.drawing.id+1;auto name=prefix+"delta-reference64";
        f.capture(name.c_str(),4);
        for(const auto& stats:completed(name.c_str(),first_frame))
            check(stats.scene_lit_shadow_rays==reference_queries*(kind==render::LightKind::point?64u:1u) &&
                  stats.scene_lit_probe_rays==0,
                  "reference traces 64 independent local RIS estimates; delta directionals retain one exact visibility query");
        light.source_radius=kind==render::LightKind::point?.2f:.02f;
        options.direct_candidates=8;options.direct_reference=false;f.edited();
        first_frame=f.drawing.id+1;name=prefix+"finite-8";
        f.capture(name.c_str(),4);
        for(const auto& stats:completed(name.c_str(),first_frame))
            check(stats.scene_lit_shadow_rays==reference_queries && stats.scene_lit_probe_rays==0,
                  "finite-size realtime source traces only its selected sample, independent of candidate count");
        options.direct_reference=true;f.edited();first_frame=f.drawing.id+1;name=prefix+"finite-reference64";
        f.capture(name.c_str(),4);
        for(const auto& stats:completed(name.c_str(),first_frame))
            check(stats.scene_lit_shadow_rays==reference_queries*64 && stats.scene_lit_probe_rays==0,
                  "finite-source reference traces 64 independent visible estimates without reuse");
    }

    // Cover the physical last row/column of both a partial 64-pixel reference
    // tile and a partial 8-pixel compute group, with less than 65K receivers.
    const auto original_extent=f.drawing.extent;
    const auto original_viewport=f.frame.scene_viewport;
    const auto original_camera=f.frame.camera;
    f.window.resize(337,191);f.drawing.extent=take(f.renderer.sync_window_target()).extent;
    check(f.drawing.extent==ui::PixelExtent{337,191},"reference tail case uses the exact nonaligned render-target extent");
    f.frame.scene_viewport={0,0,337,191};
    f.frame.camera.projection[0]=1/(2.5f*(337.f/191.f));
    light.kind=render::LightKind::point;light.source_radius=0;light.intensity=30;
    options.direct_reference=true;f.frame.camera_cut=true;f.edited();
    auto tail_first_frame=f.drawing.id+1;
    const auto tail_reference=f.capture("work-point-reference64-337x191");
    check(std::ranges::all_of(tail_reference.depth,[](float value){return value>0;}),
          "reference tail case has a receiver at every physical pixel including the last row and column");
    constexpr std::uint64_t tail_pixels=337u*191u;
    for(const auto& stats:completed("work-point-reference64-337x191",tail_first_frame))
        check(stats.scene_lit_shadow_rays==tail_pixels*64 && stats.scene_lit_probe_rays==0,
              "partial reference tiles trace exactly 64 independent point visibility queries for every receiver");
    options.direct_reference=false;f.edited();tail_first_frame=f.drawing.id+1;
    const auto tail_realtime=f.capture("work-point-realtime-337x191");
    check(tail_realtime.depth==tail_reference.depth,"reference-to-realtime transition preserves complete tail geometry coverage");
    for(const auto& stats:completed("work-point-realtime-337x191",tail_first_frame))
        check(stats.scene_lit_shadow_rays==tail_pixels && stats.scene_lit_probe_rays==0,
              "realtime dispatch resets its origin and shades every physical pixel exactly once after tiled reference");
    for(unsigned x=0;x<337;++x)
        check(pixel(tail_reference,x,190)[0]>5 && pixel(tail_realtime,x,190)[0]>5,"both estimators illuminate the physical last row");
    for(unsigned y=0;y<191;++y)
        check(pixel(tail_reference,336,y)[0]>5 && pixel(tail_realtime,336,y)[0]>5,"both estimators illuminate the physical last column");
    f.window.resize(static_cast<int>(original_extent.width),static_cast<int>(original_extent.height));
    f.drawing.extent=take(f.renderer.sync_window_target()).extent;
    check(f.drawing.extent==original_extent,"tail case restores the render target before DDGI lifecycle cases");
    f.frame.scene_viewport=original_viewport;f.frame.camera=original_camera;f.frame.camera_cut=true;f.edited();

    f.lights.clear();f.frame.environment={.05f,.05f,.05f};
    options.direct_reference=false;options.ddgi=true;
    options.probe_volumes[0].minimum={-6,-4,0};options.probe_volumes[0].maximum={6,4,8};options.probe_volumes[0].counts={4,4,4};
    options.probe_update_budget=64;options.probe_rays=64;
    const auto gather_case=[&](const char* name,bool needs_gather) {
        f.edited();const auto first_frame=f.drawing.id+1;
        const auto image=f.capture(name,32);
        for(const auto& stats:completed(name,first_frame)) {
            check(stats.scene_lit_probe_rays>0,"DDGI work elision retains real probe updates");
            check(needs_gather?stats.scene_lit_ddgi_visibility_rays[1]>0:stats.scene_lit_ddgi_visibility_rays[1]==0,
                  "DDGI receiver queries match exact material support and current consumers");
        }
        return image;
    };
    receiver.material.metallic=1;
    gather_case("work-ddgi-metal",false);
    options.ssil=true;
    gather_case("work-ddgi-metal-ssil",true);
    options.ssil=false;receiver.material.metallic=0;receiver.color={0,0,0,1};
    gather_case("work-ddgi-black",false);
    receiver.color={1e-5f,1e-5f,1e-5f,1};
    gather_case("work-ddgi-tiny-nonzero",true);
    receiver.color={.4f,.4f,.4f,1};receiver.material.metallic=1;options.probe_debug=true;
    const auto debug=gather_case("work-ddgi-metal-debug",false);
    const auto debug_color=mean_rgb(debug,region);
    check(*std::max_element(debug_color.begin(),debug_color.end())>5,
          "receiver coverage remains visible on a zero-diffuse material without changing lighting gather counters");
    options.probe_debug=false;
    // The sole open plane cannot classify probes as inside. Its nearest grid
    // layers are 4/3 m away, beyond relocation clearance, so all 64 probes can
    // issue illumination rays. Change the per-dispatch stride and scheduled
    // count in place; no new scene domain can hide stale scratch indexing.
    receiver.material.metallic=0;options.probe_bounce=0;
    for(const auto budget_rays:std::array{std::array{1u,1u},std::array{64u,256u},
                                         std::array{3u,7u},std::array{64u,64u}}) {
        options.probe_update_budget=budget_rays[0];options.probe_rays=budget_rays[1];f.edited();
        const auto first_frame=f.drawing.id+1;
        const auto name="work-ddgi-span-"+std::to_string(budget_rays[0])+"x"+std::to_string(budget_rays[1]);
        for(const auto& stats:completed(name.c_str(),first_frame))
            check(stats.scene_lit_probe_rays==std::uint64_t(budget_rays[0])*(32u+budget_rays[1]),
                  "growing and shrinking DDGI update spans trace exactly 32 fixed rays plus the requested illumination rays per scheduled probe");
    }
    // Populate scratch with real surface hits that perform lighting visibility
    // before making every probe inactive. The light is outside the later solid,
    // so stale hit shading would remain observable in the hit-visibility counter.
    f.lights.assign(1,{});auto& outside_light=f.lights.front();
    outside_light.stable_key=100;outside_light.position={0,0,-30};outside_light.range=100;
    outside_light.intensity=3000;outside_light.casts_shadow=true;f.frame.environment={};f.edited();
    auto first_frame=f.drawing.id+1;
    for(const auto& stats:completed("work-ddgi-active-hits",first_frame))
        check(stats.scene_lit_probe_rays==64u*(32u+64u) && stats.scene_lit_ddgi_visibility_rays[0]>0,
              "active probe scratch contains real receiver hits with measured lighting visibility");
    render::MeshInstance enclosure;enclosure.stable_key=99;enclosure.geometry=atelier::geometry::cube_mesh();
    enclosure.world=box(0,0,4,40,40,40);enclosure.color={.4f,.4f,.4f,1};
    // Positive-scale closed cube uses outward authored normals. The entire
    // [-6,6]x[-4,4]x[0,8] volume and every allowed relocation remain inside it.
    f.room.meshes.push_back(enclosure);f.edited();first_frame=f.drawing.id+1;
    for(const auto& stats:completed("work-ddgi-all-inactive",first_frame))
        check(stats.scene_lit_probe_rays==64u*32u && stats.scene_lit_ddgi_visibility_rays[0]==0,
              "inactive probes retain fixed classification rays but skip illumination tracing and scratch-hit shading");
    const auto enclosed=f.capture("work-ddgi-inactive-dark",32);
    const auto enclosed_color=mean_rgb(enclosed,region);
    check(*std::max_element(enclosed_color.begin(),enclosed_color.end())<=1,
          "inactive probes do not leak prior surface radiance into the enclosed receiver");
    std::puts("Renderer SceneLit work elision: PASS (candidate-independent selected visibility, independent reference64 rays, zero/tiny diffuse support, SSIL/debug consumers, DDGI span resize and inactive scratch lifecycle)");
}
