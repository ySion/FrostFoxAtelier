// Product Renderer acceptance, not a cache unit test. PCSS is a bounded
// single-depth approximation to endpoint RT visibility. Fixed image gates and
// saved reference evidence detect its bias; a failed gate requires investigation,
// not a larger tolerance. Passing these small scenes does not prove 1440p cost
// or equivalence for every source radius, thin occluder, or grazing receiver.
void shadow_cases(Fixture& f) {
    namespace material=atelier::material;
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    using Stats=render::FrameStats;
    f.frame.scene_viewport={24.25f,32.75f,159.5f,111.25f};
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.projection={1/(2.5f*aspect),0,0,0, 0,1/2.5f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    f.frame.environment={};f.frame.view.shading=render::Shading::scene_lit;
    auto& options=f.frame.view.lit;options={};
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    options.shadows=true;options.direct_clustered=true;options.direct_shadow_cache=true;options.antialiasing=render::Antialiasing::none;
    const auto quad=std::make_shared<const Mesh>(std::vector<Vertex>{
        {{-.5f,-.5f,0},{0,0,-1}},{{.5f,-.5f,0},{0,0,-1}},{{.5f,.5f,0},{0,0,-1}},
        {{-.5f,-.5f,0},{0,0,-1}},{{.5f,.5f,0},{0,0,-1}},{{-.5f,.5f,0},{0,0,-1}}});
    const auto receiver=[&](std::uint64_t key,float x,float z,float width,float height){
        render::MeshInstance mesh;mesh.stable_key=key;mesh.geometry=quad;
        mesh.world=box(x,0,z,width,height,1);mesh.color={.5f,.5f,.5f,1};mesh.material.roughness=1;
        return mesh;
    };
    const auto blocker=[](std::uint64_t key){
        render::MeshInstance mesh;mesh.stable_key=key;mesh.geometry=atelier::geometry::cube_mesh();
        mesh.world=box(-1,.5f,2,.5f,.6f,.2f);mesh.color={.3f,.3f,.3f,1};return mesh;
    };
    const auto reset_point=[&]{
        f.room.meshes={receiver(1,0,4,12,8),blocker(2)};
        f.lights.assign(1,{});auto& light=f.lights.front();light.stable_key=100;
        light.position={-2,1,0};light.range=10;light.intensity=35;
        ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    };
    std::ofstream counters(f.output/"shadow-cache-completed.csv");
    check(counters.good(),"shadow cache completed evidence file opened");
    counters<<"scenario,submitted_frame,completed_frame,completed_sequence,shadow_ms,faces,reused,static_updates,dynamic_updates,draws,pressure_batches,near_plane_faces,missing_taps,clamped_taps,rt_fallbacks,plane_fallbacks\n";
    // Capturing the changed frame completes its submission. Inspect its exact
    // completed id before settling history: a later warm sample would hide the
    // one-frame invalidation being tested.
    const auto completed=[&](std::uint64_t target){
        Stats stats;
        for(unsigned i=0;i<60;++i){stats=f.step().stats;
            if(!stats.scene_lit_gpu_timing_valid)continue;
            check(stats.scene_lit_gpu_sample_frame_id<=target,"shadow changed-frame completed sample was not skipped");
            if(stats.scene_lit_gpu_sample_frame_id==target)return stats;
        }
        check(false,"shadow exact completed GPU sample did not arrive");return stats;
    };
    const auto record=[&](const char* name,std::uint64_t submitted,const Stats& stats){
        counters<<name<<','<<submitted<<','<<stats.scene_lit_gpu_sample_frame_id<<','<<stats.scene_lit_gpu_sample_sequence<<','
            <<stats.scene_lit_shadow_cache_gpu_ms<<','<<stats.scene_lit_shadow_cache_faces<<','<<stats.scene_lit_shadow_cache_reused_faces<<','
            <<stats.scene_lit_shadow_cache_static_updates<<','<<stats.scene_lit_shadow_cache_dynamic_updates<<','<<stats.scene_lit_shadow_cache_draws<<','
            <<stats.scene_lit_shadow_cache_pressure_batches<<','<<stats.scene_lit_shadow_cache_near_plane_faces<<','<<stats.scene_lit_shadow_cache_missing_taps<<','
            <<stats.scene_lit_shadow_cache_clamped_taps<<','<<stats.scene_lit_shadow_cache_rt_fallbacks<<','<<stats.scene_lit_shadow_cache_plane_fallbacks<<'\n';
        counters.flush();
    };
    const auto submit=[&](const char* name){
        render::Capture image;const auto result=f.step(&image);png(image,f.output/(std::string(name)+"-first.png"));
        const auto stats=completed(result.accepted_id);
        check(stats.scene_lit_gpu_sample_sequence>0&&std::isfinite(stats.scene_lit_shadow_cache_gpu_ms)&&
            stats.scene_lit_shadow_cache_gpu_ms>=0,"shadow counters and finite timing identify a completed submission");
        check(stats.scene_lit_shadow_cache_missing_taps==0,"all requested shadow taps have current-frame residency");
        check(stats.scene_lit_shadow_cache_pressure_batches==0,"small shadow fixture does not require pressure streaming");
        check(stats.scene_lit_shadow_cache_rt_fallbacks==0,"ordinary point/sun fixture does not silently fall back to RT");
        check((stats.scene_lit_gpu_pass_mask&((1u<<12)|(1u<<20)))==((1u<<12)|(1u<<20))&&
            (stats.scene_lit_gpu_pass_mask&((1u<<10)|(1u<<11)))==0,"cached direct path preserves shade/list scopes and skips screen reservoirs");
        record(name,result.accepted_id,stats);return stats;
    };
    const auto warm=[&](const char* name){
        const auto stats=submit(name);
        check(stats.scene_lit_shadow_cache_faces>0&&stats.scene_lit_shadow_cache_reused_faces==stats.scene_lit_shadow_cache_faces&&
            stats.scene_lit_shadow_cache_static_updates==0&&stats.scene_lit_shadow_cache_dynamic_updates==0&&
            stats.scene_lit_shadow_cache_draws==0,"stationary shadow cache reuses every requested face without redraw");
        return stats;
    };
    const auto reference=[&](const char* name,const render::Capture& cached,double mean_limit,unsigned p95_limit){
        options.direct_reference=true;const auto image=f.capture((std::string(name)+"-rt").c_str(),2);
        const auto delta=f.compare(name,cached,image);
        options.direct_reference=false;
        check(delta.mean<=mean_limit&&delta.p95<=p95_limit,"cached shadow image stays inside its declared RT comparison gate");
        // Reference mode deliberately invalidates the atlas. Re-enter through
        // the real Renderer, then submit an unchanged cached frame before any
        // later edit is expected to preserve a warm cache.
        f.step();f.step();
        return image;
    };
    reset_point();
    auto stats=submit("shadow-point-cold");
    check(stats.scene_lit_shadow_cache_faces>0&&stats.scene_lit_shadow_cache_static_updates>0&&
        stats.scene_lit_shadow_cache_draws>0&&(stats.scene_lit_gpu_pass_mask&(1u<<21))!=0,
        "first shadow frame renders complete requested static depth and records update scope");
    const auto dark_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},2);
    const auto bright_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{1.8f,-.8f,4},2);
    const auto baseline=f.capture("shadow-point-settled",20);
    const auto blocked=mean_rgb(baseline,dark_region),bright=mean_rgb(baseline,bright_region);
    check(bright[0]>20&&blocked[0]<bright[0]*.12,"off-camera-ray caster produces an actual dark receiver shadow");
    warm("shadow-point-warm");
    const auto stable=f.capture("shadow-point-stable",4);
    auto delta=f.compare("shadow-point-stability",baseline,stable);
    check(delta.mean<=1&&delta.p95<=2,"stationary cached point visibility does not flicker");
    reference("shadow-point-reference",stable,3,8);

    f.lights.front().color={.3f,.6f,1};f.edited();
    stats=submit("shadow-light-color");
    check(stats.scene_lit_shadow_cache_static_updates==0&&stats.scene_lit_shadow_cache_dynamic_updates==0,
        "light RGB edits reuse visibility depth");
    const auto colored=f.capture("shadow-light-color-settled",16);
    const auto color=mean_rgb(colored,bright_region);
    check(color[2]>color[0]+10,"cached visibility still responds to authored light RGB");
    f.lights.front().color={1,1,1};f.room.meshes.front().material.roughness=.7f;f.edited();
    stats=submit("shadow-surface-roughness");
    check(stats.scene_lit_shadow_cache_static_updates==0&&stats.scene_lit_shadow_cache_dynamic_updates==0,
        "surface roughness and light radiance changes do not invalidate caster coverage");

    f.room.meshes[1].world[13]=1;f.edited();stats=submit("shadow-first-caster-motion");
    check(stats.scene_lit_shadow_cache_static_updates>0&&stats.scene_lit_shadow_cache_dynamic_updates>0,
        "first movement removes the old static shadow and writes the dynamic layer");
    const auto moved=f.capture("shadow-caster-moved",16);
    check(mean_rgb(moved,dark_region)[0]>blocked[0]+20,"moving a caster clears its previous shadow");
    f.room.meshes[1].world[13]=1.25f;f.edited();stats=submit("shadow-next-caster-motion");
    check(stats.scene_lit_shadow_cache_static_updates==0&&stats.scene_lit_shadow_cache_dynamic_updates>0,
        "subsequent dynamic motion does not redraw stationary geometry");
    f.room.meshes.resize(1);f.edited();stats=submit("shadow-dynamic-delete");
    check(stats.scene_lit_shadow_cache_static_updates==0&&stats.scene_lit_shadow_cache_dynamic_updates>0,
        "deleting a dynamic caster clears old dynamic coverage without redrawing static depth");
    const auto deleted=f.capture("shadow-dynamic-deleted",16);
    check(mean_rgb(deleted,dark_region)[0]>blocked[0]+20,"deleted caster leaves no phantom shadow");
    reference("shadow-deleted-reference",deleted,2,4);

    f.room.meshes.push_back(blocker(3));f.edited();stats=submit("shadow-caster-recreated");
    check(stats.scene_lit_shadow_cache_static_updates>0,"new caster identity updates stationary visibility");
    const auto opacity=[&](std::uint8_t value){
        material::Description description;description.parameters.coverage=material::Coverage::mask;
        description.parameters.base_color={.3f,.3f,.3f,1};
        description.texture(material::Input::opacity).image=std::make_shared<const material::Image>(1,1,
            std::vector<std::uint8_t>{value,value,value,255});
        auto surfaces=std::make_shared<std::vector<material::SurfaceRef>>();
        for(const auto& slot:f.room.meshes[1].geometry->materials()){
            (void)slot;surfaces->push_back(std::make_shared<const material::Surface>(description));}
        f.room.meshes[1].surfaces=std::move(surfaces);f.edited();
    };
    opacity(0);stats=submit("shadow-mask-transparent");
    check(stats.scene_lit_shadow_cache_static_updates+stats.scene_lit_shadow_cache_dynamic_updates>0,
        "Mask opacity content change invalidates previous occlusion");
    const auto transparent=f.capture("shadow-mask-transparent-settled",16);
    check(mean_rgb(transparent,dark_region)[0]>blocked[0]+20,"transparent Mask texels stop casting shadows");
    opacity(255);stats=submit("shadow-mask-opaque");
    check(stats.scene_lit_shadow_cache_static_updates==0&&stats.scene_lit_shadow_cache_dynamic_updates>0,
        "subsequent Mask edits remain in the dynamic layer");
    const auto opaque=f.capture("shadow-mask-opaque-settled",16);
    check(mean_rgb(opaque,dark_region)[0]<mean_rgb(transparent,dark_region)[0]*.12,
        "opaque Mask texels restore the occluder instead of reusing transparent depth");
    reference("shadow-mask-reference",opaque,3,8);

    f.lights.front().position[0]=-1.5f;f.edited();stats=submit("shadow-light-move");
    check(stats.scene_lit_shadow_cache_static_updates>0&&stats.scene_lit_shadow_cache_dynamic_updates>0,
        "moving a light invalidates both layers of its changed projections");
    const auto light_moved=f.capture("shadow-light-moved-settled",16);
    reference("shadow-light-moved-reference",light_moved,3,8);
    f.frame.camera.view[12]=.15f;f.frame.camera_cut=true;
    submit("shadow-camera-cut");f.capture("shadow-camera-cut-settled",12);warm("shadow-camera-cut-warm");

    // The caster is wholly outside the camera's horizontal frustum, while the
    // line from (-8,1,0) through (-4,.5,2) reaches the visible (0,0,4) probe.
    f.frame.camera.view[12]=0;f.frame.camera_cut=true;
    f.lights.front().position={-8,1,0};f.lights.front().range=16;f.lights.front().intensity=200;
    f.room.meshes[1].world=box(-4,.5f,2,.5f,.6f,.2f);f.edited();submit("shadow-offscreen-caster");
    const auto offscreen=f.capture("shadow-offscreen-caster-settled",16);
    const auto offscreen_lit=mean_rgb(offscreen,bright_region)[0];
    check(offscreen_lit>20&&mean_rgb(offscreen,dark_region)[0]<offscreen_lit*.12,
        "shadow caster discovery includes geometry outside main-camera visibility");
    reference("shadow-offscreen-caster-reference",offscreen,3,8);

    f.room.meshes.clear();f.lights.clear();++f.frame.scene_identity;f.edited();
    const auto empty=f.capture("shadow-empty-world",2);
    check(std::ranges::all_of(empty.depth,[](float value){return value==0;}),"empty world removes every old shadow receiver");
    // Empty scenes intentionally have no SceneLit completed query. The next
    // nonempty scene must publish its own cold sample, not the old warm result.
    f.frame.camera.view[12]=0;reset_point();stats=submit("shadow-world-reopened");
    check(stats.scene_lit_shadow_cache_static_updates>0&&stats.scene_lit_shadow_cache_reused_faces==0,
        "reopening a world cannot reuse retired atlas contents from another scene domain");
    f.lights.front().source_radius=.15f;f.edited();submit("shadow-finite-point");
    const auto soft=f.capture("shadow-finite-point-settled",24);
    const auto soft_reference=reference("shadow-finite-point-reference",soft,4,12);
    const auto penumbra=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{.5f,0,4},4);
    const auto penumbra_delta=region_difference(soft,soft_reference,penumbra);
    f.metrics<<"shadow-finite-point-penumbra,"<<penumbra_delta.mean<<','<<penumbra_delta.p95<<','<<penumbra_delta.changed<<",0\n";
    check(penumbra_delta.mean<=12&&penumbra_delta.p95<=40,
        "bounded PCSS penumbra bias is checked locally, not hidden in a mostly unchanged full image");
    warm("shadow-finite-point-warm");

    // Four disjoint receiver columns lie in all four camera-depth cascades.
    // Only the far receiver has a deliberately offset occluder, so the camera
    // sees the receiver probe rather than the cube's front face.
    f.room.meshes={receiver(11,-2.4f,-3.85f,1.1f,3),receiver(12,-.8f,-3.6f,1.1f,3),
        receiver(13,.8f,-2.5f,1.1f,3),receiver(14,2.4f,2,1.1f,3)};
    auto sun_blocker=blocker(15);sun_blocker.world=box(2.24f,0,1.2f,.16f,.3f,.08f);
    f.room.meshes.push_back(sun_blocker);f.lights.assign(1,{});
    auto& sun=f.lights.front();sun.stable_key=200;sun.kind=render::LightKind::directional;
    sun.direction=normalize({.2f,0,1});sun.intensity=2;
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();stats=submit("shadow-csm-cold");
    check(stats.scene_lit_shadow_cache_faces==4&&stats.scene_lit_shadow_cache_static_updates==4,
        "directional light renders four complete requested cascades");
    const auto cascades=f.capture("shadow-csm-settled",20);warm("shadow-csm-warm");
    reference("shadow-csm-reference",cascades,3,8);
    for(const Vec3 point:std::array<Vec3,3>{{{-2.4f,0,-3.85f},{-.8f,0,-3.6f},{.8f,0,-2.5f}}})
        check(mean_rgb(cascades,project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,point,1))[0]>20,
            "near/middle cascade receivers remain lit with current depth and identity");
    const auto sun_shadow=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{2.4f,0,2},1);
    check(mean_rgb(cascades,sun_shadow)[0]<15,"last cascade preserves the off-axis caster's real shadow");
    sun.direction=normalize({.2f,.03f,1});f.edited();stats=submit("shadow-sun-moved");
    check(stats.scene_lit_shadow_cache_static_updates==4,"moving the sun invalidates every cascade projection");
    const auto moved_sun=f.capture("shadow-sun-moved-settled",16);
    reference("shadow-sun-moved-reference",moved_sun,3,8);
    f.frame.camera.view[12]=.35f;f.frame.camera_cut=true;submit("shadow-csm-camera-move");
    const auto moved_camera=f.capture("shadow-csm-camera-moved-settled",16);
    reference("shadow-csm-camera-reference",moved_camera,3,8);warm("shadow-csm-camera-warm");

    // One pressure submission, not a many-frame stress benchmark. A completely
    // transparent cube requests six conservative directions without changing
    // the real GBuffer or visibility. 272*6 minimum 128 tiles exceed the fixed
    // 4096 atlas's 1024 cells, forcing the product batching path at a 31x23 ROI.
    f.frame.scene_viewport={24.25f,32.75f,31,23};
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    f.frame.camera.projection={1/(2.5f*(31.f/23)),0,0,0, 0,1/2.5f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    auto invisible=blocker(302);invisible.world=box(0,0,0,20,20,20);
    material::Description mask;mask.parameters.coverage=material::Coverage::mask;
    mask.texture(material::Input::opacity).image=std::make_shared<const material::Image>(1,1,
        std::vector<std::uint8_t>{0,0,0,255});
    invisible.surfaces=std::make_shared<const std::vector<material::SurfaceRef>>(
        invisible.geometry->materials().size(),std::make_shared<const material::Surface>(mask));
    f.room.meshes={receiver(301,0,4,12,8),std::move(invisible)};
    f.lights.assign(272,{});
    for(std::size_t i=0;i<f.lights.size();++i){auto& light=f.lights[i];
        light.stable_key=30000+i;light.position={0,0,0};light.range=10;light.intensity=24.f/272;}
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    render::Capture pressure_image;const auto pressure_submission=f.step(&pressure_image);
    png(pressure_image,f.output/"shadow-pressure-272.png");
    // Switch immediately to the equivalent single delta light. Subsequent
    // completed-query polling never renders a second 272-light pressure frame.
    f.lights.resize(1);f.lights.front().intensity=24;f.frame.camera_cut=true;f.edited();
    render::Capture single_image;const auto single_submission=f.step(&single_image);
    png(single_image,f.output/"shadow-pressure-single.png");
    Stats pressure_stats,single_stats;bool have_pressure=false,have_single=false;
    const auto observe=[&](const Stats& sample){
        if(!sample.scene_lit_gpu_timing_valid)return;
        if(sample.scene_lit_gpu_sample_frame_id==pressure_submission.accepted_id){pressure_stats=sample;have_pressure=true;}
        if(sample.scene_lit_gpu_sample_frame_id==single_submission.accepted_id){single_stats=sample;have_single=true;}
    };
    observe(single_submission.stats);
    // Completion is asynchronous; only the inexpensive single-light frame is
    // submitted while waiting. Do not assume a fixed two-frame query latency.
    for(unsigned drain=0;drain<60&&(!have_pressure||!have_single);++drain)observe(f.step().stats);
    check(have_pressure&&have_single,"pressure and equal-energy single-light samples retain their exact completed frame identities");
    record("shadow-pressure-272",pressure_submission.accepted_id,pressure_stats);
    record("shadow-pressure-single",single_submission.accepted_id,single_stats);
    check(pressure_stats.scene_lit_shadow_cache_faces==272*6&&pressure_stats.scene_lit_shadow_cache_pressure_batches>0&&
        pressure_stats.scene_lit_shadow_cache_static_updates>0,"six-face demand exercises real atlas-pressure batches at the minimum tier");
    check(pressure_stats.scene_lit_shadow_cache_missing_taps==0&&pressure_stats.scene_lit_shadow_cache_rt_fallbacks==0&&
        pressure_stats.scene_lit_shadow_rays==0,"atlas pressure neither loses residency nor routes its shading through RT");
    check(single_stats.scene_lit_shadow_cache_pressure_batches==0&&single_stats.scene_lit_shadow_cache_missing_taps==0&&
        single_stats.scene_lit_shadow_cache_rt_fallbacks==0,"single-light comparison returns to a complete resident cache");
    const Region pressure_region{24,33,55,56,
        project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},1).expected_depth};
    for(unsigned y=pressure_region.top;y<pressure_region.bottom;++y)for(unsigned x=pressure_region.left;x<pressure_region.right;++x){
        check(std::abs(depth(pressure_image,x,y)-pressure_region.expected_depth)<2e-4f&&
            std::abs(depth(single_image,x,y)-pressure_region.expected_depth)<2e-4f,
            "transparent pressure cube does not replace any real wall receiver in the ROI");
    }
    const auto pressure_delta=region_difference(pressure_image,single_image,pressure_region);
    f.metrics<<"shadow-pressure-equal-energy-roi,"<<pressure_delta.mean<<','<<pressure_delta.p95<<','<<pressure_delta.changed<<",0\n";
    std::printf("Shadow pressure ROI: mean=%.4f p95=%u batches=%u\n",pressure_delta.mean,pressure_delta.p95,
        static_cast<unsigned>(pressure_stats.scene_lit_shadow_cache_pressure_batches));
    check(mean_rgb(single_image,pressure_region)[0]>20&&pressure_delta.mean<=.7&&pressure_delta.p95<=2,
        "pressure batching preserves all 272 delta-light contributions without duplicate energy or descriptor overwrites");

    // Exercise all three compiled shade entries together. Tiny area sources
    // retain their actual area integral but make endpoint variation negligible;
    // white radiance and a rough receiver also avoid source-selection chromatic
    // noise. This isolates accumulation from finite-sphere reconstruction bias.
    const auto emitter_triangle=std::make_shared<const Mesh>(std::vector<Vertex>{
        {{-.5f,-.5f,0},{0,0,1}},{{.5f,-.5f,0},{0,0,1}},{{0,.5f,0},{0,0,1}}});
    render::MeshInstance emitter;emitter.stable_key=402;emitter.geometry=emitter_triangle;
    emitter.world=box(-1.5f,0,0,.04f,.02f,1);emitter.material.emissive={40000,40000,40000};
    f.room.meshes={receiver(401,0,4,12,8),emitter};f.lights.assign(3,{});
    f.lights[0].stable_key=40000;f.lights[0].position={0,0,0};f.lights[0].range=10;f.lights[0].intensity=12;
    f.lights[1].stable_key=40001;f.lights[1].kind=render::LightKind::directional;
    f.lights[1].direction={0,0,1};f.lights[1].intensity=.7f;
    f.lights[2].stable_key=40002;f.lights[2].kind=render::LightKind::rectangle;
    f.lights[2].position={1.5f,0,0};f.lights[2].direction={0,0,1};f.lights[2].range=10;
    f.lights[2].width=f.lights[2].height=.02f;f.lights[2].intensity=40000;f.lights[2].casts_shadow=false;
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    stats=submit("shadow-kernel-mixed");
    check(stats.scene_lit_light_sources==4&&stats.scene_lit_shadow_rays>0,
        "mixed kernel fixture includes three analytic sources and one RT-visible emitter triangle");
    const auto mixed_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},3);
    const auto mixed=f.capture("shadow-kernel-mixed-settled",8);
    const auto mixed_rgb=mean_rgb(mixed,mixed_region);
    options.direct_reference=true;
    const auto mixed_reference=f.capture("shadow-kernel-mixed-reference",2);
    const auto mixed_delta=region_difference(mixed,mixed_reference,mixed_region);
    f.metrics<<"shadow-kernel-mixed-reference-roi,"<<mixed_delta.mean<<','<<mixed_delta.p95<<','<<mixed_delta.changed<<",0\n";
    check(mixed_rgb[0]>20&&mixed_rgb[0]<235&&mixed_delta.mean<=3&&mixed_delta.p95<=8,
        "point, sun and residual kernels accumulate once each against independent reference in the real receiver ROI");
    options.direct_reference=false;f.step();f.step();
    const std::array<const char*,4> component_names{"shadow-kernel-without-point","shadow-kernel-without-sun",
        "shadow-kernel-without-rectangle","shadow-kernel-without-emitter"};
    for(std::size_t component=0;component<component_names.size();++component){
        float saved_intensity=0;
        if(component<3){saved_intensity=f.lights[component].intensity;f.lights[component].intensity=0;}
        else f.room.meshes[1].material.emissive={};
        f.frame.camera_cut=true;f.edited();
        const auto omitted=f.capture(component_names[component],4);
        const auto omitted_rgb=mean_rgb(omitted,mixed_region);
        check(mixed_rgb[0]>omitted_rgb[0]+4,
            "each mixed analytic/emitter component contributes measurable receiver energy before testing their sum");
        if(component<3)f.lights[component].intensity=saved_intensity;
        else f.room.meshes[1].material.emissive=emitter.material.emissive;
        f.edited();
    }

    // The visible wall is entirely inside radius .7, then entirely outside .1.
    // range=10 keeps both projections in far=16 and their face demand/tier
    // unchanged: only endpoint-kernel demand may change, not cached depth.
    f.frame.camera.projection={1/(.2f*(31.f/23)),0,0,0, 0,1/.2f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    f.room.meshes={receiver(501,0,.4f,.6f,.5f)};f.lights.assign(1,{});
    f.lights[0].stable_key=50000;f.lights[0].position={0,0,0};f.lights[0].range=10;
    f.lights[0].intensity=1;f.lights[0].source_radius=.7f;
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    const auto endpoint_sample=[&](const char* name,render::Capture& image){
        const auto result=f.step(&image);png(image,f.output/(std::string(name)+".png"));
        const auto sample=completed(result.accepted_id);record(name,result.accepted_id,sample);
        check(sample.scene_lit_shadow_cache_missing_taps==0&&sample.scene_lit_shadow_cache_pressure_batches==0,
            "sphere-interior fixture has complete bounded shadow residency");
        return sample;
    };
    const auto endpoint_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,.4f},3);
    render::Capture inside_image;const auto inside_stats=endpoint_sample("shadow-kernel-sphere-inside",inside_image);
    check(inside_stats.scene_lit_shadow_cache_rt_fallbacks>0&&inside_stats.scene_lit_shadow_rays>0&&
        mean_rgb(inside_image,endpoint_region)[0]>10,
        "sphere-interior receivers preserve positive endpoint RT visibility and direct energy");
    f.lights[0].source_radius=.1f;f.frame.camera_cut=true;f.edited();
    render::Capture outside_image;const auto outside_stats=endpoint_sample("shadow-kernel-sphere-outside",outside_image);
    check(outside_stats.scene_lit_shadow_cache_faces==inside_stats.scene_lit_shadow_cache_faces&&
        outside_stats.scene_lit_shadow_cache_faces>0&&
        outside_stats.scene_lit_shadow_cache_reused_faces==outside_stats.scene_lit_shadow_cache_faces&&
        outside_stats.scene_lit_shadow_cache_static_updates==0&&outside_stats.scene_lit_shadow_cache_dynamic_updates==0&&
        outside_stats.scene_lit_shadow_cache_draws==0,
        "same-far radius shrink withdraws sphere-interior demand without unnecessary depth redraw");
    check(outside_stats.scene_lit_shadow_cache_rt_fallbacks==0&&outside_stats.scene_lit_shadow_rays==0&&
        mean_rgb(outside_image,endpoint_region)[0]>10,
        "outside-sphere point returns to cached visibility without losing its contribution");
    f.lights[0].source_radius=.7f;f.lights[0].casts_shadow=false;
    // Keep the cached entry active: with no shadow-casting analytic source the
    // Renderer intentionally selects the uncached A path. This second point's
    // range cannot reach the wall, so it needs no faces and adds no radiance.
    render::Light distant_shadow;distant_shadow.stable_key=50001;
    distant_shadow.position={100,0,0};distant_shadow.range=1;distant_shadow.intensity=1;
    f.lights.push_back(distant_shadow);f.frame.camera_cut=true;f.edited();
    render::Capture unshadowed_image;const auto unshadowed_stats=endpoint_sample("shadow-kernel-sphere-unshadowed",unshadowed_image);
    check(unshadowed_stats.scene_lit_shadow_cache_faces==0&&unshadowed_stats.scene_lit_shadow_cache_rt_fallbacks==0&&
        unshadowed_stats.scene_lit_shadow_rays==0&&mean_rgb(unshadowed_image,endpoint_region)[0]>10,
        "unshadowed sphere-interior point remains in the point kernel and contributes without visibility rays");

    // A legal infinite-far reversed-Z perspective has an invertible projection
    // but w=0 when its z=0 far endpoint is unprojected. CSM must derive finite
    // coverage from current receivers instead of rejecting the Frame contract.
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    f.frame.camera.projection={2/(31.f/23),0,0,0, 0,2,0,0, 0,0,0,1, 0,0,.1f,0};
    f.room.meshes={receiver(601,0,4,6,5)};f.lights.assign(1,{});
    f.lights[0].stable_key=60000;f.lights[0].kind=render::LightKind::directional;
    f.lights[0].direction={0,0,1};f.lights[0].intensity=2;
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    stats=submit("shadow-csm-infinite-perspective-cold");
    check(stats.scene_lit_shadow_cache_faces==4&&stats.scene_lit_shadow_cache_static_updates==4,
        "infinite-far perspective initializes all four finite receiver-bounded cascades");
    const auto infinite_image=f.capture("shadow-csm-infinite-perspective-settled",4);
    const auto infinite_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},3);
    check(mean_rgb(infinite_image,infinite_region)[0]>20,
        "infinite-far perspective produces actual directional lighting on its projected receiver");
    options.direct_reference=true;
    const auto infinite_reference=f.capture("shadow-csm-infinite-perspective-reference",2);
    const auto infinite_delta=region_difference(infinite_image,infinite_reference,infinite_region);
    f.metrics<<"shadow-csm-infinite-perspective-roi,"<<infinite_delta.mean<<','<<infinite_delta.p95<<','<<infinite_delta.changed<<",0\n";
    check(infinite_delta.mean<=3&&infinite_delta.p95<=8,
        "infinite-far CSM agrees with independent directional RT reference in the visible receiver ROI");
    options.direct_reference=false;f.step();f.step();
    stats=warm("shadow-csm-infinite-perspective-warm");
    check(stats.scene_lit_shadow_cache_faces==4,"infinite-far perspective reuses all four complete cascades");
    counters.flush();f.metrics.flush();
    std::puts("Renderer shadow cache: PASS (exact completed samples, cold/warm, static/dynamic motion and deletion, Mask, light edits, scene replacement, finite source RT comparison, four CSM cascades including infinite-far perspective, bounded atlas-pressure ROI, mixed kernel accumulation and sphere-interior demand)");
}
