void verify_rotated_viewport_transforms() {
    using namespace editor;
    Session document;
    changed(document.create_light(world::LightKind::directional));
    const auto entity=document.selection().entity;
    const world::Trs authored{{-5.8103158e-9f,2.9999993f,-1.532578e-7f},
        {.761490941f,.141828924f,-.176239014f,.607417405f},{1.f,.999999762f,.999999881f}};
    take(document.set_transform(entity,authored));
    const auto original=take(world::read_transform(document.world().runtime(),entity));
    Camera camera;const auto view=ViewportView::make(camera,{0,0,1000,700},1.5f);
    ViewportTool tool;ToolSettings settings{TransformTool::transform,PivotMode::median};
    const auto key=[&](std::uint32_t code,ui::Key key=ui::Key::none) {
        return tool.key(document,view,{.kind=ui::EventKind::key_down,.key=key},code,settings,{500,350});
    };
    const auto text=[&](std::wstring value) {
        ui::Event event;event.kind=ui::EventKind::text;event.data=std::move(value);
        return tool.key(document,view,event,0,settings,{500,350});
    };
    for(const auto mode:std::array<std::uint32_t,3>{'G','R','S'}) {
        const auto history=document.undo_count();
        take(key(mode));take(key('X'));take(text(mode=='R'?L"15":mode=='S'?L"2":L"1"));
        const auto preview=take(world::read_transform(document.world().runtime(),entity));
        require(preview!=original && document.undo_count()==history,"Rotated Entity did not preview its transform.");
        if(mode=='G') require(std::abs(preview.position.x-original.position.x-1.f)<.0001f,
            "Rotated Entity world-axis move did not reach its authored position.");
        take(key(27,ui::Key::escape));
        require(!tool.active() && !document.editing() &&
            take(world::read_transform(document.world().runtime(),entity))==original && document.undo_count()==history,
            "Cancelling a rotated Entity transform changed authoring or retained its gesture.");
        take(key(mode));take(key('X'));take(text(mode=='R'?L"15":mode=='S'?L"2":L"1"));
        take(key(13,ui::Key::enter));
        require(!tool.active() && document.undo_count()==history+1,"Rotated Entity transform did not commit once.");
        changed(document.undo());
        require(take(world::read_transform(document.world().runtime(),entity))==original,
            "Rotated Entity transform undo lost its original TRS.");
    }
    std::puts("Rotated viewport G/R/S: PASS (real imported pose, preview, cancel, commit and undo)");
}
void verify_mirrored_viewport_transforms() {
    using namespace editor;
    Session document;changed(document.create_static_mesh(mesh_assets().front()));const auto entity=document.selection().entity;
    const auto pose=[&]{return take(world::read_transform(document.world().runtime(),entity));};
    // One mirrored axis: an unbiased decomposition reports (-1,1,1) plus a half turn instead.
    const world::Trs mirrored{{1,2,3},from_euler_degrees({0,30,0}),{1,-1,1}};
    take(document.set_transform(entity,mirrored));
    Camera camera;const auto view=ViewportView::make(camera,{0,0,1000,700},1.5f);
    ViewportTool tool;ToolSettings settings{TransformTool::transform,PivotMode::median};
    const auto key=[&](std::uint32_t code,ui::Key key=ui::Key::none) {
        return tool.key(document,view,{.kind=ui::EventKind::key_down,.key=key},code,settings,{500,350});
    };
    const auto text=[&](std::wstring value) {
        ui::Event event;event.kind=ui::EventKind::text;event.data=std::move(value);
        return tool.key(document,view,event,0,settings,{500,350});
    };
    const auto nearly=[](math::Vector3 a,math::Vector3 b){return math::length(a-b)<1e-4f;};
    const auto same_turn=[](math::Quaternion a,math::Quaternion b){return std::abs(std::abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w)-1)<1e-5f;};
    const auto gesture=[&](std::uint32_t mode,std::uint32_t axis,const wchar_t* amount) {
        take(key(mode));if(axis) take(key(axis));take(text(amount));take(key(13,ui::Key::enter));
    };
    const auto history=document.undo_count();
    gesture('G','X',L"2");
    require(nearly(pose().position,{3,2,3}) && nearly(pose().scale,{1,-1,1}) && same_turn(pose().rotation,mirrored.rotation),
        "Moving a mirrored Entity changed its mirror axis or rotation.");
    gesture('R','Z',L"90");
    const auto turned=pose();
    require(nearly(turned.scale,{1,-1,1}) && !same_turn(turned.rotation,mirrored.rotation),"Rotating a mirrored Entity moved its mirror to another axis.");
    gesture('S',0,L"2");
    require(nearly(pose().scale,{2,-2,2}) && same_turn(pose().rotation,turned.rotation),"Scaling a mirrored Entity lost its own mirror axis.");
    gesture('S',0,L"-0.5");
    require(nearly(pose().scale,{-1,1,-1}) && same_turn(pose().rotation,turned.rotation),"A scale gesture did not cross zero into a mirror.");
    take(key('S'));take(text(L"0"));
    require(document.editing() && pose().scale==math::Vector3{},"A zero scale preview was refused.");
    take(key(27,ui::Key::escape));
    require(nearly(pose().scale,{-1,1,-1}) && document.undo_count()==history+4,"Cancelling a zero scale preview did not restore the mirror.");
    const auto field=document.types().scale_fields[0];
    take(document.begin_edit(entity,field,77));
    require(!document.preview_edit(77,10001.f) && !document.preview_edit(77,-10001.f),"Scale field accepted a magnitude above the editor bound.");
    take(document.preview_edit(77,0.f));take(document.preview_edit(77,-3.f));take(document.commit_edit(77));
    require(pose().scale.x==-3 && document.undo_count()==history+5,"Scale field did not commit a negative value.");
    // A zero axis leaves the Entity's frame singular; G, R and S still apply, and the axis stays zero.
    take(document.set_transform(entity,{{1,2,3},{},{0,-1,1}}));
    gesture('G','Y',L"2");
    require(nearly(pose().position,{1,4,3}) && nearly(pose().scale,{0,-1,1}) && pose().scale.x==0,"Moving a zero scale Entity failed or changed its scale.");
    gesture('R','Z',L"30");
    const auto spun=pose();
    require(nearly(spun.position,{1,4,3}) && nearly(spun.scale,{0,-1,1}) && spun.scale.x==0 && std::abs(spun.rotation.x)<1e-5f && std::abs(spun.rotation.y)<1e-5f &&
        std::abs(std::abs(spun.rotation.z)-std::sin(std::numbers::pi_v<float>/12))<1e-5f,"Rotating a zero scale Entity failed, or changed its scale or turn axis.");
    gesture('S','Y',L"2");gesture('S','X',L"2");
    require(nearly(pose().scale,{0,-2,1}) && pose().scale.x==0 && nearly(pose().position,{1,4,3}),"Scaling a zero scale Entity failed or left its zero axis.");
    std::puts("Mirrored viewport G/R/S: PASS (single-axis mirror kept, scale crosses zero, zero preview, signed scale field, zero-axis G/R/S)");
}
void verify_viewport_local_constraints() {
    using namespace editor;
    Session document;changed(document.create_static_mesh(mesh_assets().front()));const auto entity=document.selection().entity;
    take(document.set_transform(entity,{{},from_euler_degrees({0,0,90}),{1,1,1}}));
    const auto before=take(world::read_transform(document.world().runtime(),entity));
    Camera camera;camera.set_view(CameraView::front);const auto view=ViewportView::make(camera,{0,0,1000,700},1);
    const auto center=*view.project({}),travel=center+math::Vector2{60,0};
    ViewportTool tool;ToolSettings settings;settings.tool=TransformTool::select;settings.orientation=Orientation::local;
    const auto key=[&](std::uint32_t code,ui::Key key=ui::Key::none) {
        return tool.key(document,view,{.kind=ui::EventKind::key_down,.key=key},code,settings,center);
    };
    for(const auto mode:std::array<std::uint32_t,2>{'G','S'}) {
        settings.orientation=mode=='G'?Orientation::local:Orientation::global;
        take(key(mode));
        take(tool.pointer(document,view,{.kind=ui::EventKind::pointer_move,.x=travel.x,.y=travel.y},settings));
        take(tool.pointer(document,view,{.kind=ui::EventKind::pointer_down,.x=travel.x,.y=travel.y,.button=ui::PointerButton::middle},settings));
        require(tool.readout().find(L"along local Y")!=std::wstring_view::npos,
            "Middle-click constraint did not use the operation's Local projected axes.");
        const auto preview=take(world::read_transform(document.world().runtime(),entity));
        if(mode=='G') require(std::abs(preview.position.x)>.1f && std::abs(preview.position.y)<.0001f,
            "Local middle-click movement changed direction after selecting its constraint.");
        else require(preview.scale.x==1 && std::abs(preview.scale.y-1)>.1f && preview.scale.z==1,
            "Middle-click Scale constrained the world-axis index instead of the Local axis.");
        std::vector<renderer::OverlayShape> shapes;const std::array meshes{entity};tool.overlay(document,view,settings,meshes,shapes);
        require(std::ranges::any_of(shapes,[](const auto& shape) {
            return shape.kind==renderer::OverlayShape::Kind::segment && std::abs(shape.points[2]-shape.points[0])>700 &&
                std::abs(shape.points[3]-shape.points[1])<.01f && shape.color.g>shape.color.r;
        }),"The modal constraint line did not follow the same Local axis as the transform.");
        take(key(27,ui::Key::escape));require(take(world::read_transform(document.world().runtime(),entity))==before,
            "Cancelling a Local constraint did not restore the authored pose.");
    }
}
void verify_viewport_gesture_input() {
    using namespace editor;
    Session document;changed(document.create_static_mesh(mesh_assets().front()));const auto first=document.selection().entity;
    changed(document.create_static_mesh(mesh_assets().front(),{2,0,0}));const auto second=document.selection().entity;take(document.select(first));
    Camera camera;camera.set_view(CameraView::front);const auto view=ViewportView::make(camera,{0,0,1000,700},1);
    const auto center=*view.project({});auto pointer=center;
    ViewportTool tool;ToolSettings settings;settings.tool=TransformTool::select;settings.grid_spacing=.25f;
    const auto key=[&](std::uint32_t code,ui::Event event=ui::Event{.kind=ui::EventKind::key_down}) {
        return tool.key(document,view,event,code,settings,pointer);
    };
    const auto text=[&](std::wstring value,bool shift=false) {
        return key(0,{.kind=ui::EventKind::text,.data=std::move(value),.shift=shift});
    };
    const auto escape=[&] {take(key(27,{.kind=ui::EventKind::key_down,.key=ui::Key::escape}));};
    const auto pose=[&] {return take(world::read_transform(document.world().runtime(),first));};
    const auto history=document.undo_count();
    take(key('I',{.kind=ui::EventKind::key_down,.control=true}));
    require(document.selected_entities().size()==1 && document.selection().entity==second,"Ctrl+I did not invert selection on its first key press.");
    take(key('I',{.kind=ui::EventKind::key_down,.control=true,.repeat=true}));
    require(document.selected_entities().size()==1 && document.selection().entity==second,"Auto-repeated Ctrl+I inverted the selection again.");
    take(key('I',{.kind=ui::EventKind::key_down,.control=true}));
    require(document.selected_entities().size()==1 && document.selection().entity==first,"A fresh Ctrl+I key press did not invert selection again.");
    take(key('B'));
    for(const auto code:std::array<std::uint32_t,4>{'G','R','S','A'}) {
        take(key(code));require(tool.active() && !tool.modal() && !document.editing() && document.selected_entities().size()==1,
            "A box-selection owner admitted another gesture or a selection shortcut.");
    }
    escape();require(!tool.active(),"Escape did not release the box-selection owner.");
    for(const auto mode:std::array<std::uint32_t,3>{'G','R','S'}) {
        take(key(mode));take(key('X'));take(text(mode=='R'?L"15":L"2"));
        const auto preview=pose();const std::wstring readout(tool.readout());
        take(key('X',{.kind=ui::EventKind::key_down,.repeat=true}));
        require(pose()==preview && tool.readout()==readout,"An auto-repeated axis key toggled the active transform constraint.");
        take(key('X'));require(tool.readout().find(L"along ")==std::wstring_view::npos,
            "A fresh axis key did not release the previous constraint.");
        escape();
    }
    take(key('G'));take(key('X'));
    pointer=*view.project({1,0,0});
    take(tool.pointer(document,view,{.kind=ui::EventKind::pointer_move,.x=pointer.x,.y=pointer.y,.shift=true},settings));
    require(std::abs(pose().position.x-.1f)<.0001f,"Shift pointer movement did not enter fine translation.");
    take(key('Q',{.kind=ui::EventKind::key_down,.shift=true}));
    require(std::abs(pose().position.x-.1f)<.0001f,"A key event dropped the held fine-movement modifier.");
    take(text(L"Q",true));
    require(std::abs(pose().position.x-.1f)<.0001f,"A text event dropped the held fine-movement modifier.");
    take(text(L"2",true));require(std::abs(pose().position.x-2)<.0001f,"Fine movement scaled an explicitly typed value.");
    take(key(8,{.kind=ui::EventKind::key_down,.key=ui::Key::backspace,.shift=true}));
    require(std::abs(pose().position.x-.1f)<.0001f,"Clearing typed input did not resume fine movement with the current Shift state.");
    take(key(0x10,{.kind=ui::EventKind::key_up}));
    require(std::abs(pose().position.x-1)<.0001f,"Releasing Shift without moving the pointer left a stale fine preview.");
    take(key(0x10,{.kind=ui::EventKind::key_down,.shift=true}));
    require(std::abs(pose().position.x-.1f)<.0001f,"Pressing Shift without moving the pointer failed to refresh the preview.");
    escape();
    for(const bool enabled:{false,true}) {
        settings.grid_snap=enabled;pointer=center;take(key('G'));take(key('X'));
        pointer=*view.project({.31f,0,0});
        take(tool.pointer(document,view,{.kind=ui::EventKind::pointer_move,.x=pointer.x,.y=pointer.y},settings));
        require(std::abs(pose().position.x-(enabled?.25f:.31f))<.0001f,"The initial movement preview ignored its snap setting.");
        take(key(0x11,{.kind=ui::EventKind::key_down,.control=true}));
        require(std::abs(pose().position.x-(enabled?.31f:.25f))<.0001f,"Pressing Ctrl did not invert snapping at a stationary pointer.");
        take(key(0x11,{.kind=ui::EventKind::key_up}));
        require(std::abs(pose().position.x-(enabled?.25f:.31f))<.0001f,"Releasing Ctrl left the previous snap preview active.");
        escape();
    }
    require(document.undo_count()==history && pose().position==math::Vector3{},"Cancelled input gestures changed author history or position.");
    settings.grid_snap=false;pointer=center;take(key('G'));take(key('X'));take(text(L"1"));
    tool.arm_box_select({100,100},false);
    take(key(13,{.kind=ui::EventKind::key_down,.key=ui::Key::enter}));
    require(!tool.active() && !document.editing() && document.undo_count()==history+1,
        "Arming a box select during a transform left a second gesture after confirmation.");
    changed(document.undo());
    pointer={100,100};take(key('B'));
    take(tool.pointer(document,view,{.kind=ui::EventKind::pointer_down,.x=pointer.x,.y=pointer.y},settings));
    pointer={900,600};take(tool.pointer(document,view,{.kind=ui::EventKind::pointer_move,.x=pointer.x,.y=pointer.y},settings));
    take(key('B',{.kind=ui::EventKind::key_down,.repeat=true}));
    take(tool.pointer(document,view,{.kind=ui::EventKind::pointer_up,.x=pointer.x,.y=pointer.y},settings));
    require(!tool.active() && document.selected_entities().size()==2 && document.undo_count()==history,
        "Auto-repeated B rearmed an active box selection or lost its original drag.");
}
void verify_viewport_duplication() {
    editor::Session document;
    changed(document.create_static_mesh(editor::mesh_assets().front(),{1,2,3}));
    const auto first=document.selection().entity;
    changed(document.add_component(document.types().component));
    changed(document.add_component(document.types().collision_component));
    auto body=physics::BodySettings{};body.sensor=true;body.radius=2.f;
    take(document.configure_body(document.selection().object,body));
    changed(document.create_light(world::LightKind::point,{4,5,6}));
    const auto second=document.selection().entity;
    const std::array selected{first,second};take(document.select_entities(selected,second));
    const auto history=document.undo_count();
    const auto before=nlohmann::json::parse(take(document.serialize()));
    changed(document.duplicate_selected());
    const std::vector copies(document.selected_entities().begin(),document.selected_entities().end());
    require(copies.size()==2 && copies[0]!=first && copies[1]!=second && document.undo_count()==history+1,
        "Duplicate did not create independent identities in one history entry.");
    auto after=nlohmann::json::parse(take(document.serialize()));
    for (std::size_t i=0;i<2;++i) {
        after["entities"][i+2]["name"]=before["entities"][i]["name"];
        auto& clone=after["entities"][i+2];const auto& original=before["entities"][i];
        require(clone["id"]!=original["id"],"Duplicate reused its source Entity local ID.");clone["id"]=original["id"];
        for(std::size_t j=0;j<clone["components"].size();++j) {
            require(clone["components"][j]["id"]!=original["components"][j]["id"],"Duplicate reused a source Component local ID.");
            clone["components"][j]["id"]=original["components"][j]["id"];
        }
        require(clone==original,"Duplicate lost component state, links, physics settings or reflected properties.");
    }
    const auto root=[&](ObjectId entity){return entity;};
    const auto original_mesh=take(world::read_static_mesh(document.world().runtime(),component(document,first,document.types().static_mesh_component)));
    require(take(world::read_static_mesh(document.world().runtime(),component(document,copies[0],document.types().static_mesh_component))).mesh.resolved==original_mesh.mesh.resolved,
        "Duplicating an Entity unnecessarily copied its immutable mesh resource.");
    editor::ViewportTool tool;editor::Camera camera;
    const auto view=editor::ViewportView::make(camera,{0,0,1000,700},1.f);
    take(tool.place_duplicates(document,view,{}, {500,350}));
    const auto event=[&](ui::Event e,std::uint32_t key=0){take(tool.key(document,view,e,key,{}, {500,350}));};
    event({.kind=ui::EventKind::key_down},'X');
    event({.kind=ui::EventKind::text,.data=std::wstring{L"2"}});
    event({.kind=ui::EventKind::key_down,.key=ui::Key::enter});
    require(document.undo_count()==history+1 && take(world::read_transform(document.world().runtime(),root(copies[0]))).position.x==3,
        "Duplicate placement was split into another undo step.");
    changed(document.undo());require(take(document.world().members()).size()==2,"Undo did not remove the complete duplicate graph.");
    changed(document.redo());require(document.selected_entities().size()==2 && document.selected_entities()[0]==copies[0] &&
        take(world::read_transform(document.world().runtime(),root(copies[0]))).position.x==3,"Redo did not retain duplicate identity and final placement.");
    changed(document.duplicate_selected());const auto stationary=document.selection().entity;
    const auto transform=take(world::read_transform(document.world().runtime(),root(stationary)));
    take(tool.place_duplicates(document,view,{}, {500,350}));
    event({.kind=ui::EventKind::key_down},'X');event({.kind=ui::EventKind::text,.data=std::wstring{L"7"}});
    event({.kind=ui::EventKind::key_down,.key=ui::Key::escape});
    require(take(document.world().members()).size()==6 && take(world::read_transform(document.world().runtime(),root(stationary)))==transform,
        "Cancelling duplicate movement removed copies or kept the preview displacement.");
    take(document.select(root(stationary)));changed(document.erase_entities());
    require(take(document.world().members()).size()==5,"Viewport deletion removed only the inspected Component.");
    changed(document.undo());take(document.select({}));const auto count=document.undo_count();
    changed(document.duplicate_selected());require(document.undo_count()==count,"Empty duplicate changed history.");
    editor::Session restricted;
    changed(restricted.create_static_mesh(editor::mesh_assets().front()));const auto mesh_entity=restricted.selection().entity;
    // A World keeps one Environment, so copying it with a mesh fails as one group.
    auto restricted_lighting=take(restricted.world().environment());restricted_lighting.exposure_ev=1;take(restricted.set_environment(restricted_lighting));
    const auto sky_entity=take(world::inspect_component(restricted.world().runtime(),restricted.environment())).owner;
    const std::array mixed{mesh_entity,sky_entity};take(restricted.select_entities(mixed,sky_entity));
    const auto checkpoint=take(restricted.serialize());const auto checkpoint_history=restricted.undo_count();
    require(!restricted.duplicate_selected() && restricted.undo_count()==checkpoint_history && take(restricted.serialize())==checkpoint &&
        restricted.selection().entity==sky_entity,"Failed group duplication left a partial Entity, changed selection or changed history.");
    take(document.select(first));take(document.rename(first,std::wstring(256,L'A')));changed(document.duplicate_selected());
    require(document.name(document.selection().entity).size()<=256,"Duplicating a maximum-length name exceeded the name limit.");
    const auto limit_folder=std::filesystem::current_path()/"out/viewport-limit-evidence"/std::to_string(GetTickCount64());
    auto limit_project=take(editor::Project::create(limit_folder));editor::Session empty;
    auto limit_graph=nlohmann::json::parse(take(empty.serialize()));
    for(std::size_t i=0;i<editor::Session::maximum_entities;++i) {
        const auto id=std::to_string(i+1);limit_graph["roots"].push_back(id);
        limit_graph["entities"].push_back({{"id",id},{"type","world.Entity"},{"name","Limit "+id},{"enabled",true},
            {"space",nullptr},{"children",nlohmann::json::array()},{"components",nlohmann::json::array()},{"properties",nlohmann::json::object()}});
    }
    limit_graph["next_id"]=std::to_string(editor::Session::maximum_entities+1);
    auto full=take(editor::Session::deserialize(limit_graph.dump(),limit_project));
    const auto last=take(full->world().members()).back().id();take(full->select(last));
    const auto limit_history=full->undo_count();const auto limit_checkpoint=take(full->serialize());
    require(!full->duplicate_selected() && full->undo_count()==limit_history && full->selection().entity==last &&
        take(full->world().members()).size()==editor::Session::maximum_entities && take(full->serialize())==limit_checkpoint,
        "Entity limit rejection partially published a duplicate.");
}
void verify_viewport_editing() {
    verify_rotated_viewport_transforms();
    verify_mirrored_viewport_transforms();
    verify_viewport_local_constraints();
    verify_viewport_gesture_input();
    verify_viewport_duplication();
    using namespace editor;
    Session document;
    const auto create=[&](math::Vector3 at) {changed(document.create_static_mesh(mesh_assets().front(),at));return document.selection().entity;};
    const auto first=create({-1.5f,0,0}),second=create({1.5f,0,0});
    const auto root=[&](ObjectId entity){return entity;};
    const auto transform=[&](ObjectId entity){return take(world::read_transform(document.world().runtime(),root(entity)));};
    const auto first_root=root(first),second_root=root(second);
    Camera camera;const auto view=ViewportView::make(camera,{30,40,1000,700},1.5f);
    ViewportTool tool;ToolSettings settings{TransformTool::select,PivotMode::median};
    const auto point=[&](ObjectId entity){return *view.project(transform(entity).position);};
    const auto pointer=[&](ui::EventKind kind,math::Vector2 p,bool shift=false,bool control=false,ui::PointerButton button=ui::PointerButton::secondary) {
        ui::Event event;event.kind=kind;event.x=p.x;event.y=p.y;event.shift=shift;event.control=control;event.button=button;
        return tool.pointer(document,view,event,settings);
    };
    const auto key=[&](std::uint32_t code,ui::Key key=ui::Key::none) {
        ui::Event event;event.kind=ui::EventKind::key_down;event.key=key;return tool.key(document,view,event,code,settings,point(second));
    };
    const auto chord=[&](std::uint32_t code,bool control,bool alt) {
        ui::Event event;event.kind=ui::EventKind::key_down;event.control=control;event.alt=alt;
        return tool.key(document,view,event,code,settings,point(second));
    };
    const auto text=[&](std::wstring value) {
        ui::Event event;event.kind=ui::EventKind::text;event.data=std::move(value);return tool.key(document,view,event,0,settings,point(second));
    };
    require(tool.pick(document,view,point(first).x,point(first).y)==first,"Viewport ray did not hit the actual first mesh.");
    require(tool.pick(document,view,point(second).x,point(second).y)==second,"Viewport ray did not hit the actual second mesh.");
    const auto before_cursor_history=document.undo_count();
    take(pointer(ui::EventKind::pointer_down,point(first),false,false,ui::PointerButton::primary));
    require(document.selection().entity==second && document.undo_count()==before_cursor_history && tool.cursor!=math::Vector3{},
        "Left click selected an Entity or failed to place the cursor on actual geometry.");
    take(pointer(ui::EventKind::pointer_down,point(first)));take(pointer(ui::EventKind::pointer_up,point(first)));
    take(pointer(ui::EventKind::pointer_down,point(second),true));take(pointer(ui::EventKind::pointer_up,point(second),true));
    require(document.selected_entities().size()==2 && document.selection().entity==second,"Shift click did not preserve and extend the Entity selection.");
    const auto history=document.undo_count();const auto before_first=transform(first),before_second=transform(second);
    take(key('G'));take(key('X'));take(text(L"2"));
    require(std::abs(transform(first).position.x-before_first.position.x-2)<.0001f &&
        std::abs(transform(second).position.x-before_second.position.x-2)<.0001f && document.undo_count()==history,
        "Modal move did not preview the whole selection without history.");
    require(!document.serialize() && !document.start(),"An active viewport draft could be saved or played.");
    take(key(13,ui::Key::enter));require(document.undo_count()==history+1 && !tool.active(),"One move gesture did not produce one history entry.");
    changed(document.undo());require(transform(first)==before_first && transform(second)==before_second && document.selected_entities().size()==2,
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
    changed(document.undo());
    {
        // Snapping panel: rotation increment and Affect gate R/S like G (Ctrl still inverts).
        settings.rotation_step=45;settings.snap_rotate=true;
        take(key('R'));take(key('Z'));
        const auto centre=*view.project(transform(second).position);
        take(pointer(ui::EventKind::pointer_move,{centre.x+80,centre.y-35}));
        const auto snapped=euler_degrees(transform(second).rotation).z;
        require(std::abs(std::remainder(snapped,45.f))<.01f,"Rotation did not snap to the panel's 45° increment.");
        take(key(27,ui::Key::escape));
        settings.snap_move=false;
        take(key('G'));take(key('X'));
        take(pointer(ui::EventKind::pointer_move,*view.project(before_second.position+math::Vector3{.31f,0,0})));
        require(std::abs(transform(second).position.x-1.81f)<.001f,"Disabling Affect Move still snapped movement.");
        take(key(27,ui::Key::escape));
        settings.snap_move=true;settings.rotation_step=15;
        // Local orientation: X constrains along the active Entity's own X axis.
        const auto snapping=settings.grid_snap;settings.grid_snap=false;settings.orientation=Orientation::local;
        auto turned=transform(second);turned.rotation=from_euler_degrees({0,90,0});
        take(document.set_transform(second,turned));
        const auto start=transform(second).position;
        take(key('G'));take(key('X'));take(text(L"1"));take(key(13,ui::Key::enter));
        const auto moved=transform(second).position-start;
        const auto local_x=math::rotate(turned.rotation,math::Vector3{1,0,0});
        require(math::length(moved-local_x)<.001f,"Local orientation did not constrain X to the Entity's own axis.");
        changed(document.undo());changed(document.undo());
        settings.orientation=Orientation::global;settings.grid_snap=snapping;
        take(document.select_entities(std::array{first,second},second));
    }
    settings.grid_snap=false;settings.grid_spacing=.2f;
    take(key('G'));take(key('X'));
    take(text(L"-"));
    require(!key(13,ui::Key::enter) && tool.active(),"Incomplete typed transform was committed.");
    take(key(27,ui::Key::escape));
    take(key('G'));take(key('X'));
    require(!text(L"10001") && !key(13,ui::Key::enter) && tool.active() && transform(first)==before_first && transform(second)==before_second,
        "Out-of-range preview was committed or partially applied.");
    take(key(27,ui::Key::escape));
    {
        // Middle click during G picks the axis matching the pointer's travel (Blender).
        const auto origin=*view.project(transform(second).position);
        const auto along_x=*view.project(transform(second).position+math::Vector3{1,0,0});
        take(key('G'));
        const auto moved=origin+(along_x-origin)*.5f;
        take(pointer(ui::EventKind::pointer_move,moved,false,false,ui::PointerButton::primary));
        take(pointer(ui::EventKind::pointer_down,moved,false,false,ui::PointerButton::middle));
        const auto picked=transform(second);
        require(std::abs(picked.position.y-before_second.position.y)<.0001f && std::abs(picked.position.z-before_second.position.z)<.0001f &&
            std::abs(picked.position.x-before_second.position.x)>.1f,"Middle click did not constrain G to the travelled X axis.");
        take(key(27,ui::Key::escape));
    }
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
    application::SceneSnapshot overlay_scene;take(overlay_scene.refresh(document));
    require(overlay_scene.mesh_entities.size()==overlay_scene.meshes.size(),"Overlay membership diverged from the render snapshot.");
    std::vector<renderer::OverlayShape> shapes;tool.overlay(document,view,settings,overlay_scene.mesh_entities,shapes);
    require(std::ranges::any_of(shapes,[](const auto& shape){return shape.kind==renderer::OverlayShape::Kind::rectangle;}),"Box selection has no visible accepted geometry.");
    take(pointer(ui::EventKind::pointer_up,{view.bounds.x+view.bounds.width-1,view.bounds.y+view.bounds.height-1}));
    require(document.selected_entities().size()==2,"Box selection missed real projected geometry.");
    const auto deletion_history=document.undo_count();changed(document.erase_selected());
    require(take(document.world().members()).empty() && document.undo_count()==deletion_history+1,"Multi-delete was not one document operation.");
    take(overlay_scene.refresh(document));
    require(overlay_scene.mesh_entities.empty() && overlay_scene.meshes.empty(),"Deleted meshes retained overlay membership.");
    changed(document.undo());
    require(take(document.world().members()).size()==2 && document.is_selected(first) && document.is_selected(second) && document.selection().entity==second,
        "Multi-delete undo lost Entity identity, active member or membership.");
    take(overlay_scene.refresh(document));
    require(std::ranges::find(overlay_scene.mesh_entities,first)!=overlay_scene.mesh_entities.end() &&
        std::ranges::find(overlay_scene.mesh_entities,second)!=overlay_scene.mesh_entities.end(),"Undo did not republish overlay membership.");
    // Runtime writes need no Session revision. Extraction must still reread
    // author values, including after rejecting an invalid transient value.
    const auto runtime_position=take(world::read_transform(document.world().runtime(),first)).position;
    const auto runtime_revision=document.revision(),snapshot_revision=overlay_scene.revision;
    const auto write_position=[&](math::Vector3 position) {
        take(document.world().runtime().with_native<world::Entity>(first,[&](auto& entity)->Result<void>{entity.position=position;return {};}));
    };
    write_position({runtime_position.x+7,runtime_position.y,runtime_position.z});take(overlay_scene.refresh(document));
    const auto moved=std::ranges::find(overlay_scene.mesh_entities,first)-overlay_scene.mesh_entities.begin();
    require(document.revision()==runtime_revision && overlay_scene.revision==snapshot_revision &&
        overlay_scene.meshes[moved].world[12]==runtime_position.x+7,
        "Scene extraction cached a direct Runtime transform behind Session revision.");
    // The World isolates the invalid Entity: the rest of the scene still extracts.
    write_position({std::numeric_limits<float>::infinity(),0,0});take(overlay_scene.refresh(document));
    require(std::ranges::find(overlay_scene.mesh_entities,first)==overlay_scene.mesh_entities.end() &&
        std::ranges::find(overlay_scene.mesh_entities,second)!=overlay_scene.mesh_entities.end(),
        "Scene extraction kept a non-finite Runtime transform or dropped valid Entities.");
    write_position(runtime_position);take(overlay_scene.refresh(document));
    require(overlay_scene.meshes[moved].world[12]==runtime_position.x && document.revision()==runtime_revision,
        "Scene extraction retained a failed traversal or changed author history.");
    changed(document.redo());require(document.selected_entities().empty(),"Delete redo retained a stale selection.");changed(document.undo());
    // Blender object-mode chords: A toggles all, Alt+A clears, Ctrl+I inverts.
    take(document.select(first));
    take(key('A'));require(document.selected_entities().size()==2,"A did not select every Entity.");
    take(key('A'));require(document.selected_entities().empty(),"A with everything selected did not deselect all.");
    take(chord('A',false,false));take(chord('A',false,true));require(document.selected_entities().empty(),"Alt+A did not deselect.");
    take(document.select(first));take(chord('I',true,false));
    require(document.selected_entities().size()==1 && document.is_selected(second),"Ctrl+I did not invert the Entity selection.");
    take(key('A'));
    {
        // Alt+G/R/S clear one channel of every selected root, one undo entry each.
        auto moved=transform(first);moved.position={1,2,3};moved.rotation=from_euler_degrees({30,0,0});moved.scale={2,2,2};
        const std::array root_one{first_root};const std::array value_one{moved};
        take(document.begin_transform(root_one,930));take(document.preview_transform(930,value_one));take(document.commit_edit(930));
        const auto history=document.undo_count();
        take(chord('G',false,true));
        require(transform(first).position==math::Vector3{} && transform(first).rotation==moved.rotation && document.undo_count()==history+1,
            "Alt+G did not clear only location as one history entry.");
        take(chord('R',false,true));take(chord('S',false,true));
        const auto cleared=transform(first);
        if(!(cleared.rotation==math::Quaternion{} && cleared.scale==math::Vector3{1,1,1} && document.undo_count()==history+3))
            throw std::runtime_error("Alt+R/S did not clear rotation and scale: q=("+std::to_string(cleared.rotation.x)+","+std::to_string(cleared.rotation.y)+","+
                std::to_string(cleared.rotation.z)+","+std::to_string(cleared.rotation.w)+") s="+std::to_string(cleared.scale.x)+
                " history="+std::to_string(document.undo_count()-history)+" selected="+std::to_string(document.selected_entities().size()));
        changed(document.undo());changed(document.undo());changed(document.undo());
        require(transform(first)==moved,"Clear channel undo did not restore the full TRS.");
        changed(document.undo());require(transform(first)==before_first,"Fixture transform was not undone.");
    }
    {
        // Surface placement orients the cursor frame: local Z follows the hit face normal.
        settings.cursor_surface=true;
        const auto face=view.project(transform(second).position+math::Vector3{0,.5f,0});require(face.has_value(),"Top face is off screen.");
        take(pointer(ui::EventKind::pointer_down,*face,false,false,ui::PointerButton::primary));
        const auto normal=math::rotate(tool.cursor_rotation,math::Vector3{0,0,1});
        require(std::abs(tool.cursor.y-.5f)<.01f && normal.y>.999f,"Surface cursor did not land on and align with the top face.");
        settings.cursor_surface=false;
    }
    const math::Vector3 angles{23,47,-12};const auto converted=euler_degrees(from_euler_degrees(angles));
    require(std::abs(converted.x-angles.x)<.001f && std::abs(converted.y-angles.y)<.001f && std::abs(converted.z-angles.z)<.001f,
        "Euler parameter conversion disagrees with native XYZ rotation.");
    for(const auto preset:{CameraView::top,CameraView::bottom,CameraView::front,CameraView::back,CameraView::right,CameraView::left}) {
        camera.set_view(preset);const auto orthographic=ViewportView::make(camera,view.bounds,view.dpi_scale);
        const auto a=orthographic.ray(400,300),b=orthographic.ray(420,300);
        require(orthographic.orthographic && math::dot(a.direction,b.direction)>.99999f && a.origin!=b.origin,
            "Orthographic camera rays are not parallel and position-dependent.");
        require(orthographic.pixel_size({})==orthographic.pixel_size({10,20,30}),"Orthographic handle size changed with depth.");
        Camera restored;require(restored.set_state(camera.state()),"A six-axis camera state could not be restored.");
        require(restored.matrices(1).view==camera.matrices(1).view,"Camera state restoration changed its basis.");
        const auto center=orthographic.project(transform(first).position);
        require(center && tool.pick(document,orthographic,center->x,center->y).has_value(),"Orthographic picking lost real geometry.");
    }
    settings.tool=TransformTool::transform;shapes.clear();take(overlay_scene.refresh(document));
    tool.overlay(document,view,settings,overlay_scene.mesh_entities,shapes);
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
        const auto* a=ui.presented()->find(workspace.field_control(root(document.selection().entity),paths[0]));
        const auto* b=ui.presented()->find(workspace.field_control(root(document.selection().entity),paths[1]));
        const auto* c=ui.presented()->find(workspace.field_control(root(document.selection().entity),paths[2]));
        require(a && b && c && a->bounds.y==b->bounds.y && b->bounds.y==c->bounds.y,"A Vector3 was split across parameter rows.");
    }
    const auto* rotation_x=ui.presented()->find(workspace.field_control(root(document.selection().entity),document.types().rotation_fields[0]));
    require(rotation_x && !ui.presented()->find(workspace.field_control(root(document.selection().entity),document.types().rotation_fields[3])),
        "Scene rotation exposed quaternion W instead of a three-angle editor.");
    workspace.commit_ui_controls();
    const auto hud=ui.presented()->find(control::viewport_hud)->bounds;
    const auto camera_menu=ui.presented()->find(control::viewport_camera)->bounds;
    require(ui.pointer_target(hud.x+8,hud.y+8)==control::viewport &&
        ui.pointer_target(camera_menu.x+camera_menu.width*.5f,camera_menu.y+camera_menu.height*.5f)==control::viewport_camera,
        "Viewport chrome did not distinguish passive mode text from the interactive camera menu.");
    const auto expanded_height=ui.presented()->find(control::viewport)->bounds.height;
    const auto publish=[&](ui_layout::Size size={1200,800}) {
        auto controls=workspace.describe(document,size,ui.presented().get(),nullptr,&ui);
        require(bool(ui.set_controls(controls)),"Updated viewport chrome was refused.");
        const auto frame=ui.prepare();require(frame && ui.accept(*frame),"Updated viewport chrome did not lay out.");
        workspace.commit_ui_controls();return controls;
    };
    publish();
    // Expansion requests address identities, apply atomically and stay out of history.
    {
        const auto history=document.undo_count();
        take(workspace.set_tree_expanded(document,Workspace::EntityTree::scene_tree,std::array{ObjectId{}},false));publish();
        require(!ui.presented()->find(workspace.object_control(first)) && !ui.presented()->find(workspace.object_control(second)),
            "Collapsing World kept its Entity rows.");
        const auto mesh_component=component(document,first,document.types().static_mesh_component);
        require(!workspace.set_tree_expanded(document,Workspace::EntityTree::scene_tree,std::array{ObjectId{},mesh_component},true),
            "Scene Tree expanded a Component identity.");
        publish();require(!ui.presented()->find(workspace.object_control(first)),"A refused expansion request changed the tree.");
        take(workspace.set_tree_expanded(document,Workspace::EntityTree::scene_tree,std::array{ObjectId{}},true));publish();
        require(ui.presented()->find(workspace.object_control(first)) && document.undo_count()==history,
            "Tree expansion did not restore its rows or entered scene history.");
    }
    const std::vector<ObjectId> previous_selection(document.selected_entities().begin(),document.selected_entities().end());
    const auto previous_active=document.selection().entity;
    const std::array projected_selection{first,second};
    for(unsigned pass=0;pass<3;++pass) {
        take(document.select_entities(pass==1?std::span<const ObjectId>{}:std::span<const ObjectId>{projected_selection},pass==1?ObjectId{}:second));
        const auto selected_controls=publish();
        for(const auto object:std::array{first,second}) {
            const auto key=workspace.object_control(object);
            const auto row=std::ranges::find(selected_controls,key,&ui::Control::key);
            require(row!=selected_controls.end() && row->selected==(pass!=1),"Tree selection projection reused a stale membership set.");
        }
    }
    take(document.select_entities(previous_selection,previous_active));publish();
    {
        const auto authoring=take(document.serialize());const auto history=document.undo_count();
        const auto diagnostic=std::ranges::find(view_options(),ViewOption::direct_shadow_cache,&ViewOptionInfo::option);
        require(diagnostic!=view_options().end() && diagnostic->kind==ViewValueKind::boolean &&
            view_value(workspace.view,ViewOption::direct_shadow_cache)==0,"Cached-shadow diagnostic metadata/default is missing.");
        for(const bool clustered:{false,true}) for(const bool cached:{false,true}) {
            auto candidate=workspace.view;
            take(set_view_option(candidate,ViewOption::direct_clustered,clustered));
            take(set_view_option(candidate,ViewOption::direct_shadow_cache,cached));
            take(workspace.set_view_settings(document,candidate,{}));
            require(workspace.view.lit.direct_shadow_cache==cached && workspace.view.lit.direct_clustered==clustered,
                "Direct-light diagnostics lost their independent recorded values.");
            const auto revision=workspace.view_revision();take(workspace.set_view_settings(document,candidate,{}));
            require(workspace.view_revision()==revision,"Repeated cached-shadow diagnostic changed the view revision.");
        }
        auto candidate=workspace.view;candidate.lit.direct_clustered=false;candidate.lit.direct_shadow_cache=false;
        take(workspace.set_view_settings(document,candidate,{}));
        require(take(document.serialize())==authoring && document.undo_count()==history,
            "Direct-light diagnostics entered World authoring or Scene history.");
    }
    workspace.viewport_options_collapsed=true;publish();
    require(std::abs(ui.presented()->find(control::viewport)->bounds.height-expanded_height-16)<.1f,
        "Collapsed viewport options did not return their height to the scene.");
    workspace.viewport_options_collapsed=false;workspace.viewport_mode=TransformTool::rotate;
    const auto rotated_controls=publish();
    require(std::ranges::any_of(rotated_controls,[](const auto& control){return control.key==editor::control::viewport_hud && control.text==L"Rotate";}),
        "The authored mode label did not follow the current operation.");
    workspace.menu=ui::ContextMenuRequest{control::viewport_snap,0,0,0};publish();
    const auto step=std::ranges::find(workspace.menu_items,static_cast<ui::ControlKey>(Command::snap_grid_02),&ui::MenuItem::key);
    const auto headings=std::ranges::count_if(workspace.menu_items,[](const auto& item){return !item.heading.empty();});
    const auto rotation=std::ranges::find(workspace.menu_items,static_cast<ui::ControlKey>(Command::snap_rotation_15),&ui::MenuItem::key);
    require(headings>=4 && rotation!=workspace.menu_items.end() && rotation->selected && rotation->behavior==ui::MenuBehavior::radio,
        "The snapping popover lost its sections or rotation increment.");
    require(step!=workspace.menu_items.end() && step->selected && step->behavior==ui::MenuBehavior::radio &&
        std::ranges::any_of(workspace.menu_items,[](const auto& item){return item.key==static_cast<ui::ControlKey>(Command::snap_toggle) && item.behavior==ui::MenuBehavior::checkbox;}),
        "The shared snap menu lost its active step or toggle semantics.");
    // Add commands and shading pie share the viewport's menu model.
    workspace.menu=ui::ContextMenuRequest{control::viewport,control::viewport_add_menu,0,0};publish();
    require(std::ranges::any_of(workspace.menu_items,[](const auto& item){return item.key==static_cast<ui::ControlKey>(Command::create_static_mesh);}) &&
        std::ranges::none_of(workspace.menu_items,[](const auto& item){return item.key==static_cast<ui::ControlKey>(Command::erase_selected);}),
        "Shift+A menu did not list exactly the Create commands.");
    workspace.menu=ui::ContextMenuRequest{control::viewport,control::viewport_shading_pie,500,350};publish();
    if (workspace.menu_items.size()!=4 || ui.presented()->popup!=control::viewport_pie || !ui.presented()->find(control::shading_preset_first))
        throw std::runtime_error("Shading pie projection: items="+std::to_string(workspace.menu_items.size())+", popup="+
            std::to_string(ui.presented()->popup)+", error="+std::string(workspace.ui_error()));
    require(workspace.menu_items.size()==4 && ui.presented()->popup==control::viewport_pie &&
        ui.presented()->find(control::shading_preset_first),"Shading pie was not projected through Source/JIT.");
    for (const auto& item:workspace.menu_items) {
        const auto* box=ui.presented()->find(item.key);
        require(box && box->bounds.x+box->bounds.width<=box->clip.right+1 && box->bounds.y+box->bounds.height<=box->clip.bottom+1,
            "Pie slots were clipped by a scroll gutter or popup bounds.");
    }
    require(take(workspace.apply_view(document,{ui::ActionKind::activate,control::shading_preset_first,true})) &&
        workspace.view.wireframe==renderer::Wireframe::only,"Wireframe pie action did not reach the view model.");
    require(take(workspace.apply_view(document,{ui::ActionKind::activate,control::shading_preset_first+2,true})) &&
        workspace.view.wireframe==renderer::Wireframe::off && workspace.view.shading==renderer::Shading::matcap,
        "Shading preset retained stale wireframe state.");
    {
        constexpr ui_layout::Size wide{1900,1000};
        // Blender header: semantic menus project as real controls and their menus
        // reuse the shared command model; header segments mirror the view and tool state.
        workspace.menu.reset();const auto header=publish(wide);
        // Narrow panes fold the text menus (W and the main View menu keep them reachable).
        for(const auto key:{control::viewport_view_menu,control::viewport_select_menu,control::viewport_header_add,control::viewport_object_menu})
            require(std::ranges::any_of(header,[&](const auto& item){return item.key==key;}),"A Blender header menu is missing from the viewport header.");
        const auto has=[&](Command command){return std::ranges::any_of(workspace.menu_items,[&](const auto& item){return item.key==static_cast<ui::ControlKey>(command);});};
        const auto branch=[&](ui::ControlKey key)->const ui::MenuItem* {
            const auto at=std::ranges::find(workspace.menu_items,key,&ui::MenuItem::key);return at==workspace.menu_items.end()?nullptr:&*at;};
        workspace.menu=ui::ContextMenuRequest{control::viewport_select_menu,0,0,0};publish(wide);
        const auto* modes=branch(control::select_mode_menu);
        require(has(Command::select_all) && has(Command::select_none) && has(Command::select_invert) && has(Command::select_box) &&
            modes && modes->children.size()==5 && modes->children.front().selected && modes->children.front().behavior==ui::MenuBehavior::radio,
            "Select menu lost its Blender operations or the active selection mode.");
        workspace.menu=ui::ContextMenuRequest{control::viewport_object_menu,0,0,0};publish(wide);
        const auto* clear=branch(control::object_clear_menu);
        require(clear && clear->children.size()==3 && branch(control::object_snap_menu) && has(Command::duplicate_selected),
            "Object menu lost Clear, Snap or Duplicate.");
        workspace.menu=ui::ContextMenuRequest{control::viewport_view_menu,0,0,0};publish(wide);
        require(has(Command::frame_all) && has(Command::focus) && branch(control::view_viewpoint_menu),"View menu lost framing or viewpoints.");
        workspace.menu.reset();publish(wide);
        // Select-mode routing: Extend keeps, Subtract removes, Intersect keeps only the overlap.
        const std::array both{first,second};take(document.select_entities(both,second));
        settings.select_mode=SelectMode::subtract;
        take(pointer(ui::EventKind::pointer_down,point(first)));take(pointer(ui::EventKind::pointer_up,point(first)));
        require(document.selected_entities().size()==1 && document.is_selected(second),"Subtract mode did not remove the clicked Entity.");
        settings.select_mode=SelectMode::extend;
        take(pointer(ui::EventKind::pointer_down,point(first)));take(pointer(ui::EventKind::pointer_up,point(first)));
        require(document.selected_entities().size()==2,"Extend mode did not add the clicked Entity.");
        settings.select_mode=SelectMode::intersect;
        take(pointer(ui::EventKind::pointer_down,point(second)));take(pointer(ui::EventKind::pointer_up,point(second)));
        require(document.selected_entities().size()==1 && document.is_selected(second),"Intersect mode did not keep only the clicked overlap.");
        settings.select_mode=SelectMode::set;
        take(tool.select_all(document,SelectAll::invert));
        require(document.selected_entities().size()==1 && document.is_selected(first),"Invert Selection did not complement the selection.");
        take(tool.select_all(document,SelectAll::deselect));require(document.selected_entities().empty(),"Select None kept a selection.");
        take(tool.select_all(document,SelectAll::select));require(document.selected_entities().size()==2,"Select All missed an Entity.");
        // Houdini display bar: every toggle edits exactly its ViewSettings value.
        const auto toggle=[&](ui::ControlKey key,bool on){take(workspace.apply_view(document,{ui::ActionKind::activate,key,on}));};
        toggle(control::display_grid,!workspace.view.reference_grid);toggle(control::display_axes,true);
        require(workspace.view.origin_axes,"Display bar did not reach origin axes.");
        toggle(control::display_wire,true);require(workspace.view.wireframe==renderer::Wireframe::overlay,"Wire overlay toggle failed.");
        toggle(control::display_wire,false);require(workspace.view.wireframe==renderer::Wireframe::off,"Wire overlay did not clear.");
        toggle(control::display_triangles,true);require(workspace.view.triangle_edges,"Triangle edge toggle failed.");
        toggle(control::display_triangles,false);
        {
            const auto bar=publish(wide);
            require(std::ranges::count_if(bar,[](const auto& c){return c.key>=control::display_toggle_first && c.key<control::display_toggle_first+8;})==8,
                "Display bar did not project its eight toggles.");
        }
        // Inline rename: the field covers the row's text, never its icon, and owns a fixed key.
        {
            publish(wide);
            const auto row_key=workspace.object_control(first);
            workspace.row_rename=Workspace::RowRename{first,std::wstring(document.name(first))};publish(wide);
            const auto& frame=*ui.presented();
            const auto* field=frame.find(control::inline_rename);const auto* row=frame.find(row_key);
            require(field && row && field->kind==ui::ControlKind::text_edit && std::abs(field->bounds.y-row->bounds.y)<1 &&
                field->bounds.x>row->bounds.x+row->style->indicator_size && field->bounds.x+field->bounds.width<=row->bounds.x+row->bounds.width+1,
                "Inline rename did not sit over the row's name.");
            workspace.row_rename.reset();publish(wide);
            require(!ui.presented()->find(control::inline_rename),"Inline rename field outlived its session.");
        }
        // Scene Tree: drop resolution, keep-world reparent as one edit, cycle refusal and row menu.
        {
            publish(wide);
            const auto& frame=*ui.presented();
            const auto centre=[&](ui::ControlKey key){const auto* box=frame.find(key);require(box,"Tree row missing.");
                return ui::DragPoint{box->bounds.x+box->bounds.width*.5f,box->bounds.y+box->bounds.height*.5f};};
            const auto first_row=workspace.object_control(first);
            const auto onto_first=workspace.tree_drop(document,frame,second,centre(first_row));
            require(onto_first && onto_first->parent==first && onto_first->row==first_row,"Dropping on a row did not target that Entity.");
            const auto onto_world=workspace.tree_drop(document,frame,second,centre(control::tree_world));
            require(onto_world && onto_world->parent==ObjectId{},"Dropping on World did not unparent.");
            const auto world_before=take(world::world_matrix(document.world().runtime(),second));
            const auto history=document.undo_count();
            take(document.select_entities(std::array{first},first));publish(wide);
            take(document.reparent(second,first));take(document.select_entities(std::array{second},second));
            require(take(world::parent_entity(document.world().runtime(),second))==first && document.undo_count()==history+1,
                "Tree reparent was not one history entry.");
            require(take(world::world_matrix(document.world().runtime(),second))==world_before,"Keep-world reparent moved the Entity.");
            publish(wide);
            const auto& nested=*ui.presented();
            const auto* child=nested.find(workspace.object_control(second));
            require(child!=nullptr,"Dropping did not reveal the moved row under its new parent.");
            const auto cycle=workspace.tree_drop(document,nested,first,ui::DragPoint{child->bounds.x+4,child->bounds.y+child->bounds.height*.5f});
            require(!cycle,"An Entity could be dropped onto its own descendant.");
            take(document.select_entities(std::array{second},second));
            workspace.menu=ui::ContextMenuRequest{workspace.object_control(second),0,0,0};publish(wide);
            const auto has=[&](Command command){return std::ranges::any_of(workspace.menu_items,[&](const auto& item){return item.key==static_cast<ui::ControlKey>(command);});};
            require(has(Command::rename_selected) && has(Command::duplicate_selected) && has(Command::erase_entities) && has(Command::unparent_selected),
                "Scene Tree row menu lost Rename, Duplicate, Delete or Clear Parent.");
            const auto unparent=std::ranges::find(workspace.menu_items,static_cast<ui::ControlKey>(Command::unparent_selected),&ui::MenuItem::key);
            require(unparent->enabled,"Clear Parent was disabled for a parented Entity.");
            workspace.menu.reset();
            take(document.undo());
            require(take(world::parent_entity(document.world().runtime(),second))==ObjectId{},"Undo did not restore the original parent.");
            take(document.select_entities(std::array{first,second},second));publish();
        }
        // Colour editor model: conversions round-trip, and a three-lane group edit is one history entry.
        {
            const auto close=[](std::array<double,3> a,std::array<double,3> b){return std::abs(a[0]-b[0])<1e-6 && std::abs(a[1]-b[1])<1e-6 && std::abs(a[2]-b[2])<1e-6;};
            require(close(hsv_to_rgb(rgb_to_hsv({.25,.5,.75})),{.25,.5,.75}) && close(rgb_to_hsv({1,0,0}),{0,1,1}),
                "HSV conversion did not round-trip.");
            require(color_hex({1,.5,0})==L"FF8000" && parse_color_hex(L"#ff8000") && !parse_color_hex(L"12345") && !parse_color_hex(L"GG0000"),
                "Hex colour formatting or parsing was wrong.");
            changed(document.create_light(world::LightKind::point,{0,2,0}));
            const auto light=document.selection().entity;
            const auto component=take(world::components(document.world().runtime(),light)).front().id();
            std::vector<world::FieldPath> lanes;
            for(const auto& property:take(document.properties(component))) if(property.name.starts_with("settings.color.")) lanes.push_back(property.path);
            require(lanes.size()==3,"Light colour did not expose three channel fields.");
            const auto history=document.undo_count();
            take(document.begin_group_edit(component,lanes,4242));
            const auto typed=[&](float v){return Scalar{v};};
            const std::array first_preview{typed(.2f),typed(.4f),typed(.6f)},second_preview{typed(.9f),typed(.1f),typed(.3f)};
            take(document.preview_group_edit(4242,first_preview));take(document.preview_group_edit(4242,second_preview));
            take(document.commit_edit(4242));
            require(document.undo_count()==history+1 && std::get<float>(take(document.read_property(component,lanes[0])))==.9f,
                "A colour edit was not exactly one history entry with its final value.");
            take(document.undo());
            require(std::get<float>(take(document.read_property(component,lanes[0])))==1.f && std::get<float>(take(document.read_property(component,lanes[2])))==1.f,
                "Undo did not restore all three colour channels together.");
            take(document.begin_group_edit(component,lanes,4243));take(document.preview_group_edit(4243,first_preview));take(document.cancel_edit());
            require(document.undo_count()==history && std::get<float>(take(document.read_property(component,lanes[1])))==1.f,"Cancel left a colour preview behind.");
            take(document.undo());
            take(document.select_entities(std::array{first,second},second));publish();
        }
        // Shading header segment reflects and drives the same presets as the Z pie.
        take(workspace.apply_view(document,{ui::ActionKind::activate,control::shading_preset_first+1,true}));publish();
        require(workspace.view.shading==renderer::Shading::smooth && workspace.view.wireframe==renderer::Wireframe::off,
            "Solid header segment did not reach the view model.");
    }
    {
        // View Selected frames the actual geometry bounds.
        Camera framing;framing.frame({1,2,3},.5f);
        require(framing.target()==math::Vector3{1,2,3} && math::length(framing.eye()-framing.target())>.5f &&
            math::length(framing.eye()-framing.target())<2.f,"Camera framing did not fit a unit-size bound.");
    }
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
