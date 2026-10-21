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
    changed(document.create_static_mesh(take(project.mesh("builtin/cube"))));const auto entity_a=document.selection().entity;
    const auto a=component(document,entity_a,document.presentation_types().static_mesh_component);
    changed(document.create_static_mesh(take(project.mesh("builtin/cube")),{2,0,0}));const auto entity_b=document.selection().entity;
    const auto b=component(document,entity_b,document.presentation_types().static_mesh_component);
    take(document.assign_material(project,a,"slot/0",key));take(document.assign_material(project,b,"slot/0",key));
    const auto count=document.undo_count();take(document.assign_material(project,a,"slot/0",key));
    require(document.undo_count()==count,"Idempotent slot assignment created history.");
    take(document.set_material_override(a,"slot/0",roughness.path,true));
    const auto instance=take(presentation::read_material_bindings(document.world().runtime(),a)).front().resource;
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
    const auto capture=[&](ObjectId id){return take(presentation::material_surfaces(document.world().runtime(),id))->front();};
    require(capture(a)->description().parameters.roughness==.8f && capture(b)->description().parameters.roughness==.4f &&
        capture(a)->description().parameters.base_color[0]==.25f && capture(b)->description().parameters.base_color[0]==.25f,
        "Explicit roughness override stopped inheriting the parent's color.");
    take(document.material_undo(key));require(capture(a)->description().parameters.base_color[0]==1,"Shared material undo missed its instances.");
    take(document.material_undo(key,true));
    take(document.begin_edit(instance.id(),roughness.path,113));take(document.preview_edit(113,.1f));take(document.cancel_edit());
    require(capture(a)->description().parameters.roughness==.8f,"Cancelling an instance edit lost its original value.");
    require(!document.set_property(instance.id(),roughness.path,Scalar{std::nanf("")},114),"Invalid material value was published.");
    require(capture(a)->description().parameters.roughness==.8f,"Rejected value damaged the last valid material.");
    // Roughness is bounded to 0–1: like every Runtime write, a larger value clamps instead of failing.
    take(document.begin_edit(instance.id(),roughness.path,117));take(document.preview_edit(117,2.f));
    require(capture(a)->description().parameters.roughness==1,"A material value above its hard maximum did not clamp to it.");
    take(document.cancel_edit());
    take(document.set_material_override(a,"slot/0",roughness.path,false));
    require(capture(a)->description().parameters.roughness==.4f,"Clearing an override did not restore inheritance.");
    changed(document.undo());require(capture(a)->description().parameters.roughness==.8f,"Scene undo did not restore override intent.");
    changed(document.redo());require(capture(a)->description().parameters.roughness==.4f,"Scene redo lost inherited state.");
    changed(document.undo());
    application::SceneSnapshot snapshot;take(snapshot.refresh(document));const auto surfaces=snapshot.meshes.at(0).surfaces;
    take(snapshot.refresh(document));require(surfaces && snapshot.meshes.at(0).surfaces==surfaces,"Unchanged material rebuilt renderer snapshots.");
    // A shared asset edit changes no component byte; the slot surfaces capture finds it.
    take(document.set_property(resource.id(),red.path,Scalar{.5f},119));
    const auto built=snapshot.built;take(snapshot.refresh(document));
    require(snapshot.built>built && snapshot.meshes.at(0).surfaces!=surfaces &&
        snapshot.meshes.at(0).surfaces->front()->description().parameters.base_color[0]==.5f,
        "A shared material edit did not reach the renderer snapshot.");
    take(document.material_undo(key));
    take(document.save_material(project,key));
    auto serialized=take(document.serialize());take(editor::write_file(project.root()/"Content/Scenes/Main.scene",serialized));
    auto changed_parent=take(project.material(key));changed_parent.description.parameters.base_color[1]=.35f;
    take(project.save_material(changed_parent));
    auto reopened=take(editor::Session::deserialize(take(editor::read_file(project.root()/"Content/Scenes/Main.scene")),project));
    const auto reopened_entities=take(reopened->world().members());
    const auto reopened_a=component(*reopened,reopened_entities[0].id(),reopened->presentation_types().static_mesh_component);
    const auto reopened_b=component(*reopened,reopened_entities[1].id(),reopened->presentation_types().static_mesh_component);
    const auto reopened_bindings=take(presentation::read_material_bindings(reopened->world().runtime(),reopened_a));
    require(take(material::read_material(reopened->world().runtime(),reopened_bindings[0].resource.id())).overrides!=0,
        "Scene serialization inferred away explicit override intent.");
    require(take(presentation::material_surfaces(reopened->world().runtime(),reopened_a))->front()->description().parameters.roughness==.8f &&
        take(presentation::material_surfaces(reopened->world().runtime(),reopened_b))->front()->description().parameters.roughness==.4f,
        "Material inheritance did not survive project save/reopen.");
    const auto reopened_surface=take(presentation::material_surfaces(reopened->world().runtime(),reopened_a))->front();
    require(reopened_surface->description().parameters.base_color[1]==.35f,"Saved instance values replaced a later parent-asset change.");
    reopened.reset();require(bool(reopened_surface->description().textures[0].image),"Closing a Runtime invalidated its published texture content.");
    take(document.select(entity_a));changed(document.duplicate_selected());
    const auto duplicate=component(document,document.selection().entity,document.presentation_types().static_mesh_component);
    const auto duplicate_material=take(presentation::read_material_bindings(document.world().runtime(),duplicate))[0].resource;
    require(duplicate_material.id()!=instance.id(),"Duplicating an Entity shared mutable instance overrides.");
    require(take(material::read_material(document.world().runtime(),duplicate_material.id())).parent.id()==resource.id(),
        "Duplicating an Entity copied the shared asset.");
    take(editor::write_file(evidence/"Replacement.mtl","newmtl replacement\nKd 1 1 1\n"));
    take(editor::write_file(evidence/"Replacement.obj",
        "mtllib Replacement.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl replacement\nf 1 2 3\n"));
    const auto replacement=take(project.mesh(take(project.import_mesh(evidence/"Replacement.obj"))));
    take(presentation::set_mesh_binding(document.world().runtime(),duplicate,{replacement.key,replacement.mesh}));
    take(document.assign_material(project,duplicate,"replacement",key));
    const auto original=take(project.mesh("builtin/cube"));
    take(presentation::set_mesh_binding(document.world().runtime(),duplicate,{original.key,original.mesh}));
    const auto retained=take(presentation::read_material_bindings(document.world().runtime(),duplicate));
    require(retained.size()==1 && retained[0].slot=="slot/0" && retained[0].resource.id()==duplicate_material.id(),
        "Replacing geometry retained a missing slot or discarded a matching instance.");
    auto swapped=take(editor::Session::deserialize(take(document.serialize()),project));static_cast<void>(swapped);
    take(presentation::set_mesh_binding(document.world().runtime(),duplicate,{}));
    require(take(presentation::read_material_bindings(document.world().runtime(),duplicate)).empty(),"Clearing a mesh retained invalid material slots.");
    auto cleared=take(editor::Session::deserialize(take(document.serialize()),project));static_cast<void>(cleared);
    {
        editor::Session local;auto& local_runtime=local.world().runtime();
        editor::MaterialAsset asset;asset.description.parameters.roughness=.3f;asset.textures[0]=texture;
        const auto original=take(local.material_resource(project,asset));
        changed(local.create_static_mesh(take(project.mesh("builtin/cube"))));
        const auto original_entity=local.selection().entity;
        const auto original_mesh=component(local,original_entity,local.presentation_types().static_mesh_component);
        const std::array<presentation::MaterialBinding,1> bindings{{{"slot/0",original,true}}};
        take(presentation::set_material_bindings(local_runtime,original_mesh,bindings));
        changed(local.duplicate_selected());
        const auto copied_mesh=component(local,local.selection().entity,local.presentation_types().static_mesh_component);
        const auto copied=take(presentation::read_material_bindings(local_runtime,copied_mesh)).front().resource;
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
        const auto entities=take(reopened_local->world().members());require(entities.size()==2,"Inline material scene lost its Entities.");
        auto& reopened_runtime=reopened_local->world().runtime();
        const auto material_of=[&](std::size_t index) {
            const auto mesh=component(*reopened_local,entities[index].id(),reopened_local->presentation_types().static_mesh_component);
            return take(presentation::read_material_bindings(reopened_runtime,mesh)).front().resource;
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
    workspace.inspecting_asset=false;take(document.select(entity_a));
    const auto instance_controls=workspace.describe(document,{1200,800},nullptr,&project);
    require(workspace.ui_error().empty() && !instance_controls.empty(),"Material instance DSL projection failed.");
    take(editor::write_file(evidence/"Two.mtl","newmtl First\nKd 1 1 1\nnewmtl Second\nKd 1 1 1\n"));
    take(editor::write_file(evidence/"Two.obj","mtllib Two.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nusemtl First\nf 1 2 3\nusemtl Second\nf 2 4 3\n"));
    const auto two_slots=take(project.mesh(take(project.import_mesh(evidence/"Two.obj"))));
    changed(document.create_static_mesh(two_slots));const auto partial_entity=document.selection().entity;
    const auto partial=component(document,partial_entity,document.presentation_types().static_mesh_component);
    const auto first_slot=presentation::material_slot_name(*two_slots.mesh,two_slots.mesh->parts().front().material);
    const auto second_slot=presentation::material_slot_name(*two_slots.mesh,two_slots.mesh->parts().back().material);
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
// Deleted project resources: strict loading rejects the scene; tolerant loading keeps
// every reference and the rest of the saved data, and saves the scene unchanged.
void verify_missing_resources() {
    namespace fs=std::filesystem;
    const auto evidence=fs::absolute("out/missing-resources-"+std::to_string(GetCurrentProcessId())+"-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto project=take(editor::Project::create(evidence/"Project"));const auto root=project.root();
    std::string bmp(70,'\0');bmp[0]='B';bmp[1]='M';bmp[2]=70;bmp[10]=54;bmp[14]=40;bmp[18]=2;bmp[22]=2;bmp[26]=1;bmp[28]=32;
    for(std::size_t i=54;i<bmp.size();i+=4) {bmp[i+2]=char(255);bmp[i+3]=char(255);}
    take(editor::write_file(evidence/"Red.bmp",bmp));
    take(editor::write_file(evidence/"Wall.mtl","newmtl wall\nKd 1 1 1\n"));
    take(editor::write_file(evidence/"Wall.obj","mtllib Wall.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl wall\nf 1 2 3\n"));
    const auto texture=take(project.import_texture(evidence/"Red.bmp"));
    const auto stone=take(project.create_material("Content/Materials/Stone.material"));
    const auto brick=take(project.create_material("Content/Materials/Brick.material"));
    const auto moss=take(project.create_material("Content/Materials/Moss.material")); // Named only by the mesh that goes missing.
    const auto wall=take(project.mesh(take(project.import_mesh(evidence/"Wall.obj"))));
    const auto wall_slot=presentation::material_slot_name(*wall.mesh,wall.mesh->parts().front().material);
    editor::Session document;auto& runtime=document.world().runtime();
    const auto stone_resource=take(document.material_resource(project,stone));
    const auto roughness=[&] {
        const auto properties=take(document.properties(stone_resource.id()));
        return std::ranges::find(properties,std::string("parameters.roughness"),&editor::Property::name)->path;
    }();
    take(document.set_material_texture(project,stone_resource.id(),material::Input::base_color,texture));
    take(document.set_property(stone_resource.id(),roughness,Scalar{.4f},201));take(document.save_material(project,stone));
    changed(document.create_static_mesh(take(project.mesh("builtin/cube"))));
    const auto textured=component(document,document.selection().entity,document.presentation_types().static_mesh_component);
    take(document.assign_material(project,textured,"slot/0",stone));
    changed(document.create_static_mesh(take(project.mesh("builtin/cube")),{2,0,0}));
    const auto instanced=component(document,document.selection().entity,document.presentation_types().static_mesh_component);
    take(document.assign_material(project,instanced,"slot/0",brick));take(document.set_material_override(instanced,"slot/0",roughness,true));
    take(document.set_property(take(presentation::read_material_bindings(runtime,instanced)).front().resource.id(),roughness,Scalar{.8f},202));
    changed(document.create_static_mesh(wall,{4,0,0}));
    const auto walled=component(document,document.selection().entity,document.presentation_types().static_mesh_component);
    take(document.assign_material(project,walled,wall_slot,moss));
    physics::BodySettings body;body.shape=physics::ShapeKind::triangle_mesh;take(document.configure_body(walled,body));
    const auto saved=take(document.serialize());take(editor::write_file(root/"Content/Scenes/Main.scene",saved));

    // Move each resource and its metadata aside; a fresh Project has no cached copy.
    const auto backup=evidence/"Backup";fs::create_directory(backup);
    std::vector<std::pair<fs::path,fs::path>> moved;
    for(const auto& key:{texture,brick,wall.key}) {
        const auto relative=std::ranges::find(project.entries(),key,&editor::ProjectEntry::key)->path;
        auto metadata=relative;metadata+=L".asset.json";
        for(const auto& file:{relative,metadata}) {moved.emplace_back(root/file,backup/file.filename());fs::rename(root/file,backup/file.filename());}
    }
    auto damaged=take(editor::Project::open(root));
    require(!editor::Session::deserialize(saved,damaged,{},editor::LoadPolicy::strict),"Strict loading accepted a scene with missing resources.");
    auto kept=take(editor::Session::deserialize(saved,damaged));auto& kept_runtime=kept->world().runtime();
    require(take(kept->serialize())==saved,"Tolerant loading did not save the references to missing resources verbatim.");
    const auto issue=[&](std::initializer_list<std::string> field) {
        return std::ranges::count_if(kept->load_issues(),[&](const auto& item) {
            return item.kind==world::DiagnosticKind::missing_resource && std::ranges::equal(item.field,field);
        });
    };
    require(kept->load_issues().size()==4 && issue({"textures","base_color"})==1 && issue({"parent"})==1 && issue({"mesh"})==1 &&
        issue({"properties","body","shape"})==1,"Missing texture, parent material, mesh and collision were not each reported once.");
    const auto entities=take(kept->world().members());
    const auto part=[&](std::size_t index){return component(*kept,entities[index].id(),kept->presentation_types().static_mesh_component);};
    const auto shared=take(presentation::read_material_bindings(kept_runtime,part(0))).front().resource;
    const auto shared_state=take(material::read_material(kept_runtime,shared.id()));
    const auto missing_texture=take(material::read_texture(kept_runtime,shared_state.textures[0].id()));
    require(shared_state.source==stone && shared_state.description.parameters.roughness==.4f && missing_texture.source==texture &&
        !missing_texture.image && kept->unavailable(editor::AssetKind::texture,texture) && !kept->unavailable(editor::AssetKind::material,stone),
        "A missing texture replaced its material or lost the material's other parameters.");
    const auto instance=take(presentation::read_material_bindings(kept_runtime,part(1))).front().resource;
    const auto instance_state=take(material::read_material(kept_runtime,instance.id()));
    const auto stand_in=instance_state.parent;
    require(take(material::capture(kept_runtime,instance.id()))->description().parameters.roughness==.8f &&
        take(material::read_material(kept_runtime,stand_in.id())).source==brick && kept->unavailable(editor::AssetKind::material,brick),
        "A missing parent material lost its instance's overrides or reference.");
    const auto mesh=take(presentation::read_static_mesh(kept_runtime,part(2))).mesh;
    const auto retained=take(kept->retained(part(2)));
    require(mesh.source==wall.key && !mesh.resolved && take(presentation::read_material_bindings(kept_runtime,part(2))).empty() &&
        retained.entries.size()==1 && retained.entries[0].path==std::vector<std::string>{"materials"} &&
        retained.entries[0].kind==world::DiagnosticKind::missing_resource &&
        !take(physics::read_settings(kept_runtime,part(2))).cooked_shape,
        "A missing mesh did not keep its binding table and skip its collision cook.");
    require(!kept->begin_material_edit(stand_in.id(),203) && !kept->save_material(damaged,brick) &&
        !kept->assign_material(damaged,part(0),"slot/0",brick) &&
        !kept->set_material_texture(damaged,shared.id(),material::Input::base_color,texture) &&
        std::ranges::none_of(take(kept->material_assets()),[&](const auto& asset){return asset.key==brick;}),
        "A stand-in became editable, saveable or assignable as a project asset.");
    const auto kept_properties=take(kept->properties(shared.id()));
    const auto kept_roughness=std::ranges::find(kept_properties,std::string("parameters.roughness"),&editor::Property::name)->path;
    take(kept->set_property(shared.id(),kept_roughness,Scalar{.5f},204));take(kept->save_material(damaged,stone));
    const auto stone_file=root/std::ranges::find(damaged.entries(),stone,&editor::ProjectEntry::key)->path;
    require(editor::material_format::decode(editor::json_format::parse(take(editor::read_file(stone_file)))).textures[0]==texture,
        "Saving a material dropped the key of its missing texture.");
    application::SceneSnapshot snapshot;take(snapshot.refresh(*kept));
    take(kept->discard_retained(part(2)));
    require(nlohmann::json::parse(take(kept->serialize()))["entities"][2]["components"][0]["materials"].empty(),"Discarding saved data kept the binding table.");
    changed(kept->undo());require(take(kept->serialize())==saved,"Undoing the discard did not restore the binding table.");
    take(kept->select(entities[2].id()));changed(kept->duplicate_selected());
    const auto copied=nlohmann::json::parse(take(kept->serialize()));
    require(copied["entities"][3]["components"][0]["materials"]==copied["entities"][2]["components"][0]["materials"],
        "Duplicating a component without its mesh lost the kept binding table.");
    take(editor::Session::deserialize(copied.dump(),damaged,{},editor::LoadPolicy::tolerant));

    for(const auto& [original,aside]:moved) fs::rename(aside,original);
    auto repaired=take(editor::Project::open(root)); // Cache/Physics has no entry for the wall; loading cooks it.
    auto restored=take(editor::Session::deserialize(saved,repaired,{},editor::LoadPolicy::strict));
    const auto restored_entities=take(restored->world().members());
    const auto restored_part=[&](std::size_t index){return component(*restored,restored_entities[index].id(),restored->presentation_types().static_mesh_component);};
    const auto restored_shared=take(presentation::read_material_bindings(restored->world().runtime(),restored_part(0))).front().resource;
    require(take(material::capture(restored->world().runtime(),restored_shared.id()))->description().textures[0].image &&
        take(presentation::read_material_bindings(restored->world().runtime(),restored_part(2))).size()==1 &&
        take(physics::read_settings(restored->world().runtime(),restored_part(2))).cooked_shape,
        "Restored resources did not resolve through the preserved references or rebuild the missing Cook cache.");
    {
        const auto stone_file=root/std::ranges::find(repaired.entries(),stone,&editor::ProjectEntry::key)->path;
        const auto stone_text=take(editor::read_file(stone_file));
        // A scene that names a material as a mesh leaves only that mesh unavailable.
        auto misnamed_scene=nlohmann::json::parse(saved);misnamed_scene["entities"][1]["components"][0]["mesh"]=stone;
        auto misnamed=take(editor::Session::deserialize(misnamed_scene.dump(),repaired));
        require(misnamed->unavailable(editor::AssetKind::mesh,stone) && !misnamed->unavailable(editor::AssetKind::material,stone) &&
            misnamed->open_material(repaired,stone) && misnamed->save_material(repaired,stone),
            "A material key named as a mesh made the healthy material unavailable.");
        // An undecodable texture file does not block saving a material that references it.
        const auto texture_file=root/std::ranges::find(repaired.entries(),texture,&editor::ProjectEntry::key)->path;
        const auto image=take(editor::read_file(texture_file));take(editor::write_file(texture_file,"not an image"));
        auto undecodable=take(editor::Project::open(root));
        auto opened=take(editor::Session::deserialize(saved,undecodable));
        const auto opened_stone=take(opened->open_material(undecodable,stone));
        const auto opened_properties=take(opened->properties(opened_stone.id()));
        take(opened->set_property(opened_stone.id(),
            std::ranges::find(opened_properties,std::string("parameters.roughness"),&editor::Property::name)->path,Scalar{.6f},205));
        require(opened->unavailable(editor::AssetKind::texture,texture) && opened->save_material(undecodable,stone),
            "An undecodable texture blocked saving a material that references it.");
        take(editor::write_file(texture_file,image));take(editor::write_file(stone_file,stone_text));
    }
    const auto moss_file=root/std::ranges::find(repaired.entries(),moss,&editor::ProjectEntry::key)->path;
    take(editor::write_file(moss_file,"{"));
    require(!editor::Session::deserialize(saved,repaired,{},editor::LoadPolicy::strict),"Strict loading accepted an unreadable material.");
    auto unreadable=take(editor::Session::deserialize(saved,repaired));const auto& unreadable_issues=unreadable->load_issues();
    take(editor::write_file(root/"Content/Scenes/Main.scene",take(unreadable->serialize())));
    require(unreadable_issues.size()==1 && unreadable_issues[0].kind==world::DiagnosticKind::unreadable_resource &&
        unreadable_issues[0].field==std::vector<std::string>{"source"} && !unreadable->open_material(repaired,moss) &&
        take(unreadable->serialize())==saved && take(editor::read_file(moss_file))=="{",
        "An unreadable material did not stand in with its own diagnostic, opened as an asset, or changed when the scene was saved.");
    const auto wall_file=root/std::ranges::find(repaired.entries(),wall.key,&editor::ProjectEntry::key)->path;
    take(editor::write_file(wall_file,"f 1 2 3\n"));
    auto reloaded=take(editor::Project::open(root));auto broken=take(editor::Session::deserialize(saved,reloaded));
    const auto unreadable_at=[&](std::initializer_list<std::string> field) {
        return std::ranges::count_if(broken->load_issues(),[&](const auto& item) {
            return item.kind==world::DiagnosticKind::unreadable_resource && std::ranges::equal(item.field,field);
        });
    };
    const auto broken_wall=component(*broken,take(broken->world().members())[2].id(),broken->presentation_types().static_mesh_component);
    require(unreadable_at({"mesh"})==1 && unreadable_at({"properties","body","shape"})==1 &&
        take(broken->retained(broken_wall)).entries.at(0).kind==world::DiagnosticKind::unreadable_resource && take(broken->serialize())==saved,
        "An unreadable mesh did not stand in with unreadable_resource diagnostics and saved data.");
    std::ofstream(evidence/"acceptance.txt")<<"Missing resources acceptance: PASS\nStrict rejection, verbatim save, texture/material/mesh stand-ins, kept bindings, skipped collision cook, discard/undo, duplicate, restore, Cook rebuild, unreadable asset.\n";
    std::puts("Missing resources acceptance: PASS");
}
