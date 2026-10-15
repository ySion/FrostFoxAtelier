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

    // Exercise the colour form through Application input and its real DSL controls.
    require(bool(Access::command(app,editor::Command::create_point_light)),"Colour fixture could not create a light.");
    auto& document=Access::physics_document(app);
    const auto light=component(app,document.selection().entity,document.types().light_component);
    std::array<world::FieldPath,3> channels;
    for(const auto& property:model(document.properties(light))) {
        if(property.name=="settings.color.x") channels[0]=property.path;
        if(property.name=="settings.color.y") channels[1]=property.path;
        if(property.name=="settings.color.z") channels[2]=property.path;
    }
    for(const auto& path:channels) require(!path.empty(),"Colour fixture lost a reflected RGB channel.");
    model(document.set_property(light,channels[0],Scalar{2.f},0x484452));
    model(Access::select_entity(app,light));
    Access::select_tab(app,editor::control::parameters_tab);tick(app);
    Access::mark_saved(app);
    const auto colour_history=document.undo_count();
    const auto channel=[&](std::size_t lane) {return std::get<float>(model(document.read_property(light,channels[lane])));};
    const auto press_key=[&](ui::Key key,WPARAM native,bool control=false) {
        Access::input(app,{.kind=ui::EventKind::key_down,.key=key,.control=control},native);
        Access::input(app,{.kind=ui::EventKind::key_up,.key=key,.control=control},native);
        tick(app,1);
    };
    const auto click_control=[&](ui::ControlKey key) {
        const auto* box=app.presented()->find(key);
        if(!box) for(const auto& control:app.presented()->controls)
            if(control.kind==ui::ControlKind::button && control.action_target==key) {box=&control;break;}
        require(box!=nullptr,"Colour fixture cannot find its input control.");
        const auto at=point(*box);
        pointer(ui::EventKind::pointer_down,at);pointer(ui::EventKind::pointer_up,at);tick(app,1);
    };
    const auto replace_text=[&](std::wstring text) {
        press_key(ui::Key::a,'A',true);
        Access::input(app,{.kind=ui::EventKind::text,.data=std::move(text)});tick(app,1);
    };
    const auto open_colour=[&] {
        const auto pick=Access::color_pick(app);require(pick!=0,"Light colour has no swatch action.");
        click_control(pick);
        require(app.presented()->find(editor::control::color_editor) && document.editing(),
            "HDR colour did not mount the real colour editor and its transaction.");
    };
    open_colour();
    require(document.dirty(),"An open scene colour transaction was reported clean.");
    click_control(editor::control::color_rgb);replace_text(L"3");
    press_key(ui::Key::tab,VK_TAB);
    require(Access::focused(app)==editor::control::color_rgb+1 && channel(0)==3.f,
        "RGB typing or Tab navigation was swallowed by the colour popup.");
    replace_text(L"0.5");press_key(ui::Key::tab,VK_TAB);
    require(Access::focused(app)==editor::control::color_rgb+2 && channel(1)==.5f,
        "Tab did not commit the green channel and focus the blue channel.");
    click_control(editor::control::color_hex);replace_text(L"804020");press_key(ui::Key::tab,VK_TAB);
    require(std::abs(channel(0)-128.f/255)<.00001f && std::abs(channel(1)-64.f/255)<.00001f &&
        std::abs(channel(2)-32.f/255)<.00001f,"Hex input did not reach the live colour transaction.");
    click_control(editor::control::color_rgb);replace_text(L"2.5");press_key(ui::Key::enter,VK_RETURN);
    require(channel(0)==2.5f && document.undo_count()==colour_history && document.dirty(),
        "HDR preview was clipped or created a premature history entry.");
    capture(app,output/"parameters-hdr-color-editor.png");
    click_control(editor::control::color_done);
    require(!document.editing() && !app.presented()->find(editor::control::color_editor) &&
        document.undo_count()==colour_history+1 && document.dirty(),"Done did not commit exactly one colour edit.");
    model(Access::command(app,editor::Command::undo));tick(app);
    require(channel(0)==2.f && channel(1)==1.f && channel(2)==1.f && !document.dirty(),
        "One undo did not restore all colour channels and the saved state.");
    open_colour();click_control(editor::control::color_rgb);replace_text(L"4");
    require(channel(0)==4.f,"Colour cancellation fixture did not preview its numeric draft.");
    Access::input(app,{.kind=ui::EventKind::focus_lost});tick(app);
    require(channel(0)==2.f && !document.editing() && !document.dirty() && document.undo_count()==colour_history &&
        !app.presented()->find(editor::control::color_editor),"Focus cancellation retained a colour draft, transaction or popup.");
    Access::mark_saved(app);app.close();
    require(!app.step(),"Tooltip acceptance did not close its owned editor normally.");
    std::ofstream report(output/"tooltips-acceptance.txt");
    report<<"Editor tooltip acceptance: PASS\nBuild: "<<build_info::revision<<" ("<<build_info::configuration<<")\n"
        <<"Real native window, Application input, DSL parameter labels, same-frame GPU drawing and capture.\n"
        <<"Position and World Exposure descriptions; pointer leave and menu cancellation; viewport extent and undo unchanged.\n"
        <<"HDR colour popup: real RGB/Hex/Tab input, dirty state, Done, single undo and focus cancellation.\n"
        <<"No global keyboard/mouse input or foreground activation; owned mesh/light fixtures.\n";
    require(report.good(),"Cannot write tooltip window evidence.");
    std::puts("Editor tooltip acceptance: PASS (Parameters/World native GPU capture, cancellation, stable viewport/undo)");
    return 0;
}
