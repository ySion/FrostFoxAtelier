// Rectangle lights integrate their solid angle with linearly transformed
// cosines. Room images come first, so a renderer that fails the truth below
// still leaves comparable evidence; the truth itself is implementation-independent.
void rectangle_cases(Fixture& f) {
    using atelier::geometry::Mesh;
    using atelier::geometry::Vertex;
    f.restore();
    auto& options=f.frame.view.lit;
    options.ddgi=false;options.shadows=true;
    f.lights={room_lights()[3]};
    auto& panel=f.lights.emplace_back();panel.stable_key=104;panel.kind=render::LightKind::rectangle;
    panel.position={-3.3f,.65f,2.6f};panel.direction={1,0,0};panel.right={0,0,1};panel.up={0,1,0};
    panel.width=1.4f;panel.height=.9f;panel.intensity=6;panel.color={1,.8f,.6f};
    f.edited();f.settle_shadows();f.capture("rect-room-direct",2);
    options.ddgi=true;f.capture("rect-room-ddgi",96);

    f.frame.scene_viewport={23.25f,32.75f,95.5f,65.25f};
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    const auto orthographic=[&](float half_height) {
        f.frame.camera.projection={1/(half_height*aspect),0,0,0, 0,1/half_height,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
        f.frame.camera_cut=true;
    };
    f.frame.environment={};
    options={};options.ddgi=options.gtao=options.sssr=options.bloom=options.shadows=false;
    options.antialiasing=render::Antialiasing::none;
    f.room.meshes.assign(1,{});
    auto& plane=f.room.meshes.front();plane.stable_key=1;plane.world=box(0,0,0,1,1,1);
    plane.geometry=std::make_shared<const Mesh>(std::vector<Vertex>{
        {{-6,-4,4},{0,0,-1}},{{6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},
        {{-6,-4,4},{0,0,-1}},{{6,4,4},{0,0,-1}},{{-6,4,4},{0,0,-1}}});
    const auto material=[&](const Rgb3& base,float roughness,float metallic) {
        plane.color={float(base[0]),float(base[1]),float(base[2]),1};plane.material={.roughness=roughness,.metallic=metallic};
    };
    auto& light=f.lights.front();
    f.lights.resize(1);light={.stable_key=100,.kind=render::LightKind::rectangle};
    const auto truth=[&](const char* name,const Rgb3& base,float roughness,float metallic,unsigned samples,double peak_slack) {
        material(base,roughness,metallic);f.edited();
        const auto image=f.capture(name,2);
        const auto result=analytic_rectangle_plane(f.frame,image,light,4,base,roughness,metallic,samples,.25,peak_slack);
        f.metrics<<"rectangle-"<<name<<','<<result.max_error<<",0,"<<result.failures<<','<<result.pixels<<'\n';
        std::printf("SceneLit rectangle %s: pixels=%zu failures=%zu max_code_error=%.3f band_margin=%.3f\n",name,result.pixels,result.failures,result.max_error,result.margin);
        return result;
    };
    // Point limit: a 1 cm square carries 24 cd along its normal. Diffuse is
    // exact, so the band is HDR half precision plus a quarter of the specular part.
    const Rgb3 grey{.5,.5,.5};
    light.position={.4f,-.3f,1};light.direction={0,0,1};light.right={1,0,0};light.up={0,1,0};
    light.width=light.height=.01f;light.intensity=240000;light.range=12;
    orthographic(2.5f);
    auto result=truth("point-limit-orthographic",grey,1,0,1,0);
    check(result.pixels>1000 && result.failures==0,"a small rectangle matches the analytic point limit on every receiver pixel");
    f.frame.camera.projection={1.8f/aspect,0,0,0, 0,1.8f,0,0, 0,0,-.1f/39.9f,1, 0,0,4/39.9f,0};f.frame.camera_cut=true;
    result=truth("point-limit-perspective",grey,1,0,1,0);
    check(result.pixels>1000 && result.failures==0,"the point limit holds for perspective view vectors");
    // Tilted by 0.9 rad and emitting away from the plane: only its back side,
    // lit because the rectangle is two-sided, faces the receivers.
    light.direction={-std::sin(.9f),0,-std::cos(.9f)};light.right={0,1,0};light.up={std::cos(.9f),0,-std::sin(.9f)};light.two_sided=true;
    orthographic(2.5f);
    result=truth("point-limit-two-sided-back",grey,1,0,1,0);
    check(result.pixels>1000 && result.failures==0,"a two-sided rectangle lights receivers behind it with the |cos| point limit");
    // Grazing incidence opposite the view. The camera 2 m from the plane sees
    // it up to 69 degrees off its normal; a 6 cm panel 0.3 m above the plane at
    // x = 5.95 lights the receivers between them from 80 to 88 degrees on the
    // opposite side, so v·h falls to about 0.27 and Fresnel takes up to 20% of
    // the diffuse. Specular there lies outside LTC's per-direction accuracy, so
    // only the diffuse is judged (analytic_rectangle_diffuse); its slack covers
    // the Fresnel of the mean light direction against the per-sample truth.
    light.two_sided=false;f.frame.camera.view[14]=-2;
    const auto perspective=[&](float degrees) {
        const float scale=1/std::tan(degrees*std::numbers::pi_v<float>/180);
        f.frame.camera.projection={scale,0,0,0, 0,scale*aspect,0,0, 0,0,-.1f/39.9f,1, 0,0,4/39.9f,0};f.frame.camera_cut=true;
    };
    perspective(65);
    light.position={5.95f,0,3.7f};light.direction={-1,0,0};light.right={0,1,0};light.up={0,0,-1};
    light.width=light.height=.06f;light.intensity=30000;light.range=20;
    material({1,.5,0},1,0);f.edited();
    const auto grazing=f.capture("grazing-backscatter",2);
    result=analytic_rectangle_diffuse(f.frame,grazing,light,4,1,1,24,.02);
    f.metrics<<"rectangle-grazing-backscatter,"<<result.max_error<<",0,"<<result.failures<<','<<result.pixels<<'\n';
    std::printf("SceneLit rectangle grazing-backscatter diffuse: pixels=%zu failures=%zu max_relative_miss=%.4f\n",result.pixels,result.failures,result.max_error);
    check(result.pixels>1000 && result.failures==0,"the diffuse part loses the Fresnel of grazing incidence opposite the view");
    // An oblique view of a glossy metal reflection. The LTC matrix is
    // anisotropic away from the normal, so a transposed inverse misplaces the
    // highlight across the image; at 60 degrees LTC stays within its accuracy.
    perspective(60);
    light.position={3,0,2};light.direction={-std::numbers::sqrt2_v<float>/2,0,std::numbers::sqrt2_v<float>/2};
    light.right={0,1,0};light.up={-std::numbers::sqrt2_v<float>/2,0,-std::numbers::sqrt2_v<float>/2};
    light.width=.6f;light.height=.4f;light.intensity=4;light.range=12;
    result=truth("oblique-metal",{.9,.6,.3},.5f,1,96,.005);
    check(result.pixels>1000 && result.failures==0,"an oblique glossy reflection matches the quadrature truth");
    f.frame.camera.view[14]=4;
    // Area truth by quadrature: tilted panels 0.6 m from the plane, and one that
    // crosses the receiver plane, so the horizon clips it.
    light.two_sided=false;light.position={.3f,-.2f,3.4f};light.width=1.2f;light.height=.6f;light.range=12;
    light.direction={std::sin(.5f),0,std::cos(.5f)};light.right={0,1,0};light.up={std::cos(.5f),0,-std::sin(.5f)};
    for(const bool projected:{false,true}) {
        if(projected){f.frame.camera.projection={3.2f/aspect,0,0,0, 0,3.2f,0,0, 0,0,-.1f/39.9f,1, 0,0,4/39.9f,0};f.frame.camera_cut=true;}
        else orthographic(1.5f);
        light.intensity=4;
        result=truth(projected?"area-dielectric-perspective":"area-dielectric",grey,.5f,0,96,.005);
        check(result.pixels>1000 && result.failures==0,"rectangle radiance matches the quadrature truth on a rough dielectric");
        light.intensity=2;
        result=truth(projected?"area-metal-perspective":"area-metal",{.9,.6,.3},.3f,1,96,.005);
        check(result.pixels>1000 && result.failures==0,"rectangle radiance matches the quadrature truth on a glossy metal");
    }
    light.position={0,0,3.8f};light.direction={1,0,0};light.right={0,1,0};light.up={0,0,1};
    light.width=.8f;light.height=1;light.intensity=4;orthographic(1.5f);
    result=truth("area-horizon",grey,.5f,0,96,.005);
    check(result.pixels>1000 && result.failures==0,"a rectangle crossing the receiver plane is clipped at its horizon");
    std::puts("Renderer rectangle lights: PASS (room evidence, point limit in both projections and two-sided, quadrature truth on dielectric and metal in both projections, horizon clipping, grazing diffuse Fresnel opposite the view, oblique glossy metal)");
}
