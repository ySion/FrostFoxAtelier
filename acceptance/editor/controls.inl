void parameter_menu(Application& app, ControlKey source) {
    require_input_focus(app);
    KeyStateScope modifiers;
    const auto at=centre(app,source);
    SendMessageW(app.window(),WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(at.x,at.y));
    SendMessageW(app.window(),WM_RBUTTONUP,0,MAKELPARAM(at.x,at.y));
    tick(app,2);
    require(app.presented()->popup==control::parameter_menu,"Parameter menu did not enter the accepted frame.");
}
bool tuple_reset_available(Application& app, ControlKey group) {
    parameter_menu(app,group);
    const bool enabled=app.presented()->find(control::menu_group_reset)->enabled;
    key(app,VK_ESCAPE); return enabled;
}
void reset_tuple(Application& app, ControlKey group) {
    parameter_menu(app,group); click(app,control::menu_group_reset);
    require(!app.presented()->popup,"Tuple reset left its popup open.");
}
void verify_basic_controls(Application& app, const std::filesystem::path& output, atelier::ObjectId actor) {
    const auto resize_panel=[&](float width) {
        require_input_focus(app);
        KeyStateScope modifiers;
        const auto from=centre(app,control::splitter);
        auto to=from;
        to.x+=static_cast<LONG>(std::lround((app.parameter_width()-width)*app.presented()->drawing->dpi_scale));
        // This check observes the completed resize. Send the whole gesture
        // before polling unrelated native motion, just as click_at does.
        SendMessageW(app.window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(from.x,from.y));
        SendMessageW(app.window(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(to.x,to.y));
        SendMessageW(app.window(),WM_LBUTTONUP,0,MAKELPARAM(to.x,to.y));
        tick(app,2);
    };
    click(app,control::parameter_search); type(app,L""); key(app,VK_RETURN); key(app,VK_ESCAPE);
    const auto target=model(atelier::world::root_transform(app.document().world().runtime(),actor));
    require(target.has_value(),"Parameter-row acceptance needs an actual component.");
    const auto position=[&](std::size_t axis) {
        return std::get<float>(model(app.document().world().read(*target,app.document().parameters()[axis].path)));
    };
    for (std::size_t axis=0;axis<3;++axis) {
        click(app,property_control(app,actor,axis));
        type(app,std::to_wstring(axis+1).c_str()); key(app,VK_RETURN);
    }
    const auto count=app.document().undo_count();
    const auto reset=app.reset_control(*target);
    require(reset && tuple_reset_available(app,reset),"Modified vector has no reset action.");
    const auto modified=app.presented()->find(property_control(app,actor,0))->style->foreground;
    capture(app,output/"controls-vector-modified.png");
    reset_tuple(app,reset);
    require(position(0)==0 && position(1)==0 && position(2)==0 && app.document().undo_count()==count+1,
        "Vector reset did not apply as one history entry.");
    require(!tuple_reset_available(app,reset) &&
        app.presented()->find(property_control(app,actor,0))->style->foreground!=modified,
        "Restoring defaults did not clear the modified state and reset availability.");
    click(app,control::undo);
    require(position(0)==1 && position(1)==2 && position(2)==3,"Undo failed to restore the whole vector.");
    click(app,control::redo);
    require(position(0)==0 && position(1)==0 && position(2)==0,"Redo failed to reset the whole vector.");

    click(app,property_control(app,actor,1)); type(app,L"5"); key(app,VK_RETURN);
    click(app,control::parameter_search); type(app,L"x"); key(app,VK_RETURN);
    require(!app.presented()->find(property_control(app,actor,1)) && tuple_reset_available(app,reset),
        "Filtering out the only modified component disabled the whole-vector reset.");
    const auto filtered_history=app.document().undo_count();
    reset_tuple(app,reset);
    require(position(0)==0 && position(1)==0 && position(2)==0 && app.document().undo_count()==filtered_history+1,
        "Filtered reset did not restore the hidden component in one transaction.");
    click(app,control::undo);
    require(position(1)==5 && app.query()==L"x" && tuple_reset_available(app,reset),
        "Undo did not restore the hidden component and whole-group modified state.");
    capture(app,output/"controls-filtered-reset.png");
    click(app,control::redo);
    click(app,control::parameter_search); type(app,L""); key(app,VK_RETURN); key(app,VK_ESCAPE);

    const auto stable=property_control(app,actor,0);
    click(app,control::position_fold);
    require(!app.presented()->find(stable) && !app.text_input(),"Collapsed group retained an editable target.");
    key(app,VK_SPACE);
    require(app.presented()->find(stable) && property_control(app,actor,0)==stable,
        "Keyboard expansion changed field identity or failed to restore the group.");

    namespace sample=atelier::editor::workbench_control;
    const auto scene_history=app.document().undo_count();
    click(app,control::workbench);
    require(app.workbench_visible() && app.presented()->find(sample::position_x),"Control workbench is not reachable from the editor.");
    capture(app,output/"controls-workbench-initial.png");
    click(app,sample::position_x); type(app,L"12.5"); key(app,VK_RETURN);
    require(app.control_workbench().values().position[0]==12.5 && app.control_workbench().undo_count()==1,
        "Workbench numeric field did not use the edit transaction.");
    key(app,'Z',true);
    require(app.control_workbench().values().position[0]==0,"Passive sample field did not route undo to sample history.");
    key(app,'Y',true);
    require(app.control_workbench().values().position[0]==12.5,"Sample redo did not restore the value.");
    click(app,sample::name); type(app,L"控件样本 · Unicode"); key(app,VK_RETURN);
    require(app.control_workbench().values().name==L"控件样本 · Unicode","Workbench text did not use the shared Unicode editor.");
    click(app,sample::enabled);
    require(!app.control_workbench().values().enabled,"Checkbox pointer activation did not publish a bool.");
    key(app,VK_SPACE);
    require(app.control_workbench().values().enabled,"Checkbox keyboard activation did not toggle once.");
    key(app,VK_DELETE);
    require(contains(app,actor),"Workbench keyboard command escaped into scene deletion.");
    click(app,sample::disable);
    require(!app.presented()->find(sample::position_x)->enabled,"Disabled group did not propagate to numeric fields.");
    click(app,sample::position_x); type(app,L"999");
    require(app.control_workbench().values().position[0]==12.5,"Disabled sample accepted input.");
    click(app,sample::disable);
    click(app,sample::long_labels);
    const auto width=app.parameter_width();
    resize_panel(180); // Cross the lower bound to verify its clamp.
    require(app.parameter_width()==220,"Narrow workbench did not reach the declared minimum.");
    const auto* first=app.presented()->find(sample::position_x);
    for (const auto field:{sample::position_x,sample::position_y,sample::position_z}) {
        const auto* box=app.presented()->find(field);
        require(box && box->bounds.y==first->bounds.y && box->bounds.width>=24 &&
            box->bounds.x>=box->clip.left && box->bounds.x+box->bounds.width<=box->clip.right,
            "Wrapped vector lost a component or overflowed the narrow panel.");
    }
    capture(app,output/"controls-workbench-narrow.png");
    wheel(app,sample::list,-12);
    const auto* four=app.presented()->find(sample::vector4_x);
    for (const auto field:{sample::vector4_x,sample::vector4_y,sample::vector4_z,sample::vector4_w}) {
        const auto* box=app.presented()->find(field);
        require(box && box->bounds.y==four->bounds.y && box->bounds.width>=24 &&
            box->bounds.x>=box->clip.left && box->bounds.x+box->bounds.width<=box->clip.right &&
            box->bounds.y>=box->clip.top && box->bounds.y+box->bounds.height<=box->clip.bottom,
            "Four-component row was clipped in the minimum-width panel.");
    }
    click(app,sample::vector4_w); type(app,L"2"); key(app,VK_RETURN);
    require(app.control_workbench().values().vector4[3]==2,"Narrow Vector4 last field was not editable.");
    capture(app,output/"controls-workbench-vector4.png");
    click(app,control::undo);
    require(app.control_workbench().values().vector4[3]==1,"Vector4 editing did not share sample history.");
    wheel(app,sample::list,12);
    resize_panel(width);
    click(app,sample::long_labels);
    // Real native messages, clipboard, accepted frames, history and renderer.
    parameter_menu(app,sample::position_x);
    capture(app,output/"controls-parameter-menu.png");
    click(app,control::menu_copy);
    require(!app.presented()->popup,"Copy left its popup open.");
    parameter_menu(app,sample::position_y); click(app,control::menu_paste);
    require(app.control_workbench().values().position[1]==12.5,"Paste Value did not commit the clipboard value.");
    click(app,control::undo);
    require(app.control_workbench().values().position[1]==0,"Menu paste did not produce one undoable edit.");
    parameter_menu(app,sample::position_x); click(app,control::menu_reset);
    require(app.control_workbench().values().position[0]==0,"Scalar default command did not restore its value.");
    click(app,control::undo);
    parameter_menu(app,sample::name); click(app,control::menu_copy);
    parameter_menu(app,sample::position_x); click(app,control::menu_paste);
    require(app.control_workbench().values().position[0]==12.5 && app.text_input(),
        "Invalid numeric paste committed or discarded the editable draft.");
    key(app,VK_ESCAPE);
    parameter_menu(app,sample::position_x);
    key(app,VK_DELETE);
    require(app.presented()->popup && contains(app,actor),"Popup leaked Delete to the scene.");
    const auto view=atelier::application::EditorAcceptanceAccess::camera_view(app);
    wheel(app,control::viewport,3);
    require(atelier::application::EditorAcceptanceAccess::camera_view(app)==view,"Popup wheel escaped to the camera.");
    click(app,control::workbench);
    require(!app.presented()->popup && app.workbench_visible(),"Outside click did not dismiss without activating the toolbar.");
    click(app,sample::position_x); key(app,VK_APPS);
    require(app.presented()->popup,"Keyboard context menu did not open.");
    key(app,VK_ESCAPE);
    require(!app.presented()->popup,"Escape failed to dismiss the menu.");
    key(app,VK_APPS); key(app,VK_ESCAPE); // Owner focus survived dismissal.
    {
        KeyStateScope modifiers(false,true);
        SendMessageW(app.window(),WM_SYSKEYDOWN,VK_F10,0);
        SendMessageW(app.window(),WM_SYSKEYUP,VK_F10,0); tick(app,2);
    }
    require(app.presented()->popup,"Native Shift+F10 did not open the parameter menu.");
    key(app,VK_ESCAPE);
    // The ladder uses one transaction and one cached text paragraph while held.
    const auto drag=[&](bool ctrl,bool shift,bool cancel) {
        require_input_focus(app); KeyStateScope modifiers(ctrl,shift);
        const auto from=centre(app,sample::position_x);
        const LONG dx=static_cast<LONG>(std::lround(10*app.presented()->drawing->dpi_scale));
        SendMessageW(app.window(),WM_MBUTTONDOWN,MK_MBUTTON,MAKELPARAM(from.x,from.y));
        if (!ctrl) SendMessageW(app.window(),WM_MOUSEMOVE,MK_MBUTTON,MAKELPARAM(from.x+dx,from.y));
        if (cancel) SendMessageW(app.window(),WM_KEYDOWN,VK_ESCAPE,0);
        SendMessageW(app.window(),WM_MBUTTONUP,0,MAKELPARAM(from.x+dx,from.y));
        if (cancel) SendMessageW(app.window(),WM_KEYUP,VK_ESCAPE,0);
        tick(app,2);
    };
    const auto history_before=app.control_workbench().undo_count();
    drag(false,false,true);
    require(app.control_workbench().values().position[0]==12.5 && app.control_workbench().undo_count()==history_before,
        "Esc failed to roll back the ladder gesture.");
    drag(false,false,false);
    require(std::abs(app.control_workbench().values().position[0]-12.6)<1e-6 && app.control_workbench().undo_count()==history_before+1,
        "Middle drag did not commit one stepped edit.");
    click(app,control::undo);
    drag(false,true,false);
    require(std::abs(app.control_workbench().values().position[0]-12.51)<1e-6,"Shift ladder did not use fine steps.");
    click(app,control::undo);
    drag(true,false,false);
    require(app.control_workbench().values().position[0]==0,"Ctrl+middle did not restore the default.");
    click(app,control::undo);
    {
        require_input_focus(app); KeyStateScope modifiers;
        const auto from=centre(app,sample::position_x);
        SendMessageW(app.window(),WM_MBUTTONDOWN,MK_MBUTTON,MAKELPARAM(from.x,from.y));
        tick(app,2);
        capture(app,output/"controls-value-ladder.png");
        const auto cached=app.ui_statistics(); tick(app,3);
        require(app.ui_statistics().text_layouts==cached.text_layouts && app.ui_statistics().rasterizations==cached.rasterizations,
            "Held value ladder rebuilt unchanged labels every frame.");
        const LONG dx=static_cast<LONG>(std::lround(10*app.presented()->drawing->dpi_scale));
        const LONG dy=static_cast<LONG>(std::lround(24*app.presented()->drawing->dpi_scale));
        SendMessageW(app.window(),WM_MOUSEMOVE,MK_MBUTTON,MAKELPARAM(from.x+dx,from.y-dy));
        SendMessageW(app.window(),WM_MBUTTONUP,0,MAKELPARAM(from.x+dx,from.y-dy)); tick(app,2);
        require(app.control_workbench().values().position[0]>12.6,"Vertical ladder motion failed to choose a coarser step.");
    }
    click(app,control::undo);
    wheel(app,sample::list,-12);
    click(app,sample::mode);
    require(app.presented()->popup,"Combo did not open its menu.");
    capture(app,output/"controls-combo.png");
    key(app,VK_DOWN); key(app,VK_RETURN);
    require(!app.presented()->popup && app.control_workbench().values().mode==1,"Combo keyboard selection failed.");
    key(app,VK_DOWN); key(app,VK_ESCAPE);
    require(app.control_workbench().values().mode==1,"Combo cancellation changed the selection.");
    click(app,control::undo);
    require(app.control_workbench().values().mode==0,"Combo selection bypassed sample history.");
    wheel(app,sample::list,12);
    click(app,sample::fold);
    require(!app.presented()->find(sample::position_x),"Workbench foldout did not collapse its subtree.");
    key(app,VK_RETURN);
    require(app.presented()->find(sample::position_x),"Workbench foldout did not reopen from keyboard.");
    capture(app,output/"controls-workbench.png");
    click(app,sample::reset);
    require(app.control_workbench().values()==atelier::editor::ControlWorkbench::Values{},"Workbench reset did not restore sample defaults.");
    click(app,control::undo);
    require(app.control_workbench().values().position[0]==12.5,"Sample reset did not have one reversible history entry.");
    require(app.document().undo_count()==scene_history && position(0)==0,
        "Workbench values or history modified the actual scene.");
    click(app,control::workbench);
    require(!app.workbench_visible() && app.presented()->find(property_control(app,actor,0)),
        "Returning from the workbench did not restore scene controls.");
}
