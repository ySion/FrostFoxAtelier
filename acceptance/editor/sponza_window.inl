void verify_sponza_ddgi(atelier::application::Application& app,const std::filesystem::path& output) {
    using Access=atelier::application::EditorAcceptanceAccess;
    namespace render=atelier::renderer;
    namespace world=atelier::world;
    namespace presentation=atelier::presentation;
    // Fitting the DDGI volume writes World lighting; the dynamic checks below must restore this fitted state.
    model(app.configure_view({8,2,0},{-6,3,0},render::Shading::scene_lit,true));
    const auto authored=model(app.document().serialize());
    tick(app,128);
    auto settings=Access::scene_lit_view(app).lit;
    const auto& meshes=Access::scene_snapshot(app).meshes;
    require(meshes.size()==1 && Access::scene_snapshot(app).lights.size()==3,
        "Sponza Scene Lit did not consume the complete model and three World lights.");
    for(const auto& item:model(app.document().presentation().render_items())) for(const auto& vertex:item.geometry->vertices()) {
        const auto p=atelier::math::transform_point(atelier::math::Vector3{vertex.position[0],vertex.position[1],vertex.position[2]},item.world);
        for(unsigned axis=0;axis<3;++axis) require(p[axis]>=settings.probe_volumes[0].minimum[axis] && p[axis]<=settings.probe_volumes[0].maximum[axis],
            "Sponza DDGI volume excludes authored geometry.");
    }
    const auto bounds=app.presented()->find(atelier::editor::control::viewport)->bounds;
    const auto shot=[&](const char* name,unsigned frames) {
        tick(app,frames);render::Capture full;
        require(app.step(&full) && full.frame_id,"Sponza DDGI capture was not accepted.");
        render::Capture image;image.extent={static_cast<unsigned>(bounds.width),static_cast<unsigned>(bounds.height)};
        image.frame_id=full.frame_id;image.rgba.resize(std::size_t(image.extent.width)*image.extent.height*4);
        image.depth.resize(std::size_t(image.extent.width)*image.extent.height);
        for(unsigned y=0;y<image.extent.height;++y) for(unsigned x=0;x<image.extent.width;++x) {
            const auto from=std::size_t(y+static_cast<unsigned>(bounds.y))*full.extent.width+x+static_cast<unsigned>(bounds.x);
            const auto to=std::size_t(y)*image.extent.width+x;
            std::copy_n(full.rgba.begin()+from*4,4,image.rgba.begin()+to*4);image.depth[to]=full.depth[from];
        }
        png(image,output/(std::string(name)+".png"));return image;
    };
    const auto luminance=[](const render::Capture& image) {
        double sum=0;std::size_t count=0;
        for(std::size_t i=0;i<image.depth.size();++i) if(image.depth[i]>0 && image.depth[i]<=1) {
            sum+=.2126*image.rgba[i*4]+.7152*image.rgba[i*4+1]+.0722*image.rgba[i*4+2];++count;
        }
        require(count>10000,"Sponza DDGI capture lacks scene coverage.");return sum/static_cast<double>(count);
    };
    const auto difference=[](const render::Capture& a,const render::Capture& b) {
        double sum=0;std::size_t count=0;
        require(a.extent.width==b.extent.width && a.extent.height==b.extent.height,"Sponza comparison changed extent.");
        for(std::size_t i=0;i<a.depth.size();++i) if(a.depth[i]>0 && a.depth[i]<=1) {
            for(unsigned c=0;c<3;++c) sum+=std::abs(int(a.rgba[i*4+c])-int(b.rgba[i*4+c]));
            count+=3;
        }
        require(count>0,"Sponza comparison has no geometry.");return sum/static_cast<double>(count);
    };
    shot("sponza-scene-lit",0);
    require(app.frame_statistics().scene_lit_probe_rays>0 && app.frame_statistics().scene_lit_shadow_rays>0,
        "Sponza did not execute hardware probe and visibility rays.");
    std::ofstream report(output/"ddgi.txt");
    report<<"Sponza DDGI: 262267 triangles, 3 World lights; real D3D12 GPU validation.\n"
        <<"Volume min="<<settings.probe_volumes[0].minimum[0]<<','<<settings.probe_volumes[0].minimum[1]<<','<<settings.probe_volumes[0].minimum[2]
        <<" max="<<settings.probe_volumes[0].maximum[0]<<','<<settings.probe_volumes[0].maximum[1]<<','<<settings.probe_volumes[0].maximum[2]
        <<" grid="<<settings.probe_volumes[0].counts[0]<<'x'<<settings.probe_volumes[0].counts[1]<<'x'<<settings.probe_volumes[0].counts[2]
        <<" budget="<<settings.probe_update_budget<<" rays/probe="<<settings.probe_rays<<'\n';
    // Sponza has no sky; isolate indirect diffuse from AO, screen reflections and bloom. Opening it fitted the probe
    // volume into a new Environment without marking the document modified.
    require(atelier::rendering::sky_radiance(model(app.document().presentation().environment()))==std::array<float,3>{} &&
        app.document().environment()!=atelier::ObjectId{} && !app.document().dirty(),"Sponza unexpectedly has environment radiance, or its fitted Environment is missing or modified.");
    // These are World-authored settings; the test edits them like the World panel and restores them below.
    const auto authored_lighting=settings;
    settings.gtao=false;settings.sssr=false;settings.bloom=false;
    model(Access::scene_lit_settings(app,settings));
    auto on=shot("sponza-ddgi-on",128);
    const auto stable=shot("sponza-ddgi-stable",32);
    settings.ddgi=false;model(Access::scene_lit_settings(app,settings));
    const auto off=shot("sponza-ddgi-off",64);
    require(app.frame_statistics().scene_lit_shadow_rays>0 && app.frame_statistics().scene_lit_probe_rays==0,
        "Sponza DDGI Off did not reach the renderer.");
    const auto contribution=difference(on,off),noise=difference(on,stable);
    report<<"Static mean absolute RGB difference (0..255): DDGI="<<contribution<<" temporal="<<noise<<'\n';report.flush();
    require(contribution>.5 && contribution>noise*2,"Sponza DDGI contribution is absent or indistinguishable from temporal noise.");
    settings.ddgi=true;model(Access::scene_lit_settings(app,settings));tick(app,64);
    const auto& scene=app.document().world();auto& runtime=scene.runtime();
    const auto edit=[&](atelier::ObjectId component,std::string_view name,float value) {
        const auto fields=model(app.document().properties(component));
        const auto field=std::ranges::find(fields,name,&atelier::editor::Property::name);
        require(field!=fields.end(),"Sponza light has no requested editable property.");
        model(Access::edit_field(app,component,field->path,value));
    };
    const auto light_items=model(app.document().presentation().light_items());
    const auto moving=std::ranges::find_if(light_items,[](const auto& item){return item.settings.kind==presentation::LightKind::point && item.settings.color.x>.9f;});
    require(moving!=light_items.end(),"Sponza has no warm movable light.");
    const auto original=model(world::read_transform(runtime,moving->entity));
    std::ofstream motion(output/"ddgi-motion.csv");motion<<"frame,x,y,z,history_resets\n";
    for(unsigned frame=1;frame<=32;++frame) {
        auto transform=original;transform.position.x+=8.f*static_cast<float>(frame)/32.f;
        edit(moving->entity,"position.x",transform.position.x);tick(app,1);
        const auto resets=app.frame_statistics().scene_lit_history_resets;
        motion<<frame<<','<<transform.position.x<<','<<transform.position.y<<','<<transform.position.z<<','<<resets<<'\n';
        require(resets==0,"Moving a Sponza light reset the DDGI history domain.");
        if(frame==8) shot("sponza-moving-08",0);
        if(frame==16) shot("sponza-moving-16",0);
        if(frame==32) shot("sponza-moving-32",0);
    }
    const auto moved=shot("sponza-moved-ddgi-on",128);
    settings.ddgi=false;model(Access::scene_lit_settings(app,settings));
    const auto moved_off=shot("sponza-moved-ddgi-off",64);
    const auto moved_contribution=difference(moved,moved_off);
    double indirect_change=0;std::size_t indirect_samples=0;
    for(std::size_t i=0;i<on.depth.size();++i) if(on.depth[i]>0 && on.depth[i]<=1) for(unsigned c=0;c<3;++c) {
        const auto pixel=i*4+c;
        indirect_change+=std::abs((int(moved.rgba[pixel])-int(moved_off.rgba[pixel]))-(int(on.rgba[pixel])-int(off.rgba[pixel])));
        ++indirect_samples;
    }
    indirect_change/=static_cast<double>(indirect_samples);
    report<<"Moved light: image change="<<difference(on,moved)<<" DDGI contribution="<<moved_contribution
        <<" indirect difference-of-differences="<<indirect_change<<'\n';report.flush();
    require(difference(on,moved)>1 && moved_contribution>.5 && indirect_change>std::max(.5,noise*2),
        "Sponza moved light did not produce a new indirect-lit result.");
    settings.ddgi=true;model(Access::scene_lit_settings(app,settings));tick(app,64);
    for(const auto& item:light_items) edit(item.component,"settings.intensity",0);
    const auto dark_early=shot("sponza-lights-off-early",1);
    shot("sponza-lights-off-128",128);
    shot("sponza-lights-off-256",128);
    const auto dark_late=shot("sponza-lights-off-settled",128);
    // With lights and probes off only editor overlays remain; stale probe light is what stays above that floor.
    settings.ddgi=false;model(Access::scene_lit_settings(app,settings));
    const auto unlit=luminance(shot("sponza-lights-off-ddgi-off",64));
    const auto early=luminance(dark_early)-unlit,late=luminance(dark_late)-unlit;
    report<<"Lights off luminance (0..255): early="<<luminance(dark_early)<<" settled="<<luminance(dark_late)<<" unlit overlay floor="<<unlit<<'\n';report.flush();
    require(early>.5 && late<early*.3,"Sponza retained stale probe lighting after lights were switched off.");
    for(const auto& item:light_items) edit(item.component,"settings.intensity",item.settings.intensity);
    edit(moving->entity,"position.x",original.position.x);
    model(Access::scene_lit_settings(app,authored_lighting));
    require(model(app.document().serialize())==authored,"Sponza dynamic acceptance did not restore authored World data.");
    model(app.configure_view({8,2,0},{-6,3,0},render::Shading::scene_lit));
    require(Access::scene_lit_view(app).lit.probe_volumes[0].minimum==settings.probe_volumes[0].minimum,"Camera configuration unexpectedly rebuilt the DDGI volume.");
    report<<"PASS.\n";
    require(report.good() && motion.good(),"Cannot write Sponza DDGI evidence.");
}

int verify_sponza_window(atelier::application::Application& app,const char* output_directory) try {
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(initialized) || initialized==RPC_E_CHANGED_MODE,"Cannot initialize capture encoding.");
    struct Apartment {bool owns;~Apartment(){if(owns) CoUninitialize();}} apartment{SUCCEEDED(initialized)};
    const auto output=std::filesystem::absolute(output_directory);std::filesystem::create_directories(output);
    const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    model(app.open_project(root/"assets/examples"));model(app.open_scene("Examples/Sponza/Scenes/Sponza.scene"));
    require(model(app.document().presentation().render_items()).front().geometry->vertices().size()==786801,"Sponza viewport lost model geometry.");
    model(app.configure_view({8,2,0},{-6,3,0},atelier::renderer::Shading::smooth));tick(app,3);
    atelier::renderer::Capture image;require(app.step(&image) && image.frame_id,"Sponza frame was not accepted.");
    const auto view=app.presented()->find(atelier::editor::control::viewport);require(view!=nullptr,"Sponza viewport is absent.");
    std::size_t covered=0,colored=0;
    for(unsigned y=static_cast<unsigned>(view->bounds.y);y<view->bounds.y+view->bounds.height && y<image.extent.height;++y)
        for(unsigned x=static_cast<unsigned>(view->bounds.x);x<view->bounds.x+view->bounds.width && x<image.extent.width;++x) {
            const auto i=std::size_t(y)*image.extent.width+x;
            if(image.depth[i]<=0 || image.depth[i]>1) continue;
            ++covered;
            const auto r=image.rgba[i*4],g=image.rgba[i*4+1],b=image.rgba[i*4+2];
            colored+=std::max({r,g,b})-std::min({r,g,b})>50;
        }
    require(covered>10000 && colored>1000,"Sponza capture lacks complete geometry or colored material textures.");
    require(app.frame_statistics().material_upload_count==0 && app.frame_statistics().cached_material_images>20,
        "Sponza warm frame reuploaded textures or lost the material cache.");
    png(image,output/"sponza-interior.png");
    model(app.configure_view({42,32,-34},{0,4,0},atelier::renderer::Shading::smooth));tick(app,2);capture(app,output/"sponza-overview.png");
    model(app.configure_view({8,2,0},{-6,3,0},atelier::renderer::Shading::matcap));tick(app,2);capture(app,output/"sponza-matcap.png");
    verify_sponza_ddgi(app,output);
    std::ofstream report(output/"acceptance.txt");report<<"Sponza window acceptance: PASS\n262267 triangles; full model; Smooth diffuse textures; MatCap; Scene Lit/DDGI on-off, moving light and lights-off decay; real D3D12 GPU validation. See ddgi.txt and ddgi-motion.csv.\n";
    require(report.good(),"Cannot write Sponza acceptance report.");app.close();
    std::puts("Sponza window acceptance: PASS");return 0;
} catch(const std::exception& error) {std::fprintf(stderr,"Sponza window acceptance: FAIL: %s\n",error.what());return 1;}
