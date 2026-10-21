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
    const auto sky=[](const Session& session){return rendering::sky_radiance(take(session.presentation().environment()));};
    require(scene.meshes.size()==10 && scene.lights.size()==4 && sky(*document)==std::array{.025f,.035f,.055f} &&
        document->environment()!=ObjectId{} && !document->lighting_supplied(),
        "Scene Lit example did not load its ten meshes, four direct lights and the Sky Light merged into its Environment.");
    require(std::ranges::count(scene.lights,renderer::LightKind::directional,&renderer::Light::kind)==1 &&
        std::ranges::count(scene.lights,renderer::LightKind::point,&renderer::Light::kind)==2 &&
        std::ranges::count(scene.lights,renderer::LightKind::rectangle,&renderer::Light::kind)==1 &&
        std::ranges::all_of(scene.lights,[](const auto& light){return light.casts_shadow;}),
        "Scene Lit example lost its one directional, two point and one rectangle shadow lights.");
    require(std::ranges::any_of(scene.meshes,[](const auto& mesh){return mesh.material.metallic>.5f;}) &&
        std::ranges::any_of(scene.meshes,[](const auto& mesh){return mesh.material.roughness<.2f;}) &&
        std::ranges::any_of(scene.meshes,[](const auto& mesh){return std::ranges::any_of(mesh.material.emissive,[](float value){return value>1;});}),
        "Scene Lit example lost its real metal, reflection roughness or HDR emission.");
    const auto entity=take(document->world().members()).front().id();
    const auto mesh=component(*document,entity,document->presentation_types().static_mesh_component);
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
        restored.meshes.front().material.roughness==.71f && sky(*reopened)==sky(*document) && reopened->lighting_supplied(),
        "Scene Lit example temporary save/reopen lost material or formal light data.");
    // Runtime presentation uses the same World snapshot contract. An editing
    // document's Parameters panel intentionally requires a stopped document.
    const auto began=take(reopened->start());
    require(!began.notification_error,"Scene Lit runtime World Begin notification failed.");
    take(reopened->update(1.f/60));
    application::SceneSnapshot running;running.reset();take(running.refresh(*reopened));
    require(reopened->world().running() && running.meshes.size()==restored.meshes.size() &&
        running.lights.size()==restored.lights.size() && sky(*reopened)==sky(*document),"Running World lost its formal render snapshot.");
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
        <<"\nTemporary saved Scene: Content/Scenes/Acceptance.scene\n10 meshes, 4 shadow lights, sky in the Environment; HDR/metal/roughness and edited material survive reopening.\n";
    std::puts("Scene Lit example Project/Scene acceptance: PASS (source read only, temporary save/reopen)");
}
void verify_environment_sky_application() {
    using namespace editor;
    world::World* sky_world{};
    Session document([&](world::World& scene)->Result<std::shared_ptr<void>> {
        sky_world=&scene;return std::shared_ptr<void>{};
    });
    application::SceneSnapshot snapshot;
    const std::array<float,3> dark{},radiance{.025f,.05f,.075f};
    const auto sky=[](const Session& session){return rendering::sky_radiance(take(session.presentation().environment()));};
    require(document.environment()==ObjectId{} && sky(document)==dark,"A World without an Environment has implicit environment radiance.");
    // The first World Settings edit creates the Environment and writes its values as one history item.
    auto lighting=take(document.presentation().environment());
    lighting.sky_enabled=true;lighting.sky_color={.25f,.5f,.75f};lighting.sky_intensity=.1f;
    take(document.set_environment(lighting));
    const auto environment=document.environment();
    const auto entity=take(world::inspect_component(document.world().runtime(),environment)).owner;
    require(environment!=ObjectId{} && document.undo_count()==1 && document.name(entity)==L"Environment" && sky(document)==radiance &&
        take(world::components(document.world().runtime(),entity)).size()==1 && !take(world::has_transform(document.world().runtime(),entity)),
        "The first World Settings edit did not create one spaceless Environment with the sky in one history item.");
    changed(document.undo());
    require(document.environment()==ObjectId{} && take(document.world().members()).empty() && sky(document)==dark,
        "Undoing the first World Settings edit left an Environment or its sky.");
    changed(document.redo());
    require(document.environment()==environment && sky(document)==radiance,"Redo did not restore the same Environment and its sky.");
    lighting.sky_intensity=.2f;take(document.set_environment(lighting));
    require(document.undo_count()==2 && take(world::components(document.world().runtime(),entity)).size()==1 &&
        sky(document)==std::array{.05f,.1f,.15f},"A later World Settings edit did not write the existing Environment.");
    changed(document.undo());require(sky(document)==radiance,"Undo did not restore the earlier sky.");
    lighting.sky_intensity=.1f;
    const auto history=document.undo_count();
    take(document.select(entity));
    require(!document.add_component(document.presentation_types().environment_component) && document.undo_count()==history,
        "A second Environment was accepted on the same Entity.");
    changed(document.add_entity());const auto other=document.selection().entity;
    require(!document.add_component(document.presentation_types().environment_component),"A second Environment was accepted in the World.");
    changed(sky_world->set_enabled(environment,false));
    require(sky(document)==dark,"A disabled Environment kept its sky.");
    // World Settings show a disabled Environment's defaults, so an edit is refused rather than diffed against them.
    auto refused=lighting;refused.exposure_ev+=1;
    const auto disabled_history=document.undo_count();
    const auto disabled=document.set_environment(refused);
    require(!disabled && disabled.error().message.find("enable it in Parameters")!=std::string::npos && document.undo_count()==disabled_history,
        "A disabled Environment accepted a World Settings edit or did not name the fix.");
    require(!document.add_component(document.presentation_types().environment_component),"A disabled Environment did not keep the only Environment slot.");
    changed(sky_world->set_enabled(environment,true));
    changed(sky_world->set_enabled(entity,false));require(sky(document)==dark,"A disabled Environment Entity kept its sky.");
    require(!document.set_environment(refused) && document.undo_count()==disabled_history,"An Environment under a disabled Entity accepted a World Settings edit.");
    changed(sky_world->set_enabled(entity,true));
    require(sky(document)==radiance && take(document.presentation().environment())==lighting,"A re-enabled Environment did not restore its sky or kept a refused edit.");
    take(document.select(other));changed(document.erase_selected());
    take(snapshot.refresh(document));
    const auto before_point=snapshot.revision;
    changed(document.create_light(presentation::LightKind::point));
    const auto point=component(document,document.selection().entity,document.presentation_types().light_component);
    take(snapshot.refresh(document));
    require(snapshot.lights.size()==1 && snapshot.revision==before_point+1,"Appending a direct light failed to advance the scene revision.");
    take(snapshot.refresh(document));
    require(snapshot.revision==before_point+1,"Unchanged appended light advanced the scene revision.");
    changed(sky_world->set_enabled(point,false));take(snapshot.refresh(document));
    require(snapshot.lights.empty() && snapshot.revision==before_point+2,"Disabling the trailing direct light failed to update scene membership and revision.");
    take(snapshot.refresh(document));
    require(snapshot.revision==before_point+2,"Unchanged disabled light advanced the scene revision.");
    changed(sky_world->set_enabled(point,true));take(snapshot.refresh(document));
    require(snapshot.lights.size()==1 && snapshot.revision==before_point+3,"Re-enabling the trailing direct light failed to advance the scene revision.");
    take(snapshot.refresh(document));
    require(snapshot.revision==before_point+3,"Unchanged re-enabled light advanced the scene revision.");
    take(document.select(entity));changed(document.remove_component(environment));
    require(sky(document)==dark && document.environment()==ObjectId{},"A removed Environment remained effective.");
    changed(document.undo());
    require(sky(document)==radiance && document.environment()==environment,"Undo did not restore the same Environment and its sky.");
    const auto folder=std::filesystem::current_path()/"out/scene-lit-application-evidence"/
        ("sky-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto project=take(Project::create(folder));
    const auto text=take(document.serialize());const auto file=take(project.resolve("Content/Scenes/Sky.scene"));
    take(write_file(file,text,WriteMode::create));
    auto reopened=take(Session::deserialize(take(read_file(file)),project));take(snapshot.refresh(*reopened));
    require(sky(*reopened)==radiance && reopened->lighting_supplied() && snapshot.lights.size()==1 && !nlohmann::json::parse(text).contains("world"),
        "The Environment sky changed after saved scene reopen.");
    std::puts("Environment sky application acceptance: PASS (first edit creates it, unique, enable/remove/undo, scene save/reopen)");
}
// A refresh extracts only when the presentation version, the selection or the
// snapshot identity changed; static frames keep the previous snapshot.
void verify_scene_snapshot_skip() {
    using namespace editor;
    Session document;
    auto& world=const_cast<world::World&>(document.world());
    changed(document.create_static_mesh(mesh_assets().front()));const auto mesh_entity=document.selection().entity;
    changed(document.create_light(presentation::LightKind::point));
    const auto light=component(document,document.selection().entity,document.presentation_types().light_component);
    application::SceneSnapshot snapshot;take(snapshot.refresh(document));
    const auto refreshed=[&](bool extracted,const char* message) {
        const auto before=snapshot.extractions;take(snapshot.refresh(document));
        require(snapshot.extractions==before+(extracted?1:0),message);
    };
    refreshed(false,"A static frame extracted the presentation again.");
    refreshed(false,"A second static frame extracted the presentation again.");
    take(document.select(mesh_entity));refreshed(true,"A selection change kept the previous highlight.");
    require(snapshot.meshes[0].selected,"The refreshed snapshot lost the selection highlight.");
    refreshed(false,"An unchanged selection extracted the presentation again.");
    take(world.runtime().write(mesh_entity,document.types().position_fields[0],Scalar{2.f}));
    refreshed(true,"A TRS write past World kept the previous mesh matrix.");
    require(snapshot.meshes[0].world[12]==2,"The refreshed snapshot lost the written position.");
    auto settings=take(presentation::read_light(world.runtime(),light));settings.intensity=9;
    take(presentation::set_light(world,light,settings));refreshed(true,"A light edit kept the previous light.");
    require(snapshot.lights[0].intensity==9,"The refreshed snapshot lost the light edit.");
    refreshed(false,"A static frame after edits extracted the presentation again.");
    snapshot.reset();refreshed(true,"A reset snapshot reused the previous extraction.");
    std::puts("Scene snapshot skip acceptance: PASS (static frames reuse the snapshot; selection, TRS past World, light edit and reset extract)");
}
void verify_scene_lit_application(const std::filesystem::path& source_example = {}) {
    verify_scene_lit_example(source_example);
    verify_environment_sky_application();
    verify_scene_snapshot_skip();
    using namespace editor;
    const auto folder=std::filesystem::current_path()/"out/scene-lit-application-evidence"/
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto project=take(Project::create(folder));Session document;
    auto& runtime=document.world().runtime();
    changed(document.create_static_mesh(mesh_assets().front()));const auto mesh_entity=document.selection().entity;
    const auto mesh=component(document,mesh_entity,document.presentation_types().static_mesh_component);
    std::uint64_t edit_session=990000;
    const auto property=[&](ObjectId object,std::string_view name) {
        const auto fields=take(document.properties(object));
        const auto at=std::ranges::find(fields,name,&Property::name);require(at!=fields.end(),"Expected reflected lighting property is absent.");return at->path;
    };
    const auto edit=[&](ObjectId object,std::string_view name,Scalar value) {
        take(document.set_property(object,property(object,name),std::move(value),edit_session++));
    };
    edit(mesh,"material.roughness",.27f);edit(mesh,"material.metallic",.6f);edit(mesh,"material.emissive.x",4.f);
    std::array<ObjectId,3> light_entities,lights;
    constexpr std::array kinds{presentation::LightKind::point,presentation::LightKind::rectangle,presentation::LightKind::directional};
    for(std::size_t i=0;i<kinds.size();++i) {
        const auto history=document.undo_count();changed(document.create_light(kinds[i],{float(i)*2,3,0}));
        light_entities[i]=document.selection().entity;lights[i]=component(document,light_entities[i],document.presentation_types().light_component);
        require(document.undo_count()==history+1 && take(world::components(runtime,light_entities[i])).size()==1 &&
            take(presentation::read_light(runtime,lights[i])).kind==kinds[i],"Light creation did not publish one complete, undoable Entity composition.");
    }
    edit(lights[0],"settings.intensity",80.f);edit(lights[0],"settings.source_radius",.05f);
    edit(lights[1],"settings.width",2.f);edit(lights[1],"settings.height",3.f);edit(lights[1],"settings.two_sided",true);
    edit(lights[2],"settings.intensity",1000.f);edit(lights[2],"settings.source_radius",.0046f);
    const auto component_types=take(document.component_types());
    require(std::ranges::any_of(component_types,[&](const auto& type){return type.type==document.presentation_types().light_component;}),
        "Entity Structure cannot discover LightComponent through the shared type list.");
    application::SceneSnapshot snapshot;take(snapshot.refresh(document));
    require(snapshot.meshes.size()==1 && snapshot.lights.size()==3 && snapshot.meshes[0].material.roughness==.27f &&
        snapshot.meshes[0].material.emissive[0]==4 && snapshot.lights[1].two_sided && snapshot.lights[1].width==2,
        "World material and light values did not reach the formal renderer snapshot.");
    const auto key=snapshot.meshes[0].stable_key,light_key=snapshot.lights[0].stable_key,revision=snapshot.revision;
    require(key && light_key && key!=light_key,"Opaque component identities collided in the renderer projection.");
    take(snapshot.refresh(document));require(snapshot.revision==revision,"An unchanged scene reset temporal history.");
    edit(mesh_entity,"position.x",2.f);edit(light_entities[0],"position.x",3.f);take(document.select(mesh_entity));take(snapshot.refresh(document));
    require(snapshot.revision==revision && snapshot.meshes[0].stable_key==key && snapshot.lights[0].stable_key==light_key &&
        snapshot.meshes[0].world[12]==2 && snapshot.lights[0].position[0]==3,"TRS or selection churn reset scene history or lost component identity.");
    edit(mesh,"material.roughness",.38f);take(snapshot.refresh(document));require(snapshot.revision==revision+1,"Material edits did not invalidate scene history.");
    changed(document.undo());take(snapshot.refresh(document));require(snapshot.revision==revision+2 && snapshot.meshes[0].material.roughness==.27f,"Material undo did not reach rendering.");
    take(document.select(light_entities[0]));changed(document.remove_component(lights[0]));take(snapshot.refresh(document));
    require(snapshot.lights.size()==2,"Removed LightComponent remained in the renderer snapshot.");
    changed(document.undo());take(snapshot.refresh(document));
    require(component(document,light_entities[0],document.presentation_types().light_component)==lights[0] && snapshot.lights.size()==3,
        "Component undo failed to restore the real light identity and presentation.");
    const auto text=take(document.serialize());const auto file=take(project.resolve("Content/Scenes/Lighting.scene"));
    take(write_file(file,text,WriteMode::create));auto reopened=take(Session::deserialize(take(read_file(file)),project));
    application::SceneSnapshot loaded;take(loaded.refresh(*reopened));
    require(loaded.meshes.size()==1 && loaded.lights.size()==3 && loaded.meshes[0].material.emissive[0]==4 &&
        loaded.meshes[0].material.metallic==.6f && loaded.lights[0].intensity==80 && loaded.lights[2].source_radius==.0046f,
        "Project scene reopen lost material or light settings.");
    const auto kept_invalid=[&](const nlohmann::json& data,const nlohmann::json::json_pointer& at,const char* message) {
        require(!Session::deserialize(data.dump(),project,{},LoadPolicy::strict),message);
        auto kept=take(Session::deserialize(data.dump(),project));
        require(!kept->load_issues().empty() && nlohmann::json::parse(take(kept->serialize()))[at]==data[at],
            "Tolerant loading did not keep a saved value rejected by World validation.");
    };
    auto data=nlohmann::json::parse(text);data["entities"][0]["components"][0]["roughness"]=2;
    kept_invalid(data,nlohmann::json::json_pointer("/entities/0/components/0/roughness"),"Invalid saved roughness bypassed World validation.");
    data=nlohmann::json::parse(text);data["entities"][1]["components"][0]["properties"]["settings"]["range"]=0;
    kept_invalid(data,nlohmann::json::json_pointer("/entities/1/components/0/properties/settings/range"),"Invalid saved light range bypassed World validation.");
    data=nlohmann::json::parse(text);data["entities"][1]["components"][0]["properties"]["settings"]["intensity"]=-1;
    kept_invalid(data,nlohmann::json::json_pointer("/entities/1/components/0/properties/settings/intensity"),"A saved light intensity outside its hard bound loaded without a diagnostic.");
    data=nlohmann::json::parse(text);auto& old_material=data["entities"][0]["components"][0];
    old_material.erase("roughness");old_material.erase("metallic");old_material.erase("emissive");
    auto old=take(Session::deserialize(data.dump(),project));const auto old_items=take(old->presentation().render_items());
    require(old_items[0].material.roughness==presentation::Material{}.roughness && old_items[0].material.emissive==math::Vector3{},
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
    take(document.select(light_entities[0]));publish();
    const auto has_text=[&](std::wstring_view text){return std::ranges::any_of(controls,[&](const auto& control){return control.text==text;});};
    require(has_text(L"Intensity (cd)") && !has_text(L"Width (m)"),"Point light parameters exposed the wrong units or rectangle-only fields.");
    take(document.select(light_entities[1]));publish();require(has_text(L"Intensity (cd/m²)") && has_text(L"Width (m)"),"Rectangle parameters are missing their authored dimensions.");
    take(document.select(light_entities[2]));publish();require(has_text(L"Illuminance (lux)") && !has_text(L"Range (m)"),"Directional parameters exposed a local light range.");
    const auto history=document.undo_count();
    require(take(workspace.apply_view(document,{ui::ActionKind::activate,control::shading_scene_lit,{}})) && workspace.view.shading==renderer::Shading::scene_lit,
        "The formal display command did not select Scene Lit.");
    require(!take(workspace.apply_view(document,{ui::ActionKind::activate,control::guide_background,true})) &&
        workspace.view.background==renderer::Background::gradient,"Scene Lit can enable a studio gradient or destroys the stored studio preference.");
    const auto antialiasing=workspace.view.lit.antialiasing;
    constexpr std::array effects{ViewOption::shadows,ViewOption::ddgi,ViewOption::gtao,ViewOption::sssr,ViewOption::bloom,ViewOption::ssil};
    for(const auto effect:effects) {
        require(take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::field(effect),false})),"Scene Lit effect toggle was not routed.");
        require(take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::field(effect),false})),"Repeated Scene Lit toggle was rejected.");
    }
    require(!workspace.view.lit.shadows && !workspace.view.lit.ddgi && !workspace.view.lit.gtao && !workspace.view.lit.sssr &&
        !workspace.view.lit.bloom && !workspace.view.lit.ssil && workspace.view.lit.antialiasing==antialiasing,
        "Scene Lit effect toggles were coupled, changed AA policy or were non-idempotent.");
    require(take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::field(ViewOption::gtao),true})) &&
        workspace.view.lit.gtao && !workspace.view.lit.ssil,
        "Enabling GTAO did not choose the independent occlusion policy.");
    require(take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::field(ViewOption::ssil),true})) &&
        workspace.view.lit.ssil && !workspace.view.lit.gtao,
        "Local GI did not replace GTAO through the formal view action.");
    require(take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::field(ViewOption::ssil),false})) &&
        !workspace.view.lit.ssil && !workspace.view.lit.gtao,
        "Disabling Local GI unexpectedly enabled GTAO.");
    require(take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::field(ViewOption::ssil),true})),
        "Local GI could not be enabled after disabling the shared local policy.");
    require(take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::field(ViewOption::gtao),true})) &&
        workspace.view.lit.gtao && !workspace.view.lit.ssil,
        "GTAO did not replace Local GI through the formal view action.");
    require(take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::field(ViewOption::gtao),false})) &&
        !workspace.view.lit.gtao && !workspace.view.lit.ssil && document.undo_count()>history,
        "Disabling the chosen local policy enabled another effect or changed scene history.");
    for(std::uint64_t i=0;i<4;++i) {
        require(take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::choice(ViewOption::antialiasing,static_cast<unsigned>(i)),{}})) &&
            take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::choice(ViewOption::antialiasing,static_cast<unsigned>(i)),{}})) &&
            workspace.view.lit.antialiasing==static_cast<renderer::Antialiasing>(i) && document.undo_count()>history,
            "World AA choices did not route through scene history.");
    }
    const auto set_world_number=[&](ViewOption option,double value) {
        const auto key=world_control::field(option);
        take(workspace.apply_view(document,{ui::ActionKind::edit_begin,key,{}}));
        require(take(workspace.apply_view(document,{ui::ActionKind::edit_commit,key,value})),"World numeric setting was not routed.");
    };
    set_world_number(ViewOption::exposure_ev,2);
    require(workspace.view.lit.exposure_ev==2 && document.undo_count()>history,"World exposure did not enter scene history or used the wrong units.");
    set_world_number(ViewOption::direct_candidates,32);
    require(workspace.view.lit.direct_candidates==32 && take(document.presentation().environment()).direct_candidates==32,
        "Direct candidates were not routed through World author settings.");
    workspace.menu=ui::ContextMenuRequest{control::scene_shading,0,0,0};publish();
    const auto lit=std::ranges::find(workspace.menu_items,control::shading_scene_lit,&ui::MenuItem::key);
    require(lit!=workspace.menu_items.end() && lit->selected && lit->behavior==ui::MenuBehavior::radio &&
        std::ranges::all_of(workspace.menu_items,[](const auto& item){return item.children.empty() && item.behavior==ui::MenuBehavior::radio;}),
        "The preview menu lost Scene Lit or retained scene authoring settings.");
    const auto aa=workspace.lighting.menu(world_control::field(ViewOption::antialiasing),workspace.view);
    require(aa.size()==4,"World AA selector omitted a rendering policy.");
    for(unsigned i=0;i<aa.size();++i)
        require(aa[i].key==world_control::choice(ViewOption::antialiasing,i) && aa[i].behavior==ui::MenuBehavior::radio &&
            aa[i].radio_group==world_control::field(ViewOption::antialiasing) && aa[i].selected==(i==3),
            "World AA selector did not publish its selected policy through one radio group.");
    {
    workspace.menu.reset();workspace.dock.select(world_control::tab);publish();
    require(!has_text(L"Scene lights"),"World settings retained a per-light navigation list.");
    take(workspace.apply_ui({ui::ActionKind::activate,world_control::ddgi_tab,{}}));publish();
    require(has_text(L"Propagation") && has_text(L"World min (m)") && has_text(L"Current layout (CPU)"),
        "Lighting DSL page omitted its volume, propagation or accepted-frame layout controls.");
    const auto count0=world_control::field(probe_volume_option(0,ProbeVolumeField::count_x));
    const auto old_view=workspace.view;const auto revision=workspace.view_revision();
    const auto edit=[&](ui::ActionKind kind,ui::ControlKey key,double value=0) {return workspace.apply_view(document,{kind,key,value});};
    take(edit(ui::ActionKind::edit_begin,count0));take(edit(ui::ActionKind::edit_preview,count0,10));publish();
    require(workspace.lighting.editing() && workspace.view==old_view && workspace.view_revision()==revision,
        "Lighting preview published renderer settings before commit.");
    take(edit(ui::ActionKind::edit_cancel,count0));
    require(!workspace.lighting.editing() && workspace.view==old_view,"Cancelled lighting input changed settings.");
    take(edit(ui::ActionKind::edit_begin,count0));take(edit(ui::ActionKind::edit_commit,count0,10));
    require(workspace.view.lit.probe_volumes[0].counts[0]==10 && workspace.view_revision()==revision+1,
        "Lighting commit did not publish once.");
    take(edit(ui::ActionKind::edit_begin,count0));take(edit(ui::ActionKind::edit_commit,count0,10));
    require(workspace.view_revision()==revision+1,"Repeated lighting commit changed the view revision.");
    const auto before_invalid=workspace.view;
    const auto max0=world_control::field(probe_volume_option(0,ProbeVolumeField::max_x));
    take(edit(ui::ActionKind::edit_begin,max0));
    require(!edit(ui::ActionKind::edit_commit,max0,before_invalid.lit.probe_volumes[0].minimum[0]) &&
        workspace.view==before_invalid && !workspace.lighting.editing(),"Invalid bounds left a partial publication or live draft.");
    take(edit(ui::ActionKind::edit_begin,count0));
    take(workspace.apply_view(document,{ui::ActionKind::activate,world_control::slot_first+1,{}}));publish();
    take(edit(ui::ActionKind::edit_commit,count0,12));take(edit(ui::ActionKind::edit_begin,count0));
    require(workspace.view==before_invalid && !workspace.lighting.editing() && workspace.lighting.selected_volume()==1,
        "A retired volume row resumed editing after a slot switch.");
    const auto count1=world_control::field(probe_volume_option(1,ProbeVolumeField::count_x));
    take(edit(ui::ActionKind::edit_begin,count1));workspace.reset_document();
    take(edit(ui::ActionKind::edit_commit,count1,12));publish();
    require(workspace.view==before_invalid && !workspace.lighting.editing() && document.undo_count()>history,
        "Document replacement kept a lighting draft or lost World history.");
    }
    {
    // World Settings edits are Environment property edits; their Undo selects the Environment.
    const auto selected=document.selection().entity;
    const auto settings=[](const Session& session){return take(session.presentation().environment());};
    // A version 6 world section: every World option under its saved name.
    const auto world_keys=[](const rendering::SceneLightingSettings& lit) {
        renderer::ViewSettings view;static_cast<rendering::SceneLightingSettings&>(view.lit)=lit;
        auto result=nlohmann::json::object();
        for(const auto& option:view_options()) {
            if(!world_option(option.option) || option.name.starts_with("sky_")) continue;
            const auto value=view_value(view,option.option);const std::string name(option.name);
            if(option.kind==ViewValueKind::boolean) result[name]=value!=0;
            else if(option.kind==ViewValueKind::enumeration) result[name]=option.choices[static_cast<std::size_t>(value)];
            else if(option.kind==ViewValueKind::integer) result[name]=static_cast<std::int64_t>(value);
            else result[name]=value;
        }
        return result;
    };
    const auto environment_record=[](const nlohmann::json& scene)->const nlohmann::json& {
        for(const auto& entity:scene.at("entities")) for(const auto& part:entity.at("components"))
            if(part.at("type")=="world.EnvironmentComponent") return part.at("properties");
        throw std::runtime_error("The saved scene has no Environment.");
    };
    const auto before=settings(document);const auto count=document.undo_count();
    auto candidate=before;candidate.exposure_ev=3.5f;
    take(document.set_environment(candidate));take(document.set_environment(candidate));
    require(document.undo_count()==count+1,"Repeated World settings created duplicate history.");
    take(document.undo());workspace.sync_world(document);
    require(settings(document)==before && static_cast<const rendering::SceneLightingSettings&>(workspace.view.lit)==before,
        "Undo did not restore World and the evaluated view together.");
    take(document.redo());workspace.sync_world(document);
    const auto saved=take(document.serialize());auto restored=take(Session::deserialize(saved,project));
    require(settings(*restored)==candidate && take(restored->serialize())==saved && !restored->dirty() && restored->lighting_supplied(),
        "Scene v7 did not round-trip World authoring values into a clean document.");
    const auto stable_history=document.undo_count();const auto diagnostic=workspace.view.lit.probe_debug;
    auto diagnostic_view=workspace.view;diagnostic_view.lit.probe_debug=!diagnostic;
    diagnostic_view.lit.direct_reference=true;
    take(workspace.set_view_settings(document,diagnostic_view));
    require(document.undo_count()==stable_history && take(document.serialize())==saved,"Viewport diagnostics entered the scene or Undo stack.");
    const auto saved_json=nlohmann::json::parse(saved);
    require(saved_json.at("version")==7 && !saved_json.contains("world") && !environment_record(saved_json).contains("direct_reference") &&
        !environment_record(saved_json).contains("probe_debug"),"Scene v7 kept a world section or saved viewport diagnostics.");
    // Every version 6 World key reaches its Environment field; distinct volumes catch a slot or axis mix-up.
    auto authored=candidate;
    for(std::uint32_t i=0;i<4;++i) {
        auto& volume=authored.probe_volumes[i];const auto f=static_cast<float>(i);
        volume.enabled=i%2==0;volume.placement=i==3?renderer::ProbePlacement::follow_focus:renderer::ProbePlacement::fixed;volume.minimum={-6-f,-1-f*2,-6-f*3};volume.maximum={6+f*3,5+f,6+f*2};volume.counts={4+i,3+i*2,5+i};volume.blend_cells=.25f*f;
    }
    const nlohmann::json v6{{"version",6},{"next_id","1"},{"world",world_keys(authored)},{"roots",nlohmann::json::array()},{"entities",nlohmann::json::array()}};
    {
        auto upgraded=take(Session::deserialize(v6.dump(),project));
        require(settings(*upgraded)==authored && upgraded->lighting_supplied() && upgraded->load_issues().empty() && !upgraded->dirty(),
            "Scene v6 World settings did not reach the Environment field by field.");
        const auto upgraded_saved=nlohmann::json::parse(take(upgraded->serialize()));
        auto upgraded_reopened=take(Session::deserialize(upgraded_saved.dump(),project));
        require(upgraded_saved.at("version")==7 && !upgraded_saved.contains("world") && settings(*upgraded_reopened)==authored,
            "Scene v6 World settings did not save and reopen as a v7 Environment.");
    }
    auto v4=nlohmann::json{{"version",4},{"actors",nlohmann::json::array()},{"world",world_keys(candidate)}};v4["world"].erase("direct_candidates");
    v4["world"]["shadow_light_budget"]=16;v4["world"]["shadow_samples"]=64;
    auto migrated=take(Session::deserialize(v4.dump(),project));
    auto migrated_settings=candidate;migrated_settings.direct_candidates=8;
    require(settings(*migrated)==migrated_settings,
        "Scene v4 migration failed to preserve author values and use eight direct candidates.");
    const auto migrated_saved=take(migrated->serialize());
    auto reopened=take(Session::deserialize(migrated_saved,project));
    require(settings(*reopened)==migrated_settings && take(reopened->serialize())==migrated_saved &&
        nlohmann::json::parse(migrated_saved).at("version")==7,
        "Migrated v4 scene did not save and reopen through the current v7 author format.");
    Workspace migrated_workspace;migrated_workspace.sync_world(*reopened);
    require(!migrated_workspace.view.lit.direct_reference,
        "Legacy 64 shadow samples silently enabled the viewport reference diagnostic.");
    for(const auto key:{"shadow_light_budget","shadow_samples"}) {
        for(const auto& bad:std::array<nlohmann::json,5>{-1,65,1.5,"8",true}) {
            auto invalid_legacy=v4;invalid_legacy["world"][key]=bad;
            require(!Session::deserialize(invalid_legacy.dump(),project,{},LoadPolicy::strict),
                "Malformed legacy shadow value was accepted during v4 migration.");
        }
        auto retired=v6;retired["world"][key]=1;
        require(!Session::deserialize(retired.dump(),project,{},LoadPolicy::strict),"Scene v6 accepted a retired shadow key.");
        auto mixed=v4;mixed["world"]["direct_candidates"]=8;
        require(!Session::deserialize(mixed.dump(),project,{},LoadPolicy::strict),"Scene v4 accepted mixed new and retired direct-light options.");
    }
    for(const auto key:{"probe_debug","direct_reference"}) {
        auto diagnostic_json=v6;diagnostic_json["world"][key]=true;
        require(!Session::deserialize(diagnostic_json.dump(),project,{},LoadPolicy::strict),"Viewport diagnostic was accepted as a World author option.");
    }
    auto bad_budget=v4;bad_budget["world"]["shadow_light_budget"]=17;
    auto bad_samples=v4;bad_samples["world"]["shadow_samples"]=0;
    require(!Session::deserialize(bad_budget.dump(),project,{},LoadPolicy::strict) && !Session::deserialize(bad_samples.dump(),project,{},LoadPolicy::strict),
        "Scene v4 migration ignored the legacy budget or sample-count boundary.");
    for(const auto bad:{0.,33.,1.5}) {
        auto bad_candidates=v6;bad_candidates["world"]["direct_candidates"]=bad;
        require(!Session::deserialize(bad_candidates.dump(),project,{},LoadPolicy::strict),"Scene v6 accepted invalid direct candidates.");
    }
    auto invalid=candidate;invalid.probe_volumes[0].maximum=invalid.probe_volumes[0].minimum;
    require(!document.set_environment(invalid) && settings(document)==candidate && document.undo_count()==stable_history,
        "Invalid World configuration partially published.");
    auto json=v6;json["world"]["probe_debug"]=true;
    require(!Session::deserialize(json.dump(),project,{},LoadPolicy::strict),"A diagnostic setting was accepted in persisted World configuration.");
    {
        auto tolerated=take(Session::deserialize(json.dump(),project));
        require(tolerated->load_issues().size()==1 && tolerated->load_issues().front().kind==world::DiagnosticKind::unknown_field &&
            environment_record(nlohmann::json::parse(take(tolerated->serialize()))).at("probe_debug")==true,
            "Tolerant loading did not keep an unknown World setting as saved Environment data.");
    }
    json=nlohmann::json{{"version",3},{"actors",nlohmann::json::array()}};
    auto legacy=take(Session::deserialize(json.dump(),project));
    require(settings(*legacy)==rendering::SceneLightingSettings{} && legacy->environment()==ObjectId{} && !legacy->lighting_supplied(),
        "An old scene inherited another document's World settings.");
    take(restored->start());
    require(restored->set_environment(candidate).has_value() && !restored->set_environment(before) && settings(*restored)==candidate,
        "A running World accepted a World settings edit.");take(restored->stop());
    take(document.select(selected));
    workspace.dock.select(control::parameters_tab);publish();
    const auto entity=document.selection().entity;const auto root=entity;
    const auto schema=take(document.properties(root));
    const auto position=std::ranges::find(schema,std::string("position.x"),&Property::name);
    require(position!=schema.end() && position->label=="Position (m)" && position->category=="Transform","AFIELD metadata did not reach the real entity parameter schema.");
    const auto key=workspace.field_control(root,document.types().position_fields[0]);
    take(workspace.apply_view(document,{ui::ActionKind::activate,control::group_categories,true}));publish();
    require(has_text(L"Transform") && workspace.field_control(root,document.types().position_fields[0])==key,
        "Category grouping changed the property target or omitted reflected groups.");
    take(workspace.apply_view(document,{ui::ActionKind::activate,control::group_components,true}));publish();
    require(workspace.field_control(root,document.types().position_fields[0])==key,"Component grouping lost field identity.");
    }
    std::puts("Scene Lit application acceptance: PASS (World/Project, light history/persistence, DSL lighting page, transactional volume edits, stale actions, display menus)");
}
