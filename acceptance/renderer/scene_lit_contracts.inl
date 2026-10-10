// Regression scenes cross the ordinary Renderer boundary and inspect real GPU
// output, completed ray counters and owning-resource lifetimes.
void quality_contracts(Fixture& f) {
    using atelier::geometry::Vertex;
    using atelier::geometry::Mesh;
    const auto plane=[](float z) {
        return std::make_shared<const Mesh>(std::vector<Vertex>{
            {{-6,-4,z},{0,0,-1}},{{6,-4,z},{0,0,-1}},{{6,4,z},{0,0,-1}},
            {{-6,-4,z},{0,0,-1}},{{6,4,z},{0,0,-1}},{{-6,4,z},{0,0,-1}}});
    };
    f.room.meshes.clear();f.room.meshes.push_back({});
    auto& receiver=f.room.meshes.front();receiver.geometry=plane(4);receiver.stable_key=1;
    receiver.world=box(0,0,0,1,1,1);receiver.color={.4f,.4f,.4f,1};receiver.material={.roughness=1};
    f.lights.assign(1,{});auto& light=f.lights.front();
    light.stable_key=100;light.kind=render::LightKind::point;light.position={0,0,.5f};
    light.range=2;light.source_radius=2;light.width=light.height=0;light.intensity=200;light.casts_shadow=false;
    f.frame.environment={};f.frame.view.shading=render::Shading::scene_lit;
    auto& options=f.frame.view.lit;options={};
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    options.antialiasing=render::Antialiasing::none;options.shadow_samples=64;
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.projection={1/(2.5f*aspect),0,0,0, 0,1/2.5f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    ++f.frame.scene_identity;f.edited();f.frame.camera_cut=true;
    const auto region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,4},12);
    const auto point=f.capture("contract-point-radius",3);
    light.width=light.height=20;f.edited();
    const auto unrelated_dimensions=f.capture("contract-point-unused-dimensions",2);
    check(mean_rgb(point,region)[0]>5 && point.rgba==unrelated_dimensions.rgba,
          "point sphere support reaches receivers beyond center range; ignored rectangle dimensions cannot change its light");
    light.kind=render::LightKind::directional;light.range=light.width=light.height=0;
    light.direction={0,0,1};light.source_radius=1.55f;light.intensity=1;f.edited();f.step();
    light.kind=render::LightKind::rectangle;light.range=2;light.width=light.height=1;light.source_radius=0;
    f.edited();f.step();

    // A single resource deliberately contains open sheets AND a closed cube.
    // Normal seams and reversed winding must not turn open half-spaces solid.
    const auto mixed=[](bool reverse) {
        std::vector<Vertex> vertices;
        const auto quad=[&](std::array<Vec3,4> corners,Vec3 normal) {
            for(unsigned i:std::array<unsigned,6>{0,1,2,0,2,3})
                vertices.push_back({{corners[i][0],corners[i][1],corners[i][2]},{normal[0],normal[1],normal[2]}});
        };
        quad({{{-12,0,-12},{12,0,-12},{12,0,12},{-12,0,12}}},{0,1,0});
        quad({{{-12,2,-12},{12,2,-12},{12,2,12},{-12,2,12}}},{0,1,0});
        quad({{{-12,0,4},{12,0,4},{12,2,4},{-12,2,4}}},{0,0,-1});
        for(auto v:atelier::geometry::cube_mesh()->vertices()){v.position[0]+=20;vertices.push_back(v);}
        if(reverse)for(std::size_t i=0;i<vertices.size();i+=3)std::swap(vertices[i+1],vertices[i+2]);
        return std::make_shared<const Mesh>(std::move(vertices));
    };
    f.room.meshes.front().geometry=mixed(false);f.room.meshes.front().color={.7f,.7f,.7f,1};
    light={};light.stable_key=100;light.kind=render::LightKind::directional;
    light.direction={0,0,1};light.intensity=8;light.range=light.width=light.height=0;
    // Tangential illumination gives the floor exactly zero direct light, but
    // illuminates the wall that reflects toward it. Inspect real radiance, not
    // the unrelated probe-state debug colors.
    options={};options.gtao=options.sssr=options.bloom=false;
    options.antialiasing=render::Antialiasing::none;
    f.frame.camera.view={1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,1,1};
    ++f.frame.scene_identity;f.edited();f.frame.camera_cut=true;
    const auto floor_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,1},12);
    auto baseline=f.capture("contract-open-sheet-gi",96);
    const double gi=mean_rgb(baseline,floor_region)[0];
    std::printf("SceneLit open-sheet probe irradiance=%.4f\n",gi);
    check(gi>5,"open-sheet backfaces do not permanently disable the floor's otherwise valid DDGI probes");
    options.ddgi=false;
    const auto direct_only=f.capture("contract-open-sheet-direct-only",2);
    check(mean_rgb(direct_only,floor_region)[0]<=1,"floor observation contains no direct or environment contribution");
    options.ddgi=true;f.step();
    f.room.meshes.front().geometry=mixed(true);f.edited();
    const auto reversed=f.capture("contract-open-sheet-reversed",32);
    check(mean_rgb(reversed,floor_region)[0]>gi*.5,
          "reversed winding preserves authored shading side and mixed-component probe eligibility");
    f.room.meshes.front().world=box(0,0,0,-1,1,1);f.edited();
    const auto mirrored=f.capture("contract-open-sheet-mirrored",32);
    check(mean_rgb(mirrored,floor_region)[0]>gi*.5,
          "mirrored instances preserve the open-sheet geometric classification");
    // Add/delete/reorder visible-domain members every frame. This used to reset
    // target/display histories before any sampled DDGI light could be displayed.
    render::MeshInstance transient;
    transient.geometry=atelier::geometry::cube_mesh();transient.world=box(25,0,0,1,1,1);transient.stable_key=200;
    for(unsigned i=0;i<24;++i) {
        if(i%2==0){transient.stable_key=200+i;f.room.meshes.push_back(transient);}
        else f.room.meshes.resize(1);
        f.edited();render::Capture current;
        const auto result=f.step(&current);
        check(result.stats.scene_lit_history_resets==0 && mean_rgb(current,floor_region)[0]>gi*.4,
              "continuous ordinary membership changes preserve DDGI display and advance its update cursor");
    }
    f.room.meshes.push_back(transient);f.edited();f.step();
    std::swap(f.room.meshes[0],f.room.meshes[1]);f.edited();
    check(f.step().stats.scene_lit_history_resets==0,"stable mesh submission reordering preserves surface and probe history");
    std::swap(f.room.meshes[0],f.room.meshes[1]);f.room.meshes.resize(1);f.edited();f.step();
    baseline=f.capture("contract-membership-gi");

    const auto valid_world=f.room.meshes.front().world;
    f.frame.view.shading=render::Shading::matcap;f.room.meshes.front().world[0]=0;
    ++f.drawing.id;const auto accepted_id=baseline.frame_id;
    const auto rejected=f.renderer.render(f.frame,&baseline);
    check(!rejected && !rejected.error().terminal && baseline.frame_id==accepted_id,
          "invalid non-Lit candidate preserves the accepted capture and recoverable execution domain");
    f.frame.view.shading=render::Shading::scene_lit;f.room.meshes.front().world=valid_world;
    check(f.step().stats.scene_lit_history_resets==0,"rejected mode change does not invalidate the accepted Lit history");

    // Uniform sky isolates low-budget energy: missing directional observations
    // must retain history instead of becoming zero-valued light samples.
    f.room.meshes.front().world=box(0,0,0,1,1,1);
    f.room.meshes.front().geometry=std::make_shared<const Mesh>(std::vector<Vertex>{
        {{-.4f,0,-.4f},{0,1,0}},{{.4f,0,-.4f},{0,1,0}},{{.4f,0,.4f},{0,1,0}},
        {{-.4f,0,-.4f},{0,1,0}},{{.4f,0,.4f},{0,1,0}},{{-.4f,0,.4f},{0,1,0}}});
    f.lights.clear();f.frame.environment={.1f,.1f,.1f};
    options.probe_min={-1,-1,-1};options.probe_max={1,3,1};options.probe_counts={2,2,2};
    options.probe_update_budget=8;options.probe_rays=64;++f.frame.scene_identity;f.edited();
    const auto sky_region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{0,0,0},6);
    const auto sky_reference=f.capture("contract-sky-64-rays",32);
    options.probe_rays=1;++f.frame.scene_identity;
    const auto sky_sparse=f.capture("contract-sky-1-ray",128);
    const double sky64=mean_rgb(sky_reference,sky_region)[0],sky1=mean_rgb(sky_sparse,sky_region)[0];
    std::printf("SceneLit uniform sky receiver: 64 rays=%.4f, 1 ray=%.4f\n",sky64,sky1);
    check(sky64>10 && std::abs(sky1-sky64)<=5,"one-ray directional history preserves constant-sky energy after convergence");
    const auto first_low_budget=f.drawing.id+1;
    bool saw_low_budget=false;
    for(unsigned i=0;i<40&&!saw_low_budget;++i) {
        const auto stats=f.step().stats;
        if(stats.scene_lit_gpu_timing_valid && stats.scene_lit_gpu_sample_frame_id>=first_low_budget) {
            check(stats.scene_lit_probe_rays<=std::uint64_t(options.probe_update_budget)*33 &&
                  stats.scene_lit_probe_rays>std::uint64_t(options.probe_update_budget)*32,
                  "one requested illumination ray is measured as one ray, plus the independent 32 geometry rays");
            saw_low_budget=true;
        }
    }
    check(saw_low_budget,"received a completed GPU sample for the low probe-ray budget");
    const std::weak_ptr<const Mesh> observed=f.room.meshes.front().geometry;
    const auto occupied=f.step().stats.scene_lit_resource_bytes;
    f.room.meshes.clear();f.edited();render::Capture empty;
    const auto released=f.step(&empty).stats.scene_lit_resource_bytes;
    check(observed.expired() && released<occupied && std::ranges::all_of(empty.depth,[](float d){return d==0;}),
          "accepted empty scene retires both raster and ray geometry ownership and captures background depth");
    check(f.step().stats.scene_lit_resource_bytes==released,"repeated empty frames are idempotent and report retained target resources");
    render::MeshInstance returned;
    returned.stable_key=1;returned.geometry=plane(4);returned.world=box(0,0,0,1,1,1);returned.color={.4f,.4f,.4f,1};
    f.room.meshes.push_back(returned);f.edited();options.ddgi=false;
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    const auto reentered=f.capture("contract-empty-reentry",2);
    check(std::ranges::any_of(reentered.depth,[](float d){return d>0;}),"same Renderer rebuilds a nonempty Lit scene after scene-resource retirement");
    check(bool(f.renderer.close()) && bool(f.renderer.close()),"repeated renderer close remains successful after empty-scene retirement");
    std::puts("Renderer SceneLit contracts: PASS (light domains/support, open/closed mixed geometry, winding/mirror, continuous membership, rejected-frame isolation, exact ray budget, empty-scene retirement, repeated close)");
}
