void material_cases(render::Renderer& renderer,const std::filesystem::path& output={}) {
    namespace geometry=atelier::geometry;
    namespace material=atelier::material;
    const auto surface=[](std::array<float,4> color,material::ImageRef image={},material::ImageRef opacity={}) {
        material::Description d;d.parameters.base_color=color;d.parameters.coverage=material::Coverage::mask;
        d.texture(material::Input::base_color).image=std::move(image);d.texture(material::Input::opacity).image=std::move(opacity);
        return std::make_shared<const material::Surface>(std::move(d));
    };
    ui::Frame ui_frame{.id=9000,.extent=take(renderer.target()).extent,.draws={}};
    std::vector<geometry::Vertex> vertices;
    const auto quad=[&](float z) {
        vertices.insert(vertices.end(),{
            {{-1,-1,z},{0,0,-1},{-1,1}},{{-1,1,z},{0,0,-1},{-1,0}},{{1,1,z},{0,0,-1},{0,0}},
            {{-1,-1,z},{0,0,-1},{-1,1}},{{1,1,z},{0,0,-1},{0,0}},{{1,-1,z},{0,0,-1},{0,1}}});
    };
    // Camera's default projection maps world z to 1-z. Keep the original
    // near/far geometry; only the expected device depths are reversed.
    quad(.2f);quad(.6f);
    auto image=std::make_shared<const material::Image>(2,2,std::vector<std::uint8_t>{255,0,0,255, 255,255,255,0, 0,0,255,255, 255,255,0,255});
    std::weak_ptr<const material::Image> observer=image;
    std::vector<material::Slot> materials{{"cutout",surface({1,1,1,1},image)},{"background",surface({0,1,0,1})}};
    std::array<render::MeshInstance,1> meshes{};meshes[0].color={1,1,1,1};
    meshes[0].geometry=std::make_shared<const geometry::Mesh>(vertices,std::vector<std::uint8_t>{},std::move(materials),
        std::vector<geometry::MeshPart>{{0,6,0},{6,6,1}});image.reset();
    render::Frame frame{ui_frame,{}, {0,0,float(ui_frame.extent.width),float(ui_frame.extent.height)},meshes};render::Capture capture;
    auto result=take(prepared_render(renderer,frame,&capture));
    const auto x=ui_frame.extent.width/4,y=ui_frame.extent.height/4;
    check(result.stats.scene_draw_calls==2 && result.stats.material_upload_count>0,"material parts bind distinct surfaces");
    for(const auto& point:std::array{std::array{x,y},std::array{x*3,y},std::array{x,y*3},std::array{x*3,y*3}}) {
        const auto* color=pixel(capture,point[0],point[1]);
        std::printf("Material coverage pixel=(%u,%u) rgba=(%u,%u,%u,%u) depth=%.9f\n",
            point[0],point[1],color[0],color[1],color[2],color[3],depth(capture,point[0],point[1]));
    }
    scene_lit_e2e::png(capture,output/"material-coverage.png");
    check(pixel(capture,x,y)[0]>150 && pixel(capture,x,y)[1]<10 && std::abs(depth(capture,x,y)-.8f)<1e-5f,
        "negative UVs repeat with correct top-left image orientation and sRGB color");
    check(pixel(capture,x*3,y)[1]>150 && pixel(capture,x*3,y)[0]<10 && std::abs(depth(capture,x*3,y)-.4f)<1e-5f,
        "alpha cutout exposes background color and depth");
    ++ui_frame.id;result=take(prepared_render(renderer,frame,&capture));check(result.stats.material_upload_count==0,"unchanged material images upload only once");
    frame.view.wireframe=render::Wireframe::only;
    ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
    check(std::abs(depth(capture,x*3,y)-.4f)<1e-5f,"wire-only depth pass respects material opacity");
    frame.view.shading=render::Shading::matcap;
    ++ui_frame.id;take(prepared_render(renderer,frame,&capture));check(std::abs(depth(capture,x*3,y)-.4f)<1e-5f,"MatCap respects cutout silhouettes");
    auto opaque=std::make_shared<const material::Image>(2,2,std::vector<std::uint8_t>{255,0,0,255, 255,255,255,255, 0,0,255,255, 255,255,0,255});
    auto opacity=std::make_shared<const material::Image>(2,2,std::vector<std::uint8_t>{255,255,255,255, 0,0,0,255, 160,160,160,255, 255,255,255,255});
    std::weak_ptr<const material::Image> opacity_observer=opacity;
    materials={{"separate opacity",surface({1,1,1,1},opaque,opacity)},{"background",surface({0,1,0,1})}};
    meshes[0].geometry=std::make_shared<const geometry::Mesh>(vertices,std::vector<std::uint8_t>{},std::move(materials),
        std::vector<geometry::MeshPart>{{0,6,0},{6,6,1}});opaque.reset();opacity.reset();
    frame.view.shading=render::Shading::smooth;frame.view.wireframe=render::Wireframe::off;
    ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
    check(observer.expired(),"replaced material image retires after its last frame");
    check(pixel(capture,x*3,y)[1]>150 && pixel(capture,x*3,y)[0]<10 && std::abs(depth(capture,x*3,y)-.4f)<1e-5f,
        "independent opacity map cuts out opaque diffuse pixels");
    check(pixel(capture,x,y*3)[2]>150 && std::abs(depth(capture,x,y*3)-.8f)<1e-5f,
        "opacity map is linear, retaining a value above the alpha threshold");
    frame.view.wireframe=render::Wireframe::only;frame.view.shading=render::Shading::matcap;
    ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
    check(std::abs(depth(capture,x*3,y)-.4f)<1e-5f,"wire-only MatCap depth respects independent opacity maps");
    frame.view.shading=render::Shading::scene_lit;
    ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
    check(std::abs(depth(capture,x*3,y)-.4f)<1e-5f,"wire-only mode retains alpha coverage with Scene Lit selected");
    // A complete material table must also drive Lit coverage and shading,
    // including the unjittered editing-depth pass used by temporal AA.
    meshes[0].stable_key=1;frame.scene_identity=9123;frame.environment={1,1,1};
    frame.view.wireframe=render::Wireframe::off;
    frame.view.lit.ddgi=false;frame.view.lit.gtao=false;frame.view.lit.sssr=false;frame.view.lit.ssil=false;
    frame.view.lit.bloom=false;frame.view.lit.shadows=false;
    std::array<render::Light,1> sunlight{};sunlight[0].kind=render::LightKind::directional;sunlight[0].intensity=4;
    sunlight[0].casts_shadow=false;frame.lights=sunlight;
    for(const auto aa:{render::Antialiasing::none,render::Antialiasing::smaa,render::Antialiasing::taa,render::Antialiasing::smaa_t2x}) {
        frame.view.lit.antialiasing=aa;
        // Compare the same fresh lighting observation on both sides of the
        // studio-only toggle; ordinary consecutive frames have temporal state.
        frame.camera_cut=aa==render::Antialiasing::none;
        ++ui_frame.id;const auto lit_result=take(prepared_render(renderer,frame,&capture));
        if(aa==render::Antialiasing::none) {
            const auto enabled=capture;frame.view.cavity.enabled=false;
            ++ui_frame.id;const auto disabled=take(prepared_render(renderer,frame,&capture));
            if(capture.rgba!=enabled.rgba||capture.depth!=enabled.depth||disabled.stats.viewport_draw_calls!=lit_result.stats.viewport_draw_calls) {
                unsigned maximum{};for(std::size_t i=0;i<capture.rgba.size();++i)maximum=std::max(maximum,unsigned(std::abs(int(capture.rgba[i])-enabled.rgba[i])));
                std::printf("Lit cavity toggle mismatch max=%u depth_equal=%u passes=%llu/%llu\n",maximum,capture.depth==enabled.depth,
                    static_cast<unsigned long long>(lit_result.stats.viewport_draw_calls),static_cast<unsigned long long>(disabled.stats.viewport_draw_calls));
            }
            check(disabled.stats.viewport_draw_calls==lit_result.stats.viewport_draw_calls&&capture.rgba==enabled.rgba&&capture.depth==enabled.depth,
                  "Scene Lit ignores studio cavity without changing output or pass count");
            frame.view.cavity.enabled=true;
        }
        frame.camera_cut=false;
        check(std::abs(depth(capture,x*3,y)-.4f)<1e-5f,"Lit alpha cutout and public FP32 depth agree for every AA mode");
        const auto* red=pixel(capture,x,y);const auto* green=pixel(capture,x*3,y);
        std::printf("Lit material AA=%u red=(%u,%u,%u) green=(%u,%u,%u)\n",static_cast<unsigned>(aa),red[0],red[1],red[2],green[0],green[1],green[2]);
        scene_lit_e2e::png(capture,output/"material-lit-coverage.png");
        check(red[0]>red[1]+20&&green[1]>green[0]+20,"Lit consumes textured and per-slot base colors");
    }
    scene_lit_e2e::png(capture,output/"material-lit-coverage.png");
    frame.view.lit.antialiasing=render::Antialiasing::none;
    frame.environment={};frame.view.lit.direct_reference=true;
    sunlight[0].direction={.89442719f,0,.4472136f};sunlight[0].casts_shadow=true;
    ++ui_frame.id;take(prepared_render(renderer,frame,&capture));const auto unshadowed=pixel(capture,x*3,y)[1];
    frame.view.lit.shadows=true;
    ++ui_frame.id;take(prepared_render(renderer,frame,&capture));const auto blocked=pixel(capture,x*3,y)[1];
    check(blocked<unshadowed*.3f,"ray-query shadow commits opaque texels of a cutout surface");
    {
        // A new mesh for the same object at the same snapshot index must be traced, not the previous one.
        const auto cutout=meshes[0].geometry;
        meshes[0].geometry=std::make_shared<const geometry::Mesh>(std::vector<geometry::Vertex>(vertices.begin()+6,vertices.end()),
            std::vector<std::uint8_t>{},std::vector<material::Slot>{cutout->materials()[1]},std::vector<geometry::MeshPart>{{0,6,0}});
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        check(pixel(capture,x*3,y)[1]>unshadowed*.7f,"a replaced mesh at an unchanged index and identity traces its new triangles");
        meshes[0].geometry=cutout;
    }
    sunlight[0].direction={0,0,1};
    ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
    check(pixel(capture,x*3,y)[1]>unshadowed*.7f,"ray-query shadow traverses transparent texels visible to the camera");
    scene_lit_e2e::png(capture,output/"material-mask-shadow.png");
    {
        // One BLAS is shared by all instances. An opaque sibling, or a change
        // in instance order, must not disable the cutout's ray-query coverage.
        std::array<render::MeshInstance,2> siblings{meshes[0],meshes[0]};
        siblings[1].stable_key=2;siblings[1].world[12]=4;
        auto opaque_surfaces=std::make_shared<std::vector<material::SurfaceRef>>();
        for(const auto& slot:meshes[0].geometry->materials()) {
            auto d=slot.surface->description();d.parameters.coverage=material::Coverage::opaque;
            opaque_surfaces->push_back(std::make_shared<const material::Surface>(std::move(d)));
        }
        siblings[1].surfaces=opaque_surfaces;frame.meshes=siblings;
        for(unsigned order=0;order<2;++order) {
            ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
            check(std::abs(depth(capture,x*3,y)-.4f)<1e-5f&&pixel(capture,x*3,y)[1]>unshadowed*.7f,
                  "shared Mask/Opaque geometry preserves cutout raster and shadow coverage in either instance order");
            std::swap(siblings[0],siblings[1]);
        }
        siblings[1].world[12]=0;frame.meshes=std::span(siblings).subspan(1);
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        check(std::abs(depth(capture,x*3,y)-.8f)<1e-5f,"removing the Mask instance restores the opaque sibling's full surface");
        frame.meshes=meshes;++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        check(std::abs(depth(capture,x*3,y)-.4f)<1e-5f&&pixel(capture,x*3,y)[1]>unshadowed*.7f,
              "restoring Mask after opaque-only geometry restores transparent ray traversal");
    }
    frame.view.lit.shadows=false;
    {
        auto d=meshes[0].geometry->materials()[0].surface->description();
        d.parameters.emissive={8,8,8};d.texture(material::Input::emissive).image=d.texture(material::Input::base_color).image;
        auto resolved=std::make_shared<std::vector<material::SurfaceRef>>();
        resolved->push_back(std::make_shared<const material::Surface>(d));
        resolved->push_back(meshes[0].geometry->materials()[1].surface);
        meshes[0].surfaces=resolved;frame.environment={};frame.lights={};
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        check(pixel(capture,x,y)[0]>150&&pixel(capture,x,y)[1]<30,"emissive texture, not a constant color, supplies visible radiance");
        // A surface-only replacement keeps the exact geometry and stable object.
        d.texture(material::Input::emissive).image=std::make_shared<const material::Image>(1,1,std::vector<std::uint8_t>{0,0,255,255});
        resolved=std::make_shared<std::vector<material::SurfaceRef>>(*resolved);
        (*resolved)[0]=std::make_shared<const material::Surface>(d);meshes[0].surfaces=resolved;
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        check(pixel(capture,x,y)[2]>150&&pixel(capture,x,y)[0]<30,"surface-only content replacement does not reuse old material radiance");
        scene_lit_e2e::png(capture,output/"material-emission-replacement.png");
        const auto front_geometry=meshes[0].geometry;
        std::vector<geometry::Vertex> back_vertices(front_geometry->vertices().begin(),front_geometry->vertices().end());
        for(auto& vertex:back_vertices)for(auto& n:vertex.normal)n=-n;
        meshes[0].geometry=std::make_shared<const geometry::Mesh>(std::move(back_vertices),std::vector<std::uint8_t>{},
            std::vector<material::Slot>(front_geometry->materials().begin(),front_geometry->materials().end()),
            std::vector<geometry::MeshPart>(front_geometry->parts().begin(),front_geometry->parts().end()));
        d.parameters.emission_two_sided=false;
        resolved=std::make_shared<std::vector<material::SurfaceRef>>(*resolved);
        (*resolved)[0]=std::make_shared<const material::Surface>(d);meshes[0].surfaces=resolved;
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        check(pixel(capture,x,y)[2]<5,"visible emission respects a single-sided material even when the surface is double-sided");
        d.parameters.emission_two_sided=true;
        resolved=std::make_shared<std::vector<material::SurfaceRef>>(*resolved);
        (*resolved)[0]=std::make_shared<const material::Surface>(d);meshes[0].surfaces=resolved;
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        check(pixel(capture,x,y)[2]>150,"double-sided emission restores the visible backface");
        meshes[0].geometry=front_geometry;
        frame.view.shading=render::Shading::smooth;
        d.parameters.emissive={};d.parameters.base_color={1,1,1,1};
        d.texture(material::Input::base_color).image.reset();d.texture(material::Input::opacity).image.reset();
        d.parameters.coverage=material::Coverage::opaque;
        resolved=std::make_shared<std::vector<material::SurfaceRef>>(*resolved);meshes[0].surfaces=resolved;
        (*resolved)[0]=std::make_shared<const material::Surface>(d);
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        const auto flat=pixel(capture,x,y)[0];
        d.texture(material::Input::normal).image=std::make_shared<const material::Image>(1,1,std::vector<std::uint8_t>{128,255,128,255});
        // Publish a new owning snapshot; never mutate a captured surface vector.
        resolved=std::make_shared<std::vector<material::SurfaceRef>>(*resolved);
        (*resolved)[0]=std::make_shared<const material::Surface>(d);meshes[0].surfaces=resolved;
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        check(std::abs(int(pixel(capture,x,y)[0])-int(flat))>20,"normal texture changes lighting through the generated tangent frame");
        scene_lit_e2e::png(capture,output/"material-normal.png");
        d.texture(material::Input::normal).image.reset();
        d.parameters.base_color={.25f,.25f,.25f,1};d.parameters.roughness=.8f;d.parameters.metallic=1;
        resolved=std::make_shared<std::vector<material::SurfaceRef>>(*resolved);(*resolved)[0]=std::make_shared<const material::Surface>(d);meshes[0].surfaces=resolved;
        frame.view.shading=render::Shading::scene_lit;frame.lights=sunlight;
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        const std::array constant{pixel(capture,x,y)[0],pixel(capture,x,y)[1],pixel(capture,x,y)[2]};
        auto packed=std::make_shared<const material::Image>(1,1,std::vector<std::uint8_t>{0,204,255,255});
        d.parameters.roughness=1;d.texture(material::Input::roughness).image=packed;d.texture(material::Input::roughness).channel=1;
        d.texture(material::Input::metallic).image=packed;d.texture(material::Input::metallic).channel=2;
        resolved=std::make_shared<std::vector<material::SurfaceRef>>(*resolved);(*resolved)[0]=std::make_shared<const material::Surface>(d);meshes[0].surfaces=resolved;
        ++ui_frame.id;take(prepared_render(renderer,frame,&capture));
        for(std::size_t c=0;c<3;++c)check(std::abs(int(constant[c])-int(pixel(capture,x,y)[c]))<=1,
            "packed linear roughness/metallic channels match constant PBR parameters");

    }
    meshes[0].surfaces.reset();
    frame.meshes={};meshes[0].geometry.reset();++ui_frame.id;result=take(prepared_render(renderer,frame));
    check(opacity_observer.expired() && result.stats.cached_material_images==0,"unused geometry and material images retire together");
    std::puts("Material renderer acceptance: PASS (parts, UV wrap, sRGB, diffuse alpha/linear opacity/depth, MatCap/Lit, all AA coverage, emission replacement, normal map, cache/lifetime)");
}

void material_transport_cases(render::Renderer& renderer,const std::filesystem::path& output={}) {
    namespace material=atelier::material;
    namespace geometry=atelier::geometry;
    using namespace scene_lit_e2e;
    Room room;auto lights=room_lights();lights.resize(3);
    for(auto& light:lights)light.color={1,1,1};
    room.meshes[0].material={.roughness=.8f,.metallic=0};room.meshes[7].material.emissive={};
    const auto textured=[](std::array<std::uint8_t,3> rgb) {
        material::Description d;
        d.texture(material::Input::base_color).image=std::make_shared<const material::Image>(1,1,
            std::vector<std::uint8_t>{rgb[0],rgb[1],rgb[2],255});
        return std::make_shared<const material::Surface>(std::move(d));
    };
    const auto warm=textured({255,32,16}),cool=textured({16,48,255});
    room.meshes[2].surfaces=std::make_shared<const std::vector<material::SurfaceRef>>(std::vector{warm});
    room.meshes[3].surfaces=std::make_shared<const std::vector<material::SurfaceRef>>(std::vector{cool});
    ui::Frame drawing{.id=12000,.extent=take(renderer.target()).extent,.draws={}};
    render::Frame frame{drawing,camera(drawing.extent),{0,0,float(drawing.extent.width),float(drawing.extent.height)},room.meshes};
    frame.scene_identity=12000;frame.scene_revision=1;frame.lights=lights;frame.view.shading=render::Shading::scene_lit;
    auto& options=frame.view.lit;options.antialiasing=render::Antialiasing::none;
    options.gtao=options.sssr=options.ssil=options.bloom=false;
    options.probe_volumes[0].minimum={-5,-1,-1};options.probe_volumes[0].maximum={5,5,8};options.probe_volumes[0].counts={11,7,10};options.probe_update_budget=96;options.probe_rays=64;
    const auto capture=[&](const char* name) {
        render::Capture result;
        // With AA and other temporal effects disabled above, reference DI
        // without DDGI has no history to settle. Each captured frame still
        // integrates 64 independent estimates; discarded frames add no evidence.
        const unsigned frames=options.direct_reference&&!options.ddgi?1u:warmup_frames;
        for(unsigned i=0;i<frames;++i) {pump();++drawing.id;take(prepared_render(renderer,frame,i+1==frames?&result:nullptr));}
        png(result,output/name);return result;
    };
    const auto left=project_region(frame.camera,frame.scene_viewport,drawing.extent,{-3.15f,.01f,1.f});
    const auto right=project_region(frame.camera,frame.scene_viewport,drawing.extent,{3.15f,.01f,1.f});
    options.ddgi=false;const auto direct=capture("material-ddgi-off.png");
    options.ddgi=true;const auto bounce=capture("material-ddgi-textured-bounce.png");
    const auto l0=mean_rgb(direct,left),l1=mean_rgb(bounce,left),r0=mean_rgb(direct,right),r1=mean_rgb(bounce,right);
    std::printf("Textured DDGI warm=(%.3f,%.3f,%.3f) cool=(%.3f,%.3f,%.3f)\n",l1[0]-l0[0],l1[1]-l0[1],l1[2]-l0[2],r1[0]-r0[0],r1[1]-r0[1],r1[2]-r0[2]);
    check(l1[0]-l0[0]>l1[2]-l0[2]+1&&r1[2]-r0[2]>r1[0]-r0[0]+1,"DDGI ray hits transport texture colors to neutral receivers");
    // An unrelated cutout island must not change the closed wall's probe support.
    const auto cube=geometry::cube_mesh();std::vector<geometry::Vertex> vertices(cube->vertices().begin(),cube->vertices().end());
    vertices.insert(vertices.end(),{{{100,-1,0},{0,0,-1},{0,1}},{{100,1,0},{0,0,-1},{0,0}},{{102,1,0},{0,0,-1},{1,0}}});
    material::Description cutout;cutout.parameters.coverage=material::Coverage::mask;cutout.parameters.base_color[3]=0;
    const auto invisible=std::make_shared<const material::Surface>(cutout);
    room.meshes[2].geometry=std::make_shared<const geometry::Mesh>(std::move(vertices),std::vector<std::uint8_t>{},
        std::vector<material::Slot>{{"wall",warm},{"unrelated-cutout",invisible}},std::vector<geometry::MeshPart>{{0,36,0},{36,3,1}});
    room.meshes[2].surfaces=std::make_shared<const std::vector<material::SurfaceRef>>(std::vector{warm,invisible});
    ++frame.scene_identity;++frame.scene_revision;const auto mixed=capture("material-ddgi-mixed-components.png");
    const auto lm=mean_rgb(mixed,left);
    check(std::abs(lm[0]-l1[0])<4&&std::abs(lm[2]-l1[2])<4,"disconnected cutout does not disable an opaque shell's DDGI closure");
    room.meshes[2].surfaces=std::make_shared<const std::vector<material::SurfaceRef>>(std::vector{cool,invisible});
    ++frame.scene_revision;const auto updated=capture("material-ddgi-texture-update.png");
    options.ddgi=false;const auto updated_direct=capture("material-ddgi-update-off.png");
    const auto lu=mean_rgb(updated,left),ld=mean_rgb(updated_direct,left);
    check(lu[2]-ld[2]>lu[0]-ld[0]+1,"texture-only edit updates DDGI without replacing geometry or object identity");
    // The source is above the camera frustum. Its image must still illuminate
    // receivers through mesh NEE, independent of screen visibility and DDGI.
    frame.lights={};options.direct_reference=true;
    room.meshes[7].geometry=std::make_shared<const geometry::Mesh>(std::vector<geometry::Vertex>{
        {{-2,6,2},{0,-1,0},{0,0}},{{2,6,2},{0,-1,0},{1,0}},{{2,6,6},{0,-1,0},{1,1}},
        {{-2,6,2},{0,-1,0},{0,0}},{{2,6,6},{0,-1,0},{1,1}},{{-2,6,6},{0,-1,0},{0,1}}});
    room.meshes[7].world=box(0,0,0,1,1,1);
    material::Description emitter;emitter.parameters.base_color={0,0,0,1};emitter.parameters.emissive={10,10,10};
    emitter.parameters.emission_two_sided=false;
    const auto publish_emitter=[&] {
        room.meshes[7].surfaces=std::make_shared<const std::vector<material::SurfaceRef>>(
            std::vector<material::SurfaceRef>{std::make_shared<const material::Surface>(emitter)});
        ++frame.scene_revision;
    };
    emitter.texture(material::Input::emissive).image=warm->description().texture(material::Input::base_color).image;
    publish_emitter();const auto emitted_warm=mean_rgb(capture("material-offscreen-emission-warm.png"),left);
    emitter.texture(material::Input::emissive).image=cool->description().texture(material::Input::base_color).image;
    publish_emitter();const auto emitted_cool=mean_rgb(capture("material-offscreen-emission-cool.png"),left);
    check(emitted_warm[0]>emitted_warm[2]+20&&emitted_cool[2]>emitted_cool[0]+20,
          "offscreen textured emission and content-only replacement illuminate real receivers");
    room.meshes[7].world=box(0,12,0,1,-1,1);++frame.scene_revision;
    const auto back=mean_rgb(capture("material-single-sided-emission-back.png"),left);
    check(back[0]<2&&back[1]<2&&back[2]<2,"single-sided emission respects authored orientation under mirror transforms");
    emitter.parameters.emission_two_sided=true;publish_emitter();
    const auto two_sided=mean_rgb(capture("material-two-sided-emission.png"),left);
    check(two_sided[2]>two_sided[0]+20,"two-sided emission is independent from geometric front-facing");
    emitter.parameters.coverage=material::Coverage::mask;emitter.parameters.base_color[3]=0;publish_emitter();
    const auto masked=mean_rgb(capture("material-masked-emission.png"),left);
    check(masked[0]<2&&masked[1]<2&&masked[2]<2,"discarded emitter texels cannot contribute light");
    frame.meshes={};frame.lights={};++drawing.id;take(prepared_render(renderer,frame));
    std::puts("Material transport acceptance: PASS (textured DDGI, material edits, disconnected Mask/opaque closure, offscreen textured NEE, emission sides/Mask)");
}
