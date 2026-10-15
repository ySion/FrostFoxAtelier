// Native metadata -> real editor document -> editor.ui.source -> accepted UI
// geometry and delayed help. This path needs no GPU or desktop input injection.
void verify_property_tooltips() {
    editor::Session document;
    changed(document.create_static_mesh(editor::mesh_assets().front()));
    const auto mesh=document.selection().entity;
    changed(document.create_light(world::LightKind::point));
    const auto light=component(document,document.selection().entity,document.types().light_component);
    take(document.select(mesh));document.mark_saved();
    const auto history=document.undo_count(),redo=document.redo_count();
    const auto lighting=document.world().lighting_settings();
    auto& runtime=document.world().runtime();
    const auto* mesh_type=take(runtime.describe(take(runtime.type_of(mesh))));
    const auto field=[&](TypeId type,std::string_view name)->const FieldInfo& {
        const auto* info=take(runtime.describe(type));
        const auto at=std::ranges::find(info->fields,name,&FieldInfo::name);
        require(at!=info->fields.end(),"Tooltip acceptance field is absent from Runtime.");return *at;
    };
    const auto& position=field(mesh_type->id,"position");
    const auto& rotation=field(mesh_type->id,"rotation");
    const auto& color=field(document.types().light_settings,"color");
    require(!position.description.empty() && !rotation.description.empty() && !color.description.empty(),
        "Real editor authoring fields have no native descriptions.");

    editor::Workspace workspace;
    const std::array fonts{(editor::asset_directory()/"fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface(fonts);
    const auto theme=ui::StyleSheet::parse(take(editor::read_file(editor::asset_directory()/"editor.ss")));
    require(theme && surface.set_style_sheet(*theme) && surface.set_surface({{1600,1000},1,1}),
        "Property tooltip editor surface failed.");
    std::vector<ui::Control> controls;
    const auto publish=[&] {
        controls=workspace.describe(document,{1600,1000},surface.presented().get());
        require(workspace.ui_error().empty(),"Property tooltip production DSL failed.");
        auto described=surface.set_controls(controls);
        require(described.has_value(),"Property tooltip descriptors were rejected.");
        for(const auto& effect:described->effects) if(const auto* action=std::get_if<ui::Action>(&effect)) {
            if(action->kind==ui::ActionKind::edit_cancel) take(document.cancel_edit());
            else take(workspace.apply_ui(*action));
        }
        workspace.commit_ui_controls();
        auto frame=surface.prepare();
        require(frame && surface.accept(*frame),"Property tooltip frame failed to prepare or accept.");
    };
    const auto label=[&](std::wstring_view text)->ui::ControlKey {
        const auto at=std::ranges::find_if(controls,[&](const auto& control) {
            return control.kind==ui::ControlKind::label && control.text==text && surface.presented()->find(control.key);
        });
        require(at!=controls.end(),"Property tooltip label is absent from accepted layout.");return at->key;
    };
    const auto help_for=[&](ui::ControlKey key)->std::shared_ptr<const ui::Tooltip> {
        for(auto* box=surface.presented()->find(key);box;box=box->parent?surface.presented()->find(box->parent):nullptr)
            if(box->tooltip) return box->tooltip;
        throw std::runtime_error("Property tooltip did not reach its accepted label or ancestors.");
    };
    const auto hover=[&](ui::ControlKey key) {
        const auto* box=surface.presented()->find(key);
        require(box!=nullptr,"Property tooltip hover target disappeared.");
        const auto left=std::max(box->bounds.x,float(box->clip.left));
        const auto right=std::min(box->bounds.x+box->bounds.width,float(box->clip.right));
        const auto top=std::max(box->bounds.y,float(box->clip.top));
        const auto bottom=std::min(box->bounds.y+box->bounds.height,float(box->clip.bottom));
        require(right>left && bottom>top,"Property tooltip label is clipped.");
        const auto expected=help_for(key);
        const auto reply=surface.input({.kind=ui::EventKind::pointer_move,.x=(left+right)*.5f,.y=(top+bottom)*.5f});
        require(!reply.error,"Property tooltip pointer input failed.");
        Sleep(520);publish();
        require(surface.presented()->tooltip && surface.presented()->tooltip->content==expected,
            "Hovering the real property label did not show its help after the delay.");
        return surface.presented();
    };
    publish();
    const auto position_key=label(L"Position (m)"),rotation_key=label(L"Rotation (°)");
    const auto position_help=help_for(position_key),rotation_help=help_for(rotation_key);
    require(position_help->title==L"Position (m)" && position_help->description==editor::wide(position.description) &&
        position_help->identity==editor::wide(mesh_type->name)+L".position : "+editor::wide(take(runtime.describe(position.value.type))->name),
        "Default Transform Position help differs from its Runtime aggregate field.");
    require(rotation_help->description==editor::wide(rotation.description) &&
        rotation_help->identity==editor::wide(mesh_type->name)+L".rotation : "+editor::wide(take(runtime.describe(rotation.value.type))->name)+L" · Euler (°)",
        "Rotation help lost its Quaternion storage type or displayed Euler unit.");
    const auto type_help=help_for(52);
    require(type_help->identity==editor::wide(mesh_type->name) && type_help->description==editor::wide(mesh_type->description),
        "Object header help differs from the actual Runtime type.");
    const auto position_frame=hover(position_key);
    publish();
    require(surface.presented()->tooltip && surface.presented()->tooltip->content==position_help,
        "Recomposing an unchanged document restarted or replaced its visible help.");

    take(document.select(light));publish();
    require(!surface.presented()->tooltip && !surface.accept(position_frame),
        "Switching inspected objects retained or accepted an old tooltip frame.");
    const auto color_help=help_for(label(L"Color"));
    const auto* light_type=take(runtime.describe(take(runtime.type_of(light))));
    require(color_help->description==editor::wide(color.description) &&
        color_help->identity==editor::wide(light_type->name)+L".settings.color : "+editor::wide(take(runtime.describe(color.value.type))->name),
        "Light Color help used a scalar lane identity or lost its aggregate description.");
    take(document.select(ObjectId{}));publish();
    require(!surface.presented()->tooltip && !surface.accept(position_frame) &&
        std::ranges::none_of(surface.presented()->controls,[&](const auto& control) {
            return control.tooltip==position_help || control.tooltip==color_help;
        }),"Empty selection retained the previous object's property help.");

    workspace.dock.select(editor::world_control::tab);publish();
    const auto* exposure=editor::view_option(editor::ViewOption::exposure_ev);
    require(exposure!=nullptr,"World exposure option metadata is missing.");
    const auto exposure_key=label(editor::wide(exposure->label));
    const auto exposure_help=help_for(exposure_key);
    require(exposure_help->title==editor::wide(exposure->label) &&
        exposure_help->description==editor::wide(exposure->description) &&
        exposure_help->identity.find(editor::wide(exposure->name))!=std::wstring::npos && exposure_help->identity.starts_with(L"Scene · "),
        "World property help diverged from the shared ViewOption descriptor.");
    const auto world_frame=hover(exposure_key);
    workspace.dock.select(editor::control::parameters_tab);publish();
    require(!surface.presented()->tooltip && !surface.accept(world_frame),
        "Leaving World settings for an empty Parameters page retained its help.");
    require(!document.dirty() && document.undo_count()==history && document.redo_count()==redo &&
        document.world().lighting_settings()==lighting,
        "Reading parameter or World help changed the document or its history.");
    std::puts("Editor property tooltip acceptance: PASS (real Runtime/Session/DSL layout, Transform/type and Light Color metadata, delayed label hover, World metadata, stale-target retirement and history isolation)");
}
