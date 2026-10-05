void verify_dynamic_ui_reads(editor::Session& document,ObjectId root) {
    constexpr auto declaration=R"(
double chosen() {
    if (input.choose_left) { return input.left; }
    return input.right;
}
ui PositionRead(in bool choose_left, in double left, in double right) {
    Text position { text: "Runtime position"; width: 128.0d + chosen(); }
}
)";
    auto component=take(ui_language::Component::compile(document.world().runtime(),"position-read.ui.source",declaration));
    bool choose_left=true;
    const auto inputs=[&] { return std::array<ui_language::InputValue,3>{choose_left,
        static_cast<double>(value(document,root,0)),static_cast<double>(value(document,root,1))}; };
    ui::ControlKey next_key=1;
    auto instance=take(ui_language::Instance::create(component,0,next_key,{},inputs()));
    ui::Session surface;
    require(surface.set_surface({{640,480},1,1}).has_value(),"Dynamic UI read surface failed.");
    const auto publish=[&] {
        take(instance->publish(inputs()));take(instance->update());
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
    auto empty_component=take(ui_language::Component::compile(document.world().runtime(),"empty.ui.source","ui Empty() { Column root {} }"));
    auto empty=take(ui_language::Instance::create(empty_component,0,next_key,{},{}));
    require(empty->controls().size()==1 && empty->statistics().node_creations==1 && empty->statistics().binding_evaluations==0,
        "A component without bindings failed to publish its static structure.");
    take(empty->update());require(empty->statistics().publications==1,"A stable static component was published again.");

    auto loop_component=take(ui_language::Component::compile(document.world().runtime(),"loop.ui.source",R"(
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
    auto component=take(ui_language::Component::compile(document.world().runtime(),"boundary.ui.source",
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
        take(instance->publish(std::array<ui_language::InputValue,1>{input}));
        const auto rejected=instance->update();
        require(!rejected && std::ranges::equal(instance->controls(),previous),
            "Invalid EditBinding escaped Result or replaced the last valid component snapshot.");
        draw();
        take(instance->publish(std::array<ui_language::InputValue,1>{valid}));take(instance->update());draw();
    }
    const std::array bodies{std::string{"text: \"A\\0B\";"},std::string{"classes: \"1foo\";"},
        std::string{"classes: \""}+std::string(65,'x')+"\";",std::string{"classes: \"a b c d e f g h i j k l m n o p q\";"}};
    for (const auto& body:bodies) {
        auto invalid_component=take(ui_language::Component::compile(document.world().runtime(),"invalid-properties.ui.source",
            "ui Invalid() { Text label { "+body+" } }"));
        const auto rejected=ui_language::Instance::create(invalid_component,0,next_key,{},{});
        require(!rejected && rejected.error().message.starts_with("invalid-properties.ui.source:"),
            "Invalid text/style reached Session instead of component diagnostics.");
    }
    auto overflow=take(ui_language::Component::compile(document.world().runtime(),"row-classes.ui.source",
        "ui Invalid(in EditBinding data) { NumberRow row { classes: \"a b c d e f g h i j k l m n o p\"; edit: input.data; } }"));
    require(!ui_language::Instance::create(overflow,0,next_key,{},std::array<ui_language::InputValue,1>{valid}),
        "NumberRow's built-in class was omitted from the final style limit.");
    require(surface.set_controls({}).has_value(),"Boundary component unmount failed.");
}

// Real document -> Source/JIT component -> shared UI/layout and input -> document.
// This is the same asset and Workspace path used by the windowed editor.
void verify_declarative_parameters() {
    editor::Session document;
    editor::Workspace workspace;
    const std::array fonts{(editor::asset_directory()/"fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session ui(fonts);
    auto theme=ui::StyleSheet::parse(take(editor::read_file(editor::asset_directory()/"editor.ss")));
    require(theme.has_value() && ui.set_style_sheet(*theme).has_value(),"Declarative Parameters theme failed.");
    changed(document.create_static_mesh(editor::mesh_assets().front()));
    const auto actor=document.selected();
    const auto root=*take(world::root_component(document.world().runtime(),actor));
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
        if (!frame) throw std::runtime_error("Declarative parameter layout/draw failed at control "+std::to_string(frame.error().key));
        require(ui.accept(*frame),"Declarative parameter frame was rejected.");
    };
    publish();
    require(workspace.ui_statistics("Position").has_value(),"Parameters did not create the Source/JIT component.");
    const auto first=*workspace.ui_statistics("Position");
    require(first.binding_evaluations>0 && first.node_creations>0,"UI component did not evaluate or create nodes.");
    const auto x=workspace.field_control(root,document.parameters()[0].path);
    require(x && ui.presented()->find(x),"The generated NumberRow lost the real typed property binding.");
    const auto incarnation=ui.presented()->find(x)->incarnation;
    publish();
    require(workspace.ui_statistics("Position")->binding_evaluations==first.binding_evaluations &&
        workspace.ui_statistics("Position")->node_creations==first.node_creations,"A stable UI projection reevaluated bindings or recreated nodes.");
    take(document.begin_edit(root,document.parameters()[0].path,x));
    take(document.preview_edit(x,1));take(document.preview_edit(x,2));take(document.preview_edit(x,3));take(document.commit_edit(x));
    publish();
    require(workspace.ui_statistics("Position")->binding_evaluations==first.binding_evaluations+1 &&
        ui.presented()->find(x)->incarnation==incarnation,"A property batch reevaluated unrelated bindings or replaced the field identity.");
    const auto value_control=std::ranges::find_if(controls,[&](const auto& item){return item.key==x;});
    require(value_control!=controls.end() && value_control->value==3,"Source/JIT edit binding did not publish the actual World value.");
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
    const auto collapsed=*workspace.ui_statistics("Position");publish();
    require(workspace.ui_statistics("Position")->binding_evaluations==collapsed.binding_evaluations,
        "Local state was reinitialized during an ordinary refresh.");
    take(workspace.apply_ui({ui::ActionKind::activate,editor::control::position_fold,true}));publish();
    changed(document.create_static_mesh(editor::mesh_assets()[1]));
    const auto other=document.selected();const auto other_root=*take(world::root_component(document.world().runtime(),other));
    publish();
    const auto other_x=workspace.field_control(other_root,document.parameters()[0].path);
    require(other_x!=x && !ui.presented()->find(x) && ui.presented()->find(other_x),"Changing the selected Actor reused an old field route.");
    take(document.begin_edit(root,document.parameters()[0].path,9999));take(document.preview_edit(9999,9));take(document.commit_edit(9999));
    const auto selected_evaluations=workspace.ui_statistics("Position")->binding_evaluations;publish();
    require(workspace.ui_statistics("Position")->binding_evaluations==selected_evaluations,"An old Actor invalidated the new Inspector.");
    // Exercise a World lifecycle change outside the editor's history writer.
    auto& external_world=const_cast<world::World&>(document.world());
    auto removal=take(external_world.detach(other));publish();
    require(!ui.presented()->find(other_x) && !workspace.field_control(other_root,document.parameters()[0].path),
        "An externally detached Actor remained an Inspector target.");
    const auto detached=*workspace.ui_statistics("Position");publish();
    require(workspace.ui_statistics("Position")->node_creations==detached.node_creations &&
        workspace.ui_statistics("Position")->binding_evaluations==detached.binding_evaluations,
        "A detached selection kept retiring and recreating its field routes.");
    changed(external_world.restore(removal));publish();
    const auto restored_x=workspace.field_control(other_root,document.parameters()[0].path);
    require(restored_x && restored_x!=other_x && ui.presented()->find(restored_x),
        "Restoring World membership failed to rebind the selected Actor with fresh field keys.");
    changed(document.erase_selected());publish();
    require(!ui.presented()->find(restored_x),"Deleting the selected Actor kept a live field route.");
    const auto source=take(editor::read_file(editor::asset_directory()/"editor.ui.source"));
    auto invalid=source;const auto expression=invalid.find("selected: state.expanded;");
    require(expression!=std::string::npos,"Cannot locate the diagnostic fixture.");
    invalid.replace(expression,std::string_view("selected: state.expanded;").size(),"selected: state.expanded = true;");
    auto rejected=ui_language::Module::compile(document.world().runtime(),"invalid-ui.source",invalid);
    require(!rejected && rejected.error().message.find("invalid-ui.source:")!=std::string::npos &&
        rejected.error().message.find("Only event blocks")!=std::string::npos,"Illegal binding effects lacked a source diagnostic.");
    auto text_error=source;const auto selected=text_error.find("selected: state.expanded;");
    text_error.replace(selected,std::string_view("selected: state.expanded;").size(),"selected: 42;");
    rejected=ui_language::Module::compile(document.world().runtime(),"typed-ui.source",text_error);
    require(!rejected && rejected.error().message.find("typed-ui.source:")!=std::string::npos,
        "UI expression type errors bypassed the ordinary Source compiler or lost their source location.");
    constexpr auto missing_member="ui Broken(in bool active) {\n  Text label {\n    collapsed: input.;\n  }\n}";
    rejected=ui_language::Module::compile(document.world().runtime(),"member-ui.source",missing_member);
    require(!rejected && rejected.error().message.starts_with("member-ui.source:3:22:"),
        "Fragment syntax errors lost their original declaration line/column.");
    rejected=ui_language::Module::compile(document.world().runtime(),"reserved-ui.source",
        "double scale(double input) { return input; }\nui Reserved() { Text label { text: \"Test\"; } }");
    require(!rejected && rejected.error().message.find("reserved UI identifiers")!=std::string::npos,
        "Reserved UI context identifiers lacked a useful diagnostic in helpers.");
    verify_dynamic_ui_reads(document,root);
    verify_ui_boundary_validation(document,root,ui);
    auto cleared=ui.set_controls({});require(cleared.has_value(),"UI scope unmount failed.");
    workspace.reset_document();take(document.close());
    std::puts("Declarative Parameters acceptance: PASS (real Source/JIT, typed World editing, sparse bindings, state/input, rebinding, unmount, diagnostics)");
}
