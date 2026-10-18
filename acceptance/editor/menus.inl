void verify_about(Application& app,std::wstring_view expected_project) {
    const auto history=app.document().undo_count();
    key(app,VK_F10);key(app,VK_LEFT);
    require(app.menu_frame() && app.menu_frame()->find(static_cast<ControlKey>(atelier::editor::Command::about)),
        "File/Help keyboard navigation did not wrap across the menu registry.");
    key(app,VK_RIGHT);
    require(app.menu_frame() && app.menu_frame()->find(control::exit),"Help/File keyboard navigation did not wrap back.");
    key(app,VK_ESCAPE);
    click(app,control::help_menu);
    require(app.menu_frame() && app.menu_frame()->find(static_cast<ControlKey>(atelier::editor::Command::about)),
        "Help omitted the About command.");
    struct DialogLookup { HWND owner{},found{}; } lookup{app.window()};
    const auto thread_id=GetWindowThreadProcessId(app.window(),nullptr);
    std::wstring title,content;
    std::jthread dismiss([&](std::stop_token stop) {
        while (!stop.stop_requested() && !lookup.found && IsWindow(lookup.owner)) {
            EnumThreadWindows(thread_id,[](HWND window,LPARAM parameter)->BOOL {
                auto& state=*reinterpret_cast<DialogLookup*>(parameter);
                wchar_t name[32]{};GetClassNameW(window,name,32);
                if (GetWindow(window,GW_OWNER)==state.owner && IsWindowVisible(window) && std::wstring_view(name)==L"#32770") {
                    state.found=window;return FALSE;
                }
                return TRUE;
            },reinterpret_cast<LPARAM>(&lookup));
            if (!lookup.found) Sleep(10);
        }
        if (!lookup.found) return;
        const auto text=[](HWND window) {
            std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(window)),L'\0');
            const auto size=GetWindowTextW(window,value.data(),static_cast<int>(value.size()+1));
            value.resize(static_cast<std::size_t>(size));return value;
        };
        title=text(lookup.found);
        for (auto child=GetWindow(lookup.found,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) content+=text(child);
        // Close the native dialog through its lifecycle message. A synthesized
        // WM_COMMAND/IDOK is not a portable substitute for a button notification.
        const auto deadline=GetTickCount64()+5000;
        while (!stop.stop_requested() && IsWindow(lookup.found) && GetTickCount64()<deadline) {
            DWORD_PTR ignored{};
            SendMessageTimeoutW(lookup.found,WM_CLOSE,0,0,SMTO_ABORTIFHUNG,500,&ignored);
            Sleep(10);
        }
    });
    {const ExpectedNativeDialogs expected;click(app,static_cast<ControlKey>(atelier::editor::Command::about));}
    dismiss.request_stop();dismiss.join();tick(app);
    const bool valid=lookup.found && !IsWindow(lookup.found) && title==L"About FrostFox Atelier" &&
        content.contains(atelier::editor::wide(atelier::build_info::revision)) &&
        content.contains(atelier::editor::wide(atelier::build_info::configuration)) && content.contains(expected_project);
    if (!valid) std::fprintf(stderr,"About verification: found=%d closed=%d title=%d revision=%d configuration=%d project=%d\n",
        lookup.found!=nullptr,!IsWindow(lookup.found),title==L"About FrostFox Atelier",
        content.contains(atelier::editor::wide(atelier::build_info::revision)),content.contains(atelier::editor::wide(atelier::build_info::configuration)),content.contains(expected_project));
    require(valid,"About did not show this binary's build identity and current project.");
    require(!app.menu_window() && app.document().undo_count()==history,"About changed document history or retained its menu.");
}

void verify_menu_input_transitions(Application& app) {
    using namespace atelier;
    using Access=atelier::application::EditorAcceptanceAccess;
    const auto history=app.document().undo_count();
    const auto selection=app.document().selection();
    const std::vector selected(app.document().selected_entities().begin(),app.document().selected_entities().end());
    const auto member_count=entities(app).size();
    const auto key=[&](ui::Key mapped,WPARAM native,bool present=true) {
        Access::input(app,{.kind=ui::EventKind::key_down,.key=mapped},native);
        Access::input(app,{.kind=ui::EventKind::key_up,.key=mapped},native);
        if(present) tick(app,1);
    };
    const auto click_control=[&](ui::ControlKey key) {
        const auto point=centre(app,key);
        Access::input(app,{.kind=ui::EventKind::pointer_move,.x=float(point.x),.y=float(point.y)});
        Access::input(app,{.kind=ui::EventKind::pointer_down,.x=float(point.x),.y=float(point.y),.button=ui::PointerButton::primary});
        Access::input(app,{.kind=ui::EventKind::pointer_up,.x=float(point.x),.y=float(point.y),.button=ui::PointerButton::primary});
        tick(app,1);
    };
    const auto live_menu=[&]() {
        require(app.menu_frame()!=nullptr,"Expected a live accepted native menu frame.");
        return app.menu_frame();
    };
    click_control(control::create_menu);key(ui::Key::home,VK_HOME);
    // Both input sequences arrive before any replacement menu frame is prepared.
    key(ui::Key::left,VK_LEFT,false);key(ui::Key::enter,VK_RETURN,false);tick(app,1);
    require(entities(app).size()==member_count && app.document().undo_count()==history,
        "Menu switching executed a command from the retired dropdown before its replacement was accepted.");
    require(app.menu_frame() && live_menu()->find(control::undo),"Menu switching lost the requested Edit dropdown.");
    key(ui::Key::escape,VK_ESCAPE);
    require(!app.menu_window(),"Escape did not close the replacement menu.");

    click_control(editor::menu_key(editor::CommandGroup::project));key(ui::Key::home,VK_HOME);
    const auto parent=app.menu_window();
    require(live_menu()->find(control::examples_menu),"Project menu is missing its Examples branch.");
    key(ui::Key::right,VK_RIGHT);
    require(app.menu_window()!=parent && live_menu()->find(static_cast<ControlKey>(editor::Command::example_sponza)),
        "Right from a top-level menu branch switched menu groups instead of opening its child.");
    key(ui::Key::escape,VK_ESCAPE);
    require(app.menu_window()==parent && live_menu()->find(control::examples_menu),"Escape did not restore the Project parent menu.");
    key(ui::Key::escape,VK_ESCAPE);
    require(!app.menu_window(),"Escape did not close the Project root menu.");

    click_control(control::viewport_select_menu);
    click_control(static_cast<ControlKey>(editor::Command::select_box));
    require(!app.menu_window() && Access::tool_active(app) && GetCapture()==app.window(),
        "Closing the Select menu released the new Box Select tool's mouse capture.");
    const auto viewport=centre(app,control::viewport);
    Access::input(app,{.kind=ui::EventKind::pointer_move,.x=float(viewport.x),.y=float(viewport.y)});tick(app,1);
    require(Access::tool_active(app) && GetCapture()==app.window(),"Menu-started Box Select lost capture on its first movement.");
    key(ui::Key::escape,VK_ESCAPE);
    require(!Access::tool_active(app) && !Access::menu_open(app) && GetCapture()!=app.window(),
        "Cancelling menu-started Box Select retained a tool, menu or mouse capture.");
    require(app.document().undo_count()==history && entities(app).size()==member_count && app.document().selection()==selection &&
        std::ranges::equal(app.document().selected_entities(),selected),"Menu transitions changed scene history or selection.");
    std::puts("Menu input transitions: PASS (queued replacement, top-level branch, command capture transfer).");
}

void verify_native_menus(Application& app,const std::filesystem::path& output,atelier::ObjectId entity) {
    const auto live_menu=[&]() {
        require(app.menu_frame()!=nullptr,"Expected a live accepted native menu frame.");
        return app.menu_frame();
    };
    const auto closed_titles=[&] {
        require(!app.menu_window(),"Menu remained open after dismissal.");
        for (const auto& group:atelier::editor::command_groups) {
            const auto key=group.key;
            const auto* box=app.presented()->find(key);
            const auto& style=*box->style;
            require(!box->selected && !style.image && (!style.background || style.background->a==0) && style.border_width==0,
                "Closed menu title retained an active face from logical focus.");
        }
    };
    require(app.presented()->find(control::file_menu) && !app.presented()->find(control::undo),
        "Main commands were not moved into the menubar.");
    const auto field=property_control(app,entity,0);
    const auto pane=app.docking().pane(control::scene_tab);
    const auto expanded=app.presented()->find(pane)->bounds;
    const auto history=app.document().undo_count();
    const auto dpi_before=app.presented()->drawing->dpi_scale;
    verify_stow_alignment(app);
    click(app,control::menu_bar_handle);
    verify_stow_alignment(app);
    require(!app.presented()->find(control::file_menu) && app.presented()->find(field) &&
        std::abs(app.presented()->find(pane)->bounds.y-expanded.y+20*dpi_before)<.1f && app.document().undo_count()==history,
        "Stowing the menubar did not release space or changed the document.");
    capture(app,output/"menubar-stowed.png");
    click(app,control::menu_bar_handle);
    require(app.presented()->find(pane)->bounds==expanded,"Restoring the menubar changed the pane geometry.");
    click(app,control::menu_bar_handle); key(app,VK_F10);
    require(app.presented()->find(control::file_menu) && app.menu_frame() && live_menu()->find(control::exit),
        "F10 did not reveal the stowed menubar and open File.");
    key(app,VK_LEFT);
    require(live_menu()->find(static_cast<ControlKey>(atelier::editor::Command::about)),"Left from File did not wrap to Help.");
    key(app,VK_RIGHT);
    require(live_menu()->find(control::exit),"Right from Help did not wrap to File.");
    key(app,VK_ESCAPE);
    verify_about(app,L"No project open");closed_titles();
    verify_menu_input_transitions(app);closed_titles();
    for (const bool middle:{false,true}) {
        key(app,VK_ESCAPE);
        const auto original=x(app,entity);
        const auto before_history=app.document().undo_count();
        const auto from=centre(app,field);
        auto physical=from; ClientToScreen(app.window(),&physical);
        require(SetCursorPos(physical.x,physical.y)!=FALSE,"Cannot align menu-transition gesture."); tick(app,2);
        SendMessageW(app.window(),middle?WM_MBUTTONDOWN:WM_LBUTTONDOWN,middle?MK_MBUTTON:MK_LBUTTON,MAKELPARAM(from.x,from.y));
        const auto dx=static_cast<LONG>(std::lround(24*dpi_before));
        require(SetCursorPos(physical.x+dx,physical.y)!=FALSE,"Cannot align menu-transition drag.");
        SendMessageW(app.window(),WM_MOUSEMOVE,middle?MK_MBUTTON:MK_LBUTTON,MAKELPARAM(from.x+dx,from.y)); tick(app,2);
        require(app.document().editing() && x(app,entity)!=original,"Menu-transition drag did not start an actual edit.");
        key(app,VK_F10);
        require(app.menu_window() && !app.document().editing() && x(app,entity)==original && !app.presented()->value_ladder,
            "Opening a native menu retained its owner's captured edit.");
        SendMessageW(app.window(),middle?WM_MBUTTONUP:WM_LBUTTONUP,0,MAKELPARAM(from.x+dx,from.y)); tick(app,1);
        key(app,VK_ESCAPE);
        SendMessageW(app.window(),WM_MOUSEMOVE,0,MAKELPARAM(from.x+dx*2,from.y)); tick(app,2);
        require(!app.document().editing() && x(app,entity)==original && app.document().undo_count()==before_history && GetCapture()!=app.window(),
            "Dismissed native menu resumed a stale drag or changed history.");
        RECT host{}; GetWindowRect(app.window(),&host);
        require(SetCursorPos(host.left+32,host.top+12)!=FALSE,"Cannot park menu-transition pointer."); tick(app,2);
    }
    click(app,field); key(app,VK_ESCAPE); key(app,VK_F10);
    require(app.menu_frame() && live_menu()->find(control::exit),"F10 did not open File.");
    const auto native_popup=app.menu_window();
    key(app,VK_ESCAPE);
    const auto menu_pane=app.docking().pane(control::scene_tab);
    click(app,atelier::ui::DockSpace::menu_button(menu_pane));
    require(live_menu()->find(menu_pane+5) && live_menu()->find(menu_pane+6),"Pane menu omitted close/maximize actions.");
    const auto parent_popup=app.menu_window();
    key(app,VK_HOME); key(app,VK_RIGHT);
    require(app.menu_window()!=parent_popup && live_menu()->scrollbars.empty(),"Pane type submenu did not open as a separate, bounded popup.");
    atelier::renderer::Capture submenu_pixels; app.capture_menu(submenu_pixels); png(submenu_pixels,output/"pane-types.png");
    key(app,VK_ESCAPE);
    require(app.menu_window()==parent_popup,"Escape did not return from the submenu to its parent.");
    key(app,VK_ESCAPE);
    require(!app.menu_window(),"Root Escape did not close the menu chain.");
    click(app,field); key(app,VK_ESCAPE); key(app,VK_F10);

    require(native_popup!=app.window() && GetWindow(native_popup,GW_OWNER)==app.window() &&
        (GetWindowLongPtrW(native_popup,GWL_STYLE)&WS_POPUP) && !(GetWindowLongPtrW(native_popup,GWL_STYLE)&WS_CHILD),
        "Dropdown is not an independently owned native popup window.");
    require(GetFocus()==app.window() && GetCapture()==app.window(),"Menu stole owner focus or lost capture.");
    key(app,VK_RIGHT);
    require(live_menu()->find(control::undo),"Right arrow did not switch to Edit.");
    key(app,VK_RIGHT);
    require(live_menu()->find(control::add_entity) && app.menu_window()==native_popup,"Menu switching rebuilt its native host.");
    mouse(app,WM_MOUSEMOVE,centre(app,control::view_menu));
    require(live_menu()->find(control::focus),"Menubar hover did not switch the open dropdown.");
    atelier::renderer::Capture menu_pixels; app.capture_menu(menu_pixels);
    require(menu_pixels.extent.width>0 && !menu_pixels.rgba.empty(),"Native menu did not render through the GPU.");
    const auto menu_pixel=[&](unsigned x,unsigned y,int gray) {
        const auto offset=(static_cast<std::size_t>(y)*menu_pixels.extent.width+x)*4;
        for (std::size_t channel=0;channel<3;++channel)
            if (std::abs(static_cast<int>(menu_pixels.rgba[offset+channel])-gray)>1)
                throw std::runtime_error("Native menu frame pixel ("+std::to_string(x)+", "+std::to_string(y)+") is "+
                    std::to_string(menu_pixels.rgba[offset])+"/"+std::to_string(menu_pixels.rgba[offset+1])+"/"+std::to_string(menu_pixels.rgba[offset+2])+
                    ", expected edge gray "+std::to_string(gray)+".");
    };
    png(menu_pixels,output/"native-menu-view.png");
    menu_pixel(0,5,148); menu_pixel(5,0,148);
    menu_pixel(menu_pixels.extent.width-1,5,38);
    menu_pixel(5,menu_pixels.extent.height-1,38);
    require(live_menu()->scrollbars.empty(),"Short native menu retained an empty scrollbar.");
    for (const auto& box:live_menu()->controls) if (box.kind==atelier::ui::ControlKind::label)
        require(box.style->font.style==atelier::ui::FontStyle::italic &&
            box.style->text_vertical_align==atelier::ui::TextVerticalAlign::center,"Menu shortcut lost its italic, vertically centered style.");
    key(app,VK_ESCAPE);
    if (app.menu_window() || !app.wants_keyboard() || GetCapture()==app.window())
        throw std::runtime_error("Menu dismissal: visible="+std::to_string(app.menu_window()!=nullptr)+
            " field focus="+std::to_string(app.wants_keyboard())+" capture="+std::to_string(GetCapture()==app.window())+".");
    closed_titles();
    {
        KeyStateScope modifiers(false,false,true);
        SendMessageW(app.window(),WM_SYSKEYDOWN,'W',LPARAM{1}<<29);
        SendMessageW(app.window(),WM_SYSKEYUP,'W',LPARAM{1}<<29); tick(app,2);
    }
    require(app.menu_frame() && live_menu()->find(control::reset_layout),"Alt+W did not open Window.");
    key(app,VK_ESCAPE);
    RECT previous{}; GetWindowRect(app.window(),&previous);
    MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
    require(GetMonitorInfoW(MonitorFromWindow(app.window(),MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE,"Cannot query menu acceptance monitor.");
    const auto area=monitor.rcWork; const auto dpi=app.presented()->drawing->dpi_scale;
    // Leave enough monitor space beyond the owner for an unconstrained popup.
    const auto test_width=std::min(static_cast<int>(1000*dpi),static_cast<int>((area.right-area.left)*.6f));
    SetWindowPos(app.window(),nullptr,area.left+12,area.top+12,test_width,static_cast<int>(650*dpi),SWP_NOZORDER|SWP_NOACTIVATE);
    tick(app,4);
    // Anchor at the rightmost tuple field; its position is independent of the
    // inspector's caption width and field wrapping.
    const auto edge_field=property_control(app,app.document().selection().entity,2);
    auto point=centre(app,edge_field);
    point.x=static_cast<LONG>(app.presented()->find(edge_field)->bounds.x+app.presented()->find(edge_field)->bounds.width-3);
    const auto context_at=[&] {
        require_input_focus(app); KeyStateScope modifiers;
        SendMessageW(app.window(),WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(point.x,point.y));
        SendMessageW(app.window(),WM_RBUTTONUP,0,MAKELPARAM(point.x,point.y)); tick(app,2);
    };
    context_at();
    require(app.menu_window() && app.menu_frame(),"The numeric field context menu did not open.");
    RECT owner{},popup{}; GetWindowRect(app.window(),&owner); GetWindowRect(app.menu_window(),&popup);
    if(popup.right<=owner.right) std::fprintf(stderr,"Menu edge fixture: owner=(%ld,%ld,%ld,%ld), popup=(%ld,%ld,%ld,%ld), work=(%ld,%ld,%ld,%ld), dpi=%.2f, point=(%ld,%ld)\n",
        owner.left,owner.top,owner.right,owner.bottom,popup.left,popup.top,popup.right,popup.bottom,area.left,area.top,area.right,area.bottom,dpi,point.x,point.y);
    require(popup.right>owner.right,"Context menu remained confined to the editor rectangle.");
    require(!app.presented()->find(control::parameter_menu),"Native menu still has a clipped copy in the main surface.");
    app.capture_menu(menu_pixels); png(menu_pixels,output/"native-menu-parameter.png");
    click(app,control::menu_copy);
    require(!app.menu_window(),"Selecting a menu item outside the editor did not dismiss it.");
    // Put the anchor near the work-area corner, where the editor can extend
    // off screen but its menu must remain reachable on the anchor monitor.
    SetWindowPos(app.window(),nullptr,area.right-point.x-30,area.bottom-point.y-55,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    tick(app,3); context_at(); GetWindowRect(app.menu_window(),&popup);
    require(popup.left>=area.left && popup.top>=area.top && popup.right<=area.right && popup.bottom<=area.bottom,
        "Popup placement escaped the monitor work area.");
    key(app,VK_ESCAPE);
    SetWindowPos(app.window(),nullptr,previous.left,previous.top,previous.right-previous.left,previous.bottom-previous.top,SWP_NOZORDER|SWP_NOACTIVATE);
    tick(app,4);
    // Drive the same native host with boundary-sized data, retaining its actual
    // theme, private fonts, monitor placement and GPU accepted-frame path.
    click(app,control::window_menu);
    const auto ordinary_width=live_menu()->drawing->extent.width;
    const std::array long_items{
        atelier::ui::MenuItem{9000001,L"A substantially longer editor command title",true,false,{},L"Ctrl+Alt+Shift+F12"},
        atelier::ui::MenuItem{9000002,L"Unavailable command",false,false,{},L"Ctrl+Shift+F11"}};
    atelier::application::EditorAcceptanceAccess::menu_items(app,long_items);
    app.capture_menu(menu_pixels);
    require(menu_pixels.frame_id && menu_pixels.extent.width>ordinary_width && live_menu()->scrollbars.empty(),
        "Native menu did not fit long title and shortcut columns.");
    for (const auto& box:live_menu()->controls) if (box.kind==atelier::ui::ControlKind::label) {
        const auto* row=live_menu()->find(box.parent);
        const auto command=std::find_if(live_menu()->controls.begin(),live_menu()->controls.end(),
            [&](const auto& item){return item.parent==box.parent && item.kind==atelier::ui::ControlKind::menu_item;});
        require(row && command!=live_menu()->controls.end() && box.bounds.x+box.bounds.width<=box.clip.right+1,
            "Native menu shortcut was clipped by its column.");
        const auto title=text_coverage(*app.menu_frame(),command->key);
        for (const auto& draw:live_menu()->drawing->draws) if (draw.coverage==title)
            require(draw.bounds.x+draw.bounds.width<box.bounds.x,"Native menu title overlaps its shortcut.");
    }
    png(menu_pixels,output/"native-menu-long-columns.png");
    const auto menu_dpi=live_menu()->drawing->dpi_scale;
    const auto count=static_cast<unsigned>(std::ceil((area.bottom-area.top)/(25.f*menu_dpi)))+8;
    std::vector<atelier::ui::MenuItem> overflow_items;
    for (unsigned i=0;i<count;++i) overflow_items.push_back({9100000+i,L"Menu entry "+std::to_wstring(i+1),true,false,{}});
    atelier::application::EditorAcceptanceAccess::menu_items(app,overflow_items);
    app.capture_menu(menu_pixels);
    require(menu_pixels.frame_id && !live_menu()->scrollbars.empty(),"Screen-limited native menu lost its scrollbar.");
    atelier::application::EditorAcceptanceAccess::menu_end(app);
    app.capture_menu(menu_pixels);
    const auto* last=live_menu()->find(overflow_items.back().key);
    require(last && last->bounds.y>=last->clip.top && last->bounds.y+last->bounds.height<=last->clip.bottom+.1f,
        "Native menu End navigation did not reveal the final command.");
    GetWindowRect(app.menu_window(),&popup);
    require(popup.left>=area.left && popup.top>=area.top && popup.right<=area.right && popup.bottom<=area.bottom,
        "Screen-limited menu escaped its monitor work area.");
    png(menu_pixels,output/"native-menu-overflow-end.png");
    key(app,VK_ESCAPE); closed_titles();
    click(app,control::edit_menu); SendMessageW(app.window(),WM_CANCELMODE,0,0); tick(app,2);
    require(!app.menu_window() && GetCapture()!=app.window(),"Native cancel mode left the menu alive.");
    click(app,control::create_menu); SetFocus(nullptr); tick(app,2); SetFocus(app.window()); tick(app,2);
    require(!app.menu_window() && GetCapture()!=app.window(),"Owner focus loss left an interactive popup.");
    // Opening without a previous field focus can leave logical focus on the title.
    // Its closed face must still match every other menubar item.
    click(app,control::create_menu); key(app,VK_ESCAPE); closed_titles();
    capture(app,output/"native-menu-closed.png");
    capture(app,output/"native-menubar.png");
}
