int verify_editor_surfaces(Application& app,const char* output_directory) {
    using namespace atelier;
    using Access=application::EditorAcceptanceAccess;
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);hr(com);
    struct ComScope {~ComScope(){CoUninitialize();}} com_scope;
    const auto output=std::filesystem::absolute(editor::wide(output_directory));std::filesystem::create_directories(output);
    tick(app);
    require(bool(Access::command(app,editor::Command::create_static_mesh)),"Surface scene creation failed.");tick(app);
    const auto point=[&](ui::ControlKey key) {
        const auto* control=app.presented()->find(key);require(control!=nullptr,"Surface control is absent.");
        return math::Vector2{control->bounds.x+control->bounds.width*.5f,control->bounds.y+control->bounds.height*.5f};
    };
    const auto pointer=[&](ui::EventKind kind,math::Vector2 p) {Access::input(app,{.kind=kind,.x=p.x,.y=p.y,.button=ui::PointerButton::primary});};
    const auto escape=[&] {Access::input(app,{.kind=ui::EventKind::key_down,.key=ui::Key::escape},VK_ESCAPE);
        Access::input(app,{.kind=ui::EventKind::key_up,.key=ui::Key::escape},VK_ESCAPE);};
    std::ofstream report(output/"surfaces.csv");report<<"menu,open,presents,cpu_us,wall_us\n";
    unsigned redundant_presents{};HWND retained{};
    for(unsigned run=0;run<6;++run) {
        const auto menu=run%2?control::scene_shading:control::viewport_camera;
        const auto p=point(menu);const auto start=std::chrono::steady_clock::now();
        pointer(ui::EventKind::pointer_move,p);pointer(ui::EventKind::pointer_down,p);pointer(ui::EventKind::pointer_up,p);
        tick(app,1);
        require(app.menu_frame()!=nullptr,"Surface menu was not ready after its opening frame.");
        if(retained) require(retained==app.menu_window(),"Alternating menu sizes replaced the native popup.");
        retained=app.menu_window();
        const auto presents=Access::menu_presents(app);
        if(run && presents!=1) ++redundant_presents;
        const auto wall=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
        report<<menu<<','<<run<<','<<presents<<','<<app.menu_open_cpu_us()<<','<<wall<<'\n';report.flush();
        escape();tick(app,1);
    }
    const auto original=app.presented()->find(control::viewport)->bounds;
    const auto* divider=app.presented()->find(app.parameter_splitter());require(divider!=nullptr,"Dock divider is absent.");
    const math::Vector2 begin{divider->bounds.x+divider->bounds.width*.5f,divider->bounds.y+divider->bounds.height*.2f};
    pointer(ui::EventKind::pointer_move,begin);pointer(ui::EventKind::pointer_down,begin);
    require(GetCapture()==app.window(),"Dock shrink did not acquire native capture on press.");
    unsigned resized_during_drag{};
    for(unsigned step=1;step<=8;++step) {
        pointer(ui::EventKind::pointer_move,{begin.x-12.f*step,begin.y});tick(app,1);
        report<<"dock_step,"<<step<<','<<app.docking().dragging()<<','<<(GetCapture()==app.window())<<'\n';report.flush();
        require(app.docking().dragging() && GetCapture()==app.window(),"Dock shrink lost its drag or native capture before release.");
        if(app.presented()->find(control::viewport)->bounds!=original) ++resized_during_drag;
    }
    capture(app,output/"divider-preview.png");
    pointer(ui::EventKind::pointer_up,{begin.x-96,begin.y});tick(app);
    const auto committed=app.presented()->find(control::viewport)->bounds;
    require(committed.width<original.width-40,"Dock release did not commit the requested resize.");
    report<<"dock_resized_during_drag,"<<resized_during_drag<<"\n";report.flush();
    capture(app,output/"divider-committed.png");
    for(unsigned reason=0;reason<3;++reason) {
        const auto key=app.docking().divider(control::scene_tab,ui::DragAxis::vertical);
        const auto* rail=app.presented()->find(key);require(rail!=nullptr,"Horizontal dock rail is absent.");
        const math::Vector2 start{rail->bounds.x+rail->bounds.width*.2f,rail->bounds.y+rail->bounds.height*.5f};
        const auto before=app.presented()->find(control::viewport)->bounds;
        pointer(ui::EventKind::pointer_move,start);pointer(ui::EventKind::pointer_down,start);
        pointer(ui::EventKind::pointer_move,{start.x,start.y-45});tick(app,1);
        require(app.docking().dragging() && app.presented()->find(control::viewport)->bounds==before,
            "Vertical divider did not retain its preview without resizing the viewport.");
        if(reason==0) escape();
        else Access::input(app,{.kind=reason==1?ui::EventKind::focus_lost:ui::EventKind::capture_lost});
        pointer(ui::EventKind::pointer_up,{start.x,start.y-45});tick(app);
        require(!app.docking().dragging() && app.presented()->find(control::viewport)->bounds==before,
            "Cancelled divider committed a late release or retained its preview.");
    }
    Access::mark_saved(app);app.close();
    std::printf("Editor surfaces: drag resized %u/8 frames; redundant popup presentations %u/5 warm opens\n",resized_during_drag,redundant_presents);
    require(resized_during_drag==0,"Dock dragging resized the live viewport before release.");
    require(redundant_presents==0,"Popup resize presented an obsolete canvas before its accepted frame.");
    std::puts("Editor surfaces acceptance: PASS (single-present menus, deferred dock geometry, both axes, Escape/focus/capture loss, late release)");return 0;
}
