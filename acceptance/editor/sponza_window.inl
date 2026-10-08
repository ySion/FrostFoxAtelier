int verify_sponza_window(atelier::application::Application& app,const char* output_directory) try {
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(initialized) || initialized==RPC_E_CHANGED_MODE,"Cannot initialize capture encoding.");
    struct Apartment {bool owns;~Apartment(){if(owns) CoUninitialize();}} apartment{SUCCEEDED(initialized)};
    const auto output=std::filesystem::absolute(output_directory);std::filesystem::create_directories(output);
    const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    model(app.open_project(root/"examples/physics-test"));model(app.open_scene("Content/Scenes/Sponza.scene"));
    require(model(app.document().world().render_items()).front().geometry->vertices().size()==786801,"Sponza viewport lost model geometry.");
    model(app.configure_view({8,2,0},{-6,3,0},atelier::renderer::Shading::smooth));tick(app,3);
    atelier::renderer::Capture image;require(app.step(&image) && image.frame_id,"Sponza frame was not accepted.");
    const auto view=app.presented()->find(atelier::editor::control::viewport);require(view!=nullptr,"Sponza viewport is absent.");
    std::size_t covered=0,colored=0;
    for(unsigned y=static_cast<unsigned>(view->bounds.y);y<view->bounds.y+view->bounds.height && y<image.extent.height;++y)
        for(unsigned x=static_cast<unsigned>(view->bounds.x);x<view->bounds.x+view->bounds.width && x<image.extent.width;++x) {
            const auto i=std::size_t(y)*image.extent.width+x;
            if(image.depth[i]>=1) continue;
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
    std::ofstream report(output/"acceptance.txt");report<<"Sponza window acceptance: PASS\n262267 triangles; full model; diffuse materials; shared mipmapped textures; interior/overview/MatCap; real D3D12 validation.\n";
    require(report.good(),"Cannot write Sponza acceptance report.");app.close();
    std::puts("Sponza window acceptance: PASS");return 0;
} catch(const std::exception& error) {std::fprintf(stderr,"Sponza window acceptance: FAIL: %s\n",error.what());return 1;}
