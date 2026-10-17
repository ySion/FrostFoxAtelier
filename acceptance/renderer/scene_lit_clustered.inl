// Product Renderer GPU acceptance: finite support, overflow, signal continuity
// and completed timing/counter identity. Small receivers keep validation bounded.
void clustered_cases(Fixture& f) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    f.frame.scene_viewport={23.25f,32.75f,95.5f,65.25f};
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.projection={1/(2.5f*aspect),0,0,0, 0,1/2.5f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    f.frame.environment={};f.frame.view.shading=render::Shading::scene_lit;
    auto& options=f.frame.view.lit;options={};
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=options.shadows=false;
    options.antialiasing=render::Antialiasing::none;options.direct_clustered=true;options.direct_cluster_inspect=true;
    f.room.meshes.assign(1,{});
    auto& plane=f.room.meshes.front();plane.stable_key=1;plane.world=box(0,0,0,1,1,1);
    plane.geometry=std::make_shared<const Mesh>(std::vector<Vertex>{
        {{-6,-4,4},{0,0,-1}},{{6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},
        {{-6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},{{-6,4,4},{0,0,-1}}});
    plane.color={.5f,.5f,.5f,1};plane.material={.roughness=1};
    f.lights.assign(272,{});
    for(std::size_t i=0;i<f.lights.size();++i) {
        auto& light=f.lights[i];light.stable_key=100+i;light.position={0,0,1};
        light.intensity=24.f/272;light.range=12;
    }
    ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
    const auto completed=[&] {
        const auto first=f.drawing.id;render::FrameStats stats;
        for(unsigned i=0;i<60;++i){stats=f.step().stats;if(stats.scene_lit_gpu_timing_valid&&stats.scene_lit_gpu_sample_frame_id>first)return stats;}
        check(false,"clustered completed GPU sample did not arrive");return stats;
    };
    const auto overflow=f.capture("clustered-272-overflow",12);
    auto stats=completed();
    check(stats.scene_lit_light_sources==272 && stats.scene_lit_direct_cluster_cells>0 &&
        stats.scene_lit_direct_cluster_overflows==stats.scene_lit_direct_cluster_cells &&
        stats.scene_lit_direct_cluster_max_sources==272,"overflow retains all 272 analytic sources in completed cluster statistics");
    check((stats.scene_lit_gpu_pass_mask&(1u<<20))!=0 && (stats.scene_lit_gpu_pass_mask&((1u<<10)|(1u<<11)))==0,
        "clustered DI times list construction and skips both screen reservoir passes");
    const auto receivers=static_cast<std::uint32_t>(std::count_if(overflow.depth.begin(),overflow.depth.end(),[](float d){return d>0;}));
    check(stats.scene_lit_direct_receiver_histogram[65]==receivers && stats.scene_lit_direct_receiver_max_sources==272,
        "optional support histogram counts actual GBuffer receivers, including >64 support");
    options.direct_cluster_inspect=false;
    f.lights.resize(1);f.lights.front().intensity=24;f.edited();f.frame.camera_cut=true;
    const auto single=f.capture("clustered-equivalent-single",12);
    auto delta=f.compare("clustered-overflow-preserves-energy",overflow,single);
    check(delta.mean<.7 && delta.p95<=2,"272 coincident lights agree with one equal-power light without capacity loss");
    stats=completed();check(stats.scene_lit_direct_cluster_overflows==0 && stats.scene_lit_direct_cluster_max_sources==1,
        "smaller light set cannot reuse stale overflowing lists");
    check(stats.scene_lit_direct_receiver_max_sources==0 &&
        std::ranges::all_of(stats.scene_lit_direct_receiver_histogram,[](auto n){return n==0;}),
        "normal rendering clears inspection counters and does not collect receiver histograms");
    options.direct_reference=true;
    const auto reference=f.capture("clustered-single-independent-reference",2);
    delta=f.compare("clustered-single-reference",single,reference);
    check(delta.mean<1.5 && delta.p95<=3,"single delta light agrees with independent reference at fractional viewport and tail tiles");
    stats=completed();check((stats.scene_lit_gpu_pass_mask&(1u<<20))==0 && stats.scene_lit_direct_cluster_cells==0,
        "reference takes precedence and completed cluster counters are cleared");
    options.direct_reference=false;f.lights.front().position={0,0,-10};f.lights.front().range=1;f.edited();
    f.capture("clustered-outside-support",4);stats=completed();
    check(stats.scene_lit_direct_cluster_max_sources==0,"lights outside receiver support are removed from the list");
    // The source center lies outside the range, but the emitting sphere reaches
    // the receiver. Omitting source radius from the culling bound loses this light.
    f.lights.front().position={0,0,0};f.lights.front().range=2;f.lights.front().source_radius=3;
    f.lights.front().intensity=1000;f.edited();
    const auto region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},3);
    const auto finite=lighting_mean(f,"clustered-finite-radius",region,20,24);
    stats=completed();check(stats.scene_lit_direct_cluster_max_sources==1 && finite[0]>8,
        "finite point endpoints beyond the center cutoff still illuminate the receiver");
    options.direct_reference=true;
    const auto finite_reference=lighting_mean(f,"clustered-finite-radius-reference",region,1,24);
    std::printf("Clustered finite-source mean: %.3f / reference %.3f\n",finite[0],finite_reference[0]);
    for(unsigned channel=0;channel<3;++channel)check(std::abs(finite[channel]-finite_reference[channel])<std::max(7.,.15*finite_reference[channel]),
        "finite-radius light enumeration retains the reference endpoint integral");
    options.direct_reference=false;
    auto& rectangle=f.lights.front();rectangle.kind=render::LightKind::rectangle;
    rectangle.position={4.5f,0,3};rectangle.width=8;rectangle.height=2;rectangle.range=2;
    rectangle.source_radius=0;rectangle.intensity=10;rectangle.direction={0,0,1};
    f.edited();const auto area=lighting_mean(f,"clustered-area-support",region,20,24);
    stats=completed();check(stats.scene_lit_direct_cluster_max_sources==1 && area[0]>8,
        "rectangle corners beyond the anchor cutoff contribute through conservative extent bounds");
    options.direct_reference=true;
    const auto area_reference=lighting_mean(f,"clustered-area-reference",region,1,24);
    std::printf("Clustered rectangle mean: %.3f / reference %.3f\n",area[0],area_reference[0]);
    for(unsigned channel=0;channel<3;++channel)check(std::abs(area[channel]-area_reference[channel])<std::max(7.,.15*area_reference[channel]),
        "rectangle enumeration retains reference energy and facing semantics");
    options.direct_reference=false;
    const auto ordinary_projection=f.frame.camera.projection;
    f.frame.camera.projection[0]=10000/aspect;f.frame.camera.projection[5]=10000;
    f.frame.camera_cut=true;f.lights.front()={.stable_key=100,.position={0,0,3.9995f},.intensity=.1f,.range=.0001f};f.edited();
    const auto tiny=f.capture("clustered-minimum-range",4);
    options.direct_reference=true;const auto tiny_reference=f.capture("clustered-minimum-range-reference",2);
    delta=f.compare("clustered-minimum-range-reference",tiny,tiny_reference);
    check(delta.mean<1 && delta.p95<=2 && mean_rgb(tiny,region)[0]>10,
        "culling and shading share the evaluator's one-millimetre minimum range");
    options.direct_reference=false;options.shadows=true;
    f.frame.camera.projection=ordinary_projection;f.frame.camera_cut=true;
    f.lights.assign(65,{});
    for(std::size_t i=0;i<f.lights.size();++i){auto& light=f.lights[i];light.stable_key=100+i;
        light.position={-2,2,0};light.range=12;light.intensity=24.f/65;}
    render::MeshInstance blocker;blocker.stable_key=2;blocker.geometry=atelier::geometry::cube_mesh();
    blocker.world=box(-1,1,2,1.4f,1.4f,.4f);f.room.meshes.push_back(blocker);f.edited();
    const auto shadow=lighting_mean(f,"clustered-overflow-shadow",region,12,2);
    options.shadows=false;
    const auto lit=lighting_mean(f,"clustered-overflow-unshadowed",region,12,2);
    check(lit[0]>10 && shadow[0]<lit[0]*.2,"overflow preserves visibility for every contributing light");
    f.room.meshes.resize(1);f.lights.resize(1);f.lights.front().intensity=24;f.edited();
    f.frame.camera.projection={1.8f/aspect,0,0,0, 0,1.8f,0,0, 0,0,-.1f/39.9f,1, 0,0,4/39.9f,0};
    f.frame.camera_cut=true;
    const auto perspective=f.capture("clustered-perspective",12);
    options.direct_reference=true;const auto perspective_reference=f.capture("clustered-perspective-reference",2);
    delta=f.compare("clustered-perspective-reference",perspective,perspective_reference);
    check(delta.mean<1.5 && delta.p95<=3,"perspective receivers match reference without orthographic assumptions");
    options.direct_reference=false;f.lights.clear();f.edited();
    const auto empty=f.capture("clustered-deleted-lights",2);
    stats=completed();check(stats.scene_lit_direct_cluster_max_sources==0 && stats.scene_lit_shadow_rays==0,
        "deleting every source leaves no stale list or visibility work");
    for(std::size_t i=0;i<empty.depth.size();++i)if(empty.depth[i]>0)
        check(empty.rgba[i*4]==0&&empty.rgba[i*4+1]==0&&empty.rgba[i*4+2]==0,"removed light radiance clears immediately");
    f.room.meshes.clear();++f.frame.scene_identity;f.edited();f.step();
    f.room=Room{};f.edited();f.frame.camera=camera({95,65});f.frame.camera_cut=true;
    f.capture("clustered-replaced-world-emission",8);
    stats=completed();check(stats.scene_lit_light_sources>0 && stats.scene_lit_direct_cluster_max_sources==0,
        "emissive-only replacement uses the separate source estimator after empty-scene retirement");
    std::puts("Renderer clustered direct: PASS (272-light overflow, finite source extents, shadows, fractional/tail ROI, both projections, reference priority, empty/replaced scene)");
}
