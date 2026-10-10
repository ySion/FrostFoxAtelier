// Ordinary Renderer frames verify skipped work through completed GPU queries.
void quality_work_elision(Fixture& f) {
    using atelier::geometry::Vertex;
    using atelier::geometry::Mesh;
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
    options.shadows=true;options.shadow_light_budget=1;options.antialiasing=render::Antialiasing::none;
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
        for(auto sample_count:{1u,8u,64u}) {
            options.shadow_samples=sample_count;f.edited();
            const auto first_frame=f.drawing.id+1;
            const auto name=prefix+std::to_string(sample_count);
            const auto image=f.capture(name.c_str(),16);
            const auto color=mean_rgb(image,region);
            const auto samples=completed(name.c_str(),first_frame);
            if(sample_count==1) {
                reference_queries=samples.front().scene_lit_shadow_rays;
                reference_color=color;reference_depth=image.depth;
                check(reference_queries>0 && color[0]>5,"delta-light receiver is lit and actually traces visibility");
            }
            for(const auto& stats:samples)
                check(stats.scene_lit_shadow_rays==reference_queries && stats.scene_lit_probe_rays==0,
                      "delta-light 1/8/64 sample settings trace the same real visibility queries");
            check(image.depth==reference_depth,"delta-light work reduction preserves captured geometry depth");
            for(std::size_t channel=0;channel<3;++channel)
                check(std::abs(color[channel]-reference_color[channel])<=2,
                      "delta-light sample settings preserve settled receiver brightness within two RGB8 levels");
        }
        light.source_radius=kind==render::LightKind::point?.2f:.02f;options.shadow_samples=8;f.edited();
        const auto first_frame=f.drawing.id+1;const auto name=prefix+"finite-8";
        f.capture(name.c_str(),4);
        for(const auto& stats:completed(name.c_str(),first_frame))
            check(stats.scene_lit_shadow_rays>reference_queries*4 && stats.scene_lit_probe_rays==0,
                  "finite-size lights retain distinct sample visibility queries");
    }

    f.lights.clear();f.frame.environment={.05f,.05f,.05f};
    options.shadow_samples=1;options.ddgi=true;
    options.probe_min={-6,-4,0};options.probe_max={6,4,8};options.probe_counts={4,4,4};
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
          "probe visualization remains visible on a zero-diffuse material");
    options.probe_debug=false;
    std::puts("Renderer SceneLit work elision: PASS (delta-light queries and brightness, finite-source samples, zero/tiny diffuse support, SSIL/debug consumers)");
}
