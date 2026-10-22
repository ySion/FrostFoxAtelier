// Product Renderer acceptance, not a cache unit test. Visibility is judged
// against geometric truth (segment-box tests on reconstructed receivers); the
// band within each declared margin of a blocker silhouette is not judged.
// Passing these small scenes does not prove 1440p cost or equivalence for every
// source radius, thin occluder, or grazing receiver.
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
    options.shadows=true;options.antialiasing=render::Antialiasing::none;
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
    // Unit-cube blockers placed by box(): world AABB from translation and scale.
    const auto bounds=[](const render::MeshInstance& mesh){
        return Box{{mesh.world[12],mesh.world[13],mesh.world[14]},
                   {std::abs(mesh.world[0])/2,std::abs(mesh.world[5])/2,std::abs(mesh.world[10])/2}};
    };
    const auto reset_point=[&]{
        f.room.meshes={receiver(1,0,4,12,8),blocker(2)};
        f.lights.assign(1,{});auto& light=f.lights.front();light.stable_key=100;
        light.position={-2,1,0};light.range=10;light.intensity=35;
        ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    };
    std::ofstream counters(f.output/"shadow-cache-completed.csv");
    check(counters.good(),"shadow cache completed evidence file opened");
    counters<<"scenario,submitted_frame,completed_frame,completed_sequence,shadow_ms,faces,reused,static_updates,dynamic_updates,draws,dropped_lights,near_plane_faces,missing_taps,plane_fallbacks\n";
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
            <<stats.scene_lit_shadow_cache_dropped_lights<<','<<stats.scene_lit_shadow_cache_near_plane_faces<<','<<stats.scene_lit_shadow_cache_missing_taps<<','
            <<stats.scene_lit_shadow_cache_plane_fallbacks<<'\n';
        counters.flush();
    };
    const auto submit=[&](const char* name){
        render::Capture image;const auto result=f.step(&image);png(image,f.output/(std::string(name)+"-first.png"));
        const auto stats=completed(result.accepted_id);
        check(stats.scene_lit_gpu_sample_sequence>0&&std::isfinite(stats.scene_lit_shadow_cache_gpu_ms)&&
            stats.scene_lit_shadow_cache_gpu_ms>=0,"shadow counters and finite timing identify a completed submission");
        check(stats.scene_lit_shadow_cache_missing_taps==0,"all requested shadow taps have current-frame residency");
        check(stats.scene_lit_shadow_cache_dropped_lights==0,"small shadow fixture keeps every casting light resident");
        check((stats.scene_lit_gpu_pass_mask&((1u<<12)|(1u<<14)))==((1u<<12)|(1u<<14)),
            "direct lighting records its light-list and fused shading scopes");
        record(name,result.accepted_id,stats);return stats;
    };
    const auto warm=[&](const char* name){
        const auto stats=submit(name);
        check(stats.scene_lit_shadow_cache_faces>0&&stats.scene_lit_shadow_cache_reused_faces==stats.scene_lit_shadow_cache_faces&&
            stats.scene_lit_shadow_cache_static_updates==0&&stats.scene_lit_shadow_cache_dynamic_updates==0&&
            stats.scene_lit_shadow_cache_draws==0&&(stats.scene_lit_gpu_pass_mask&(1u<<13))==0,
            "stationary shadow cache reuses every requested face without redraw");
        return stats;
    };
    // Compares a settled shadowed capture with the same frame without shadows.
    // Disabling shadows retires the atlas, so two frames re-enter the cache
    // before any later edit is expected to preserve it.
    const auto truth=[&](const char* name,const render::Capture& shadowed,std::vector<Box> blockers,double margin,std::size_t occluded){
        options.shadows=false;const auto unshadowed=f.capture((std::string(name)+"-unshadowed").c_str(),2);
        options.shadows=true;
        const auto result=shadow_truth(f.frame,shadowed,unshadowed,f.lights.front(),blockers,margin);
        f.metrics<<name<<','<<result.max_lit_delta<<','<<result.max_occluded<<','<<result.failures<<','<<result.occluded<<'\n';
        std::printf("Shadow truth %s: occluded=%zu lit=%zu failures=%zu max_lit_delta=%u max_occluded=%u\n",
            name,result.occluded,result.lit,result.failures,result.max_lit_delta,result.max_occluded);
        check(result.lit>=256&&result.occluded>=occluded&&result.failures==0,
            "cached visibility darkens geometrically occluded receivers and leaves unoccluded receivers unchanged");
        f.step();f.step();
    };
    reset_point();
    auto stats=submit("shadow-point-cold");
    check(stats.scene_lit_shadow_cache_faces>0&&stats.scene_lit_shadow_cache_static_updates>0&&
        stats.scene_lit_shadow_cache_draws>0&&(stats.scene_lit_gpu_pass_mask&(1u<<13))!=0,
        "first shadow frame renders complete requested static depth and records update scope");
    const auto dark_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},2);
    const auto bright_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{1.8f,-.8f,4},2);
    const auto baseline=f.capture("shadow-point-settled",20);
    const auto blocked=mean_rgb(baseline,dark_region),bright=mean_rgb(baseline,bright_region);
    check(bright[0]>20&&blocked[0]<bright[0]*.12,"off-camera-ray caster produces an actual dark receiver shadow");
    warm("shadow-point-warm");
    const auto stable=f.capture("shadow-point-stable",4);
    auto delta=f.compare("shadow-point-stability",baseline,stable);
    check(delta.mean==0&&delta.p95==0,"stationary cached point visibility is identical across frames");
    truth("shadow-point-truth",stable,{bounds(f.room.meshes[1])},.03,64);

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
    truth("shadow-deleted-truth",deleted,{},0,0);

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
    truth("shadow-mask-truth",opaque,{bounds(f.room.meshes[1])},.03,64);

    f.lights.front().position[0]=-1.5f;f.edited();stats=submit("shadow-light-move");
    check(stats.scene_lit_shadow_cache_static_updates>0&&stats.scene_lit_shadow_cache_dynamic_updates>0,
        "moving a light invalidates both layers of its changed projections");
    const auto light_moved=f.capture("shadow-light-moved-settled",16);
    truth("shadow-light-moved-truth",light_moved,{bounds(f.room.meshes[1])},.03,64);
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
    truth("shadow-offscreen-caster-truth",offscreen,{bounds(f.room.meshes[1])},.05,64);

    f.room.meshes.clear();f.lights.clear();++f.frame.scene_identity;f.edited();
    const auto empty=f.capture("shadow-empty-world",2);
    check(std::ranges::all_of(empty.depth,[](float value){return value==0;}),"empty world removes every old shadow receiver");
    // Empty scenes intentionally have no SceneLit completed query. The next
    // nonempty scene must publish its own cold sample, not the old warm result.
    f.frame.camera.view[12]=0;reset_point();stats=submit("shadow-world-reopened");
    check(stats.scene_lit_shadow_cache_static_updates>0&&stats.scene_lit_shadow_cache_reused_faces==0,
        "reopening a world cannot reuse retired atlas contents from another scene domain");
    // A finite sphere keeps one depth projection; outside the penumbra band its
    // receivers must still match the unshadowed frame exactly.
    f.lights.front().source_radius=.15f;f.edited();submit("shadow-finite-point");
    const auto soft=f.capture("shadow-finite-point-settled",24);
    truth("shadow-finite-point-truth",soft,{bounds(f.room.meshes[1])},.35,0);
    warm("shadow-finite-point-warm");

    // Four disjoint receiver columns lie in all four camera-depth cascades.
    // Only the far receiver has a deliberately offset occluder, so the camera
    // sees the receiver probe rather than the cube's front face. The sun drifts
    // along y only, so the x-disjoint columns never shadow one another.
    f.room.meshes={receiver(11,-2.4f,-3.85f,1.1f,3),receiver(12,-.8f,-3.6f,1.1f,3),
        receiver(13,.8f,-2.5f,1.1f,3),receiver(14,2.4f,2,1.1f,3)};
    auto sun_blocker=blocker(15);sun_blocker.world=box(2.24f,0,1.2f,.16f,.3f,.08f);
    f.room.meshes.push_back(sun_blocker);f.lights.assign(1,{});
    auto& sun=f.lights.front();sun.stable_key=200;sun.kind=render::LightKind::directional;
    sun.direction=normalize({0,.2f,1});sun.intensity=2;
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();stats=submit("shadow-csm-cold");
    check(stats.scene_lit_shadow_cache_faces==4&&stats.scene_lit_shadow_cache_static_updates==4,
        "directional light renders four complete requested cascades");
    const auto cascades=f.capture("shadow-csm-settled",20);warm("shadow-csm-warm");
    truth("shadow-csm-truth",cascades,{bounds(sun_blocker)},.03,4);
    for(const Vec3 point:std::array<Vec3,3>{{{-2.4f,0,-3.85f},{-.8f,0,-3.6f},{.8f,0,-2.5f}}})
        check(mean_rgb(cascades,project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,point,1))[0]>20,
            "near/middle cascade receivers remain lit with current depth and identity");
    const auto sun_shadow=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{2.24f,.24f,2},1);
    check(mean_rgb(cascades,sun_shadow)[0]<15,"last cascade preserves the off-axis caster's real shadow");
    sun.direction=normalize({.03f,.2f,1});f.edited();stats=submit("shadow-sun-moved");
    check(stats.scene_lit_shadow_cache_static_updates==4,"moving the sun invalidates every cascade projection");
    const auto moved_sun=f.capture("shadow-sun-moved-settled",16);
    truth("shadow-sun-moved-truth",moved_sun,{bounds(sun_blocker)},.03,4);
    f.frame.camera.view[12]=.35f;f.frame.camera_cut=true;submit("shadow-csm-camera-move");
    const auto moved_camera=f.capture("shadow-csm-camera-moved-settled",16);
    truth("shadow-csm-camera-truth",moved_camera,{bounds(sun_blocker)},.03,4);warm("shadow-csm-camera-warm");

    // Grazing sun, 70 degrees from the receiver normal, and a fin standing 1 cm
    // in front of the receiver. One interior texel (about 4.6 mm) of normal
    // offset moves the far shadow edge about 1.3 cm, inside the 5.6 cm judged
    // band; ten texels move it past the band. Without slope handling the lit
    // slope shadows itself.
    f.frame.scene_viewport={0,0,float(f.drawing.extent.width),float(f.drawing.extent.height)};
    const float wide=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    f.frame.camera.projection={1/(.25f*wide),0,0,0, 0,1/.25f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    // The fin's half thickness exceeds the margin, so its shrunk box is not empty.
    auto fin=blocker(16);fin.world=box(0,0,-.06f,.04f,1,.1f);
    f.room.meshes={receiver(17,0,0,2.4f,1.6f),fin};
    f.lights.front().direction=normalize({2.75f,0,1});
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();submit("shadow-grazing-contact-cold");
    truth("shadow-grazing-contact-truth",f.capture("shadow-grazing-contact-settled",16),{bounds(fin)},.015,1000);
    warm("shadow-grazing-contact-warm");

    // One pressure submission, not a many-frame stress benchmark. A completely
    // transparent cube requests six conservative directions without changing
    // the real GBuffer or visibility. 272*6 minimum 128 tiles exceed the fixed
    // atlas's cells: the single plan keeps whole lights in importance order and
    // drops the rest, which then contribute no direct light at all.
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
    // Switch immediately to one light. Subsequent completed-query polling
    // never renders a second 272-light pressure frame.
    f.lights.resize(1);f.frame.camera_cut=true;f.edited();
    const auto single_submission=f.step();
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
    check(have_pressure&&have_single,"pressure and single-light samples retain their exact completed frame identities");
    record("shadow-pressure-272",pressure_submission.accepted_id,pressure_stats);
    record("shadow-pressure-single",single_submission.accepted_id,single_stats);
    const auto dropped=pressure_stats.scene_lit_shadow_cache_dropped_lights;
    check(dropped>0&&dropped<272&&pressure_stats.scene_lit_shadow_cache_faces==(272-dropped)*6&&
        pressure_stats.scene_lit_shadow_cache_static_updates>0&&pressure_stats.scene_lit_shadow_cache_missing_taps==0,
        "atlas pressure keeps whole lights with all six faces and drops the remainder");
    check(single_stats.scene_lit_shadow_cache_dropped_lights==0&&single_stats.scene_lit_shadow_cache_missing_taps==0,
        "single-light comparison returns to a complete resident cache");
    // Equal importance ties keep the lowest indices: the resident set alone is
    // the exact expected image, so a dropped light leaking energy shows here.
    f.lights.assign(272-dropped,{});
    for(std::size_t i=0;i<f.lights.size();++i){auto& light=f.lights[i];
        light.stable_key=30000+i;light.position={0,0,0};light.range=10;light.intensity=24.f/272;}
    f.frame.camera_cut=true;f.edited();
    const auto resident_image=f.capture("shadow-pressure-resident",2);
    const Region pressure_region{24,33,55,56,
        project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},1).expected_depth};
    for(unsigned y=pressure_region.top;y<pressure_region.bottom;++y)for(unsigned x=pressure_region.left;x<pressure_region.right;++x){
        check(std::abs(depth(pressure_image,x,y)-pressure_region.expected_depth)<2e-4f&&
            std::abs(depth(resident_image,x,y)-pressure_region.expected_depth)<2e-4f,
            "transparent pressure cube does not replace any real wall receiver in the ROI");
    }
    const auto pressure_delta=region_difference(pressure_image,resident_image,pressure_region);
    f.metrics<<"shadow-pressure-resident-roi,"<<pressure_delta.mean<<','<<pressure_delta.p95<<','<<pressure_delta.changed<<",0\n";
    std::printf("Shadow pressure ROI: mean=%.4f p95=%u dropped=%u\n",pressure_delta.mean,pressure_delta.p95,static_cast<unsigned>(dropped));
    check(mean_rgb(resident_image,pressure_region)[0]>20&&pressure_delta.mean<=.7&&pressure_delta.p95<=2,
        "dropped lights contribute nothing and resident lights keep their exact energy");

    // Point, sun and rectangle accumulate exactly once each. Captures are
    // decoded through the documented display transform to linear radiance.
    const auto linear=[](std::uint8_t code){
        const double s=code/255.,y=s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);
        const double a=2.51-2.43*y,b=.03-.59*y,c=-.14*y;
        return (-b+std::sqrt(b*b-4*a*c))/(2*a);
    };
    const auto radiance=[&](const render::Capture& image,Region region){
        double sum=0;unsigned count=0;
        for(unsigned y=region.top;y<region.bottom;++y)for(unsigned x=region.left;x<region.right;++x){sum+=linear(pixel(image,x,y)[0]);++count;}
        return sum/count;
    };
    f.room.meshes={receiver(401,0,4,12,8)};f.lights.assign(3,{});
    f.lights[0].stable_key=40000;f.lights[0].position={0,0,0};f.lights[0].range=10;f.lights[0].intensity=4;
    f.lights[1].stable_key=40001;f.lights[1].kind=render::LightKind::directional;
    f.lights[1].direction={0,0,1};f.lights[1].intensity=.25f;
    f.lights[2].stable_key=40002;f.lights[2].kind=render::LightKind::rectangle;
    f.lights[2].position={1.5f,0,0};f.lights[2].direction={0,0,1};f.lights[2].range=10;
    f.lights[2].width=f.lights[2].height=.02f;f.lights[2].intensity=12000;f.lights[2].casts_shadow=false;
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    stats=submit("shadow-kernel-mixed");
    check(stats.scene_lit_light_sources==3,"mixed fixture includes one point, one sun and one rectangle");
    const auto mixed_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},3);
    const auto mixed=radiance(f.capture("shadow-kernel-mixed-settled",8),mixed_region);
    double components=0;
    const std::array<const char*,3> component_names{"shadow-kernel-only-point","shadow-kernel-only-sun","shadow-kernel-only-rectangle"};
    const auto intensities=std::array{f.lights[0].intensity,f.lights[1].intensity,f.lights[2].intensity};
    for(std::size_t component=0;component<component_names.size();++component){
        for(std::size_t i=0;i<3;++i)f.lights[i].intensity=i==component?intensities[i]:0;
        f.frame.camera_cut=true;f.edited();
        const auto alone=radiance(f.capture(component_names[component],4),mixed_region);
        check(alone>mixed*.1,"each mixed component contributes measurable receiver radiance");
        components+=alone;
    }
    for(std::size_t i=0;i<3;++i)f.lights[i].intensity=intensities[i];
    f.frame.camera_cut=true;f.edited();
    std::printf("Shadow mixed radiance: together=%.4f sum-of-parts=%.4f\n",mixed,components);
    check(mixed<.9&&std::abs(mixed-components)<=mixed*.03,"point, sun and rectangle radiance accumulate exactly once each");

    // The visible wall is entirely inside radius .7, then entirely outside .1.
    // range=10 keeps both projections in far=16 and their face demand/tier
    // unchanged: the radius changes shading only, not cached depth.
    f.frame.camera.projection={1/(.2f*(31.f/23)),0,0,0, 0,1/.2f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    f.room.meshes={receiver(501,0,.4f,.6f,.5f)};f.lights.assign(1,{});
    f.lights[0].stable_key=50000;f.lights[0].position={0,0,0};f.lights[0].range=10;
    f.lights[0].intensity=1;f.lights[0].source_radius=.7f;
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    const auto sphere_sample=[&](const char* name,render::Capture& image){
        const auto result=f.step(&image);png(image,f.output/(std::string(name)+".png"));
        const auto sample=completed(result.accepted_id);record(name,result.accepted_id,sample);
        check(sample.scene_lit_shadow_cache_missing_taps==0&&sample.scene_lit_shadow_cache_dropped_lights==0,
            "sphere-interior fixture has complete bounded shadow residency");
        return sample;
    };
    const auto sphere_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,.4f},3);
    render::Capture inside_image;const auto inside_stats=sphere_sample("shadow-sphere-inside",inside_image);
    check(inside_stats.scene_lit_shadow_cache_faces>0&&mean_rgb(inside_image,sphere_region)[0]>10,
        "sphere-interior receivers keep cached visibility and direct energy");
    f.lights[0].source_radius=.1f;f.frame.camera_cut=true;f.edited();
    render::Capture outside_image;const auto outside_stats=sphere_sample("shadow-sphere-outside",outside_image);
    check(outside_stats.scene_lit_shadow_cache_faces==inside_stats.scene_lit_shadow_cache_faces&&
        outside_stats.scene_lit_shadow_cache_reused_faces==outside_stats.scene_lit_shadow_cache_faces&&
        outside_stats.scene_lit_shadow_cache_static_updates==0&&outside_stats.scene_lit_shadow_cache_dynamic_updates==0&&
        outside_stats.scene_lit_shadow_cache_draws==0&&mean_rgb(outside_image,sphere_region)[0]>10,
        "same-far radius shrink changes shading without redrawing cached depth");
    f.lights[0].source_radius=.7f;f.lights[0].casts_shadow=false;f.frame.camera_cut=true;f.edited();
    render::Capture unshadowed_image;const auto unshadowed_stats=sphere_sample("shadow-sphere-unshadowed",unshadowed_image);
    check(unshadowed_stats.scene_lit_shadow_cache_faces==0&&mean_rgb(unshadowed_image,sphere_region)[0]>10,
        "a non-casting sphere needs no atlas faces and still contributes");

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
    truth("shadow-csm-infinite-perspective-truth",infinite_image,{},0,0);
    stats=warm("shadow-csm-infinite-perspective-warm");
    check(stats.scene_lit_shadow_cache_faces==4,"infinite-far perspective reuses all four complete cascades");
    counters.flush();f.metrics.flush();
    std::puts("Renderer shadow cache: PASS (exact completed samples, cold/warm, static/dynamic motion and deletion, Mask, light edits, scene replacement, geometric visibility truth, four CSM cascades including infinite-far perspective, grazing near-caster normal offset, whole-light atlas drops, exact mixed accumulation, sphere-interior receivers)");
}
