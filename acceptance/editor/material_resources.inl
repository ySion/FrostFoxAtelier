// Real Project/Runtime/World/Session/DSL/SceneSnapshot consumer. No mocks or
// desktop input. Renderer quality remains the renderer's actual GPU consumer.
void verify_material_resources() {
    namespace fs=std::filesystem;
    const auto evidence=fs::absolute("out/material-resources-"+std::to_string(GetCurrentProcessId())+"-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto project=take(editor::Project::create(evidence/"Project"));
    std::string bmp(70,'\0');bmp[0]='B';bmp[1]='M';bmp[2]=70;bmp[10]=54;bmp[14]=40;bmp[18]=2;bmp[22]=2;bmp[26]=1;bmp[28]=32;
    for(std::size_t i=54;i<bmp.size();i+=4) {bmp[i+2]=char(255);bmp[i+3]=char(255);}
    take(editor::write_file(evidence/"Red.bmp",bmp));
    const auto texture=take(project.import_texture(evidence/"Red.bmp"));
    const auto key=take(project.create_material("Content/Materials/./Unused/../Stone.material"));
    const auto material_path=project.root()/"Content/Materials/Stone.material";
    auto metadata_path=material_path;metadata_path+=L".asset.json";
    const auto material_bytes=take(editor::read_file(material_path)),metadata_bytes=take(editor::read_file(metadata_path));
    const auto catalog_revision=project.revision();
    require(take(editor::Project::open(project.root())).material(key).has_value(),"Normalized material path did not survive reopening the project.");
    require(!project.create_material("Content/Materials/Stone.material") &&
        !project.create_material("Content/Materials/Unused/../Stone.material"),"Material creation overwrote an existing resource.");
    require(project.revision()==catalog_revision && take(editor::read_file(material_path))==material_bytes &&
        take(editor::read_file(metadata_path))==metadata_bytes && !fs::exists(project.root()/"Content/Materials/Unused"),
        "Rejected material creation changed an existing file, identity or catalog.");
    require(!project.create_material("Content/../Source/Escaped.material") &&
        !project.create_material("Content/../../Escaped.material") && !fs::exists(project.root()/"Source/Escaped.material") &&
        !fs::exists(evidence/"Escaped.material"),"Material path normalization escaped Content.");
    editor::Session document;auto resource=take(document.material_resource(project,key));
    require(take(document.material_resource(project,key)).id()==resource.id(),"A shared material acquired a second Runtime identity.");
    take(document.set_material_texture(project,resource.id(),material::Input::base_color,texture));
    const auto find_property=[&](ObjectId object,std::string_view name) {
        auto properties=take(document.properties(object));const auto at=std::ranges::find(properties,name,&editor::Property::name);
        require(at!=properties.end(),"Material reflection is incomplete.");return *at;
    };
    const auto roughness=find_property(resource.id(),"parameters.roughness");
    const auto red=find_property(resource.id(),"parameters.base_color.x");
    take(document.set_property(resource.id(),roughness.path,Scalar{.4f},110));
    require(document.material_dirty(key) && !document.dirty(),"Asset editing polluted scene dirty state.");
    take(document.save_material(project,key));
    auto& runtime=document.world().runtime();
    const auto saved_state=take(material::read_material(runtime,resource.id()));
    const auto saved_surface=take(material::capture(runtime,resource.id()));
    auto transient_texture=take(material::create_texture(runtime,{},take(project.texture(texture)).image));
    auto invalid_state=saved_state;invalid_state.textures[0]=std::move(transient_texture);invalid_state.description.parameters.roughness=.2f;
    require(!material::update_material(runtime,resource.id(),invalid_state),"A persistent material accepted a texture without a persistent source.");
    const auto retained_state=take(material::read_material(runtime,resource.id()));
    require(take(material::capture(runtime,resource.id()))==saved_surface && retained_state.description==saved_state.description &&
        retained_state.textures[0].reference()==saved_state.textures[0].reference() &&
        take(document.material_asset(resource.id())).textures[0]==texture && !document.material_dirty(key),
        "Rejected texture binding changed the saved material, references or published surface.");
    take(document.begin_edit(resource.id(),roughness.path,115));take(document.preview_edit(115,.9f));
    take(document.set_material_texture(project,resource.id(),material::Input::base_color,texture));
    require(take(material::capture(document.world().runtime(),resource.id()))->description().parameters.roughness==.4f &&
        !document.material_dirty(key),"Changing a texture committed a cancelled parameter draft or created no-op history.");
    changed(document.create_static_mesh(take(project.mesh("builtin/cube"))));const auto actor_a=document.selection().actor;
    const auto a=component(document,actor_a,document.types().static_mesh_component);
    changed(document.create_static_mesh(take(project.mesh("builtin/cube")),{2,0,0}));const auto actor_b=document.selection().actor;
    const auto b=component(document,actor_b,document.types().static_mesh_component);
    take(document.assign_material(project,a,"slot/0",key));take(document.assign_material(project,b,"slot/0",key));
    const auto count=document.undo_count();take(document.assign_material(project,a,"slot/0",key));
    require(document.undo_count()==count,"Idempotent slot assignment created history.");
    take(document.set_material_override(a,"slot/0",roughness.path,true));
    const auto instance=take(world::read_material_bindings(document.world().runtime(),a)).front().resource;
    require(instance.id()!=resource.id(),"Instance override changed the shared asset identity.");
    require(!material::create_instance(document.world().runtime(),instance),"A second material inheritance level was accepted.");
    auto cyclic=take(material::read_material(document.world().runtime(),instance.id()));cyclic.parent=instance;
    require(!material::update_material(document.world().runtime(),instance.id(),cyclic),"Material inherited itself.");
    take(document.set_property(instance.id(),roughness.path,Scalar{.8f},111));
    const auto override_history=document.undo_count();
    take(document.begin_edit(instance.id(),roughness.path,116));take(document.preview_edit(116,.1f));
    take(document.set_material_override(a,"slot/0",roughness.path,true));
    require(!document.material_editing(116) && document.undo_count()==override_history &&
        take(material::capture(document.world().runtime(),instance.id()))->description().parameters.roughness==.8f,
        "Idempotent override assignment retained an unfinished parameter draft.");
    take(document.set_property(resource.id(),red.path,Scalar{.25f},112));
    const auto capture=[&](ObjectId id){return take(world::material_surfaces(document.world().runtime(),id))->front();};
    require(capture(a)->description().parameters.roughness==.8f && capture(b)->description().parameters.roughness==.4f &&
        capture(a)->description().parameters.base_color[0]==.25f && capture(b)->description().parameters.base_color[0]==.25f,
        "Explicit roughness override stopped inheriting the parent's color.");
    take(document.material_undo(key));require(capture(a)->description().parameters.base_color[0]==1,"Shared material undo missed its instances.");
    take(document.material_undo(key,true));
    take(document.begin_edit(instance.id(),roughness.path,113));take(document.preview_edit(113,.1f));take(document.cancel_edit());
    require(capture(a)->description().parameters.roughness==.8f,"Cancelling an instance edit lost its original value.");
    require(!document.set_property(instance.id(),roughness.path,Scalar{2.f},114),"Invalid material value was published.");
    require(capture(a)->description().parameters.roughness==.8f,"Rejected value damaged the last valid material.");
    take(document.set_material_override(a,"slot/0",roughness.path,false));
    require(capture(a)->description().parameters.roughness==.4f,"Clearing an override did not restore inheritance.");
    changed(document.undo());require(capture(a)->description().parameters.roughness==.8f,"Scene undo did not restore override intent.");
    changed(document.redo());require(capture(a)->description().parameters.roughness==.4f,"Scene redo lost inherited state.");
    changed(document.undo());
    application::SceneSnapshot snapshot;take(snapshot.refresh(document));const auto revision=snapshot.revision;
    take(snapshot.refresh(document));require(snapshot.revision==revision,"Unchanged material rebuilt renderer snapshots.");
    take(document.save_material(project,key));
    auto serialized=take(document.serialize());take(editor::write_file(project.root()/"Content/Scenes/Main.scene",serialized));
    auto changed_parent=take(project.material(key));changed_parent.description.parameters.base_color[1]=.35f;
    take(project.save_material(changed_parent));
    auto reopened=take(editor::Session::deserialize(take(editor::read_file(project.root()/"Content/Scenes/Main.scene")),project));
    const auto reopened_actors=take(reopened->world().members());
    const auto reopened_a=component(*reopened,reopened_actors[0].id(),reopened->types().static_mesh_component);
    const auto reopened_b=component(*reopened,reopened_actors[1].id(),reopened->types().static_mesh_component);
    const auto reopened_bindings=take(world::read_material_bindings(reopened->world().runtime(),reopened_a));
    require(take(material::read_material(reopened->world().runtime(),reopened_bindings[0].resource.id())).overrides!=0,
        "Scene serialization inferred away explicit override intent.");
    require(take(world::material_surfaces(reopened->world().runtime(),reopened_a))->front()->description().parameters.roughness==.8f &&
        take(world::material_surfaces(reopened->world().runtime(),reopened_b))->front()->description().parameters.roughness==.4f,
        "Material inheritance did not survive project save/reopen.");
    const auto reopened_surface=take(world::material_surfaces(reopened->world().runtime(),reopened_a))->front();
    require(reopened_surface->description().parameters.base_color[1]==.35f,"Saved instance values replaced a later parent-asset change.");
    reopened.reset();require(bool(reopened_surface->description().textures[0].image),"Closing a Runtime invalidated its published texture content.");
    take(document.select(actor_a));changed(document.duplicate_selected());
    const auto duplicate=component(document,document.selection().actor,document.types().static_mesh_component);
    const auto duplicate_material=take(world::read_material_bindings(document.world().runtime(),duplicate))[0].resource;
    require(duplicate_material.id()!=instance.id(),"Duplicating an Actor shared mutable instance overrides.");
    require(take(material::read_material(document.world().runtime(),duplicate_material.id())).parent.id()==resource.id(),
        "Duplicating an Actor copied the shared asset.");
    take(editor::write_file(evidence/"Replacement.mtl","newmtl replacement\nKd 1 1 1\n"));
    take(editor::write_file(evidence/"Replacement.obj",
        "mtllib Replacement.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl replacement\nf 1 2 3\n"));
    const auto replacement=take(project.mesh(take(project.import_mesh(evidence/"Replacement.obj"))));
    take(world::set_mesh_binding(document.world().runtime(),duplicate,{replacement.key,replacement.mesh}));
    take(document.assign_material(project,duplicate,"replacement",key));
    const auto original=take(project.mesh("builtin/cube"));
    take(world::set_mesh_binding(document.world().runtime(),duplicate,{original.key,original.mesh}));
    const auto retained=take(world::read_material_bindings(document.world().runtime(),duplicate));
    require(retained.size()==1 && retained[0].slot=="slot/0" && retained[0].resource.id()==duplicate_material.id(),
        "Replacing geometry retained a missing slot or discarded a matching instance.");
    auto swapped=take(editor::Session::deserialize(take(document.serialize()),project));static_cast<void>(swapped);
    take(world::set_mesh_binding(document.world().runtime(),duplicate,{}));
    require(take(world::read_material_bindings(document.world().runtime(),duplicate)).empty(),"Clearing a mesh retained invalid material slots.");
    auto cleared=take(editor::Session::deserialize(take(document.serialize()),project));static_cast<void>(cleared);
    {
        editor::Session local;auto& local_runtime=local.world().runtime();
        editor::MaterialAsset asset;asset.description.parameters.roughness=.3f;asset.textures[0]=texture;
        const auto original=take(local.material_resource(project,asset));
        changed(local.create_static_mesh(take(project.mesh("builtin/cube"))));
        const auto original_actor=local.selection().actor;
        const auto original_mesh=component(local,original_actor,local.types().static_mesh_component);
        const std::array<world::MaterialBinding,1> bindings{{{"slot/0",original,true}}};
        take(world::set_material_bindings(local_runtime,original_mesh,bindings));
        changed(local.duplicate_selected());
        const auto copied_mesh=component(local,local.selection().actor,local.types().static_mesh_component);
        const auto copied=take(world::read_material_bindings(local_runtime,copied_mesh)).front().resource;
        const auto original_state=take(material::read_material(local_runtime,original.id()));
        const auto copied_state=take(material::read_material(local_runtime,copied.id()));
        require(copied.id()!=original.id() && copied_state.source.empty() && !copied_state.parent.registered() &&
            copied_state.textures[0].reference()==original_state.textures[0].reference(),
            "Duplicating an inline material shared mutable parameters or copied its texture resource.");
        const auto properties=take(local.properties(original.id()));
        const auto field=std::ranges::find(properties,std::string("parameters.roughness"),&editor::Property::name);
        require(field!=properties.end(),"Inline material parameters are not reflected.");
        const auto roughness_of=[&](ObjectId id){return take(material::capture(local_runtime,id))->description().parameters.roughness;};
        take(local.set_property(copied.id(),field->path,Scalar{.7f},117));
        require(roughness_of(original.id())==.3f && roughness_of(copied.id())==.7f,"Copied inline material could not be edited independently.");
        changed(local.undo());require(roughness_of(copied.id())==.3f,"Inline material undo lost the copied value.");
        changed(local.redo());
        take(local.set_property(original.id(),field->path,Scalar{.45f},118));
        require(roughness_of(original.id())==.45f && roughness_of(copied.id())==.7f,"Editing the original changed its inline material copy.");
        const auto path=project.root()/"Content/Scenes/Inline.scene";
        take(editor::write_file(path,take(local.serialize())));
        auto reopened_project=take(editor::Project::open(project.root()));
        auto reopened_local=take(editor::Session::deserialize(take(editor::read_file(path)),reopened_project));
        const auto actors=take(reopened_local->world().members());require(actors.size()==2,"Inline material scene lost its Actors.");
        auto& reopened_runtime=reopened_local->world().runtime();
        const auto material_of=[&](std::size_t index) {
            const auto mesh=component(*reopened_local,actors[index].id(),reopened_local->types().static_mesh_component);
            return take(world::read_material_bindings(reopened_runtime,mesh)).front().resource;
        };
        const auto reopened_original=material_of(0),reopened_copy=material_of(1);
        const auto left=take(material::read_material(reopened_runtime,reopened_original.id())),right=take(material::read_material(reopened_runtime,reopened_copy.id()));
        require(reopened_original.id()!=reopened_copy.id() && left.description.parameters.roughness==.45f && right.description.parameters.roughness==.7f &&
            left.textures[0].reference()==right.textures[0].reference(),"Inline material values, independent identities or shared texture were lost on reopening.");
    }
    editor::Workspace workspace;workspace.project_open=true;workspace.assets.reset(true);
    workspace.inspecting_asset=true;workspace.material_inspection=key;
    const auto controls=workspace.describe(document,{1200,800},nullptr,&project);
    require(workspace.ui_error().empty() && !controls.empty(),"Material asset DSL projection failed.");
    const auto field=workspace.field_control(resource.id(),roughness.path);
    require(field!=0,"Material fields did not use the shared reflected field bindings.");
    workspace.inspecting_asset=false;take(document.select(actor_a));
    const auto instance_controls=workspace.describe(document,{1200,800},nullptr,&project);
    require(workspace.ui_error().empty() && !instance_controls.empty(),"Material instance DSL projection failed.");
    take(editor::write_file(evidence/"Two.mtl","newmtl First\nKd 1 1 1\nnewmtl Second\nKd 1 1 1\n"));
    take(editor::write_file(evidence/"Two.obj","mtllib Two.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nusemtl First\nf 1 2 3\nusemtl Second\nf 2 4 3\n"));
    const auto two_slots=take(project.mesh(take(project.import_mesh(evidence/"Two.obj"))));
    changed(document.create_static_mesh(two_slots));const auto partial_actor=document.selection().actor;
    const auto partial=component(document,partial_actor,document.types().static_mesh_component);
    const auto first_slot=world::material_slot_name(*two_slots.mesh,two_slots.mesh->parts().front().material);
    const auto second_slot=world::material_slot_name(*two_slots.mesh,two_slots.mesh->parts().back().material);
    take(document.assign_material(project,partial,first_slot,key));
    const auto fallback=find_property(partial,"material.roughness");
    const auto fallback_visible=[&] {
        const auto projected=workspace.describe(document,{1200,800},nullptr,&project);
        const auto control=workspace.field_control(partial,fallback.path);
        return control && std::ranges::find(projected,control,&ui::Control::key)!=projected.end();
    };
    require(fallback_visible(),"A partially bound mesh lost its unbound-slot material controls.");
    take(document.assign_material(project,partial,second_slot,key));
    require(!fallback_visible(),"A fully bound mesh retained ineffective fallback material controls.");
    take(document.clear_material(partial,second_slot));
    require(fallback_visible(),"Clearing a material slot did not restore fallback controls.");
    const auto before_history=take(material::capture(runtime,resource.id()));
    take(document.material_undo(key));
    const auto paused_history=take(material::capture(runtime,resource.id()));
    take(document.start());const auto running_revision=document.revision();
    require(!document.material_undo(key) && !document.material_undo(key,true),"Running World accepted material history replay.");
    require(document.revision()==running_revision && take(material::capture(runtime,resource.id()))==paused_history,
        "Rejected material history replay changed the material or document revision.");
    take(document.stop());take(document.material_undo(key,true));
    require(take(material::capture(runtime,resource.id()))->description()==before_history->description(),
        "Rejected running history replay advanced its cursor or prevented redo after stop.");
    const auto old_surface=capture(a);changed(document.close());collect(document);
    require(old_surface->description().parameters.roughness==.8f && old_surface->description().textures[0].image,
        "Closing the Runtime invalidated an owning render capture.");
    std::ofstream(evidence/"acceptance.txt")<<"Material resources acceptance: PASS\nRuntime identity, reflection, texture import, shared edits, explicit inheritance, cancel, Undo/Redo, save/reopen, duplicate, clear mesh, DSL, snapshot lifetime.\n";
    std::puts("Material resources acceptance: PASS");
}
