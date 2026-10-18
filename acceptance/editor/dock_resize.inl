void verify_viewport_splitter_drag(Application& app,const std::filesystem::path& output) {
    using namespace atelier;
    namespace control=atelier::editor::control;
    using Access=application::EditorAcceptanceAccess;
    // Capture lets Windows deliver WM_MOUSEMOVE at the physical cursor, and a splitter drag adds pointer
    // deltas, so the physical cursor follows every synthetic point to keep those moves on the gesture path.
    const auto pointer=[&](ui::EventKind kind,math::Vector2 p) {
        POINT physical{std::lround(p.x),std::lround(p.y)};ClientToScreen(app.window(),&physical);
        require(SetCursorPos(physical.x,physical.y)!=FALSE,"Cannot align the splitter gesture pointer.");
        Access::input(app,{.kind=kind,.x=p.x,.y=p.y,.button=ui::PointerButton::primary});
    };
    std::ofstream report(output/"splitter-drag.csv");
    report<<"axis,direction,step,captured,width,height\n";
    for(const auto axis:{ui::DragAxis::horizontal,ui::DragAxis::vertical}) for(const float direction:{-1.f,1.f}) {
        const auto before=app.presented()->find(control::viewport)->bounds;
        const auto key=app.docking().divider(control::scene_tab,axis);
        const auto* divider=app.presented()->find(key);
        require(divider!=nullptr,"Viewport resize divider is absent.");
        const bool horizontal=axis==ui::DragAxis::horizontal;
        const math::Vector2 start=horizontal?
            math::Vector2{divider->bounds.x+divider->bounds.width*.5f,before.y+before.height*.35f}:
            math::Vector2{before.x+before.width*.3f,divider->bounds.y+divider->bounds.height*.5f};
        // Moves queued from the earlier cursor position arrive as hover before the press.
        pointer(ui::EventKind::pointer_move,start);tick(app,2);pointer(ui::EventKind::pointer_down,start);
        require(GetCapture()==app.window(),"Viewport splitter did not acquire native pointer capture.");
        const auto delta=std::min(96.f,(horizontal?before.width:before.height)*.2f);
        math::Vector2 end=start;
        for(unsigned step=1;step<=8;++step) {
            end={start.x+(horizontal?direction*delta*step/8:0),start.y+(horizontal?0:direction*delta*step/8)};
            pointer(ui::EventKind::pointer_move,end);tick(app,1);
            const auto view=app.presented()->find(control::viewport)->bounds;
            report<<horizontal<<','<<direction<<','<<step<<','<<(GetCapture()==app.window())<<','<<view.width<<','<<view.height<<'\n';
            report.flush();
            require(GetCapture()==app.window(),"Viewport splitter lost pointer capture while moving across the viewport.");
            require(app.docking().dragging() && view==before,"Viewport splitter resized live geometry or lost its preview before release.");
        }
        // A native move trails its SetCursorPos by a frame; moving to the end again just before release,
        // with no message pump between, re-anchors the drag where it is released.
        pointer(ui::EventKind::pointer_move,end);pointer(ui::EventKind::pointer_up,end);tick(app,2);
        const auto after=app.presented()->find(control::viewport)->bounds;
        report<<horizontal<<','<<direction<<",commit,"<<(GetCapture()==app.window())<<','<<after.width<<','<<after.height<<'\n';
        report.flush();
        require(GetCapture()!=app.window(),"Viewport splitter retained capture after release.");
        const auto moved=horizontal?end.x-start.x:end.y-start.y;
        require(std::abs((horizontal?after.width-before.width:after.height-before.height)-moved)<=1.f,
            "Viewport splitter failed to commit its full shrink/expand gesture.");
    }
    for(const auto axis:{ui::DragAxis::horizontal,ui::DragAxis::vertical}) for(unsigned reason=0;reason<3;++reason) {
        const auto before=app.presented()->find(control::viewport)->bounds;
        const auto* divider=app.presented()->find(app.docking().divider(control::scene_tab,axis));
        require(divider!=nullptr,"Cancelled viewport resize divider is absent.");
        const bool horizontal=axis==ui::DragAxis::horizontal;
        const math::Vector2 start=horizontal?
            math::Vector2{divider->bounds.x+divider->bounds.width*.5f,before.y+before.height*.35f}:
            math::Vector2{before.x+before.width*.3f,divider->bounds.y+divider->bounds.height*.5f};
        const math::Vector2 end{start.x-(horizontal?48.f:0),start.y-(horizontal?0:48.f)};
        pointer(ui::EventKind::pointer_move,start);pointer(ui::EventKind::pointer_down,start);
        pointer(ui::EventKind::pointer_move,end);tick(app,1);
        require(app.docking().dragging() && GetCapture()==app.window(),"Viewport resize cancellation setup failed.");
        if(reason==0) {
            Access::input(app,{.kind=ui::EventKind::key_down,.key=ui::Key::escape},VK_ESCAPE);
            Access::input(app,{.kind=ui::EventKind::key_up,.key=ui::Key::escape},VK_ESCAPE);
        } else Access::input(app,{.kind=reason==1?ui::EventKind::focus_lost:ui::EventKind::capture_lost});
        pointer(ui::EventKind::pointer_up,end);tick(app,2);
        require(!app.docking().dragging() && GetCapture()!=app.window() && app.presented()->find(control::viewport)->bounds==before,
            "Cancelled viewport resize retained capture or committed a late release.");
    }
    RECT host{};GetWindowRect(app.window(),&host);
    require(SetCursorPos(host.left+32,host.top+12)!=FALSE,"Cannot park the splitter gesture pointer.");tick(app,2);
    std::puts("Viewport splitter acceptance: PASS (both axes, shrink/expand, viewport crossing, deferred geometry, Escape/focus/capture loss)");
}
