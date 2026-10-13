std::filesystem::path scene_lit_window_example(const std::filesystem::path& requested) {
    if (!requested.empty()) return std::filesystem::canonical(requested);
    for (auto base : {std::filesystem::current_path(),atelier::editor::asset_directory()})
        for (unsigned depth=0;depth<16 && !base.empty();++depth,base=base.parent_path())
            if (std::filesystem::is_regular_file(base/"assets/examples/Config/Project.json"))
                return std::filesystem::canonical(base/"assets/examples");
    throw std::runtime_error("Cannot locate assets/examples; pass its project directory to --acceptance-scene-lit.");
}
std::string scene_lit_selection_diagnostic(Application& app) {
    auto& runtime=app.document().world().runtime();
    const auto type_name=[&](atelier::ObjectId object) {
        if(object==atelier::ObjectId{}) return std::string("none");
        const auto type=runtime.type_of(object);
        if(!type) return std::string("invalid: ")+type.error().message;
        const auto info=runtime.describe(*type);
        return info?(*info)->name:std::string("unknown type");
    };
    const auto selected=app.document().selection();
    std::string result="; selected Actor="+type_name(selected.actor)+"/"+atelier::editor::utf8(app.document().name(selected.actor))+
        "; selected Object="+type_name(selected.object)+"; components=";
    const auto components=atelier::world::components(runtime,selected.actor);
    if(!components) return result+components.error().message;
    for(const auto& component:*components) result+=type_name(component.id())+",";
    return result;
}
void scene_lit_display(Application& app,ControlKey option) {
    using Access=atelier::application::EditorAcceptanceAccess;
    const auto history=app.document().undo_count();
    const auto document=model(app.document().serialize());
    click(app,control::scene_shading);
    const auto* item=app.menu_frame()?app.menu_frame()->find(option):nullptr;
    require(item && item->menu_behavior==atelier::ui::MenuBehavior::radio && item->radio_group,
        "Scene Lit display choice is absent from the formal native radio menu.");
    click(app,option);tick(app,2);
    require(!app.menu_frame() && app.document().undo_count()==history && model(app.document().serialize())==document,
        "Native Scene Lit/MatCap display choice changed authored scene data or history.");
    const auto expected=option==control::shading_scene_lit?atelier::renderer::Shading::scene_lit:atelier::renderer::Shading::matcap;
    require(Access::scene_lit_view(app).shading==expected,"Native display choice did not reach the accepted rendering policy.");
}
std::size_t scene_lit_pixel_difference(const atelier::renderer::Capture& a,const atelier::renderer::Capture& b) {
    require(a.extent==b.extent,"Scene Lit product captures have different physical extents.");
    std::size_t changed=0;
    for (std::size_t index=0;index<a.depth.size();++index) {
        if (a.depth[index]<=0 || b.depth[index]<=0 || std::abs(a.depth[index]-b.depth[index])>1e-4f) continue;
        for (unsigned channel=0;channel<3;++channel)
            if (std::abs(int(a.rgba[index*4+channel])-int(b.rgba[index*4+channel]))>3) {++changed;break;}
    }
    return changed;
}
void scene_lit_antialiasing(Application& app,unsigned mode) {
    using Access=atelier::application::EditorAcceptanceAccess;
    const auto history=app.document().undo_count();
    const auto document=model(app.document().serialize());
    click(app,control::scene_shading);
    click(app,control::lit_menu);
    click(app,control::lit_aa_menu);
    const auto option=control::lit_aa_first+mode;
    const auto* item=app.menu_frame()?app.menu_frame()->find(option):nullptr;
    require(item && item->menu_behavior==atelier::ui::MenuBehavior::radio && item->radio_group==control::lit_aa_first,
        "Native Scene Lit menu omitted the formal AA radio policy.");
    click(app,option);tick(app,2);
    require(!app.menu_frame() && Access::scene_lit_view(app).lit.antialiasing==static_cast<atelier::renderer::Antialiasing>(mode) &&
        app.document().undo_count()==history && model(app.document().serialize())==document,
        "Native AA selection did not reach the accepted view policy independently of authored scene data/history.");
}
void verify_scene_lit_window(Application& app,const std::filesystem::path& output,const std::filesystem::path& requested) {
    using Access=atelier::application::EditorAcceptanceAccess;
    using atelier::editor::Command;
    namespace render=atelier::renderer;
    namespace world=atelier::world;
    const auto source=scene_lit_window_example(requested);
    const auto outside=output.lexically_relative(source);
    require(!outside.empty() && *outside.begin()==L"..","Scene Lit evidence output must be outside the source example project.");
    auto source_project=model(atelier::editor::Project::open(source));
    const auto source_scene=model(source_project.resolve("Examples/Scene Lit Room/Scenes/Main.scene"));
    const auto source_text=model(atelier::editor::read_file(source_scene));
    const auto source_config=model(atelier::editor::read_file(source/"Config/Project.json"));
    const auto copy=output/(L"Project-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(copy);
    for (const auto* directory : {"Config","Content","Examples"})
        std::filesystem::copy(source/directory,copy/directory,
            std::filesystem::copy_options::recursive|std::filesystem::copy_options::skip_symlinks);
    std::filesystem::create_directories(copy/"Source");
    Access::mark_saved(app);model(app.open_project(copy));model(app.open_scene("Examples/Scene Lit Room/Scenes/Main.scene"));tick(app,4);
    require(!app.document().dirty() && actors(app).size()==15 &&
        model(app.document().world().render_items()).size()==10 && model(app.document().world().light_items()).size()==5,
        "The formal application failed to load the complete Scene Lit example World.");
    const auto loaded=model(app.document().serialize());
    scene_lit_display(app,control::shading_first+2);
    render::Capture matcap;require(app.step(&matcap),"Scene Lit example MatCap frame failed.");png(matcap,output/"01-example-matcap.png");
    scene_lit_display(app,control::shading_scene_lit);
    tick(app,96);
    render::Capture lit;require(app.step(&lit),"Scene Lit example formal GPU frame failed.");png(lit,output/"02-example-scene-lit.png");
    require(scene_lit_pixel_difference(matcap,lit)>100,"Native Scene Lit selection did not change actual geometry pixels.");
    const auto& initial=Access::scene_snapshot(app);
    require(initial.meshes.size()==10 && initial.lights.size()==4 && initial.identity &&
        app.frame_statistics().scene_lit_shadow_sources>=4 &&
        app.frame_statistics().scene_lit_shadow_sources==app.frame_statistics().scene_lit_light_sources &&
        app.frame_statistics().scene_draw_calls>0 && app.frame_statistics().ui_draw_calls>0 &&
        std::ranges::all_of(initial.meshes,[](const auto& mesh){return mesh.stable_key!=0;}) &&
        std::ranges::all_of(initial.lights,[](const auto& light){return light.stable_key!=0;}),
        "Loaded World light/material identities did not reach the ordinary Scene Lit Renderer frame.");
    require(model(app.document().serialize())==loaded,"Selecting Scene Lit changed the example scene.");

    for(unsigned mode=0;mode<4;++mode) {
        scene_lit_antialiasing(app,mode);tick(app,16);
        render::Capture image;require(app.step(&image),"Native AA policy did not accept a real GPU frame.");
        require(std::count_if(image.depth.begin(),image.depth.end(),[](float value){return value>0;})>1000 &&
            Access::scene_snapshot(app).meshes.size()==10 && app.frame_statistics().scene_draw_calls>0,
            "Native AA choice lost the example's actual visible geometry.");
        png(image,output/("02-example-aa-"+std::to_string(mode)+".png"));
    }
    scene_lit_antialiasing(app,static_cast<unsigned>(render::Antialiasing::smaa));

    const auto authored_lights=model(app.document().world().light_items());
    const auto sky=std::ranges::find_if(authored_lights,[](const auto& light){return light.settings.kind==world::LightKind::sky;});
    require(sky!=authored_lights.end(),"Example has no explicit Sky Light to replace.");
    model(Access::select_actor(app,sky->actor));tick(app,2);
    click(app,atelier::editor::menu_key(atelier::editor::CommandGroup::edit));
    click(app,static_cast<ControlKey>(Command::erase_selected));tick(app,2);
    require(Access::scene_snapshot(app).environment==std::array<float,3>{},"Deleting the only Sky Light left environment radiance.");
    constexpr std::array commands{Command::create_point_light,Command::create_rectangle_light,Command::create_directional_light,Command::create_sky_light};
    constexpr std::array kinds{world::LightKind::point,world::LightKind::rectangle,world::LightKind::directional,world::LightKind::sky};
    constexpr std::array captions{L"Intensity (cd)",L"Intensity (cd/m²)",L"Illuminance (lux)",L"Radiance (cd/m²)"};
    constexpr std::array intensities{79.25f,22.5f,3.25f,.12f};
    std::array<atelier::ObjectId,4> created;
    for (std::size_t index=0;index<commands.size();++index) {
        const auto history=app.document().undo_count();
        const auto count=actors(app).size();
        std::printf("Scene Lit window step: create light %zu (%llu)\n",index,static_cast<unsigned long long>(commands[index]));
        click(app,control::create_menu);
        const auto command=static_cast<ControlKey>(commands[index]);
        require(app.menu_frame() && app.menu_frame()->find(command),"Native Create menu omitted a real light command.");
        click(app,command);tick(app,2);
        const auto diagnostic=scene_lit_selection_diagnostic(app);
        if(actors(app).size()!=count+1 || app.document().undo_count()!=history+1)
            throw std::runtime_error("Scene Lit native light creation step "+std::to_string(index)+
                " did not publish one Actor/history entry"+diagnostic);
        created[index]=app.document().selection().actor;
        auto& runtime=app.document().world().runtime();
        const auto root=model(world::root_component(runtime,created[index]));
        if(!root || !model(runtime.is_a(model(runtime.type_of(*root)),app.document().types().light_component)))
            throw std::runtime_error("Scene Lit native light creation step "+std::to_string(index)+
                " selected an Actor without its expected LightComponent root"+diagnostic);
        const auto light=*root;
        require(actors(app).size()==count+1 && app.document().undo_count()==history+1 &&
            model(world::read_light(runtime,light)).kind==kinds[index],
            "Native light creation did not create one complete, undoable World Actor.");
        click(app,static_cast<ControlKey>(Command::show_parameters));
        const auto fields=model(app.document().properties(light));
        const auto has_field=[&](std::string_view name) {
            const auto field=std::ranges::find(fields,name,&atelier::editor::Property::name);
            return field!=fields.end() && app.presented()->find(app.field_control(light,field->path));
        };
        require(index==1?has_field("settings.width") && has_field("settings.height"):!has_field("settings.width"),
            "Real Parameters exposed the wrong light-shape fields.");
        if(index==2) require(!has_field("settings.range") && has_field("settings.source_radius"),
            "Directional Parameters used a local-light range or omitted angular radius.");
        if(index==3) require(!has_field("settings.range") && !has_field("settings.source_radius") && !has_field("settings.casts_shadow"),
            "Sky Light Parameters exposed local emitter or direct shadow controls.");
        const auto intensity=std::ranges::find(fields,"settings.intensity",&atelier::editor::Property::name);
        require(intensity!=fields.end(),"Created light has no reflected intensity field.");
        const auto edit_history=app.document().undo_count();
        click(app,app.field_control(light,intensity->path));
        capture(app,output/("03-before-edit-light-"+std::to_string(index)+".png"));
        // Static captions are painted coverage, not editable TextGeometry.
        // The document E2E checks the exact unit string; this native window
        // check requires its adjacent label to be visibly painted as well.
        const auto* input=app.presented()->find(app.field_control(light,intensity->path));
        require(input && std::ranges::any_of(app.presented()->controls,[&](const auto& label) {
            const auto centre_y=input->bounds.y+input->bounds.height*.5f;
            return label.kind==atelier::ui::ControlKind::label &&
                label.bounds.x+label.bounds.width<=input->bounds.x+1 &&
                label.bounds.x>=app.presented()->find(control::parameter_list)->bounds.x &&
                label.bounds.y<=centre_y && label.bounds.y+label.bounds.height>centre_y &&
                text_coverage(*app.presented(),label.key)!=nullptr;
        }),"Native light intensity row has no visible painted caption.");
        std::printf("Scene Lit native parameter units: %s\n",atelier::editor::utf8(captions[index]).c_str());
        const auto text=std::to_wstring(intensities[index]);type(app,text.c_str());key(app,VK_RETURN);
        require(model(world::read_light(runtime,light)).intensity==intensities[index] &&
            app.document().undo_count()==edit_history+1,
            "Native light Parameters did not commit one typed World edit.");
        if(index==3) require(Access::scene_snapshot(app).environment==std::array{.12f,.12f,.12f},
            "Edited Sky Light Parameters did not reach the scene radiance.");
        else {
            const auto target=std::ranges::find(Access::scene_snapshot(app).lights,intensities[index],&render::Light::intensity);
            require(target!=Access::scene_snapshot(app).lights.end(),"Edited light Parameters did not reach the formal renderer snapshot.");
        }
        capture(app,output/("03-parameters-light-"+std::to_string(index)+".png"));
        if(index==0) {
            tick(app,2);
            const auto* status=app.presented()->find(101); // EditorShell's accepted static status label.
            require(Access::scene_snapshot(app).lights.size()==5 && app.frame_statistics().scene_lit_shadow_sources>=5 &&
                app.frame_statistics().scene_lit_shadow_sources==app.frame_statistics().scene_lit_light_sources && Access::render_notice(app).empty() &&
                status && text_coverage(*app.presented(),101)!=nullptr,
                "Fifth casting light did not remain shadowed or retained the retired shadow-budget warning.");
            capture(app,output/"04-five-shadow-lights.png");
        }
    }

    // Native editor mode changes stay within the editing document's lifecycle.
    // Running World snapshots are exercised by the document + Renderer E2E.
    const auto history=app.document().undo_count();
    const auto authored=model(app.document().serialize());
    scene_lit_display(app,control::shading_first+2);
    capture(app,output/"05-matcap-seven-lights.png");
    scene_lit_display(app,control::shading_scene_lit);tick(app,16);
    require(app.frame_statistics().scene_lit_shadow_sources>=7 &&
        app.frame_statistics().scene_lit_shadow_sources==app.frame_statistics().scene_lit_light_sources,
        "Scene Lit did not consume all seven authored lights after the native display mode change.");
    capture(app,output/"06-scene-lit-seven-lights.png");
    require(!app.document().world().running() && Access::scene_lit_view(app).shading==render::Shading::scene_lit &&
        app.document().undo_count()==history && model(app.document().serialize())==authored,
        "Native display mode changes altered the editing lifecycle, authored scene or history.");
    capture(app,output/"07-scene-lit-edit.png");

    model(app.save_scene("Content/Scenes/Acceptance.scene"));tick(app,2);
    const auto saved=model(atelier::editor::read_file(copy/"Content/Scenes/Acceptance.scene"));
    require(!app.document().dirty(),"Scene Lit temporary save did not mark the authored document clean.");
    model(app.open_scene("Content/Scenes/Acceptance.scene"));tick(app,96);
    require(model(app.document().serialize())==saved && actors(app).size()==18 &&
        Access::scene_snapshot(app).meshes.size()==10 && Access::scene_snapshot(app).lights.size()==7 &&
        Access::scene_snapshot(app).environment==std::array{.12f,.12f,.12f} &&
        app.frame_statistics().scene_lit_shadow_sources>=7 &&
        app.frame_statistics().scene_lit_shadow_sources==app.frame_statistics().scene_lit_light_sources,
        "Reopening the actual saved Scene lost edited World lights, materials or Scene Lit rendering.");
    for(auto intensity:std::span(intensities).first(3)) require(std::ranges::any_of(Access::scene_snapshot(app).lights,
        [&](const auto& light){return light.intensity==intensity;}),"Reopened Scene lost a light edited through native Parameters.");
    capture(app,output/"08-scene-lit-saved-reopened.png");
    // Exercise the real native middle-drag input path from a known front view.
    auto orbit=centre(app,control::viewport);Access::move_pointer(app,orbit);tick(app,1);
    key(app,VK_NUMPAD1);const auto eye=Access::camera_eye(app);
    mouse(app,WM_MBUTTONDOWN,orbit);orbit.x+=32;mouse(app,WM_MOUSEMOVE,orbit);mouse(app,WM_MBUTTONUP,orbit);
    require(Access::camera_eye(app).x<eye.x && Access::camera_mode(app)==atelier::editor::CameraView::perspective,
        "Dragging middle mouse right still reverses horizontal orbit.");
    mouse(app,WM_MBUTTONDOWN,orbit);orbit.x-=32;mouse(app,WM_MOUSEMOVE,orbit);mouse(app,WM_MBUTTONUP,orbit);
    require(std::abs(Access::camera_eye(app).x-eye.x)<.001f && std::abs(Access::camera_eye(app).z-eye.z)<.001f,
        "Opposite horizontal middle drag did not return the camera to its starting side.");
    capture(app,output/"09-middle-orbit.png");
    require(model(atelier::editor::read_file(source_scene))==source_text &&
        model(atelier::editor::read_file(source/"Config/Project.json"))==source_config,
        "Scene Lit product acceptance modified the source example project.");
    std::ofstream(output/"scene-lit-product.txt")<<"Source project: "<<atelier::editor::utf8(source.wstring())
        <<"\nTemporary project: "<<atelier::editor::utf8(copy.wstring())
        <<"\nSource Config/Content retained. No Source compilation files copied.\n"
        <<"Native Scene Lit/MatCap menu, point/rectangle/directional and unique Sky Light creation/Parameters; requested=5, budget=4, shadowed=4, unshadowed=1, status=Shadows 4/5.\n"
        <<"Actual GPU captures, native middle-orbit direction and editor modes. Runtime World snapshots are verified separately through document/Renderer E2E; no Simulate UI exists. Saved scene reopened with 10 meshes, 7 direct lights and one Sky Light.\n";
}
