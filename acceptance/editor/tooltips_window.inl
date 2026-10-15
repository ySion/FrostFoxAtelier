int verify_editor_tooltips(Application& app,const char* output_directory) {
    using namespace atelier;
    using Access=application::EditorAcceptanceAccess;
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);hr(com);
    struct ComScope {~ComScope(){CoUninitialize();}} com_scope;
    const auto output=std::filesystem::absolute(editor::wide(output_directory));std::filesystem::create_directories(output);
    tick(app);
    require(bool(Access::command(app,editor::Command::create_static_mesh)),"Tooltip fixture could not create its mesh.");tick(app);
    const auto history=app.document().undo_count();
    const auto point=[&](const ui::ControlBox& box) {
        const auto left=std::max(box.bounds.x,static_cast<float>(box.clip.left));
        const auto top=std::max(box.bounds.y,static_cast<float>(box.clip.top));
        const auto right=std::min(box.bounds.x+box.bounds.width,static_cast<float>(box.clip.right));
        const auto bottom=std::min(box.bounds.y+box.bounds.height,static_cast<float>(box.clip.bottom));
        require(right>left && bottom>top,"Tooltip control is not visible.");
        return math::Vector2{(left+right)*.5f,(top+bottom)*.5f};
    };
    const auto label=[&](std::wstring_view title) {
        const auto& frame=*app.presented();
        for (const auto& control:frame.controls) if (control.kind==ui::ControlKind::label &&
            control.bounds.width>0 && control.bounds.height>0) {
            for (const auto* parent=&control;parent;parent=parent->parent?frame.find(parent->parent):nullptr)
                if (parent->tooltip && parent->tooltip->title==title) return point(control);
        }
        throw std::runtime_error("Tooltip parameter label is absent.");
    };
    const auto pointer=[&](ui::EventKind kind,math::Vector2 p={}) {Access::input(app,{.kind=kind,.x=p.x,.y=p.y});};
    const auto hover_capture=[&](std::wstring_view title,const char* file) {
        pointer(ui::EventKind::pointer_leave);tick(app,1);
        const auto* viewport=app.presented()->find(editor::control::viewport);
        require(viewport!=nullptr,"Tooltip fixture lost its Scene View.");
        const auto bounds=viewport->bounds;
        const auto extent=app.presented()->drawing->extent;
        pointer(ui::EventKind::pointer_move,label(title));
        std::this_thread::sleep_for(std::chrono::milliseconds(550));tick(app,1);
        const auto& tip=app.presented()->tooltip;
        require(tip && tip->content && tip->content->title==title && !tip->content->identity.empty() && !tip->content->description.empty(),
            "Real parameter label did not show its reflected documentation.");
        require(tip->bounds.x>=0 && tip->bounds.y>=0 && tip->bounds.x+tip->bounds.width<=extent.width &&
            tip->bounds.y+tip->bounds.height<=extent.height,"Real tooltip escaped the editor surface.");
        capture(app,output/file);
        require(app.presented()->tooltip && app.presented()->tooltip->content->title==title &&
            app.presented()->drawing->extent==extent && app.presented()->find(editor::control::viewport)->bounds==bounds &&
            app.document().undo_count()==history,"Tooltip changed the Scene View target or authored history.");
    };
    hover_capture(L"Position (m)","parameters-position-tooltip.png");
    pointer(ui::EventKind::pointer_leave);tick(app,1);
    require(!app.presented()->tooltip,"Real pointer leave retained parameter help.");
    require(bool(Access::command(app,editor::Command::show_world_settings)),"Tooltip fixture could not open World settings.");tick(app);
    hover_capture(L"Exposure (EV)","world-exposure-tooltip.png");

    // Opening and closing the real retained menu must retire the current help episode.
    const auto* menu=app.presented()->find(editor::control::file_menu);
    require(menu!=nullptr,"Tooltip fixture has no File menu.");
    const auto at=point(*menu);pointer(ui::EventKind::pointer_down,at);pointer(ui::EventKind::pointer_up,at);tick(app,1);
    require(app.menu_frame()!=nullptr && !app.presented()->tooltip,"Opening a menu retained World help.");
    Access::input(app,{.kind=ui::EventKind::key_down,.key=ui::Key::escape},VK_ESCAPE);
    Access::input(app,{.kind=ui::EventKind::key_up,.key=ui::Key::escape},VK_ESCAPE);tick(app,1);
    require(!Access::menu_open(app) && !app.presented()->tooltip && app.document().undo_count()==history,
        "Menu dismissal resurrected help or changed scene history.");
    Access::mark_saved(app);app.close();
    require(!app.step(),"Tooltip acceptance did not close its owned editor normally.");
    std::ofstream report(output/"tooltips-acceptance.txt");
    report<<"Editor tooltip acceptance: PASS\nBuild: "<<build_info::revision<<" ("<<build_info::configuration<<")\n"
        <<"Real native window, Application input, DSL parameter labels, same-frame GPU drawing and capture.\n"
        <<"Position and World Exposure descriptions; pointer leave and menu cancellation; viewport extent and undo unchanged.\n"
        <<"No global keyboard/mouse input or foreground activation; one owned mesh fixture.\n";
    require(report.good(),"Cannot write tooltip window evidence.");
    std::puts("Editor tooltip acceptance: PASS (Parameters/World native GPU capture, cancellation, stable viewport/undo)");
    return 0;
}
