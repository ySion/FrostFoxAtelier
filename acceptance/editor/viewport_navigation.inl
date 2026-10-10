void verify_viewport_navigation(Application& app,const std::filesystem::path& output) {
    using namespace atelier;
    using Access=application::EditorAcceptanceAccess;
    const auto viewport=app.presented()->find(control::viewport)->bounds;
    const math::Vector2 origin{viewport.x+viewport.width*.5f,viewport.y+viewport.height*.5f};
    const auto pointer=[&](ui::EventKind kind,math::Vector2 point) {Access::input(app,{.kind=kind,.x=point.x,.y=point.y});};
    const auto key=[&](WPARAM code,bool up=false,bool ctrl=false,ui::Key mapped=ui::Key::none) {
        Access::input(app,{.kind=up?ui::EventKind::key_up:ui::EventKind::key_down,.key=mapped,.control=ctrl},code);
    };
    const auto click=[&](math::Vector2 point) {
        pointer(ui::EventKind::pointer_move,point);pointer(ui::EventKind::pointer_down,point);pointer(ui::EventKind::pointer_up,point);tick(app,3);
    };
    const auto centre=[&](ui::ControlKey key) {
        const auto* box=app.presented()->find(key);require(box!=nullptr,"Viewport navigation control is absent.");
        return math::Vector2{box->bounds.x+box->bounds.width*.5f,box->bounds.y+box->bounds.height*.5f};
    };
    const auto history=app.document().undo_count();const auto cursor=Access::cursor_position(app);
    for(const auto command:editor::camera_commands) {
        pointer(ui::EventKind::pointer_move,origin);key(VK_OEM_3);key(VK_OEM_3,true);tick(app,3);
        require(Access::menu_open(app) && app.presented()->popup==control::viewport_pie,
            "Accent-grave did not open the viewport axis pie.");
        if(command==editor::Command::view_perspective) capture(app,output/"view-axis-pie.png");
        click(centre(static_cast<ui::ControlKey>(command)));
        require(!Access::menu_open(app) && Access::camera_mode(app)==*editor::camera_view_command(command),
            "Axis pie selected the wrong camera view or retained the menu.");
        for(const auto value:Access::camera_view(app)) require(std::isfinite(value),"Axis camera produced a non-finite view basis.");
    }
    pointer(ui::EventKind::pointer_move,origin);key(VK_OEM_3);
    pointer(ui::EventKind::pointer_move,{origin.x,origin.y-120});key(VK_OEM_3,true);tick(app,3);
    require(!Access::menu_open(app) && Access::camera_mode(app)==editor::CameraView::top,
        "Fast axis pie hold/swipe/release was lost before its first accepted frame.");
    pointer(ui::EventKind::pointer_move,origin);key(VK_OEM_3);tick(app,2);
    key(VK_ESCAPE,false,false,ui::Key::escape);key(VK_ESCAPE,true,false,ui::Key::escape);tick(app,2);
    require(!Access::menu_open(app),"Axis pie Escape retained modal state.");
    for(const auto [code,preset]:std::array<std::pair<WPARAM,editor::CameraView>,3>{{
            {VK_NUMPAD1,editor::CameraView::back},{VK_NUMPAD3,editor::CameraView::left},{VK_NUMPAD7,editor::CameraView::bottom}}}) {
        pointer(ui::EventKind::pointer_move,origin);key(code,false,true);key(code,true,true);tick(app,2);
        require(Access::camera_mode(app)==preset,"Ctrl+Numpad did not select the opposite axis view.");
    }
    require(bool(Access::command(app,editor::Command::reset_camera)),"Camera reset failed.");tick(app,2);
    const auto menu_point=[&](ui::ControlKey key,bool indicator) {
        const auto frame=app.menu_frame();require(frame!=nullptr,"Native checkbox menu is absent.");
        const auto* box=frame->find(key);require(box!=nullptr,"Native checkbox menu item is absent.");
        RECT rect{};require(GetWindowRect(app.menu_window(),&rect)!=FALSE,"Cannot inspect native checkbox menu bounds.");
        POINT host{};ClientToScreen(app.window(),&host);
        const auto dpi=frame->drawing->dpi_scale;
        return math::Vector2{float(rect.left-host.x)+box->bounds.x+(box->style->padding.left+
            (indicator?box->style->indicator_size*.5f:box->style->indicator_size+12))*dpi,
            float(rect.top-host.y)+box->bounds.y+box->bounds.height*.5f};
    };
    const auto grid=Access::scene_lit_view(app).reference_grid;
    click(centre(control::scene_guides));const auto popup=app.menu_window();
    for(unsigned i=0;i<2;++i) {
        click(menu_point(control::guide_grid,true));
        require(Access::menu_open(app) && app.menu_window()==popup &&
            Access::scene_lit_view(app).reference_grid==(i?grid:!grid),"Indicator click dismissed/recreated the menu or failed to toggle.");
        const auto* checked=app.menu_frame()->find(control::guide_grid);
        require(checked && checked->selected==Access::scene_lit_view(app).reference_grid,"Retained checkbox did not publish its new checked state.");
        capture(app,output/(i?"menu-checkbox-restored.png":"menu-checkbox-toggled.png"));
    }
    click(menu_point(control::guide_grid,false));
    require(!Access::menu_open(app) && Access::scene_lit_view(app).reference_grid!=grid,"Text click did not toggle and dismiss the menu.");
    click(centre(control::scene_guides));click(menu_point(control::guide_grid,false));
    require(Access::scene_lit_view(app).reference_grid==grid,"Checkbox cleanup did not restore the view preference.");
    click(centre(control::viewport_cavity));
    const auto cavity=Access::scene_lit_view(app).cavity.enabled;
    click(menu_point(control::cavity_enabled,true));
    require(Access::menu_open(app) && Access::scene_lit_view(app).cavity.enabled!=cavity,"Cavity checkbox failed to use the retained-menu path.");
    click(menu_point(control::cavity_enabled,true));
    click(menu_point(control::cavity_group_first,false));
    click(menu_point(control::cavity_value_first+4,false));
    require(!Access::menu_open(app) && Access::scene_lit_view(app).cavity.ridge==2,"Cavity ridge menu did not reach ViewSettings.");
    click(centre(control::viewport_cavity));click(menu_point(control::cavity_group_first,false));
    click(menu_point(control::cavity_value_first+2,false));
    require(Access::scene_lit_view(app).cavity.ridge==1,"Cavity ridge cleanup failed.");
    require(!app.presented()->find(control::viewport_fps),"FPS overlay was enabled by default.");
    click(centre(control::scene_guides));click(menu_point(control::command_key(editor::Command::show_fps),true));
    require(Access::menu_open(app) && app.presented()->find(control::viewport_fps),"FPS checkbox did not retain the menu and show the HUD.");
    key(VK_ESCAPE,false,false,ui::Key::escape);key(VK_ESCAPE,true,false,ui::Key::escape);tick(app,2);
    const auto fps_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(Access::fps_text(app).empty() && std::chrono::steady_clock::now()<fps_deadline) tick(app,1);
    const auto measured=Access::fps_text(app);
    require(measured.find(L" FPS")!=std::wstring::npos && measured.find(L" ms")!=std::wstring::npos,"Accepted frames did not produce an FPS and frame interval reading.");
    require(bool(Access::fps_setting(app,true)) && Access::fps_text(app)==measured,"Repeated FPS enable reset the current sample.");
    tick(app,1);const auto* fps_box=app.presented()->find(control::viewport_fps);
    require(fps_box && !fps_box->enabled && fps_box->bounds.y>viewport.y && text_coverage(*app.presented(),control::viewport_fps),
        "FPS HUD did not render as passive text inside the viewport.");
    capture(app,output/"viewport-fps.png");
    require(Access::fps_text(app).empty(),"Diagnostic readback contaminated the live FPS sample.");
    require(bool(Access::command(app,editor::Command::show_fps)),"FPS command failed.");tick(app,2);
    require(!app.presented()->find(control::viewport_fps) && Access::fps_text(app).empty(),"Disabling FPS retained the overlay or sampler.");
    require(app.document().undo_count()==history && Access::cursor_position(app)==cursor,"View/menu gestures changed scene history or placed the 3D cursor.");
    std::puts("Viewport navigation acceptance: PASS (axis pie, opposite views, fast release, retained checkbox, Cavity controls, FPS HUD)");
}
