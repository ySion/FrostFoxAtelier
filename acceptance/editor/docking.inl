void verify_docking(Application& app,const std::filesystem::path& output,atelier::ObjectId actor) {
    using atelier::ui::DockSpace;
    using atelier::ui::DragAxis;
    namespace sample=atelier::editor::workbench_control;
    const auto dpi=app.presented()->drawing->dpi_scale;
    SetWindowPos(app.window(),nullptr,0,0,static_cast<int>(1280*dpi),static_cast<int>(840*dpi),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    tick(app,3);
    click(app,control::reset_layout);
    const auto field=property_control(app,actor,0);
    const auto value=x(app,actor); const auto history=app.document().undo_count();
    const auto parameter_pane=app.docking().pane(control::parameters_tab);
    click(app,control::workbench);
    click(app,sample::position_x); type(app,L"2"); key(app,VK_RETURN);
    const auto sample_history=app.control_workbench().undo_count();
    const auto drag_to=[&](ControlKey source,POINT target) {
        mouse(app,WM_LBUTTONDOWN,centre(app,source));
        mouse(app,WM_MOUSEMOVE,target);
        mouse(app,WM_LBUTTONUP,target);
    };
    const auto scene=app.docking().pane(control::scene_tab);
    const auto* scene_box=app.presented()->find(scene);
    POINT target{static_cast<LONG>(scene_box->bounds.x+scene_box->bounds.width*.5f),
                 static_cast<LONG>(scene_box->bounds.y+scene_box->bounds.height-8*dpi)};
    mouse(app,WM_LBUTTONDOWN,centre(app,control::workbench)); mouse(app,WM_MOUSEMOVE,target);
    require(app.docking().dragging(),"Tab drag did not start through UI capture.");
    capture(app,output/"workspace-dock-preview.png");
    mouse(app,WM_LBUTTONUP,target);
    require(!app.docking().dragging() && app.docking().pane(control::workbench)!=parameter_pane &&
            app.presented()->find(control::viewport) && app.presented()->find(sample::position_x),"Edge drop did not split the pane.");
    require(property_control(app,actor,0)==field && x(app,actor)==value && app.document().undo_count()==history &&
            app.control_workbench().values().position[0]==2,"Docking changed document state or field binding.");
    const auto new_pane=app.docking().pane(control::workbench);
    auto divider=centre(app,app.docking().divider(control::workbench,DragAxis::vertical));
    const auto old_height=app.presented()->find(new_pane)->bounds.height;
    mouse(app,WM_LBUTTONDOWN,divider); divider.y-=35; mouse(app,WM_MOUSEMOVE,divider); mouse(app,WM_LBUTTONUP,divider);
    require(app.presented()->find(new_pane)->bounds.height>old_height+20,"Horizontal pane divider failed.");
    const auto before=app.presented()->find(new_pane)->bounds;
    click(app,DockSpace::maximize_button(new_pane));
    require(!app.presented()->find(control::viewport) && app.presented()->find(sample::position_x),"Maximize left another pane visible.");
    capture(app,output/"workspace-maximized.png");
    key(app,'B',true);
    require(app.presented()->find(control::viewport) && app.presented()->find(new_pane)->bounds==before,"Restore lost pane geometry.");
    const auto* scene_tab=app.presented()->find(control::scene_tab);
    target={static_cast<LONG>(scene_tab->bounds.x+scene_tab->bounds.width*.5f),static_cast<LONG>(scene_tab->bounds.y+8*dpi)};
    mouse(app,WM_LBUTTONDOWN,centre(app,control::workbench)); mouse(app,WM_MOUSEMOVE,target);
    key(app,VK_ESCAPE); mouse(app,WM_LBUTTONUP,target);
    require(app.docking().pane(control::workbench)==new_pane && !app.docking().dragging() && GetCapture()!=app.window(),"Escape did not cancel tab docking.");
    drag_to(control::workbench,target);
    require(app.docking().pane(control::workbench)==app.docking().pane(control::scene_tab) &&
            !app.presented()->find(control::viewport),"Dropping on tab strip did not merge/select the tab.");
    const auto* first_tab=app.presented()->find(control::scene_tab);
    target={static_cast<LONG>(first_tab->bounds.x+2*dpi),static_cast<LONG>(first_tab->bounds.y+8*dpi)};
    drag_to(control::workbench,target);
    require(app.presented()->find(control::workbench)->bounds.x<app.presented()->find(control::scene_tab)->bounds.x,
            "Tab strip drop did not reorder before the target tab.");
    key(app,VK_TAB,true); // Cycle back to Scene, keeping both view instances.
    require(app.presented()->find(control::viewport),"Ctrl+Tab did not cycle pane tabs.");
    click(app,control::workbench);
    key(app,'W',true);
    require(!app.docking().pane(control::workbench) && app.presented()->find(control::viewport),"Close did not normalize the tab group.");
    click(app,DockSpace::menu_button(app.docking().pane(control::scene_tab)));
    key(app,VK_HOME); key(app,VK_DOWN); key(app,VK_DOWN); key(app,VK_RETURN);
    require(app.workbench_visible() && app.control_workbench().values().position[0]==2 &&
            app.control_workbench().undo_count()==sample_history,"Reopening lost the view's state or history.");
    capture(app,output/"workspace-tabs.png");
    click(app,sample::position_x); key(app,VK_ESCAPE); key(app,'Z',true);
    require(app.control_workbench().values().position[0]==0,"Reopened sample could not undo its edit.");
    click(app,control::reset_layout);
    require(app.presented()->find(control::viewport) && app.presented()->find(field) && !app.workbench_visible() &&
            app.document().undo_count()==history,"Default layout restoration changed document state.");
    capture(app,output/"workspace-default.png");
    for (auto view:{control::workbench,control::parameters_tab,control::actors_tab,control::scene_tab}) {
        click(app,view); key(app,'W',true);
    }
    require(!app.presented()->find(control::viewport) && !app.presented()->find(field),"Closing the final pane left stale content.");
    click(app,control::reset_layout);
    require(app.presented()->find(field) && app.presented()->find(control::viewport),"Empty workspace could not restore its views.");
}
