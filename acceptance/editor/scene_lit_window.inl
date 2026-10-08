std::filesystem::path scene_lit_window_example(const std::filesystem::path& requested) {
    if (!requested.empty()) return std::filesystem::canonical(requested);
    for (auto base : {std::filesystem::current_path(),atelier::editor::asset_directory()})
        for (unsigned depth=0;depth<16 && !base.empty();++depth,base=base.parent_path())
            if (std::filesystem::is_regular_file(base/"examples/scene-lit/Config/Project.json"))
                return std::filesystem::canonical(base/"examples/scene-lit");
    throw std::runtime_error("Cannot locate examples/scene-lit; pass its project directory to --acceptance-scene-lit.");
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
        if (a.depth[index]>=1 || b.depth[index]>=1 || std::abs(a.depth[index]-b.depth[index])>1e-4f) continue;
        for (unsigned channel=0;channel<3;++channel)
            if (std::abs(int(a.rgba[index*4+channel])-int(b.rgba[index*4+channel]))>3) {++changed;break;}
    }
    return changed;
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
    const auto source_scene=model(source_project.resolve(source_project.startup_scene()));
    const auto source_text=model(atelier::editor::read_file(source_scene));
    const auto source_config=model(atelier::editor::read_file(source/"Config/Project.json"));
    const auto copy=output/(L"Project-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(copy);
    for (const auto* directory : {"Config","Content"})
        std::filesystem::copy(source/directory,copy/directory,
            std::filesystem::copy_options::recursive|std::filesystem::copy_options::skip_symlinks);
    std::filesystem::create_directories(copy/"Source");
    Access::mark_saved(app);model(app.open_project(copy));tick(app,4);
    require(!app.document().dirty() && actors(app).size()==14 &&
        model(app.document().world().render_items()).size()==10 && model(app.document().world().light_items()).size()==4,
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
        app.frame_statistics().scene_lit_shadow_lights==4 && app.frame_statistics().scene_lit_unshadowed_lights==0 &&
        app.frame_statistics().scene_draw_calls>0 && app.frame_statistics().ui_draw_calls>0 &&
        std::ranges::all_of(initial.meshes,[](const auto& mesh){return mesh.stable_key!=0;}) &&
        std::ranges::all_of(initial.lights,[](const auto& light){return light.stable_key!=0;}),
        "Loaded World light/material identities did not reach the ordinary Scene Lit Renderer frame.");
    require(model(app.document().serialize())==loaded,"Selecting Scene Lit changed the example scene.");

    constexpr std::array commands{Command::create_point_light,Command::create_rectangle_light,Command::create_directional_light};
    constexpr std::array kinds{world::LightKind::point,world::LightKind::rectangle,world::LightKind::directional};
    constexpr std::array captions{L"Intensity (cd)",L"Intensity (cd/m²)",L"Illuminance (lux)"};
    constexpr std::array intensities{79.25f,22.5f,3.25f};
    std::array<atelier::ObjectId,3> created;
    for (std::size_t index=0;index<commands.size();++index) {
        const auto history=app.document().undo_count();
        const auto count=actors(app).size();
        click(app,control::create_menu);
        const auto command=static_cast<ControlKey>(commands[index]);
        require(app.menu_frame() && app.menu_frame()->find(command),"Native Create menu omitted a real light command.");
        click(app,command);tick(app,2);
        created[index]=app.document().selection().actor;
        const auto light=component(app,created[index],app.document().types().light_component);
        auto& runtime=app.document().world().runtime();
        require(actors(app).size()==count+1 && app.document().undo_count()==history+1 &&
            model(world::read_light(runtime,light)).kind==kinds[index],
            "Native light creation did not create one complete, undoable World Actor.");
        click(app,static_cast<ControlKey>(Command::show_parameters));
        const auto fields=model(app.document().properties(light));
        const auto has_text=[&](std::wstring_view text) {
            return std::ranges::any_of(app.presented()->text_geometry,[&](const auto& item){return item.paragraph && item.paragraph->matches(text);});
        };
        const auto has_field=[&](std::string_view name) {
            const auto field=std::ranges::find(fields,name,&atelier::editor::Property::name);
            return field!=fields.end() && app.presented()->find(app.field_control(light,field->path));
        };
        require(index==1?has_field("settings.width") && has_field("settings.height"):!has_field("settings.width"),
            "Real Parameters exposed the wrong light-shape fields.");
        if(index==2) require(!has_field("settings.range") && has_field("settings.source_radius"),
            "Directional Parameters used a local-light range or omitted angular radius.");
        const auto intensity=std::ranges::find(fields,"settings.intensity",&atelier::editor::Property::name);
        require(intensity!=fields.end(),"Created light has no reflected intensity field.");
        const auto edit_history=app.document().undo_count();
        click(app,app.field_control(light,intensity->path));
        require(has_text(captions[index]),"Visible real Parameters omitted the light's physical intensity units.");
        const auto text=std::to_wstring(intensities[index]);type(app,text.c_str());key(app,VK_RETURN);
        require(model(world::read_light(runtime,light)).intensity==intensities[index] &&
            app.document().undo_count()==edit_history+1,
            "Native light Parameters did not commit one typed World edit.");
        const auto target=std::ranges::find(Access::scene_snapshot(app).lights,intensities[index],&render::Light::intensity);
        require(target!=Access::scene_snapshot(app).lights.end(),"Edited light Parameters did not reach the formal renderer snapshot.");
        capture(app,output/("03-parameters-light-"+std::to_string(index)+".png"));
        if(index==0) {
            tick(app,2);
            const auto* status=app.presented()->find_text(101); // EditorShell's accepted status paragraph.
            const auto status_text=app.status_text()+L"  |  "+Access::render_notice(app)+L"  |  "+std::to_wstring(actors(app).size())+L" Actors";
            require(Access::scene_snapshot(app).lights.size()==5 && app.frame_statistics().scene_lit_shadow_lights==4 &&
                app.frame_statistics().scene_lit_unshadowed_lights==1 && Access::render_notice(app)==L"Shadows 4/5" &&
                status && status->paragraph && status->paragraph->matches(status_text),
                "Fifth casting light did not remain accepted with the formal Shadows 4/5 budget status.");
            capture(app,output/"04-shadow-budget-4-of-5.png");
        }
    }

    // The product currently has no Simulate UI. Exercise the existing Session
    // lifecycle on the displayed World, keeping all display commands native.
    const auto history=app.document().undo_count();
    const auto authored=model(app.document().serialize());
    scene_lit_display(app,control::shading_first+2);
    model(Access::simulate(app,true));model(Access::simulate_tick(app,1.f/60));tick(app,2);
    require(app.document().world().running() && Access::scene_lit_view(app).shading==render::Shading::matcap,
        "Starting the displayed World changed MatCap into a simulation-dependent display mode.");
    capture(app,output/"05-matcap-simulating.png");
    scene_lit_display(app,control::shading_scene_lit);model(Access::simulate_tick(app,1.f/60));tick(app,16);
    require(app.document().world().running() && app.frame_statistics().scene_lit_shadow_lights==4 &&
        app.frame_statistics().scene_lit_unshadowed_lights==3,
        "Scene Lit did not consume the same seven lights while the displayed World was running.");
    capture(app,output/"06-scene-lit-simulating.png");
    model(Access::simulate(app,false));tick(app,2);
    require(!app.document().world().running() && Access::scene_lit_view(app).shading==render::Shading::scene_lit &&
        app.document().undo_count()==history && model(app.document().serialize())==authored,
        "Stopping simulation changed the display policy, authored scene or history.");
    capture(app,output/"07-scene-lit-edit.png");

    model(app.save_scene("Content/Scenes/Acceptance.scene"));tick(app,2);
    const auto saved=model(atelier::editor::read_file(copy/"Content/Scenes/Acceptance.scene"));
    require(!app.document().dirty(),"Scene Lit temporary save did not mark the authored document clean.");
    model(app.open_scene("Content/Scenes/Acceptance.scene"));tick(app,96);
    require(model(app.document().serialize())==saved && actors(app).size()==17 &&
        Access::scene_snapshot(app).meshes.size()==10 && Access::scene_snapshot(app).lights.size()==7 &&
        app.frame_statistics().scene_lit_shadow_lights==4 && app.frame_statistics().scene_lit_unshadowed_lights==3,
        "Reopening the actual saved Scene lost edited World lights, materials or Scene Lit rendering.");
    for(auto intensity:intensities) require(std::ranges::any_of(Access::scene_snapshot(app).lights,
        [&](const auto& light){return light.intensity==intensity;}),"Reopened Scene lost a light edited through native Parameters.");
    capture(app,output/"08-scene-lit-saved-reopened.png");
    require(model(atelier::editor::read_file(source_scene))==source_text &&
        model(atelier::editor::read_file(source/"Config/Project.json"))==source_config,
        "Scene Lit product acceptance modified the source example project.");
    std::ofstream(output/"scene-lit-product.txt")<<"Source project: "<<atelier::editor::utf8(source.wstring())
        <<"\nTemporary project: "<<atelier::editor::utf8(copy.wstring())
        <<"\nSource Config/Content retained. No Source compilation files copied.\n"
        <<"Native Scene Lit/MatCap menu, point/rectangle/directional creation and Parameters; requested=5, budget=4, shadowed=4, unshadowed=1, status=Shadows 4/5.\n"
        <<"Formal displayed World Session lifecycle and actual GPU captures; no Simulate UI exists. Saved scene reopened with 10 meshes and 7 authored lights.\n";
}
