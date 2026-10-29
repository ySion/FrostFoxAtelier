void verify_viewport_edit_window(Application& app,const std::filesystem::path& output) {
    using Access=atelier::application::EditorAcceptanceAccess;
    using atelier::editor::Command;
    const auto position=[&](atelier::ObjectId entity) {
        return model(atelier::world::read_transform(app.document().world().runtime(),
            entity)).position;
    };
    const auto hover=[&](atelier::math::Vector3 at) {Access::move_pointer(app,Access::viewport_point(app,at));tick(app,1);};
    const auto digits=[&](const wchar_t* text) {
        require_input_focus(app);KeyStateScope modifiers;
        while(*text) SendMessageW(app.window(),WM_CHAR,static_cast<WPARAM>(*text++),0);tick(app,1);
    };
    click(app,control::add_static_mesh);const auto first=app.document().selection().entity;
    click(app,property_control(app,first,0));type(app,L"-1.5");key(app,VK_RETURN);
    click(app,control::add_static_mesh);const auto second=app.document().selection().entity;
    click(app,property_control(app,second,0));type(app,L"1.5");key(app,VK_RETURN);
    click(app,static_cast<ControlKey>(Command::tool_select));
    mouse(app,WM_RBUTTONDOWN,Access::viewport_point(app,position(first)));mouse(app,WM_RBUTTONUP,Access::viewport_point(app,position(first)));
    {
        auto point=Access::viewport_point(app,position(second));Access::move_pointer(app,point);
        KeyStateScope modifiers(false,true);
        SendMessageW(app.window(),WM_RBUTTONDOWN,MK_RBUTTON|MK_SHIFT,MAKELPARAM(point.x,point.y));
        SendMessageW(app.window(),WM_RBUTTONUP,MK_SHIFT,MAKELPARAM(point.x,point.y));tick(app,2);
    }
    require(app.document().selected_entities().size()==2,"Native Shift click lost multi-selection.");
    click(app,static_cast<ControlKey>(Command::tool_transform));
    require(app.frame_statistics().overlay_shapes>100 && app.frame_statistics().overlay_draw_calls==1,
        "Universal handle was not rendered in one clipped overlay batch.");
    capture(app,output/"multi-selection.png");
    const auto history=app.document().undo_count();hover({0,0,0});
    key(app,'G');key(app,'X');digits(L"2");
    require(std::abs(x(app,first)-.5f)<.0001f && std::abs(x(app,second)-3.5f)<.0001f && app.document().undo_count()==history,
        "Native modal preview failed to update both Entities and parameters.");
    key(app,VK_RETURN);require(app.document().undo_count()==history+1,"Modal confirmation was not one undo.");
    key(app,'Z',true);require(x(app,first)==-1.5f && x(app,second)==1.5f,"Native transform undo failed.");
    key(app,'Y',true);key(app,'Z',true);
    hover({0,0,0});key(app,'R');key(app,'Y');digits(L"90");key(app,VK_ESCAPE);
    require(x(app,first)==-1.5f && x(app,second)==1.5f && !app.document().editing(),"Esc failed to cancel native rotation.");
    key(app,'S');digits(L"2");key(app,VK_RETURN);key(app,'Z',true);
    click(app,static_cast<ControlKey>(Command::tool_move));
    auto center=Access::viewport_point(app,{0,0,0});const auto drag_history=app.document().undo_count();
    mouse(app,WM_LBUTTONDOWN,center);auto moved=center;moved.x+=36;moved.y-=12;mouse(app,WM_MOUSEMOVE,moved);mouse(app,WM_LBUTTONUP,moved);
    require(app.document().undo_count()==drag_history+1 && position(first)!=atelier::math::Vector3{-1.5f,0,0},"Pointer handle drag did not commit one group transform.");
    key(app,'Z',true);
    hover({0,0,0});key(app,'G');key(app,'X');digits(L"3");
    SendMessageW(app.window(),WM_CANCELMODE,0,0);tick(app,2);
    require(x(app,first)==-1.5f && x(app,second)==1.5f && !app.document().editing(),"Native capture cancellation retained a viewport draft.");
    hover({0,0,0});key(app,'G');key(app,'X');digits(L"1");key(app,'Z',true);
    require(x(app,second)==0 && !app.document().editing(),"Ctrl+Z during a transform became an axis key or kept a draft.");
    key(app,'Y',true);
    const auto root=second;
    const auto rotation=app.field_control(root,app.document().types().rotation_fields[0]);
    click(app,rotation);type(app,L"45");key(app,VK_RETURN);
    const auto transformed=model(atelier::world::read_transform(app.document().world().runtime(),root));
    require(std::abs(atelier::editor::euler_degrees(transformed.rotation).x-45)<.001f,
        "Angle field edited quaternion X instead of the complete rotation.");
    // Resolve a declared grid color through the material binding, not fixed component reflection.
    const auto mesh_component=component(app,root,app.document().presentation_types().static_mesh_component);
    auto& document=Access::physics_document(app);
    const auto material=model(document.create_scene_material(mesh_component,"slot/0"));
    model(document.begin_material_edit(material.id(),881002));
    model(document.preview_material_entry(881002,{"light",atelier::math::Vector3{.19f,.32f,.46f},true}));
    model(document.commit_material_edit(881002));tick(app,2);
    const auto color_key=Access::material_control(app,material.id(),"light");
    require(color_key!=0,"Scene MI light color has no declared numeric field.");
    const auto color_history=app.document().undo_count();
    click(app,color_key);key(app,VK_RETURN);
    require(app.document().undo_count()==color_history,"Unchanged float input created an edit history entry.");
    require(std::ranges::any_of(app.presented()->text_geometry,[&](const auto& text){return text.key==color_key && text.paragraph->matches(L"0.19");}),
        "Float parameter leaked binary conversion digits.");
    capture(app,output/"material-parameters.png");
    const auto rotation_y=app.field_control(root,app.document().types().rotation_fields[1]);
    click(app,rotation_y);type(app,L"120");key(app,VK_RETURN);
    const auto rotated=model(atelier::world::read_transform(app.document().world().runtime(),root)).rotation;
    const auto expected=atelier::editor::from_euler_degrees({45,120,0});
    require(std::abs(rotated.x*expected.x+rotated.y*expected.y+rotated.z*expected.z+rotated.w*expected.w)>.99999f,
        "Euler preview fed its normalized result into the next typed digit.");
    key(app,'Z',true);
    require(app.presented()->find_text(rotation)->paragraph->matches(L"45"),"Euler conversion exposed avoidable float intermediate noise.");
    capture(app,output/"transform-parameters.png");key(app,'Z',true);
    click(app,static_cast<ControlKey>(Command::tool_select));
    const auto bounds=app.presented()->find(control::viewport)->bounds;
    POINT start{static_cast<LONG>(bounds.x+3),static_cast<LONG>(bounds.y+3)};
    POINT end{static_cast<LONG>(bounds.x+bounds.width-4),static_cast<LONG>(bounds.y+bounds.height-4)};
    mouse(app,WM_RBUTTONDOWN,start);mouse(app,WM_MOUSEMOVE,end);capture(app,output/"box-selection.png");mouse(app,WM_RBUTTONUP,end);
    require(app.document().selected_entities().size()==2,"Native box selection missed Entities.");
    key(app,VK_DELETE);require(entities(app).empty(),"Native delete did not delete the selected group.");key(app,'Z',true);
    require(entities(app).size()==2,"Undo did not restore the deleted Entity group.");
    click(app,Access::asset_key(app,"builtin/cube"));
    capture(app,output/"resource-parameters.png");
    require(Access::ui_error(app).empty(),"Resource inspector produced a rejected UI candidate.");
    require(!app.presented()->find(property_control(app,app.document().selection().entity,0)) &&
        Access::inspecting_resource(app,"builtin/cube"),"Resource selection did not replace instance inspection.");
    click(app,app.object_control(first));
    require(app.presented()->find(property_control(app,first,0)),"Scene Tree did not restore instance inspection.");
    capture(app,output/"edit-mode.png");
    const auto selection_before=app.document().selection();const auto cursor_history=app.document().undo_count();
    click_at(app,Access::viewport_point(app,position(second)));
    const auto cursor=Access::cursor_position(app);
    require(cursor!=atelier::math::Vector3{} && app.document().selection()==selection_before && app.document().undo_count()==cursor_history,
        "Left click failed to place the cursor independently of Entity selection/history.");
    capture(app,output/"cursor-placement.png");
    const auto camera_before=Access::camera_view(app);
    mouse(app,WM_RBUTTONDOWN,Access::viewport_point(app,position(second)));mouse(app,WM_RBUTTONUP,Access::viewport_point(app,position(second)));
    require(app.document().selection().entity==second && Access::cursor_position(app)==cursor && Access::camera_view(app)==camera_before,
        "Right selection moved the cursor or orbited the camera.");
    hover(position(second));key(app,'W');
    require(app.menu_window()!=nullptr && Access::menu_item(app,L"Snap")!=0,"W did not open the viewport context menu.");
    capture(app,output/"viewport-menu.png");key(app,VK_ESCAPE);
    click(app,control::viewport_camera);
    require(Access::cursor_position(app)==cursor,"The viewport overlay menu click leaked into cursor placement.");
    click(app,static_cast<ControlKey>(Command::view_front));
    require(Access::camera_mode(app)==atelier::editor::CameraView::front,"Front view command did not change camera projection.");
    click(app,control::viewport_snap);click(app,static_cast<ControlKey>(Command::snap_grid_1));
    click(app,static_cast<ControlKey>(Command::snap_toggle));
    require(Access::viewport_settings(app).grid_snap && Access::viewport_settings(app).grid_spacing==1,"Grid controls lost their shared settings.");
    hover(position(second));key(app,'G');key(app,'X');
    Access::move_pointer(app,Access::viewport_point(app,{2.23f,0,0}));tick(app,2);
    require(std::abs(x(app,second)-2)<.001f,"Native grid drag did not align the active Entity to a world cell.");
    key(app,VK_RETURN);capture(app,output/"front-grid-snapping.png");key(app,'Z',true);
    require(std::abs(x(app,second)-1.5f)<.001f,"Grid drag did not undo as one edit.");
    {
        // Native Alt chords arrive as WM_SYSKEYDOWN and must reach the viewport, not the system menu.
        hover(position(second));
        const auto system_key=[&](WPARAM code) {
            require_input_focus(app);KeyStateScope modifiers(false,false,true);
            SendMessageW(app.window(),WM_SYSKEYDOWN,code,1u<<29);tick(app,1);
            SendMessageW(app.window(),WM_SYSKEYUP,code,(1u<<29)|(3u<<30));tick(app,1);
        };
        const auto chord_history=app.document().undo_count();
        system_key('G');
        require(x(app,second)==0 && app.document().undo_count()==chord_history+1 && !app.menu_window(),"Native Alt+G did not clear location as one edit.");
        key(app,'Z',true);require(std::abs(x(app,second)-1.5f)<.001f,"Alt+G did not undo.");
        system_key('A');require(app.document().selected_entities().empty(),"Native Alt+A did not deselect all.");
        click(app,static_cast<ControlKey>(Command::tool_select));
        mouse(app,WM_RBUTTONDOWN,Access::viewport_point(app,position(second)));mouse(app,WM_RBUTTONUP,Access::viewport_point(app,position(second)));
        // A modal gesture swaps the tool options for Blender's live header readout.
        hover({0,0,0});key(app,'G');key(app,'Z');
        const auto* readout=app.presented()->find(Access::viewport_readout_key(app));
        require(readout && readout->bounds.width>0,"Modal readout band is not visible during G.");
        capture(app,output/"modal-readout.png");key(app,VK_ESCAPE);
        require(std::abs(x(app,second)-1.5f)<.001f && !app.document().editing(),"Esc after readout capture left a draft.");
    }
    const auto expanded_height=app.presented()->find(control::viewport)->bounds.height;
    click(app,static_cast<ControlKey>(Command::viewport_options));
    require(app.presented()->find(control::viewport)->bounds.height>expanded_height,"Collapsing tool options did not return height to the viewport.");
    click(app,static_cast<ControlKey>(Command::viewport_options));
    require(Access::ui_error(app).empty(),"Viewport chrome produced a rejected DSL/layout candidate.");
    auto orbit=centre(app,control::viewport);const auto orbit_before=Access::camera_view(app);
    mouse(app,WM_MBUTTONDOWN,orbit);orbit.x+=32;mouse(app,WM_MOUSEMOVE,orbit);mouse(app,WM_MBUTTONUP,orbit);
    require(Access::camera_view(app)!=orbit_before && Access::camera_mode(app)==atelier::editor::CameraView::perspective,
        "Middle mouse orbit did not leave the axis view.");
}
