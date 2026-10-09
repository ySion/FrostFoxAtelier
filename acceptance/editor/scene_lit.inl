std::filesystem::path scene_lit_example(const std::filesystem::path& requested) {
    if (!requested.empty()) return std::filesystem::canonical(requested);
    for (auto base : {std::filesystem::current_path(),editor::asset_directory()})
        for (unsigned depth=0;depth<16 && !base.empty();++depth,base=base.parent_path())
            if (std::filesystem::is_regular_file(base/"assets/examples/Config/Project.json"))
                return std::filesystem::canonical(base/"assets/examples");
    throw std::runtime_error("Cannot locate assets/examples; pass its project directory to --scene-lit.");
}
void verify_scene_lit_example(const std::filesystem::path& requested) {
    using namespace editor;
    const auto source=scene_lit_example(requested);
    const auto source_project=take(Project::open(source));
    const auto source_scene=take(source_project.resolve("Examples/Scene Lit Room/Scenes/Main.scene"));
    const auto source_text=take(read_file(source_scene));
    const auto source_config=take(read_file(source/"Config/Project.json"));
    const auto copy=std::filesystem::absolute("out/scene-lit-application-evidence")/
        ("example-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(copy);
    for (const auto* directory : {"Config","Content","Examples"})
        std::filesystem::copy(source/directory,copy/directory,
            std::filesystem::copy_options::recursive|std::filesystem::copy_options::skip_symlinks);
    std::filesystem::create_directories(copy/"Source");
    auto project=take(Project::open(copy));
    auto document=take(Session::deserialize(take(read_file(take(project.resolve("Examples/Scene Lit Room/Scenes/Main.scene")))),project));
    application::SceneSnapshot scene;take(scene.refresh(*document));
    require(scene.meshes.size()==10 && scene.lights.size()==4,
        "Scene Lit example did not load its ten meshes and four formal World lights.");
    require(std::ranges::count(scene.lights,renderer::LightKind::directional,&renderer::Light::kind)==1 &&
        std::ranges::count(scene.lights,renderer::LightKind::point,&renderer::Light::kind)==2 &&
        std::ranges::count(scene.lights,renderer::LightKind::rectangle,&renderer::Light::kind)==1 &&
        std::ranges::all_of(scene.lights,[](const auto& light){return light.casts_shadow;}),
        "Scene Lit example lost its one directional, two point and one rectangle shadow lights.");
    require(std::ranges::any_of(scene.meshes,[](const auto& mesh){return mesh.material.metallic>.5f;}) &&
        std::ranges::any_of(scene.meshes,[](const auto& mesh){return mesh.material.roughness<.2f;}) &&
        std::ranges::any_of(scene.meshes,[](const auto& mesh){return std::ranges::any_of(mesh.material.emissive,[](float value){return value>1;});}),
        "Scene Lit example lost its real metal, reflection roughness or HDR emission.");
    const auto actor=take(document->world().members()).front().id();
    const auto mesh=*take(world::root_component(document->world().runtime(),actor));
    const auto fields=take(document->properties(mesh));
    const auto roughness=std::ranges::find(fields,"material.roughness",&Property::name);
    require(roughness!=fields.end(),"Scene Lit example floor has no reflected roughness.");
    take(document->set_property(mesh,roughness->path,.71f,991000));
    const auto saved=take(document->serialize());
    const auto saved_path=take(project.resolve("Content/Scenes/Acceptance.scene"));
    take(write_file(saved_path,saved,WriteMode::create));
    auto reopened=take(Session::deserialize(take(read_file(saved_path)),project));
    application::SceneSnapshot restored;take(restored.refresh(*reopened));
    require(take(reopened->serialize())==saved && restored.meshes.size()==10 && restored.lights.size()==4 &&
        restored.meshes.front().material.roughness==.71f,
        "Scene Lit example temporary save/reopen lost material or formal light data.");
    // Runtime presentation uses the same World snapshot contract. An editing
    // document's Parameters panel intentionally requires a stopped document.
    const auto began=take(reopened->start());
    require(!began.notification_error,"Scene Lit runtime World Begin notification failed.");
    take(reopened->update(1.f/60));
    application::SceneSnapshot running;running.reset();take(running.refresh(*reopened));
    require(reopened->world().running() && running.meshes.size()==restored.meshes.size() &&
        running.lights.size()==restored.lights.size(),"Running World lost its formal render snapshot.");
    for(std::size_t i=0;i<running.meshes.size();++i)
        require(running.meshes[i].world==restored.meshes[i].world &&
            running.meshes[i].geometry==restored.meshes[i].geometry &&
            running.meshes[i].material.roughness==restored.meshes[i].material.roughness &&
            running.meshes[i].material.emissive==restored.meshes[i].material.emissive,
            "Unchanged runtime World altered geometry or materials based on execution state.");
    for(std::size_t i=0;i<running.lights.size();++i)
        require(running.lights[i].kind==restored.lights[i].kind &&
            running.lights[i].position==restored.lights[i].position &&
            running.lights[i].intensity==restored.lights[i].intensity,
            "Unchanged runtime World altered authored lights based on execution state.");
    const auto ended=take(reopened->stop());
    require(!ended.notification_error && take(reopened->serialize())==saved,
        "Runtime World presentation modified the saved editing document.");
    require(take(read_file(source_scene))==source_text && take(read_file(source/"Config/Project.json"))==source_config,
        "Scene Lit example acceptance modified the source project.");
    std::ofstream(copy/"acceptance.txt")<<"Scene Lit example Project/Scene: PASS\nSource read only: "<<utf8(source.wstring())
        <<"\nTemporary saved Scene: Content/Scenes/Acceptance.scene\n10 meshes, 4 shadow lights; HDR/metal/roughness and edited material survive reopening.\n";
    std::puts("Scene Lit example Project/Scene acceptance: PASS (source read only, temporary save/reopen)");
}
void verify_scene_lit_application(const std::filesystem::path& source_example = {}) {
    verify_scene_lit_example(source_example);
    using namespace editor;
    const auto folder=std::filesystem::current_path()/"out/scene-lit-application-evidence"/
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto project=take(Project::create(folder));Session document;
    auto& runtime=document.world().runtime();
    changed(document.create_static_mesh(mesh_assets().front()));const auto mesh_actor=document.selection().actor;
    const auto mesh=*take(world::root_component(runtime,mesh_actor));
    std::uint64_t edit_session=990000;
    const auto property=[&](ObjectId object,std::string_view name) {
        const auto fields=take(document.properties(object));
        const auto at=std::ranges::find(fields,name,&Property::name);require(at!=fields.end(),"Expected reflected lighting property is absent.");return at->path;
    };
    const auto edit=[&](ObjectId object,std::string_view name,Scalar value) {
        take(document.set_property(object,property(object,name),std::move(value),edit_session++));
    };
    edit(mesh,"material.roughness",.27f);edit(mesh,"material.metallic",.6f);edit(mesh,"material.emissive.x",4.f);
    std::array<ObjectId,3> light_actors,lights;
    constexpr std::array kinds{world::LightKind::point,world::LightKind::rectangle,world::LightKind::directional};
    for(std::size_t i=0;i<kinds.size();++i) {
        const auto history=document.undo_count();changed(document.create_light(kinds[i],{float(i)*2,3,0}));
        light_actors[i]=document.selection().actor;lights[i]=*take(world::root_component(runtime,light_actors[i]));
        require(document.undo_count()==history+1 && take(world::components(runtime,light_actors[i])).size()==1 &&
            take(world::read_light(runtime,lights[i])).kind==kinds[i],"Light creation did not publish one complete, undoable Actor composition.");
    }
    edit(lights[0],"settings.intensity",80.f);edit(lights[0],"settings.source_radius",.05f);
    edit(lights[1],"settings.width",2.f);edit(lights[1],"settings.height",3.f);edit(lights[1],"settings.two_sided",true);
    edit(lights[2],"settings.intensity",1000.f);edit(lights[2],"settings.source_radius",.0046f);
    const auto component_types=take(document.component_types());
    require(std::ranges::any_of(component_types,[&](const auto& type){return type.type==document.types().light_component;}),
        "Actor Structure cannot discover LightComponent through the shared type list.");
    application::SceneSnapshot snapshot;take(snapshot.refresh(document));
    require(snapshot.meshes.size()==1 && snapshot.lights.size()==3 && snapshot.meshes[0].material.roughness==.27f &&
        snapshot.meshes[0].material.emissive[0]==4 && snapshot.lights[1].two_sided && snapshot.lights[1].width==2,
        "World material and light values did not reach the formal renderer snapshot.");
    const auto key=snapshot.meshes[0].stable_key,light_key=snapshot.lights[0].stable_key,revision=snapshot.revision;
    require(key && light_key && key!=light_key,"Opaque component identities collided in the renderer projection.");
    take(snapshot.refresh(document));require(snapshot.revision==revision,"An unchanged scene reset temporal history.");
    edit(mesh,"position.x",2.f);edit(lights[0],"position.x",3.f);take(document.select(mesh_actor));take(snapshot.refresh(document));
    require(snapshot.revision==revision && snapshot.meshes[0].stable_key==key && snapshot.lights[0].stable_key==light_key &&
        snapshot.meshes[0].world[12]==2 && snapshot.lights[0].position[0]==3,"TRS or selection churn reset scene history or lost component identity.");
    edit(mesh,"material.roughness",.38f);take(snapshot.refresh(document));require(snapshot.revision==revision+1,"Material edits did not invalidate scene history.");
    changed(document.undo());take(snapshot.refresh(document));require(snapshot.revision==revision+2 && snapshot.meshes[0].material.roughness==.27f,"Material undo did not reach rendering.");
    take(document.select(light_actors[0]));changed(document.remove_component(lights[0]));take(snapshot.refresh(document));
    require(snapshot.lights.size()==2,"Removed LightComponent remained in the renderer snapshot.");
    changed(document.undo());take(snapshot.refresh(document));
    require(*take(world::root_component(runtime,light_actors[0]))==lights[0] && snapshot.lights.size()==3,
        "Component undo failed to restore the real light identity and presentation.");
    const auto text=take(document.serialize());const auto file=take(project.resolve("Content/Scenes/Lighting.scene"));
    take(write_file(file,text,WriteMode::create));auto reopened=take(Session::deserialize(take(read_file(file)),project));
    application::SceneSnapshot loaded;take(loaded.refresh(*reopened));
    require(loaded.meshes.size()==1 && loaded.lights.size()==3 && loaded.meshes[0].material.emissive[0]==4 &&
        loaded.meshes[0].material.metallic==.6f && loaded.lights[0].intensity==80 && loaded.lights[2].source_radius==.0046f,
        "Project scene reopen lost material or light settings.");
    auto data=nlohmann::json::parse(text);data["actors"][0]["components"][0]["roughness"]=2;
    require(!Session::deserialize(data.dump(),project),"Invalid saved roughness bypassed World validation.");
    data=nlohmann::json::parse(text);data["actors"][1]["components"][0]["properties"]["settings"]["intensity"]=-1;
    require(!Session::deserialize(data.dump(),project),"Invalid saved light intensity bypassed World validation.");
    data=nlohmann::json::parse(text);auto& old_material=data["actors"][0]["components"][0];
    old_material.erase("roughness");old_material.erase("metallic");old_material.erase("emissive");
    auto old=take(Session::deserialize(data.dump(),project));const auto old_items=take(old->world().render_items());
    require(old_items[0].material.roughness==world::Material{}.roughness && old_items[0].material.emissive==math::Vector3{},
        "Existing v2 scenes did not retain World material defaults.");
    const auto identity=snapshot.identity;snapshot.reset();take(snapshot.refresh(*reopened));
    require(snapshot.identity!=identity,"Document replacement reused the previous renderer history domain.");

    Workspace workspace;const std::array fonts{(asset_directory()/"fonts/SarasaUiSC-Regular.ttf").wstring()};ui::Session surface(fonts);
    const auto theme=ui::StyleSheet::parse(take(read_file(asset_directory()/"editor.ss")));
    require(theme.has_value() && surface.set_style_sheet(*theme).has_value(),"Lighting production editor.ss failed to resolve.");
    require(bool(surface.set_surface({{1500,1000},1,1})),"Lighting UI surface failed.");
    std::vector<ui::Control> controls;
    const auto publish=[&] {
        controls=workspace.describe(document,{1500,1000},surface.presented().get(),&project);
        if(!workspace.ui_error().empty()) throw std::runtime_error("Lighting DSL describe failed: "+std::string(workspace.ui_error()));
        const auto error=[](const ui::Error& value) {
            return "code="+std::to_string(static_cast<unsigned>(value.code))+", key="+std::to_string(value.key)+
                ", native="+std::to_string(value.native_code);
        };
        const auto described=surface.set_controls(controls);
        if(!described) throw std::runtime_error("Lighting DSL controls failed: "+error(described.error()));
        workspace.commit_ui_controls();
        const auto frame=surface.prepare();
        if(!frame) throw std::runtime_error("Lighting DSL layout prepare failed: "+error(frame.error()));
        require(surface.accept(*frame),"Lighting DSL candidate accept failed after successful preparation.");
    };
    take(document.select(light_actors[0]));publish();
    const auto has_text=[&](std::wstring_view text){return std::ranges::any_of(controls,[&](const auto& control){return control.text==text;});};
    require(has_text(L"Intensity (cd)") && !has_text(L"Width (m)"),"Point light parameters exposed the wrong units or rectangle-only fields.");
    take(document.select(light_actors[1]));publish();require(has_text(L"Intensity (cd/m²)") && has_text(L"Width (m)"),"Rectangle parameters are missing their authored dimensions.");
    take(document.select(light_actors[2]));publish();require(has_text(L"Illuminance (lux)") && !has_text(L"Range (m)"),"Directional parameters exposed a local light range.");
    const auto history=document.undo_count();
    require(workspace.apply_view({ui::ActionKind::activate,control::shading_scene_lit,{}}) && workspace.view.shading==renderer::Shading::scene_lit,
        "The formal display command did not select Scene Lit.");
    const auto antialiasing=workspace.view.lit.antialiasing;
    for(std::uint64_t i=0;i<6;++i) {
        require(workspace.apply_view({ui::ActionKind::activate,control::lit_effects_first+i,false}),"Scene Lit effect toggle was not routed.");
        require(workspace.apply_view({ui::ActionKind::activate,control::lit_effects_first+i,false}),"Repeated Scene Lit toggle was rejected.");
    }
    require(!workspace.view.lit.shadows && !workspace.view.lit.ddgi && !workspace.view.lit.gtao && !workspace.view.lit.sssr &&
        !workspace.view.lit.bloom && !workspace.view.lit.ssil && workspace.view.lit.antialiasing==antialiasing,
        "Scene Lit effect toggles were coupled, changed AA policy or were non-idempotent.");
    require(workspace.apply_view({ui::ActionKind::activate,control::lit_effects_first+2,true}) &&
        workspace.view.lit.gtao && !workspace.view.lit.ssil,
        "Enabling GTAO did not choose the independent occlusion policy.");
    require(workspace.apply_view({ui::ActionKind::activate,control::lit_effects_first+5,true}) &&
        workspace.view.lit.ssil && !workspace.view.lit.gtao,
        "Local GI did not replace GTAO through the formal view action.");
    require(workspace.apply_view({ui::ActionKind::activate,control::lit_effects_first+5,false}) &&
        !workspace.view.lit.ssil && !workspace.view.lit.gtao,
        "Disabling Local GI unexpectedly enabled GTAO.");
    require(workspace.apply_view({ui::ActionKind::activate,control::lit_effects_first+5,true}),
        "Local GI could not be enabled after disabling the shared local policy.");
    require(workspace.apply_view({ui::ActionKind::activate,control::lit_effects_first+2,true}) &&
        workspace.view.lit.gtao && !workspace.view.lit.ssil,
        "GTAO did not replace Local GI through the formal view action.");
    require(workspace.apply_view({ui::ActionKind::activate,control::lit_effects_first+2,false}) &&
        !workspace.view.lit.gtao && !workspace.view.lit.ssil && document.undo_count()==history,
        "Disabling the chosen local policy enabled another effect or changed scene history.");
    for(std::uint64_t i=0;i<4;++i) {
        require(workspace.apply_view({ui::ActionKind::activate,control::lit_aa_first+i,{}}) &&
            workspace.apply_view({ui::ActionKind::activate,control::lit_aa_first+i,{}}) &&
            workspace.view.lit.antialiasing==static_cast<renderer::Antialiasing>(i) && document.undo_count()==history,
            "Scene Lit AA choices did not route one idempotent display policy independently of scene history.");
    }
    require(workspace.apply_view({ui::ActionKind::activate,control::lit_exposure_first+4,{}}) && workspace.view.lit.exposure_ev==2 && document.undo_count()==history,
        "Display exposure changed scene history or used the wrong units.");
    require(workspace.apply_view({ui::ActionKind::activate,control::lit_shadow_budget_first+2,{}}) && workspace.view.lit.shadow_light_budget==16,
        "Shadow budget was not routed through the shared display settings.");
    workspace.menu=ui::ContextMenuRequest{control::scene_shading,0,0,0};publish();
    const auto lit=std::ranges::find(workspace.menu_items,control::shading_scene_lit,&ui::MenuItem::key);
    const auto options=std::ranges::find(workspace.menu_items,control::lit_menu,&ui::MenuItem::key);
    require(lit!=workspace.menu_items.end() && lit->selected && lit->behavior==ui::MenuBehavior::radio &&
        options!=workspace.menu_items.end() && options->children.size()==9 && options->children[0].behavior==ui::MenuBehavior::checkbox &&
        options->children[5].key==control::lit_effects_first+5 && options->children[5].behavior==ui::MenuBehavior::checkbox,
        "The display menu lost Scene Lit radio/effect checkbox semantics.");
    const auto aa=std::ranges::find(options->children,control::lit_aa_menu,&ui::MenuItem::key);
    require(aa!=options->children.end() && aa->children.size()==4,
        "Scene Lit menu omitted the four independent AA policies.");
    for(std::size_t i=0;i<aa->children.size();++i)
        require(aa->children[i].key==control::lit_aa_first+i && aa->children[i].behavior==ui::MenuBehavior::radio &&
            aa->children[i].radio_group==control::lit_aa_first && aa->children[i].selected==(i==3),
            "Scene Lit AA menu did not publish the selected policy through one radio group.");
    std::puts("Scene Lit application acceptance: PASS (real World/Project, light creation/history, reflected parameters, scene persistence/rejection, stable snapshots and display menus)");
}
