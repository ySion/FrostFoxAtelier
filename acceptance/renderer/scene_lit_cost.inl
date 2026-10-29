// Real Renderer consumers compare exact visibility reuse with an independent
// continuously invalidated view. Camera cuts retain the world probe transport.
void cost_cases(Fixture& f) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    f.window.resize(352,224);f.drawing.extent=take(f.renderer.sync_window_target()).extent;
    f.frame.scene_viewport={23.25f,32.75f,127.5f,95.25f};
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.projection={1/(2.5f*aspect),0,0,0, 0,1/2.5f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    auto& options=f.frame.view.lit;options={};
    options.ddgi=options.gtao=options.sssr=options.bloom=false;
    options.antialiasing=render::Antialiasing::none;
    f.frame.environment={};f.room.meshes.assign(1,{});
    auto& receiver=f.room.meshes.front();receiver.stable_key=1;
    receiver.geometry=std::make_shared<const Mesh>(std::vector<Vertex>{
        {{-6,-4,4},{0,0,-1}},{{6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},
        {{-6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},{{-6,4,4},{0,0,-1}}});
    receiver.color={.5f,.5f,.5f,1};receiver.material.roughness=1;
    f.lights.assign(1,{});f.lights.front().stable_key=100;
    f.lights.front().position={0,0,1};f.lights.front().intensity=24;f.lights.front().range=12;
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    f.capture("cost-delta-warm",12);
    // Direct light keeps no history: an edit's first frame is already final.
    const auto exact_current=[&](const char* name) {
        const auto current=f.capture(name);
        const auto settled=f.capture((std::string(name)+"-settled").c_str(),4);
        const auto delta=f.compare(name,current,settled);
        check(delta.mean==0 && delta.p95==0 && current.depth==settled.depth,
              "deterministic direct light shows its settled value in the edit's first frame");
    };
    f.lights.front().position[0]=1;f.lights.front().intensity=5;f.edited();
    exact_current("cost-delta-light-edit");
    f.lights.front().source_radius=.3f;f.edited();f.capture("cost-finite-support",16);
    f.lights.front().source_radius=0;f.edited();
    exact_current("cost-finite-to-delta");
    render::MeshInstance direct_blocker;direct_blocker.stable_key=2;direct_blocker.geometry=atelier::geometry::cube_mesh();
    direct_blocker.world=box(-1,0,2,.4f,3,.3f);f.room.meshes.push_back(direct_blocker);f.edited();
    f.capture("cost-delta-caster-warm",4);
    f.room.meshes.back().world[12]=.5f;f.edited();
    exact_current("cost-delta-caster-first-frame");
    f.room.meshes.pop_back();f.lights.front().kind=render::LightKind::directional;
    f.lights.front().direction={0,0,1};f.edited();
    exact_current("cost-delta-sun");

    // Both consumers receive identical author snapshots and probe visits. Only
    // the comparison consumer cuts its screen history before every frame.
    Fixture reference(false,f.output/"visibility-reference");
    reference.window.resize(352,224);reference.drawing.extent=take(reference.renderer.sync_window_target()).extent;
    check(reference.drawing.extent==f.drawing.extent,"visibility pair uses identical physical extents");
    options.ddgi=true;options.probe_bounce=0;
    options.probe_volumes[0].minimum={-6,-4,0};options.probe_volumes[0].maximum={6,4,8};
    options.probe_volumes[0].counts={4,4,4};options.probe_update_budget=64;options.probe_rays=64;
    f.lights.clear();f.frame.environment={.1f,.1f,.1f};++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    std::ofstream counts(f.output/"visibility-cache.csv");
    counts<<"case,cached_frame,reference_frame,cached_queries,reference_queries\n";
    struct PairedSample {
        const char* name{};
        std::array<std::uint64_t,2> frames{},rays{};
        std::array<bool,2> received{};
        bool written{};
    };
    std::vector<PairedSample> samples;
    bool saw_reuse=false;
    const auto pair=[&](const char* name,unsigned frames,bool expect_reuse=false) {
        const auto first=samples.size();
        for(unsigned i=0;i<frames;++i) {
            reference.room=f.room;reference.lights=f.lights;
            reference.frame.view=f.frame.view;reference.frame.camera=f.frame.camera;
            reference.frame.scene_viewport=f.frame.scene_viewport;
            reference.frame.environment=f.frame.environment;reference.frame.lighting_focus=f.frame.lighting_focus;
            reference.frame.scene_identity=f.frame.scene_identity;
            reference.frame.meshes=reference.room.meshes;reference.frame.lights=reference.lights;
            reference.frame.camera_cut=true;
            render::Capture actual,expected;
            const auto a=f.step(&actual),b=reference.step(&expected);
            // Transient skips may consume different public frame IDs. Pair by
            // accepted observation, then attach each delayed sample by its own
            // actual ID; never assume the two contexts complete in lockstep.
            samples.push_back({name,{a.accepted_id,b.accepted_id},{},{},false});
            const auto delta=difference(actual,expected);
            check(delta.mean<.01 && delta.p95<=1 && actual.depth==expected.depth,
                  "cached DDGI visibility preserves the continuously retraced image and FP32 depth");
            if(i+1==frames){png(actual,f.output/(std::string(name)+".png"));png(expected,reference.output/(std::string(name)+".png"));}
            const std::array completed{&a.stats,&b.stats};
            for(std::size_t side=0;side<2;++side)if(completed[side]->scene_lit_gpu_timing_valid) {
                const auto id=completed[side]->scene_lit_gpu_sample_frame_id;
                const auto row=std::ranges::find_if(samples,[&](const auto& sample){return sample.frames[side]==id;});
                if(row==samples.end())continue;
                row->received[side]=true;row->rays[side]=completed[side]->scene_lit_ddgi_visibility_rays[1];
                if(row->received[0]&&row->received[1]&&!row->written) {
                    row->written=true;
                    counts<<row->name<<','<<row->frames[0]<<','<<row->frames[1]<<','<<row->rays[0]<<','<<row->rays[1]<<'\n';
                    check(row->rays[0]<=row->rays[1],"exact visibility reuse never adds segment queries");
                }
            }
        }
        bool case_reuse=false,case_sample=false;
        for(std::size_t i=first;i<samples.size();++i)if(samples[i].written) {
            case_sample=true;case_reuse|=samples[i].rays[1]>0&&samples[i].rays[0]==0;
        }
        counts.flush();check(counts.good(),"visibility cache completed evidence written");
        check(case_sample,"each visibility lifecycle case has matched completed evidence");
        if(expect_reuse)check(case_reuse,"stationary receiver reuses every valid segment while probe lighting keeps updating");
        saw_reuse|=case_reuse;
    };
    pair("cost-visibility-warm",48,true);
    f.frame.environment={.3f,.1f,.05f};f.edited();pair("cost-visibility-radiance-edit",16,true);
    f.frame.camera.view[12]=.3f;pair("cost-visibility-camera",8,true);
    render::MeshInstance blocker;blocker.stable_key=2;blocker.geometry=atelier::geometry::cube_mesh();
    blocker.world=box(-1,0,2,.2f,3,3);f.room.meshes.push_back(blocker);f.edited();
    pair("cost-visibility-added-blocker",16);
    f.room.meshes.back().world[12]=1;f.edited();pair("cost-visibility-moved-blocker",16);
    standard_fixture::Description mask;mask.parameters.coverage=atelier::material::Coverage::mask;
    mask.parameters.base_color[3]=0;
    f.room.meshes.back().surfaces=std::make_shared<const std::vector<standard_fixture::SurfaceRef>>(
        std::vector<standard_fixture::SurfaceRef>{std::make_shared<const standard_fixture::Surface>(mask)});
    f.edited();pair("cost-visibility-mask-hole",16);
    f.room.meshes.back().surfaces.reset();f.edited();pair("cost-visibility-mask-restored",16);
    f.room.meshes.pop_back();f.edited();pair("cost-visibility-removed-blocker",16);
    options.probe_volumes[0].counts={5,5,5};options.probe_update_budget=125;
    options.probe_volumes[0].placement=render::ProbePlacement::follow_focus;
    pair("cost-visibility-follow-volume",24);
    f.frame.lighting_focus={3.1f,0,0};pair("cost-visibility-volume-roll",24);
    options.ddgi=false;pair("cost-visibility-disabled",8);
    options.ddgi=true;pair("cost-visibility-reenabled",16);
    ++f.frame.scene_identity;pair("cost-visibility-new-scene",24);
    check(saw_reuse,"visibility acceptance observed a completed exact-cache hit frame");
    reference.close();

    // A fresh jittered consumer starts with only the disabled-buffer binding.
    // Switching to an unjittered view must allocate/fill it before any reuse.
    Fixture jittered(false,f.output/"jitter-cache");
    jittered.window.resize(352,224);jittered.drawing.extent=take(jittered.renderer.sync_window_target()).extent;
    jittered.room=f.room;jittered.lights.clear();jittered.frame.environment=f.frame.environment;
    jittered.frame.camera=f.frame.camera;jittered.frame.scene_viewport=f.frame.scene_viewport;
    jittered.frame.view=f.frame.view;
    auto& jitter_options=jittered.frame.view.lit;
    jitter_options.probe_volumes[0].placement=render::ProbePlacement::fixed;
    jitter_options.probe_volumes[0].counts={4,4,4};jitter_options.probe_update_budget=64;
    ++jittered.frame.scene_identity;jittered.edited();
    for(auto aa:{render::Antialiasing::taa,render::Antialiasing::smaa_t2x,render::Antialiasing::none,
                 render::Antialiasing::taa,render::Antialiasing::smaa_t2x,render::Antialiasing::none}) {
        jitter_options.antialiasing=aa;
        const auto first=jittered.drawing.id+1;bool saw_expected_queries=false;
        for(unsigned i=0;i<24;++i) {
            render::Capture image;const auto stats=jittered.step(i==23?&image:nullptr).stats;
            if(stats.scene_lit_gpu_timing_valid&&stats.scene_lit_gpu_sample_frame_id>=first)
                saw_expected_queries|=aa==render::Antialiasing::none?stats.scene_lit_ddgi_visibility_rays[1]==0:
                    stats.scene_lit_ddgi_visibility_rays[1]>0;
        }
        check(saw_expected_queries,"jitter modes retrace, and unjittered modes rebuild then reuse valid segments");
    }
    jittered.close();
    std::puts("Renderer SceneLit cost: PASS (deterministic direct support, exact DDGI cache lifecycle and retraced image pair)");
}
