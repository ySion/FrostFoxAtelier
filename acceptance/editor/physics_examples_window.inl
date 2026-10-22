int verify_physics_examples_window(Application& app,const char* output_directory) try {
    using Access=atelier::application::EditorAcceptanceAccess;
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(initialized)||initialized==RPC_E_CHANGED_MODE,"Cannot initialize physics captures.");
    struct Apartment {bool owns;~Apartment(){if(owns) CoUninitialize();}} apartment{SUCCEEDED(initialized)};
    const auto output=std::filesystem::absolute(output_directory);std::filesystem::create_directories(output);
    model(app.open_example(atelier::editor::Command::example_physic));
    if(const auto& build=Access::project_build(app);build && build->error) throw std::runtime_error(build->error->message);
    require(Access::scene_lit_view(app).shading==atelier::renderer::Shading::smooth &&
        static_cast<const atelier::rendering::SceneLightingSettings&>(Access::scene_lit_view(app).lit)==model(app.document().presentation().environment()),
        "Physic did not open Smooth rendering with its authored lighting settings preserved.");
    struct Example {const char* name;std::size_t entities,roots;};
    constexpr std::array examples{
        Example{"Overview",23,10},Example{"RigidBodies",8,5},Example{"Forces",8,5},Example{"Triggers",10,6},Example{"Queries",8,5},
        Example{"Character",8,5},Example{"Constraints",6,4},Example{"Ragdoll",11,4},Example{"Raycasts",20,8}};
    for(const auto& example:examples) {
        const auto* name=example.name;
        model(app.open_scene(std::string("Examples/Physic/Scenes/")+name+".scene"));
        const auto members=model(app.document().world().members());
        const auto roots=std::ranges::count_if(members,[&](const auto& member) {
            return model(atelier::world::parent_entity(app.document().world().runtime(),member.id()))==atelier::ObjectId{};
        });
        require(members.size()==example.entities && static_cast<std::size_t>(roots)==example.roots,
            (std::string(name)+" lost authored Entity members or composition roots.").c_str());
        const bool overview=std::string_view(name)=="Overview";
        const bool close=std::string_view(name)=="Ragdoll" || std::string_view(name)=="Constraints" || std::string_view(name)=="Character";
        model(app.configure_view(overview?atelier::math::Vector3{22,18,-26}:close?atelier::math::Vector3{6,5,-8}:atelier::math::Vector3{10,8,-14},{0,2,0},atelier::renderer::Shading::smooth));
        tick(app,2);capture(app,output/(std::string(name)+"-edit.png"));
        auto& document=Access::physics_document(app);
        const auto started=model(document.start());require(!started.notification_error,"Physics scene failed during activation.");
        for(int step=0;step<90;++step) model(document.update(1.f/60));
        tick(app,2);atelier::renderer::Capture captured;require(app.step(&captured)&&captured.frame_id,"Physics frame was not accepted.");
        require(std::ranges::count_if(captured.depth,[](float depth){return depth<1;})>10000,"Physics scene did not render simulated meshes.");
        png(captured,output/(std::string(name)+"-simulated.png"));
        const auto stopped=model(document.stop());require(!stopped.notification_error,"Physics scene failed during teardown.");
    }
    std::ofstream(output/"acceptance.txt")<<"Physics examples window acceptance: PASS (9 authored scenes, real World/Jolt simulation, GPU captures).\n";
    app.close();std::puts("Physics examples window acceptance: PASS");return 0;
} catch(const std::exception& error) {std::fprintf(stderr,"Physics examples window acceptance: FAIL: %s\n",error.what());return 1;}
