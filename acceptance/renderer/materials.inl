void material_cases(render::Renderer& renderer) {
    namespace geometry=atelier::geometry;
    ui::Frame ui_frame{.id=9000,.extent=take(renderer.target()).extent,.draws={}};
    std::vector<geometry::Vertex> vertices;
    const auto quad=[&](float z) {
        vertices.insert(vertices.end(),{
            {{-1,-1,z},{0,0,-1},{-1,1}},{{-1,1,z},{0,0,-1},{-1,0}},{{1,1,z},{0,0,-1},{0,0}},
            {{-1,-1,z},{0,0,-1},{-1,1}},{{1,1,z},{0,0,-1},{0,0}},{{1,-1,z},{0,0,-1},{0,1}}});
    };
    quad(.2f);quad(.6f);
    auto image=std::make_shared<const geometry::Image>(2,2,std::vector<std::uint8_t>{255,0,0,255, 255,255,255,0, 0,0,255,255, 255,255,0,255});
    std::weak_ptr<const geometry::Image> observer=image;
    std::vector<geometry::Material> materials{{"cutout",{1,1,1,1},image,{}},{"background",{0,1,0,1},{},{}}};
    std::array<render::MeshInstance,1> meshes{};meshes[0].color={1,1,1,1};
    meshes[0].geometry=std::make_shared<const geometry::Mesh>(vertices,std::vector<std::uint8_t>{},std::move(materials),
        std::vector<geometry::MeshPart>{{0,6,0},{6,6,1}});image.reset();
    render::Frame frame{ui_frame,{}, {0,0,float(ui_frame.extent.width),float(ui_frame.extent.height)},meshes};render::Capture capture;
    auto result=take(renderer.render(frame,&capture));
    const auto x=ui_frame.extent.width/4,y=ui_frame.extent.height/4;
    check(result.stats.scene_draw_calls==2 && result.stats.material_upload_count>0,"material parts bind distinct surfaces");
    check(pixel(capture,x,y)[0]>150 && pixel(capture,x,y)[1]<10 && std::abs(depth(capture,x,y)-.2f)<1e-5f,
        "negative UVs repeat with correct top-left image orientation and sRGB color");
    check(pixel(capture,x*3,y)[1]>150 && pixel(capture,x*3,y)[0]<10 && std::abs(depth(capture,x*3,y)-.6f)<1e-5f,
        "alpha cutout exposes background color and depth");
    ++ui_frame.id;result=take(renderer.render(frame,&capture));check(result.stats.material_upload_count==0,"unchanged material images upload only once");
    frame.view.wireframe=render::Wireframe::only;
    ++ui_frame.id;take(renderer.render(frame,&capture));
    check(std::abs(depth(capture,x*3,y)-.6f)<1e-5f,"wire-only depth pass respects material opacity");
    frame.view.shading=render::Shading::matcap;
    ++ui_frame.id;take(renderer.render(frame,&capture));check(std::abs(depth(capture,x*3,y)-.6f)<1e-5f,"MatCap respects cutout silhouettes");
    auto opaque=std::make_shared<const geometry::Image>(2,2,std::vector<std::uint8_t>{255,0,0,255, 255,255,255,255, 0,0,255,255, 255,255,0,255});
    auto opacity=std::make_shared<const geometry::Image>(2,2,std::vector<std::uint8_t>{255,255,255,255, 0,0,0,255, 160,160,160,255, 255,255,255,255});
    std::weak_ptr<const geometry::Image> opacity_observer=opacity;
    materials={{"separate opacity",{1,1,1,1},opaque,opacity},{"background",{0,1,0,1},{},{}}};
    meshes[0].geometry=std::make_shared<const geometry::Mesh>(vertices,std::vector<std::uint8_t>{},std::move(materials),
        std::vector<geometry::MeshPart>{{0,6,0},{6,6,1}});opaque.reset();opacity.reset();
    frame.view.shading=render::Shading::smooth;frame.view.wireframe=render::Wireframe::off;
    ++ui_frame.id;take(renderer.render(frame,&capture));
    check(observer.expired(),"replaced material image retires after its last frame");
    check(pixel(capture,x*3,y)[1]>150 && pixel(capture,x*3,y)[0]<10 && std::abs(depth(capture,x*3,y)-.6f)<1e-5f,
        "independent opacity map cuts out opaque diffuse pixels");
    check(pixel(capture,x,y*3)[2]>150 && std::abs(depth(capture,x,y*3)-.2f)<1e-5f,
        "opacity map is linear, retaining a value above the alpha threshold");
    frame.view.wireframe=render::Wireframe::only;frame.view.shading=render::Shading::matcap;
    ++ui_frame.id;take(renderer.render(frame,&capture));
    check(std::abs(depth(capture,x*3,y)-.6f)<1e-5f,"wire-only MatCap depth respects independent opacity maps");
    frame.meshes={};meshes[0].geometry.reset();++ui_frame.id;result=take(renderer.render(frame));
    check(opacity_observer.expired() && result.stats.cached_material_images==0,"unused geometry and material images retire together");
    std::puts("Material renderer acceptance: PASS (parts, UV wrap, sRGB, diffuse alpha/linear opacity/depth, MatCap, cache/lifetime)");
}
