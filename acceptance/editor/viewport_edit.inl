void verify_viewport_editing() {
    using namespace editor;
    Session document;
    const auto create=[&](math::Vector3 at) {changed(document.create_static_mesh(mesh_assets().front(),at));return document.selection().actor;};
    const auto first=create({-1.5f,0,0}),second=create({1.5f,0,0});
    const auto root=[&](ObjectId actor){return *take(world::root_component(document.world().runtime(),actor));};
    const auto transform=[&](ObjectId actor){return take(world::read_transform(document.world().runtime(),root(actor)));};
    const auto first_root=root(first),second_root=root(second);
    Camera camera;const auto view=ViewportView::make(camera,{30,40,1000,700},1.5f);
    ViewportTool tool;ViewportTool::Settings settings{TransformTool::select,PivotMode::median};
    const auto point=[&](ObjectId actor){return *view.project(transform(actor).position);};
    const auto pointer=[&](ui::EventKind kind,math::Vector2 p,bool shift=false,bool control=false,ui::PointerButton button=ui::PointerButton::secondary) {
        ui::Event event;event.kind=kind;event.x=p.x;event.y=p.y;event.shift=shift;event.control=control;event.button=button;
        return tool.pointer(document,view,event,settings);
    };
    const auto key=[&](std::uint32_t code,ui::Key key=ui::Key::none) {
        ui::Event event;event.kind=ui::EventKind::key_down;event.key=key;return tool.key(document,view,event,code,settings,point(second));
    };
    const auto text=[&](std::wstring value) {
        ui::Event event;event.kind=ui::EventKind::text;event.data=std::move(value);return tool.key(document,view,event,0,settings,point(second));
    };
    require(tool.pick(document,view,point(first).x,point(first).y)==first,"Viewport ray did not hit the actual first mesh.");
    require(tool.pick(document,view,point(second).x,point(second).y)==second,"Viewport ray did not hit the actual second mesh.");
    const auto before_cursor_history=document.undo_count();
    take(pointer(ui::EventKind::pointer_down,point(first),false,false,ui::PointerButton::primary));
    require(document.selection().actor==second && document.undo_count()==before_cursor_history && tool.cursor!=math::Vector3{},
        "Left click selected an Actor or failed to place the cursor on actual geometry.");
    take(pointer(ui::EventKind::pointer_down,point(first)));take(pointer(ui::EventKind::pointer_up,point(first)));
    take(pointer(ui::EventKind::pointer_down,point(second),true));take(pointer(ui::EventKind::pointer_up,point(second),true));
    require(document.selected_actors().size()==2 && document.selection().actor==second,"Shift click did not preserve and extend the Actor selection.");
    const auto history=document.undo_count();const auto before_first=transform(first),before_second=transform(second);
    take(key('G'));take(key('X'));take(text(L"2"));
    require(std::abs(transform(first).position.x-before_first.position.x-2)<.0001f &&
        std::abs(transform(second).position.x-before_second.position.x-2)<.0001f && document.undo_count()==history,
        "Modal move did not preview the whole selection without history.");
    require(!document.serialize() && !document.start(),"An active viewport draft could be saved or played.");
    take(key(13,ui::Key::enter));require(document.undo_count()==history+1 && !tool.active(),"One move gesture did not produce one history entry.");
    changed(document.undo());require(transform(first)==before_first && transform(second)==before_second && document.selected_actors().size()==2,
        "Transform undo lost a member, selection or original TRS.");
    changed(document.redo());changed(document.undo());
    settings.grid_spacing=1;settings.grid_snap=true;
    take(key('G'));take(key('X'));
    const auto target=*view.project(before_second.position+math::Vector3{.31f,0,0});
    take(pointer(ui::EventKind::pointer_move,target));
    require(std::abs(transform(second).position.x-2)<.0001f && std::abs(transform(first).position.x+1)<.0001f,
        "Grid movement did not align the active origin while preserving group offsets.");
    take(pointer(ui::EventKind::pointer_move,target,false,true));
    require(std::abs(transform(second).position.x-1.81f)<.001f,"Ctrl did not temporarily invert grid snapping.");
    take(key(27,ui::Key::escape));
    take(tool.snap_selection(document,settings,false));
    require(transform(first).position.x==-2 && transform(second).position.x==2,"Selection to Grid did not align each root.");
    changed(document.undo());settings.grid_snap=false;settings.grid_spacing=.2f;
    take(key('G'));take(key('X'));
    take(text(L"-"));
    require(!key(13,ui::Key::enter) && tool.active(),"Incomplete typed transform was committed.");
    take(key(27,ui::Key::escape));
    take(key('G'));take(key('X'));
    require(!text(L"10001") && !key(13,ui::Key::enter) && tool.active() && transform(first)==before_first && transform(second)==before_second,
        "Out-of-range preview was committed or partially applied.");
    take(key(27,ui::Key::escape));
    take(key('R'));take(key('Y'));take(text(L"90"));take(key(27,ui::Key::escape));
    require(transform(first)==before_first && transform(second)==before_second && !document.editing(),"Rotation cancellation left a preview behind.");
    take(key('S'));take(text(L"2"));take(key(13,ui::Key::enter));
    require(transform(first).scale==math::Vector3{2,2,2} && transform(second).scale==math::Vector3{2,2,2},"Modal scale failed to update the group.");
    changed(document.undo());
    const std::array roots{first_root,second_root};take(document.begin_transform(roots,918));
    std::array values{before_first,before_second};values[0].position.x=42;values[1].position.x=std::numeric_limits<float>::quiet_NaN();
    require(!document.preview_transform(918,values) && transform(first)==before_first && transform(second)==before_second,
        "Rejected group preview partially changed earlier objects.");
    require(!document.preview_edit(918,2.f),"Scalar preview accepted a transform transaction token.");
    take(document.cancel_edit());
    take(document.select(second_root));const std::array one_root{second_root};take(document.begin_transform(one_root,919));
    auto turned=before_second;turned.rotation=from_euler_degrees({45,120,0});const std::array turn{turned};
    take(document.preview_transform(919,turn));
    require(take(document.original_transform(919,second_root))==before_second && !document.original_transform(918,second_root),
        "Transform origin changed during preview or accepted a stale token.");
    take(document.commit_edit(919));changed(document.undo());
    require(document.selection().object==second_root && transform(second)==before_second,"Euler history lost the inspected Component or its transform.");
    take(pointer(ui::EventKind::pointer_down,{view.bounds.x+1,view.bounds.y+1}));
    take(pointer(ui::EventKind::pointer_move,{view.bounds.x+view.bounds.width-1,view.bounds.y+view.bounds.height-1}));
    std::vector<renderer::OverlayShape> shapes;tool.overlay(document,view,settings,shapes);
    require(std::ranges::any_of(shapes,[](const auto& shape){return shape.kind==renderer::OverlayShape::Kind::rectangle;}),"Box selection has no visible accepted geometry.");
    take(pointer(ui::EventKind::pointer_up,{view.bounds.x+view.bounds.width-1,view.bounds.y+view.bounds.height-1}));
    require(document.selected_actors().size()==2,"Box selection missed real projected geometry.");
    const auto deletion_history=document.undo_count();changed(document.erase_selected());
    require(take(document.world().members()).empty() && document.undo_count()==deletion_history+1,"Multi-delete was not one document operation.");
    changed(document.undo());
    require(take(document.world().members()).size()==2 && document.is_selected(first) && document.is_selected(second) && document.selection().actor==second,
        "Multi-delete undo lost Actor identity, active member or membership.");
    changed(document.redo());require(document.selected_actors().empty(),"Delete redo retained a stale selection.");changed(document.undo());
    const math::Vector3 angles{23,47,-12};const auto converted=euler_degrees(from_euler_degrees(angles));
    require(std::abs(converted.x-angles.x)<.001f && std::abs(converted.y-angles.y)<.001f && std::abs(converted.z-angles.z)<.001f,
        "Euler parameter conversion disagrees with native XYZ rotation.");
    for(const auto preset:{CameraView::top,CameraView::front,CameraView::right}) {
        camera.set_view(preset);const auto orthographic=ViewportView::make(camera,view.bounds,view.dpi_scale);
        const auto a=orthographic.ray(400,300),b=orthographic.ray(420,300);
        require(orthographic.orthographic && math::dot(a.direction,b.direction)>.99999f && a.origin!=b.origin,
            "Orthographic camera rays are not parallel and position-dependent.");
        require(orthographic.pixel_size({})==orthographic.pixel_size({10,20,30}),"Orthographic handle size changed with depth.");
        const auto center=orthographic.project(transform(first).position);
        require(center && tool.pick(document,orthographic,center->x,center->y).has_value(),"Orthographic picking lost real geometry.");
    }
    settings.tool=TransformTool::transform;shapes.clear();tool.overlay(document,view,settings,shapes);
    require(shapes.size()>100,"Universal handle did not expose axes, planes and rotation rings.");
    Workspace workspace;
    const std::array fonts{(editor::asset_directory()/"fonts/SarasaUiSC-Regular.ttf").wstring()};ui::Session ui(fonts);
    const auto theme=ui::StyleSheet::parse(take(editor::read_file(editor::asset_directory()/"editor.ss")));
    if(!theme) throw std::runtime_error("Viewport UI theme failed at "+std::to_string(theme.error().line)+": "+theme.error().message);
    require(bool(ui.set_style_sheet(*theme)),"Viewport UI theme could not be applied.");
    require(bool(ui.set_surface({{1200,800},1,1})),"Viewport UI surface failed.");
    const auto controls=workspace.describe(document,{1200,800},nullptr);
    require(bool(ui.set_controls(controls)),"Viewport/parameter DSL could not be published.");
    const auto frame=ui.prepare();
    if(!frame) throw std::runtime_error("Viewport/parameter layout failed at "+std::to_string(frame.error().key)+" code "+std::to_string(int(frame.error().code)));
    require(bool(ui.accept(*frame)),"Viewport/parameter frame was refused.");
    for(const auto& paths:{document.types().position_fields,document.types().scale_fields}) {
        const auto* a=ui.presented()->find(workspace.field_control(root(document.selection().actor),paths[0]));
        const auto* b=ui.presented()->find(workspace.field_control(root(document.selection().actor),paths[1]));
        const auto* c=ui.presented()->find(workspace.field_control(root(document.selection().actor),paths[2]));
        require(a && b && c && a->bounds.y==b->bounds.y && b->bounds.y==c->bounds.y,"A Vector3 was split across parameter rows.");
    }
    const auto* rotation_x=ui.presented()->find(workspace.field_control(root(document.selection().actor),document.types().rotation_fields[0]));
    require(rotation_x && !ui.presented()->find(workspace.field_control(root(document.selection().actor),document.types().rotation_fields[3])),
        "Scene rotation exposed quaternion W instead of a three-angle editor.");
    workspace.commit_ui_controls();
    const auto hud=ui.presented()->find(control::viewport_hud)->bounds;
    const auto camera_menu=ui.presented()->find(control::viewport_camera)->bounds;
    require(ui.pointer_target(hud.x+8,hud.y+8)==control::viewport &&
        ui.pointer_target(camera_menu.x+camera_menu.width*.5f,camera_menu.y+camera_menu.height*.5f)==control::viewport_camera,
        "Viewport chrome did not distinguish passive mode text from the interactive camera menu.");
    const auto expanded_height=ui.presented()->find(control::viewport)->bounds.height;
    const auto publish=[&] {
        auto controls=workspace.describe(document,{1200,800},ui.presented().get());
        require(bool(ui.set_controls(controls)),"Updated viewport chrome was refused.");
        const auto frame=ui.prepare();require(frame && ui.accept(*frame),"Updated viewport chrome did not lay out.");
        workspace.commit_ui_controls();return controls;
    };
    workspace.viewport_options_collapsed=true;publish();
    require(std::abs(ui.presented()->find(control::viewport)->bounds.height-expanded_height-20)<.1f,
        "Collapsed viewport options did not return their height to the scene.");
    workspace.viewport_options_collapsed=false;workspace.viewport_mode=TransformTool::rotate;
    const auto rotated_controls=publish();
    require(std::ranges::any_of(rotated_controls,[](const auto& control){return control.key==editor::control::viewport_hud && control.text==L"Rotate";}),
        "The authored mode label did not follow the current operation.");
    workspace.menu=ui::ContextMenuRequest{control::viewport_snap,0,0,0};publish();
    const auto step=std::ranges::find(workspace.menu_items,static_cast<ui::ControlKey>(Command::snap_grid_02),&ui::MenuItem::key);
    require(step!=workspace.menu_items.end() && step->selected && step->behavior==ui::MenuBehavior::radio &&
        std::ranges::any_of(workspace.menu_items,[](const auto& item){return item.key==static_cast<ui::ControlKey>(Command::snap_toggle) && item.behavior==ui::MenuBehavior::checkbox;}),
        "The shared snap menu lost its active step or toggle semantics.");
    workspace.menu.reset();publish();
    require(workspace.assets.apply({ui::ActionKind::select,browser_control::results,
        ui::TableSelection{workspace.assets.row_key("builtin/cube"),browser_control::name_column}}),"Resource selection failed.");
    workspace.inspecting_asset=true;
    const auto resource_controls=workspace.describe(document,{1200,800},ui.presented().get());
    const auto resource_submission=ui.set_controls(resource_controls);
    if(!resource_submission) throw std::runtime_error("Resource controls failed at "+std::to_string(resource_submission.error().key)+" code "+std::to_string(int(resource_submission.error().code)));
    const auto resource_frame=ui.prepare();
    require(resource_frame && ui.accept(*resource_frame),"Resource inspector layout failed.");
    require(workspace.ui_error().empty() && std::ranges::any_of(resource_controls,[](const auto& control){return control.text==L"builtin/cube";}),
        "Resource inspection did not publish resource metadata.");
    std::puts("Viewport editing acceptance: PASS (cursor/right select, world grid snapping, orthographic rays, mode chrome/hit routing, multi/box selection, G/R/S, atomic edits, history, Euler, vector/resource UI)");
}
