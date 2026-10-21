void verify_dynamic_ui_reads(editor::Session& document,ObjectId root) {
    Runtime runtime;take(ui_language::register_types(runtime));
    constexpr auto declaration=R"(
double chosen() {
    if (input.choose_left) { return input.left; }
    return input.right;
}
ui PositionRead(in bool choose_left, in double left, in double right) {
    Text position { text: "Runtime position"; width: 128.0d + chosen(); }
}
)";
    auto component=take(ui_language::Component::compile(runtime,"position-read.ui.source",declaration));
    bool choose_left=true;
    const auto inputs=[&] { return std::array<ui_language::InputValue,3>{choose_left,
        static_cast<double>(value(document,root,0)),static_cast<double>(value(document,root,1))}; };
    ui::ControlKey next_key=1;
    auto instance=take(ui_language::Instance::create(component,0,next_key,{},inputs()));
    ui::Session surface;
    require(surface.set_surface({{640,480},1,1}).has_value(),"Dynamic UI read surface failed.");
    const auto publish=[&] {
        take(instance->update(inputs()));
        require(surface.set_controls(instance->controls()).has_value(),"Dynamic UI read controls failed.");instance->commit_controls();
        auto frame=surface.prepare();require(frame.has_value() && surface.accept(*frame),"Dynamic UI read layout/draw failed.");
    };
    publish();const auto initial=instance->statistics().binding_evaluations;
    take(document.begin_edit(root,document.parameters()[1].path,9998));
    take(document.preview_edit(9998,8));take(document.commit_edit(9998));publish();
    require(instance->statistics().binding_evaluations==initial,"An unread helper branch invalidated the binding.");
    choose_left=false;publish();
    require(instance->statistics().binding_evaluations==initial+1 && instance->controls().front().layout.width.value==136,
        "Changing a helper branch failed to replace its dynamic dependencies.");
    take(document.begin_edit(root,document.parameters()[0].path,9998));
    take(document.preview_edit(9998,10));take(document.commit_edit(9998));publish();
    require(instance->statistics().binding_evaluations==initial+1,"The old helper branch remained subscribed.");
    auto second=take(ui_language::Instance::create(component,0,next_key,{},inputs()));
    require(second->controls().front().key!=instance->controls().front().key &&
        second->controls().front().layout.width.value==136,"Two instances did not share a component with independent identities.");
    require(surface.set_controls({}).has_value(),"Dynamic UI read unmount failed.");
    instance->close();instance->close();
    require(instance->controls().empty() && !instance->update(),"An unmounted instance still evaluated bindings.");
    auto empty_component=take(ui_language::Component::compile(runtime,"empty.ui.source","ui Empty() { Column root {} }"));
    auto empty=take(ui_language::Instance::create(empty_component,0,next_key,{},{}));
    require(empty->controls().size()==1 && empty->statistics().node_creations==1 && empty->statistics().binding_evaluations==0,
        "A component without bindings failed to publish its static structure.");
    take(empty->update());require(empty->statistics().publications==1,"A stable static component was published again.");

    auto loop_component=take(ui_language::Component::compile(runtime,"loop.ui.source",R"(
double increment(double value) { return value + 1.0d; }
ui Loop() {
    state double count = 0.0d;
    Column body {
        Foldout run {
            text: "Run";
            on activate { for (state.count = 0.0d; state.count < 3.0d; state.count = increment(state.count)) {} }
        }
        Text current { text: "Count"; width: 128.0d + state.count; }
    }
}
)"));
    auto loop=take(ui_language::Instance::create(loop_component,0,next_key,{},{}));
    const auto show_loop=[&] {
        take(loop->update());require(surface.set_controls(loop->controls()).has_value(),"Loop component controls failed.");loop->commit_controls();
        auto frame=surface.prepare();require(frame.has_value() && surface.accept(*frame),"Loop component frame failed.");
    };
    show_loop();
    const auto run=std::ranges::find_if(loop->controls(),[](const auto& control){return control.kind==ui::ControlKind::foldout;});
    const auto bounds=surface.presented()->find(run->key)->bounds;
    const auto dispatch=[&](ui::Reply reply) {
        for (const auto& effect:reply.effects) if (const auto* action=std::get_if<ui::Action>(&effect)) take(loop->event(*action));
    };
    dispatch(surface.input({.kind=ui::EventKind::pointer_down,.x=bounds.x+8,.y=bounds.y+8}));
    dispatch(surface.input({.kind=ui::EventKind::pointer_up,.x=bounds.x+8,.y=bounds.y+8}));
    show_loop();
    require(loop->controls().back().layout.width.value==131 && loop->statistics().event_evaluations==1,
        "A Source for-step state assignment or its nested call was lowered incorrectly.");
    require(surface.set_controls({}).has_value(),"Loop component unmount failed.");
}

void verify_ui_boundary_validation(editor::Session& document,ObjectId root,ui::Session& surface) {
    Runtime runtime;take(ui_language::register_types(runtime));
    auto component=take(ui_language::Component::compile(runtime,"boundary.ui.source",
        "ui Boundary(in EditBinding data) { NumberRow row { text: \"Position\"; edit: input.data; } }"));
    ui_language::EditBinding valid{7000,true,false,{{7001,L"X",value(document,root),0,-10000,10000,.01,.001,ui::VectorAxis::x}}};
    ui::ControlKey next_key=100;
    auto instance=take(ui_language::Instance::create(component,0,next_key,{},std::array<ui_language::InputValue,1>{valid}));
    const auto draw=[&] {
        require(surface.set_controls(instance->controls()).has_value(),"Validated component was rejected by Session.");instance->commit_controls();
        auto frame=surface.prepare();require(frame.has_value() && surface.accept(*frame),"Validated component failed to draw.");
    };
    draw();const auto previous=std::vector<ui::Control>(instance->controls().begin(),instance->controls().end());
    std::array<ui_language::EditBinding,5> invalid{valid,valid,valid,valid,valid};
    invalid[0].fields.resize(5,valid.fields.front());
    invalid[1].fields.front().key=0;
    invalid[2].fields.front().value=std::numeric_limits<double>::quiet_NaN();
    invalid[3].fields.front().slider=ui::SliderRange{7002,10,5};
    invalid[4].fields.front().axis=static_cast<ui::VectorAxis>(255);
    for (const auto& input:invalid) {
        const auto rejected=instance->update(std::array<ui_language::InputValue,1>{input});
        require(!rejected && std::ranges::equal(instance->controls(),previous),
            "Invalid EditBinding escaped Result or replaced the last valid component snapshot.");
        draw();
        take(instance->update(std::array<ui_language::InputValue,1>{valid}));draw();
    }
    const std::array bodies{std::string{"text: \"A\\0B\";"},std::string{"classes: \"1foo\";"},
        std::string{"classes: \""}+std::string(65,'x')+"\";",std::string{"classes: \"a b c d e f g h i j k l m n o p q\";"}};
    for (const auto& body:bodies) {
        auto invalid_component=take(ui_language::Component::compile(runtime,"invalid-properties.ui.source",
            "ui Invalid() { Text label { "+body+" } }"));
        const auto rejected=ui_language::Instance::create(invalid_component,0,next_key,{},{});
        require(!rejected && rejected.error().message.starts_with("invalid-properties.ui.source:"),
            "Invalid text/style reached Session instead of component diagnostics.");
    }
    auto overflow=take(ui_language::Component::compile(runtime,"row-classes.ui.source",
        "ui Invalid(in EditBinding data) { NumberRow row { classes: \"a b c d e f g h i j k l m n o p\"; edit: input.data; } }"));
    require(!ui_language::Instance::create(overflow,0,next_key,{},std::array<ui_language::InputValue,1>{valid}),
        "NumberRow's built-in class was omitted from the final style limit.");
    require(surface.set_controls({}).has_value(),"Boundary component unmount failed.");
}

// Real document -> Source/JIT component -> shared UI/layout and input -> document.
// This is the same asset and Workspace path used by the windowed editor.
void verify_declarative_parameters() {
    Runtime runtime;take(ui_language::register_types(runtime));
    editor::Session document;
    editor::Workspace workspace;
    const std::array fonts{(editor::asset_directory()/"fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session ui(fonts);
    auto theme=ui::StyleSheet::parse(take(editor::read_file(editor::asset_directory()/"editor.ss")));
    require(theme.has_value() && ui.set_style_sheet(*theme).has_value(),"Declarative Parameters theme failed.");
    changed(document.create_static_mesh(editor::mesh_assets().front()));
    const auto entity=document.selection().entity;
    const auto root=entity;
    const auto surface=ui.set_surface({{1600,1000},1,1});
    require(surface.has_value(),"Declarative UI surface failed.");
    std::vector<ui::Control> controls;
    const auto publish=[&] {
        controls=workspace.describe(document,{1600,1000},ui.presented().get());
        if (!workspace.ui_error().empty()) throw std::runtime_error(std::string(workspace.ui_error()));
        auto described=ui.set_controls(controls);
        require(described.has_value(),"Declarative parameter controls were rejected.");
        for (const auto& effect:described->effects) if (const auto* action=std::get_if<ui::Action>(&effect)) {
            if (action->kind==ui::ActionKind::edit_cancel) take(document.cancel_edit());
        }
        workspace.commit_ui_controls();
        auto frame=ui.prepare();
        if (!frame) {
            for(auto key=frame.error().key;key;) {
                const auto at=std::ranges::find(controls,key,&ui::Control::key);if(at==controls.end())break;
                std::fprintf(stderr,"UI failed control=%llu parent=%llu kind=%u\n",static_cast<unsigned long long>(at->key),static_cast<unsigned long long>(at->parent),static_cast<unsigned>(at->kind));
                key=at->parent;
            }
            throw std::runtime_error("Declarative parameter layout/draw failed at control "+std::to_string(frame.error().key)+
                " code="+std::to_string(static_cast<unsigned>(frame.error().code))+" native="+std::to_string(frame.error().native_code));
        }
        require(ui.accept(*frame),"Declarative parameter frame was rejected.");
    };
    publish();
    require(workspace.ui_statistics("Parameters").has_value(),"Parameters did not create its complete Source/JIT region.");
    for (const bool menu_closed:{false,true}) for (const bool shelf_closed:{false,true}) {
        workspace.menu_bar_collapsed=menu_closed;workspace.shelf_collapsed=shelf_closed;publish();
        for (const auto key:{editor::control::menu_bar_handle,editor::control::shelf_handle}) {
            const auto* handle=ui.presented()->find(key);
            const auto* bar=ui.presented()->find(handle->parent);
            const bool closed=key==editor::control::menu_bar_handle?menu_closed:shelf_closed;
            require(handle->selected==closed &&
                std::abs(handle->bounds.width-(closed?bar->bounds.width:12))<.1f &&
                (!closed || std::abs(handle->bounds.height-12)<.1f) && handle->style->icon_align==ui::TextAlign::left,
                "Native and DSL bars disagree on rail size, full-width restore geometry or arrow alignment.");
        }
    }
    workspace.menu_bar_collapsed=workspace.shelf_collapsed=false;publish();
    require(!ui.presented()->find(editor::control::parameter_search) &&
        ui.presented()->find(editor::control::parameter_search_toggle) &&
        !ui.presented()->find(editor::control::parameter_search_toggle)->selected,
        "Parameter search must start collapsed with a visible inactive magnifier.");
    const auto closed_properties=ui.presented()->find(editor::control::parameter_list)->bounds;
    workspace.parameter_search_visible=true;publish();
    const auto first=*workspace.ui_statistics("Parameters");
    require(first.binding_evaluations>0 && first.node_creations>0,"UI component did not evaluate or create nodes.");
    for (const auto key:{ui::ControlKey{50},ui::ControlKey{75},editor::control::parameter_path,editor::control::parameter_back,
             editor::control::parameter_forward,ui::ControlKey{51},ui::ControlKey{52},
             editor::control::parameter_search,editor::control::parameter_list,
             editor::control::position_fold,ui::ControlKey{70},ui::ControlKey{73}})
        require(ui.presented()->find(key),"The composed Parameters region lost a stable host key alias.");
    require(workspace.ui_statistics("SceneViewport").has_value() &&
        ui.presented()->find(editor::control::scene_shading) && ui.presented()->find(editor::control::scene_guides),
        "The compact viewport menus did not come from their Source/JIT region.");
    const auto search=ui.presented()->find(editor::control::parameter_search)->bounds;
    const auto header=ui.presented()->find(51)->bounds;
    const auto properties=ui.presented()->find(editor::control::parameter_list)->bounds;
    require(search.y>=header.y+header.height-.1f && search.y+search.height<=properties.y+.1f,
        "The shared Parameters Filter did not precede its continuous property list.");
    require(properties.y>closed_properties.y && ui.presented()->find(editor::control::parameter_search_toggle)->selected,
        "Opening search did not reserve space above properties or select its magnifier.");
    require(!ui.presented()->find(editor::control::add_component),"Parameters retained a structural Add Component action.");
    require(ui.presented()->find(workspace.reset_control(root)),"Position lost its typed group-reset binding.");
    require(ui.presented()->find(workspace.name_control(entity)),"ObjectHeader lost the typed Entity name binding.");
    require(workspace.ui_statistics("ToolShelf").has_value() && !workspace.ui_statistics("ToolShelfTools"),
        "Only the tool buttons, rather than the complete shelf, came from the Source/JIT region.");
    const auto shelf_first=*workspace.ui_statistics("ToolShelf");
    const auto in_shelf=[&](ui::ControlKey key) {
        while (key) {
            if (key==editor::control::shelf) return true;
            const auto at=std::ranges::find(controls,key,&ui::Control::key);
            if (at==controls.end()) break;
            key=at->parent;
        }
        return false;
    };
    const auto shelf_nodes=std::ranges::count_if(controls,[&](const auto& control){return in_shelf(control.key);});
    require(shelf_first.node_creations==static_cast<std::uint64_t>(shelf_nodes),
        "ToolShelf's Source node statistics did not account for the complete shelf subtree.");
    struct ShelfIdentity {ui::ControlKey target,key;editor::CommandGroup group;};
    std::vector<ShelfIdentity> tools;
    for (const auto& definition:editor::command_definitions) {
        if (definition.group!=editor::CommandGroup::create && definition.group!=editor::CommandGroup::view) continue;
        if (definition.group==editor::CommandGroup::view && definition.shelf_title.empty()) continue;
        const auto target=static_cast<ui::ControlKey>(definition.id);
        const auto tool=std::ranges::find(controls,target,&ui::Control::action_target);
        require(tool!=controls.end() && std::ranges::count(controls,target,&ui::Control::action_target)==1 &&
            in_shelf(tool->key) && tool->key!=target,"The complete DSL shelf lost or duplicated a command route.");
        tools.push_back({target,tool->key,definition.group});
    }
    for (const auto page:{editor::control::shelf_view,editor::control::shelf_create}) {
        workspace.shelf_page=page;publish();
        for (const auto& tool:tools) {
            const auto item=std::ranges::find(controls,tool.key,&ui::Control::key);
            const bool visible=tool.group==(page==editor::control::shelf_create?editor::CommandGroup::create:editor::CommandGroup::view);
            require(item!=controls.end() && item->action_target==tool.target && (ui.presented()->find(tool.key)!=nullptr)==visible,
                "Shelf category switching recreated its tools or published a hidden category.");
        }
        const auto current=*workspace.ui_statistics("ToolShelf");
        require(current.instance==shelf_first.instance && current.node_creations==shelf_first.node_creations &&
            current.scope_creations==shelf_first.scope_creations && current.scope_retirements==shelf_first.scope_retirements,
            "Shelf category switching changed Source Context or control lifetimes.");
    }
    const auto x=workspace.field_control(root,document.parameters()[0].path);
    require(x && ui.presented()->find(x),"The generated NumberRow lost the real typed property binding.");
    const auto incarnation=ui.presented()->find(x)->incarnation;
    publish();
    require(workspace.ui_statistics("Parameters")->binding_evaluations==first.binding_evaluations &&
        workspace.ui_statistics("Parameters")->node_creations==first.node_creations,"A stable UI projection reevaluated bindings or recreated nodes.");
    const auto shelf_before=*workspace.ui_statistics("ToolShelf");
    take(document.begin_edit(root,document.parameters()[0].path,x));
    take(document.preview_edit(x,1));take(document.preview_edit(x,2));take(document.preview_edit(x,3));take(document.commit_edit(x));
    publish();
    const auto edited=*workspace.ui_statistics("Parameters");
    require(edited.binding_evaluations>first.binding_evaluations && edited.instance==first.instance &&
        edited.node_creations==first.node_creations && edited.scope_creations==first.scope_creations &&
        edited.scope_retirements==first.scope_retirements && ui.presented()->find(x)->incarnation==incarnation,
        "A property batch failed to update the composed bindings or replaced live field/Context identities.");
    require(workspace.ui_statistics("ToolShelf")->binding_evaluations==shelf_before.binding_evaluations,
        "A World field change reevaluated the unrelated tool shelf.");
    const auto value_control=std::ranges::find_if(controls,[&](const auto& item){return item.key==x;});
    require(value_control!=controls.end() && value_control->value==3,"Source/JIT edit binding did not publish the actual World value.");
    publish();require(workspace.ui_statistics("Parameters")->binding_evaluations==edited.binding_evaluations,
        "A settled property batch kept reevaluating composed bindings.");
    changed(document.undo());publish();require(value(document,root)==0,"Declarative property undo lost the model value.");
    changed(document.redo());publish();require(value(document,root)==3,"Declarative property redo lost the model value.");
    const auto box=ui.presented()->find(editor::control::position_fold)->bounds;
    const auto route=[&](ui::Reply reply) {
        for (const auto& effect:reply.effects) if (const auto* action=std::get_if<ui::Action>(&effect)) take(workspace.apply_ui(*action));
    };
    route(ui.input({.kind=ui::EventKind::pointer_down,.x=box.x+box.width*.5f,.y=box.y+box.height*.5f}));
    route(ui.input({.kind=ui::EventKind::pointer_up,.x=box.x+box.width*.5f,.y=box.y+box.height*.5f}));
    publish();
    require(!ui.presented()->find(editor::control::position_fold)->selected && !ui.presented()->find(x),
        "The Source event did not retain local state or collapse its actual content.");
    const auto collapsed=*workspace.ui_statistics("Parameters");publish();
    require(workspace.ui_statistics("Parameters")->binding_evaluations==collapsed.binding_evaluations,
        "Local state was reinitialized during an ordinary refresh.");
    take(workspace.apply_ui({ui::ActionKind::activate,editor::control::position_fold,true}));publish();
    const auto empty_state=[&] {
        return std::ranges::any_of(controls,[&](const auto& control) {
            if (control.text.empty() || !std::ranges::contains(control.classes,std::string{"empty-state"}) ||
                !ui.presented()->find(control.key)) return false;
            auto parent=control.parent;
            while (parent) {
                if (parent==50) return true;
                const auto at=std::ranges::find(controls,parent,&ui::Control::key);
                if (at==controls.end()) break;
                parent=at->parent;
            }
            return false;
        });
    };
    const auto inspection_history=document.undo_count();
    const auto root_group=std::ranges::find_if(controls,[&](const auto& control) {
        const auto* binding=workspace.binding(control.key);
        return binding && binding->kind==editor::Workspace::Binding::Kind::property_group && binding->object==component(document,entity,document.presentation_types().static_mesh_component);
    });
    require(root_group!=controls.end(),"The mesh capability has no property group.");
    const auto root_group_key=root_group->key;
    workspace.query=L"staticmesh";publish();
    require(ui.presented()->find(root_group_key) && !ui.presented()->find(x),
        "The shared Filter failed to match the inspected component's property group.");
    workspace.query=L"no-such-property";publish();
    require(!ui.presented()->find(root_group_key) && empty_state() && ui.presented()->find(editor::control::parameter_search),
        "Unmatched components lost their empty state or retained rows.");
    require(!ui.presented()->find(x) && !ui.presented()->find(editor::control::position_fold) && empty_state(),
        "An unmatched query retained a fixed transform section.");
    workspace.parameter_search_visible=false;publish();
    require(!ui.presented()->find(editor::control::parameter_search) && ui.presented()->find(x) &&
        workspace.query==L"no-such-property" && ui.presented()->find(editor::control::parameter_list)->bounds==closed_properties,
        "Closing search retained invisible filtering, erased its query, or failed to reclaim the row.");
    workspace.parameter_search_visible=true;publish();
    require(!ui.presented()->find(x) && ui.presented()->find(editor::control::parameter_search),
        "Reopening search lost its retained query or failed to reapply it.");
    workspace.query.clear();publish();
    require(workspace.field_control(root,document.parameters()[0].path)==x && ui.presented()->find(x) &&
        document.undo_count()==inspection_history,"Clearing Filter changed the World field key or document history.");
    changed(document.add_entity());publish();
    require(!ui.presented()->find(editor::control::position_fold) && !ui.presented()->find(x) && empty_state() &&
        !ui.presented()->find(editor::control::add_component),"An Entity without a root acquired assumed transform or structure controls.");
    changed(document.undo());take(document.select(entity));publish();
    require(ui.presented()->find(editor::control::position_fold) && ui.presented()->find(x),
        "Returning to a rooted Entity did not expose its actual spatial properties.");
    take(document.select({}));publish();
    require(!ui.presented()->find(editor::control::position_fold) && !ui.presented()->find(editor::control::parameter_search) &&
        !ui.presented()->find(x) && empty_state(),
        "No selection retained Entity-specific controls or lost its neutral empty state.");
    take(document.select(entity));publish();
    require(workspace.field_control(root,document.parameters()[0].path)==x && ui.presented()->find(x) &&
        document.undo_count()==inspection_history,"Object inspection availability changed World field routing or scene history.");
    changed(document.create_static_mesh(editor::mesh_assets()[1]));
    const auto other=document.selection().entity;const auto other_root=other;
    publish();
    const auto other_x=workspace.field_control(other_root,document.parameters()[0].path);
    require(other_x!=x && !ui.presented()->find(x) && ui.presented()->find(other_x),"Changing the selected Entity reused an old field route.");
    take(document.begin_edit(root,document.parameters()[0].path,9999));take(document.preview_edit(9999,9));take(document.commit_edit(9999));
    const auto selected_evaluations=workspace.ui_statistics("Parameters")->binding_evaluations;publish();
    require(workspace.ui_statistics("Parameters")->binding_evaluations==selected_evaluations,"An old Entity invalidated the new Inspector.");
    // Exercise a World lifecycle change outside the editor's history writer.
    auto& external_world=const_cast<world::World&>(document.world());
    auto removal=take(external_world.detach(other));publish();
    require(!ui.presented()->find(other_x) && !workspace.field_control(other_root,document.parameters()[0].path),
        "An externally detached Entity remained an Inspector target.");
    const auto detached=*workspace.ui_statistics("Parameters");publish();
    require(workspace.ui_statistics("Parameters")->node_creations==detached.node_creations &&
        workspace.ui_statistics("Parameters")->binding_evaluations==detached.binding_evaluations,
        "A detached selection kept retiring and recreating its field routes.");
    changed(external_world.restore(removal));publish();
    const auto restored_x=workspace.field_control(other_root,document.parameters()[0].path);
    require(restored_x && restored_x!=other_x && ui.presented()->find(restored_x),
        "Restoring World membership failed to rebind the selected Entity with fresh field keys.");
    changed(document.erase_selected());publish();
    require(!ui.presented()->find(restored_x),"Deleting the selected Entity kept a live field route.");
    // Exercise the remaining editor regions through the real authoring file,
    // accepted hit geometry and native workbench/dock state owners.
    require(workspace.ui_statistics("EditorShell") && workspace.ui_statistics("DockWorkspace") && workspace.ui_statistics("SceneTree"),
        "Editor chrome or pane navigation did not originate in the authoring document.");
    require(workspace.dock.select(editor::control::workbench),"Cannot expose authored Controls.");publish();
    const auto sample_instance=workspace.ui_statistics("Controls")->instance;
    const auto click_sample=[&](ui::ControlKey key) {
        const auto* box=ui.presented()->find(key);require(box && box->enabled,"Missing live sample control.");
        const float x=box->bounds.x+box->bounds.width*.5f,y=box->bounds.y+box->bounds.height*.5f;
        static_cast<void>(ui.input({.kind=ui::EventKind::pointer_down,.x=x,.y=y}));
        const auto reply=ui.input({.kind=ui::EventKind::pointer_up,.x=x,.y=y});bool handled{};
        for (const auto& effect:reply.effects) if (const auto* action=std::get_if<ui::Action>(&effect)) handled|=workspace.workbench.apply(*action);
        require(handled,"Authored Controls lost its accepted native action route.");publish();
    };
    click_sample(editor::workbench_control::disable);
    require(!ui.presented()->find(editor::workbench_control::position_x)->enabled,"Checkbox did not disable the authored parameter body.");
    click_sample(editor::workbench_control::disable);
    click_sample(editor::workbench_control::long_labels);
    require(std::ranges::find(controls,std::wstring{L"Local position in metres"},&ui::Control::text)!=controls.end(),"Long parameter labels were lost in migration.");
    click_sample(editor::workbench_control::appearance_page);
    require(ui.presented()->find(editor::workbench_control::red),"Authored RGB tuple did not mount.");
    click_sample(editor::workbench_control::channels_page);
    require(ui.presented()->find(480) && ui.presented()->find(481) && ui.presented()->find(482),"Authored channel sliders did not mount.");
    click_sample(editor::workbench_control::rgb_page);
    click_sample(editor::workbench_control::general_page);
    require(workspace.dock.move(editor::control::workbench,editor::control::scene_tab,ui::DockSide::right),"Cannot move the authored pane.");publish();
    require(ui.presented()->find(editor::control::viewport) && ui.presented()->find(editor::workbench_control::position_x) &&
        workspace.ui_statistics("Controls")->instance==sample_instance,"Docking remounted a business scope or lost a native host slot.");
    workspace.dock.maximize(editor::control::workbench);publish();
    require(!ui.presented()->find(editor::control::viewport),"DSL dock maximization kept the other pane interactive.");
    workspace.dock.maximize(editor::control::workbench);publish();
    workspace.reset_layout();publish();
    {
        auto rows=std::make_shared<std::vector<ui_language::Item>>();
        rows->push_back({9100,{std::wstring{L"Grid"},std::wstring{},true,true,false,false,std::wstring{L"checkbox"},ui::ControlKey{},0.0,0.0,std::wstring{L"menu-shortcut"},std::wstring{},false},{{"item",9101}}});
        rows->push_back({9200,{std::wstring{L"Flat"},std::wstring{L"Ctrl+F"},true,true,false,true,std::wstring{L"radio"},ui::ControlKey{9300},0.0,0.0,std::wstring{L"menu-shortcut"},std::wstring{L"Shading"},true},{{"item",9201}}});
        ui::ControlKey next=1ULL<<62;
        const std::array<ui_language::KeyBinding,1> keys{{{"menu",500}}};
        auto popup=take(ui_language::Instance::create(workspace.ui_component("PopupMenu"),0,next,keys,
            std::array<ui_language::InputValue,1>{ui_language::ItemList{rows}}));
        ui::Session surface(fonts);require(surface.set_surface({{600,500},1,1}).has_value(),"Popup surface failed.");
        require(surface.set_controls(popup->controls()).has_value(),"Authored popup descriptors were rejected.");popup->commit_controls();
        auto frame=surface.prepare();require(frame.has_value() && surface.accept(*frame),"Authored popup did not draw.");
        require((*frame)->find(9101)->menu_behavior==ui::MenuBehavior::checkbox && (*frame)->find(9201)->radio_group==9300,
            "Menu DSL lost checked/radio semantics.");
        const auto box=(*frame)->find(9101)->bounds;
        static_cast<void>(surface.input({.kind=ui::EventKind::pointer_down,.x=box.x+8,.y=box.y+8}));
        const auto reply=surface.input({.kind=ui::EventKind::pointer_up,.x=box.x+8,.y=box.y+8});
        require(std::ranges::any_of(reply.effects,[](const auto& effect){const auto* action=std::get_if<ui::Action>(&effect);
            return action && action->key==9101 && action->kind==ui::ActionKind::activate && std::get_if<bool>(&action->value) && !std::get<bool>(action->value);}),
            "Authored menu failed to request the unchecked value through native input.");
        require(surface.set_controls({}).has_value(),"Popup retirement failed.");popup->close();
    }
    std::puts("Editor shell DSL acceptance: PASS (root, native slots, keyed dock reparent/maximize, Controls checkbox/tabs/sliders, popup check/radio)");
    const auto source=take(editor::read_file(editor::asset_directory()/"editor.ui.source"));
    auto invalid=source;const auto expression=invalid.find("selected: state.expanded;");
    require(expression!=std::string::npos,"Cannot locate the diagnostic fixture.");
    invalid.replace(expression,std::string_view("selected: state.expanded;").size(),"selected: state.expanded = true;");
    auto rejected=ui_language::Module::compile(runtime,"invalid-ui.source",invalid);
    require(!rejected && rejected.error().message.find("invalid-ui.source:")!=std::string::npos &&
        rejected.error().message.find("Only event blocks")!=std::string::npos,"Illegal binding effects lacked a source diagnostic.");
    auto text_error=source;const auto selected=text_error.find("selected: state.expanded;");
    text_error.replace(selected,std::string_view("selected: state.expanded;").size(),"selected: 42;");
    rejected=ui_language::Module::compile(runtime,"typed-ui.source",text_error);
    require(!rejected && rejected.error().message.find("typed-ui.source:")!=std::string::npos,
        "UI expression type errors bypassed the ordinary Source compiler or lost their source location.");
    constexpr auto missing_member="ui Broken(in bool active) {\n  Text label {\n    collapsed: input.;\n  }\n}";
    rejected=ui_language::Module::compile(runtime,"member-ui.source",missing_member);
    require(!rejected && rejected.error().message.starts_with("member-ui.source:3:22:"),
        "Fragment syntax errors lost their original declaration line/column.");
    rejected=ui_language::Module::compile(runtime,"reserved-ui.source",
        "double scale(double input) { return input; }\nui Reserved() { Text label { text: \"Test\"; } }");
    require(!rejected && rejected.error().message.find("reserved UI identifiers")!=std::string::npos,
        "Reserved UI context identifiers lacked a useful diagnostic in helpers.");
    verify_dynamic_ui_reads(document,root);
    verify_ui_boundary_validation(document,root,ui);
    auto cleared=ui.set_controls({});require(cleared.has_value(),"UI scope unmount failed.");
    workspace.reset_document();
    require(!workspace.parameter_search_visible && workspace.query.empty(),"A new document inherited parameter search state.");
    take(document.close());
    std::puts("Declarative Parameters acceptance: PASS (real Source/JIT, typed World editing, sparse bindings, flat properties/shared filter, state/input, rebinding, unmount, diagnostics)");
}
