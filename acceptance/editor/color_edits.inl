// Exercise the same document transactions used by the color popup, including
// real material assets, component instances and their existing save/history paths.
void verify_color_edits() {
    const auto root=std::filesystem::absolute("out/color-edits-"+std::to_string(GetCurrentProcessId())+"-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto project=take(editor::Project::create(root/"Project"));
    editor::Session document;
    auto& runtime=document.world().runtime();
    const auto paths=[&](ObjectId object,std::string_view prefix) {
        std::vector<world::FieldPath> result;
        for(const auto& property:take(document.properties(object))) if(property.name.starts_with(prefix)) result.push_back(property.path);
        require(result.size()==3,"Color did not expose three channels.");return result;
    };
    const auto read=[&](ObjectId object,const auto& fields) {
        std::vector<Scalar> result;
        for(const auto& path:fields) result.push_back(take(document.read_property(object,path)));
        return result;
    };
    const std::array first{Scalar{.2f},Scalar{.4f},Scalar{.6f}},last{Scalar{.7f},Scalar{.3f},Scalar{.5f}};
    const std::array invalid_type{Scalar{.8f},Scalar{.9},Scalar{.1f}},invalid_range{Scalar{.8f},Scalar{1e30f},Scalar{.1f}};
    // Material colors are bounded 0–1, so only a non-finite channel is rejected; 1e30 clamps.
    const std::array non_finite{Scalar{.8f},Scalar{std::nanf("")},Scalar{.1f}},clamped_range{Scalar{.8f},Scalar{1.f},Scalar{.1f}};

    changed(document.create_light(presentation::LightKind::point,{0,2,0}));
    const auto light=take(world::components(runtime,document.selection().entity)).front().id();
    const auto light_paths=paths(light,"settings.color.");
    const auto original=read(light,light_paths);const auto light_history=document.undo_count();
    document.mark_saved();
    take(document.begin_group_edit(light,light_paths,9001));
    require(document.dirty() && document.editing(),"An active color group was omitted from scene dirty state.");
    take(document.preview_group_edit(9001,first));
    require(!document.preview_group_edit(9001,invalid_type) && read(light,light_paths)==std::vector<Scalar>(first.begin(),first.end()),
        "A rejected group type changed an earlier channel.");
    require(!document.preview_group_edit(9001,invalid_range) && read(light,light_paths)==std::vector<Scalar>(first.begin(),first.end()),
        "A rejected group value changed an earlier channel.");
    take(document.cancel_edit());
    require(!document.dirty() && read(light,light_paths)==original && document.undo_count()==light_history,
        "Cancel did not restore the color and saved state.");
    take(document.begin_group_edit(light,light_paths,9002));take(document.preview_group_edit(9002,first));
    take(document.preview_group_edit(9002,last));take(document.commit_edit(9002));
    require(document.undo_count()==light_history+1,"Color preview created more than one history entry.");
    changed(document.undo());require(read(light,light_paths)==original && !document.dirty(),"Color undo missed its saved value.");
    changed(document.redo());require(read(light,light_paths)==std::vector<Scalar>(last.begin(),last.end()),"Color redo missed its final value.");

    const auto key=take(project.create_material("Content/Materials/Color.material"));
    const auto material=take(document.material_resource(project,key));
    const auto material_paths=paths(material.id(),"parameters.base_color.");
    const auto material_before=read(material.id(),material_paths);
    document.mark_saved();const auto scene_history=document.undo_count();
    take(document.begin_group_edit(material.id(),material_paths,9010));
    require(document.material_dirty(key) && !document.dirty(),"Asset color edited the scene's dirty state.");
    take(document.preview_group_edit(9010,first));
    require(!document.preview_group_edit(9010,non_finite) && read(material.id(),material_paths)==std::vector<Scalar>(first.begin(),first.end()),
        "Rejected material color left a partial preview.");
    take(document.preview_group_edit(9010,invalid_range));
    require(read(material.id(),material_paths)==std::vector<Scalar>(clamped_range.begin(),clamped_range.end()),
        "A material color channel above its hard maximum did not clamp to it.");
    take(document.preview_group_edit(9010,last));take(document.commit_edit(9010));
    require(document.undo_count()==scene_history && !document.dirty(),"Asset color created scene history.");
    take(document.material_undo(key));
    require(read(material.id(),material_paths)==material_before && !document.material_dirty(key),"Asset color did not undo in one step.");
    take(document.material_undo(key,true));take(document.save_material(project,key));
    require(!document.material_dirty(key),"Asset color did not use the material save path.");
    auto opened=take(editor::Project::open(project.root()));editor::Session reloaded;
    const auto reloaded_material=take(reloaded.material_resource(opened,key));
    require(take(material::capture(reloaded.world().runtime(),reloaded_material.id()))->description().parameters.base_color==
        take(material::capture(runtime,material.id()))->description().parameters.base_color,"Saved color did not survive material reopen.");

    changed(document.create_static_mesh(take(project.mesh("builtin/cube"))));
    const auto entity=document.selection().entity;
    const auto mesh=component(document,entity,document.presentation_types().static_mesh_component);
    take(document.assign_material(project,mesh,"slot/0",key));
    take(document.set_material_override(mesh,"slot/0",material_paths.front(),true));
    const auto instance=take(presentation::read_material_bindings(runtime,mesh)).front().resource;
    take(document.set_material_override(mesh,"slot/0",material_paths.front(),false));
    const auto before=take(material::read_material(runtime,instance.id()));
    const auto instance_history=document.undo_count();document.mark_saved();
    take(document.begin_group_edit(instance.id(),material_paths,9020));
    require(!document.preview_group_edit(9020,non_finite),"An instance accepted an invalid color channel.");
    auto state=take(material::read_material(runtime,instance.id()));
    require(state.overrides==before.overrides && state.description==before.description,
        "Rejected instance preview retained a color override or partial channels.");
    take(document.preview_group_edit(9020,first));take(document.cancel_edit());
    state=take(material::read_material(runtime,instance.id()));
    require(state.overrides==before.overrides && state.description==before.description && !document.dirty(),
        "Cancel did not restore inherited instance color.");
    take(document.begin_group_edit(instance.id(),material_paths,9021));take(document.preview_group_edit(9021,first));
    take(document.preview_group_edit(9021,last));take(document.commit_edit(9021));
    require(document.undo_count()==instance_history+1 && document.dirty() && !document.material_dirty(key),
        "Instance color did not use exactly one scene history entry.");
    changed(document.undo());state=take(material::read_material(runtime,instance.id()));
    require(state.overrides==before.overrides && state.description==before.description && !document.dirty(),
        "Instance undo did not restore inheritance.");
    changed(document.redo());
    auto scene=take(editor::Session::deserialize(take(document.serialize()),project));
    ObjectId saved_mesh;
    for(const auto& member:take(scene->world().members())) for(const auto& part:take(world::components(scene->world().runtime(),member.id())))
        if(take(scene->world().runtime().is_a(take(scene->world().runtime().type_of(part.id())),scene->presentation_types().static_mesh_component))) saved_mesh=part.id();
    require(saved_mesh!=ObjectId{} && take(presentation::material_surfaces(scene->world().runtime(),saved_mesh))->front()->description().parameters.base_color==
        take(material::capture(runtime,instance.id()))->description().parameters.base_color,"Instance color did not survive scene roundtrip.");
    std::puts("Editor color transactions: PASS (dirty, reject rollback, cancel, asset/instance history and save)");
}
