void verify_native_menus(Application& app,const std::filesystem::path& output,atelier::ObjectId actor) {
    require(app.presented()->find(control::file_menu) && !app.presented()->find(control::undo),
        "Main commands were not moved into the menubar.");
    const auto field=property_control(app,actor,0);
    const auto pane=app.docking().pane(control::scene_tab);
    const auto expanded=app.presented()->find(pane)->bounds;
    const auto history=app.document().undo_count();
    const auto dpi_before=app.presented()->drawing->dpi_scale;
    require(std::abs(app.presented()->find(control::menu_bar_handle)->bounds.width/dpi_before-12)<.1f,
        "Menubar stow rail width differs from the reference.");
    click(app,control::menu_bar_handle);
    require(!app.presented()->find(control::file_menu) && app.presented()->find(field) &&
        app.presented()->find(pane)->bounds.y<expanded.y-20*dpi_before && app.document().undo_count()==history,
        "Stowing the menubar did not release space or changed the document.");
    capture(app,output/"menubar-stowed.png");
    click(app,control::menu_bar_handle);
    require(app.presented()->find(pane)->bounds==expanded,"Restoring the menubar changed the pane geometry.");
    click(app,control::menu_bar_handle); key(app,VK_F10);
    require(app.presented()->find(control::file_menu) && app.menu_frame() && app.menu_frame()->find(control::exit),
        "F10 did not reveal the stowed menubar and open File.");
    key(app,VK_ESCAPE);
    for (const bool middle:{false,true}) {
        key(app,VK_ESCAPE);
        const auto original=x(app,actor);
        const auto before_history=app.document().undo_count();
        const auto from=centre(app,field);
        auto physical=from; ClientToScreen(app.window(),&physical);
        require(SetCursorPos(physical.x,physical.y)!=FALSE,"Cannot align menu-transition gesture."); tick(app,2);
        SendMessageW(app.window(),middle?WM_MBUTTONDOWN:WM_LBUTTONDOWN,middle?MK_MBUTTON:MK_LBUTTON,MAKELPARAM(from.x,from.y));
        const auto dx=static_cast<LONG>(std::lround(24*dpi_before));
        require(SetCursorPos(physical.x+dx,physical.y)!=FALSE,"Cannot align menu-transition drag.");
        SendMessageW(app.window(),WM_MOUSEMOVE,middle?MK_MBUTTON:MK_LBUTTON,MAKELPARAM(from.x+dx,from.y)); tick(app,2);
        require(app.document().editing() && x(app,actor)!=original,"Menu-transition drag did not start an actual edit.");
        key(app,VK_F10);
        require(app.menu_window() && !app.document().editing() && x(app,actor)==original && !app.presented()->value_ladder,
            "Opening a native menu retained its owner's captured edit.");
        SendMessageW(app.window(),middle?WM_MBUTTONUP:WM_LBUTTONUP,0,MAKELPARAM(from.x+dx,from.y)); tick(app,1);
        key(app,VK_ESCAPE);
        SendMessageW(app.window(),WM_MOUSEMOVE,0,MAKELPARAM(from.x+dx*2,from.y)); tick(app,2);
        require(!app.document().editing() && x(app,actor)==original && app.document().undo_count()==before_history && GetCapture()!=app.window(),
            "Dismissed native menu resumed a stale drag or changed history.");
        RECT host{}; GetWindowRect(app.window(),&host);
        require(SetCursorPos(host.left+32,host.top+12)!=FALSE,"Cannot park menu-transition pointer."); tick(app,2);
    }
    click(app,field); key(app,VK_ESCAPE); key(app,VK_F10);
    require(app.menu_frame() && app.menu_frame()->find(control::exit),"F10 did not open File.");
    const auto native_popup=app.menu_window();
    require(native_popup!=app.window() && GetWindow(native_popup,GW_OWNER)==app.window() &&
        (GetWindowLongPtrW(native_popup,GWL_STYLE)&WS_POPUP) && !(GetWindowLongPtrW(native_popup,GWL_STYLE)&WS_CHILD),
        "Dropdown is not an independently owned native popup window.");
    require(GetFocus()==app.window() && GetCapture()==app.window(),"Menu stole owner focus or lost capture.");
    key(app,VK_RIGHT);
    require(app.menu_frame()->find(control::undo),"Right arrow did not switch to Edit.");
    key(app,VK_RIGHT);
    require(app.menu_frame()->find(control::add_actor) && app.menu_window()==native_popup,"Menu switching rebuilt its native host.");
    mouse(app,WM_MOUSEMOVE,centre(app,control::view_menu));
    require(app.menu_frame()->find(control::reset),"Menubar hover did not switch the open dropdown.");
    atelier::renderer::Capture menu_pixels; app.capture_menu(menu_pixels);
    require(menu_pixels.extent.width>0 && !menu_pixels.rgba.empty(),"Native menu did not render through the GPU.");
    png(menu_pixels,output/"native-menu-view.png");
    key(app,VK_ESCAPE);
    if (app.menu_window() || !app.wants_keyboard() || GetCapture()==app.window())
        throw std::runtime_error("Menu dismissal: visible="+std::to_string(app.menu_window()!=nullptr)+
            " field focus="+std::to_string(app.wants_keyboard())+" capture="+std::to_string(GetCapture()==app.window())+".");
    {
        KeyStateScope modifiers(false,false,true);
        SendMessageW(app.window(),WM_SYSKEYDOWN,'W',LPARAM{1}<<29);
        SendMessageW(app.window(),WM_SYSKEYUP,'W',LPARAM{1}<<29); tick(app,2);
    }
    require(app.menu_frame() && app.menu_frame()->find(control::reset_layout),"Alt+W did not open Window.");
    key(app,VK_ESCAPE);
    RECT previous{}; GetWindowRect(app.window(),&previous);
    MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
    require(GetMonitorInfoW(MonitorFromWindow(app.window(),MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE,"Cannot query menu acceptance monitor.");
    const auto area=monitor.rcWork; const auto dpi=app.presented()->drawing->dpi_scale;
    SetWindowPos(app.window(),nullptr,area.left+12,area.top+12,static_cast<int>(1000*dpi),static_cast<int>(650*dpi),SWP_NOZORDER|SWP_NOACTIVATE);
    tick(app,4);
    auto point=centre(app,field);
    point.x=static_cast<LONG>(app.presented()->find(field)->bounds.x+app.presented()->find(field)->bounds.width-3);
    const auto context_at=[&] {
        require_input_focus(app); KeyStateScope modifiers;
        SendMessageW(app.window(),WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(point.x,point.y));
        SendMessageW(app.window(),WM_RBUTTONUP,0,MAKELPARAM(point.x,point.y)); tick(app,2);
    };
    context_at();
    RECT owner{},popup{}; GetWindowRect(app.window(),&owner); GetWindowRect(app.menu_window(),&popup);
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
    click(app,control::edit_menu); SendMessageW(app.window(),WM_CANCELMODE,0,0); tick(app,2);
    require(!app.menu_window() && GetCapture()!=app.window(),"Native cancel mode left the menu alive.");
    click(app,control::create_menu); SetFocus(nullptr); tick(app,2); SetFocus(app.window()); tick(app,2);
    require(!app.menu_window() && GetCapture()!=app.window(),"Owner focus loss left an interactive popup.");
    capture(app,output/"native-menubar.png");
}
