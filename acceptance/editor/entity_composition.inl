struct EntityDirectionalLight : presentation::LightComponent {
    EntityDirectionalLight() {settings.kind=presentation::LightKind::directional;}
};

void verify_entity_composition() {
    {
        editor::Session document;auto& runtime=document.world().runtime();
        changed(document.add_entity());const auto parent=document.selection().entity;
        take(document.set_transform(parent,{{.1f,0,0},{},{1,1,1}}));
        changed(document.create_static_mesh(assets::mesh_assets().front(),{.3f,0,0}));const auto child=document.selection().entity;
        take(document.set_transform(child,{{.3f,0,0},editor::from_euler_degrees({0,0,90}),{1,1,1}}));
        changed(document.reparent(child,parent,world::ReparentRule::keep_local));
        take(document.select_entities(std::array{parent,child},child));
        const auto parent_before=take(world::read_transform(runtime,parent)),child_before=take(world::read_transform(runtime,child));
        application::SceneSnapshot snapshot;take(snapshot.refresh(document));
        editor::Camera camera;camera.set_view(editor::CameraView::front);
        auto view=editor::ViewportView::make(camera,{0,0,1000,700},1);view.scene=snapshot.picking();
        const auto pointer=*view.project(parent_before.position);
        editor::ViewportTool tool;editor::ToolSettings settings;
        settings.tool=editor::TransformTool::move;settings.orientation=editor::Orientation::local;
        const auto key=[&](std::uint32_t code,ui::Key key=ui::Key::none) {
            return tool.key(document,view,{.kind=ui::EventKind::key_down,.key=key},code,settings,pointer);
        };
        const auto text=[&](std::wstring value) {
            return tool.key(document,view,{.kind=ui::EventKind::text,.data=std::move(value)},0,settings,pointer);
        };
        const auto handle=pointer+math::Vector2{0,-60};
        take(tool.pointer(document,view,{.kind=ui::EventKind::pointer_move,.x=handle.x,.y=handle.y},settings));
        require(tool.hovered()==editor::ViewportTool::Handle::move_x,"Parent/child selection lost the active child's Local move handle.");
        take(key('G'));take(key('X'));take(text(L"1"));
        auto moved=take(world::read_transform(runtime,parent));
        require(std::abs(moved.position.x-.1f)<.0001f && std::abs(moved.position.y-1)<.0001f &&
            take(world::read_transform(runtime,child))==child_before && tool.readout().find(L"D: 1.0000 m")!=std::wstring_view::npos,
            "Local group movement did not use the active child's axis and distance without double-transforming it.");
        take(key(27,ui::Key::escape));
        require(take(world::read_transform(runtime,parent))==parent_before,"Cancelling active-child movement did not restore the parent.");
        settings.orientation=editor::Orientation::global;settings.grid_snap=true;settings.grid_spacing=1;
        const auto history=document.undo_count();take(key('G'));take(key('X'));
        require(std::abs(take(world::world_matrix(runtime,child))[12])<.0001f &&
            std::abs(take(world::read_transform(runtime,parent)).position.x+.3f)<.0001f,
            "Group grid snapping aligned the mutation root instead of the active child.");
        take(key(13,ui::Key::enter));require(document.undo_count()==history+1,"Active-child snapping did not record one group edit.");
        changed(document.undo());require(take(world::read_transform(runtime,parent))==parent_before &&
            take(world::read_transform(runtime,child))==child_before,"Active-child snapping Undo lost the original local poses.");
        changed(document.create_static_mesh(assets::mesh_assets().front(),{2.1f,0,0}));const auto other=document.selection().entity;
        take(document.select_entities(std::array{parent,other,child},child));settings.grid_snap=false;
        take(key('S'));take(key('X'));take(text(L"2"));
        require(std::abs(take(world::read_transform(runtime,parent)).position.x-.1f)<.0001f &&
            std::abs(take(world::read_transform(runtime,other)).position.x-2.1f)<.0001f &&
            tool.readout().find(L"along local X")!=std::wstring_view::npos,
            "Group Scale did not use the active child's Local basis with a Global header setting.");
        take(key(27,ui::Key::escape));
    }
    {
        editor::Session inherited;auto& rt=inherited.world().runtime();
        changed(inherited.add_entity());const auto parent=inherited.selection().entity;
        changed(inherited.create_static_mesh(assets::mesh_assets().front(),{3,0,0}));const auto child=inherited.selection().entity;
        changed(inherited.reparent(child,parent,world::ReparentRule::keep_local));
        const auto child_pose=take(world::read_transform(rt,child));
        const auto before=take(inherited.serialize());inherited.mark_saved();const auto cursor=inherited.undo_count();
        take(inherited.set_transform(parent,{{10,0,0},{},{1,1,1}}));
        require(take(world::world_matrix(rt,child))[12]==13 && inherited.undo_count()==cursor+1,
            "Adding parent space did not update inherited child space in one history item.");
        changed(inherited.undo());
        require(inherited.undo_count()==cursor && !inherited.dirty() && !take(world::has_transform(rt,parent)) &&
            take(world::spatial_parent(rt,child))==ObjectId{} && take(world::read_transform(rt,child))==child_pose &&
            take(inherited.serialize())==before,"Undo of parent space failed or left a partial document/history change.");
        changed(inherited.redo());require(take(world::world_matrix(rt,child))[12]==13,"Parent-space Redo lost inherited child placement.");
        changed(inherited.undo());take(inherited.select(parent));
        changed(inherited.add_component(inherited.types().collision_component));const auto collision=inherited.selection().object;
        require(take(world::has_transform(rt,parent)) && inherited.undo_count()==cursor+1,
            "Adding Collision failed to create its required owner space.");
        changed(inherited.undo());
        require(inherited.undo_count()==cursor && !inherited.dirty() && !take(world::has_transform(rt,parent)) &&
            take(world::components(rt,parent)).empty() && take(world::world_matrix(rt,child))[12]==3 &&
            take(inherited.serialize())==before,"Collision Undo removed only the component or failed to restore space/history.");
        changed(inherited.redo());
        require(component(inherited,parent,inherited.types().collision_component)==collision && take(world::has_transform(rt,parent)),
            "Collision Redo replaced its component identity or omitted owner space.");
        changed(inherited.undo());take(inherited.select(parent));
        editor::Workspace workspace;
        require(workspace.command_enabled(editor::Command::focus,inherited) &&
            !workspace.command_enabled(editor::Command::selection_grid,inherited),
            "A nonspatial group with frameable descendants cannot Focus or incorrectly enables a Transform gesture.");
        application::SceneSnapshot snapshot;take(snapshot.refresh(inherited));
        require(snapshot.meshes.size()==1 && snapshot.meshes.front().selected,"A nonspatial selected ancestor did not highlight its rendered child.");
        changed(inherited.create_static_mesh(assets::mesh_assets().front(),{8,0,0}));const auto other=inherited.selection().entity;
        take(inherited.select(parent));take(snapshot.refresh(inherited));
        require(snapshot.meshes.size()==2 && snapshot.meshes[0].selected && !snapshot.meshes[1].selected,
            "Composition selection propagated into a different root.");
        take(inherited.select(other));take(snapshot.refresh(inherited));
        require(!snapshot.meshes[0].selected && snapshot.meshes[1].selected,"Snapshot retained an earlier ancestor selection.");
        take(inherited.select({}));take(snapshot.refresh(inherited));
        require(!snapshot.meshes[0].selected && !snapshot.meshes[1].selected,"Empty selection retained inherited highlight state.");
        changed(inherited.add_entity());require(!workspace.command_enabled(editor::Command::focus,inherited),
            "An empty nonspatial group enabled Focus without a frameable descendant.");
    }
    {
        TypeId sky_type;
        editor::Session sky([&](world::World& world)->Result<std::shared_ptr<void>> {
            TypeDefinition type;type.name="acceptance.EntityDirectionalLight";type.base=TypeReference::native<presentation::LightComponent>();
            type.native={sizeof(EntityDirectionalLight),alignof(EntityDirectionalLight),atelier::detail::representation<EntityDirectionalLight>(),
                [](void* storage){std::construct_at(static_cast<EntityDirectionalLight*>(storage));},
                [](void* storage) noexcept {std::destroy_at(static_cast<EntityDirectionalLight*>(storage));}};
            type.native.base_address=[](void* storage) noexcept -> void* {return static_cast<presentation::LightComponent*>(static_cast<EntityDirectionalLight*>(storage));};
            auto registered=world.runtime().register_type(std::move(type));if(!registered) return std::unexpected(registered.error());
            sky_type=*registered;return std::shared_ptr<void>{};
        });
        changed(sky.add_entity());const auto entity=sky.selection().entity;sky.mark_saved();const auto cursor=sky.undo_count();
        changed(sky.add_component(sky_type));const auto light=sky.selection().object;
        // Space follows the type: a project type derived from a SpatialComponent needs it whatever its values.
        require(take(presentation::read_light(sky.world().runtime(),light)).kind==presentation::LightKind::directional &&
            take(world::has_transform(sky.world().runtime(),entity)),"A derived spatial component did not give its Entity space.");
        changed(sky.undo());require(sky.undo_count()==cursor && !sky.dirty() && !take(world::has_transform(sky.world().runtime(),entity)),
            "Derived spatial component Undo kept the space it created or changed document history.");
        changed(sky.redo());require(sky.selection().object==light && take(world::has_transform(sky.world().runtime(),entity)),
            "Derived spatial component Redo replaced identity or lost its space.");
    }
    editor::Session document;auto& runtime=document.world().runtime();
    changed(document.add_entity());const auto group=document.selection().entity;
    require(!take(world::has_transform(runtime,group)),"Logical Entity acquired implicit space.");
    const auto initial=document.undo_count();take(document.set_transform(group,{{10,0,0},{},{1,1,1}}));
    require(take(world::has_transform(runtime,group)) && document.undo_count()==initial+1,"Adding Entity space did not use document history.");
    changed(document.undo());require(!take(world::has_transform(runtime,group)),"Transform addition Undo did not restore absence.");
    changed(document.redo());
    changed(document.create_static_mesh(assets::mesh_assets().front(),{2,0,0}));const auto child=document.selection().entity;
    changed(document.reparent(child,group,world::ReparentRule::keep_local));
    require(take(world::parent_entity(runtime,child))==group && take(world::world_matrix(runtime,child))[12]==12,
        "Composition reparent did not preserve the requested local frame.");
    changed(document.create_static_mesh(assets::mesh_assets().front(),{3,0,0}));const auto leaf=document.selection().entity;
    changed(document.reparent(leaf,child,world::ReparentRule::keep_local));
    require(take(world::world_matrix(runtime,leaf))[12]==15 && take(world::children(runtime,child)).front().id()==leaf,
        "Nested Entity composition lost space or child identity.");
    changed(document.undo());require(take(world::parent_entity(runtime,leaf))==ObjectId{} && take(world::read_transform(runtime,leaf)).position.x==3,
        "Reparent Undo did not restore original composition and local pose.");
    changed(document.redo());
    const auto mesh=component(document,child,document.presentation_types().static_mesh_component);
    const auto pose=take(world::read_transform(runtime,child));
    changed(document.remove_component(mesh));
    require(take(world::has_transform(runtime,child)) && take(world::read_transform(runtime,child))==pose &&
        take(world::parent_entity(runtime,leaf))==child,"Removing mesh destroyed Entity space or recursive composition.");
    changed(document.undo());require(component(document,child,document.presentation_types().static_mesh_component)==mesh,"Mesh Undo changed component identity.");
    const std::array selection{group,child};take(document.select_entities(selection,child));
    editor::ViewportTool tool;tool.cursor={20,0,0};take(tool.snap_selection(document,{},true));
    require(take(world::read_transform(runtime,group)).position.x==20 && take(world::read_transform(runtime,child)).position.x==2 &&
        take(world::world_matrix(runtime,leaf))[12]==25,"Selecting an ancestor and descendant transformed the child twice.");
    changed(document.undo());require(take(world::world_matrix(runtime,leaf))[12]==15,"Hierarchy viewport Undo did not restore world placement.");
    changed(document.set_space_reference(leaf,ObjectId{},world::ReparentRule::keep_world));
    require(take(world::parent_entity(runtime,leaf))==child && take(world::spatial_parent(runtime,leaf))==ObjectId{},
        "Changing the space reference changed composition ownership.");
    take(document.set_transform(child,{{4,0,0},{},{1,1,1}}));
    require(take(world::world_matrix(runtime,leaf))[12]==15,"Explicit world space inherited a composition parent's movement.");
    changed(document.undo());changed(document.undo());
    require(take(world::spatial_parent(runtime,leaf))==child && take(world::read_transform(runtime,leaf)).position.x==3,
        "Space-reference Undo failed to restore default inheritance and local TRS.");
    editor::Workspace workspace;take(document.select(group));
    take(workspace.set_tree_expanded(document,editor::Workspace::EntityTree::scene_tree,std::array{group},true));
    const auto controls=workspace.describe(document,{1600,1000},nullptr);
    const auto group_control=std::ranges::find(controls,workspace.object_control(group),&ui::Control::key);
    const auto child_control=std::ranges::find(controls,workspace.object_control(child),&ui::Control::key);
    require(group_control!=controls.end() && child_control!=controls.end() && child_control->tree_depth==group_control->tree_depth+1 &&
        child_control->tree_index==group_control->tree_index+1,"Scene Tree flattened recursive Entity composition.");
    const auto original=take(document.serialize());
    changed(document.duplicate_selected());const auto copy=document.selection().entity;
    require(copy!=group && take(document.world().members()).size()==6 && take(world::children(runtime,copy)).size()==1,
        "Duplicating a complete Entity did not clone its recursive subtree.");
    changed(document.undo());auto undone=nlohmann::json::parse(take(document.serialize()));auto original_graph=nlohmann::json::parse(original);
    require(std::stoull(undone["next_id"].get<std::string>())>=std::stoull(original_graph["next_id"].get<std::string>()),"Undo reused persistent local IDs.");
    undone.erase("next_id");original_graph.erase("next_id");
    require(undone==original_graph,"Duplicate Undo did not preserve original graph identities.");
    changed(document.redo());require(document.selection().entity==copy,"Duplicate Redo replaced copied root identity.");
    take(document.select_entities(selection,child));changed(document.erase_entities());
    require(take(document.world().members()).size()==3,"Deleting ancestor and descendant did not detach one complete subtree.");
    collect(document);changed(document.undo());
    require(take(world::parent_entity(runtime,child))==group && take(world::parent_entity(runtime,leaf))==child &&
        component(document,child,document.presentation_types().static_mesh_component)==mesh,"Delete/collect/Undo did not restore recursive object identities.");
    {
        editor::Session affine;auto& rt=affine.world().runtime();
        changed(affine.add_entity());const auto parent=affine.selection().entity;
        take(affine.set_transform(parent,{{},{},{2,1,1}}));
        changed(affine.create_static_mesh(assets::mesh_assets().front(),{2,0,0}));const auto target=affine.selection().entity;
        const auto rotation=editor::from_euler_degrees({0,0,45});take(affine.set_transform(target,{{2,0,0},rotation,{1,1,1}}));
        changed(affine.reparent(target,parent,world::ReparentRule::keep_local));
        const auto before=take(world::world_matrix(rt,target));editor::ViewportTool handle;handle.cursor={6,0,0};
        take(handle.snap_selection(affine,{},true));const auto after=take(world::world_matrix(rt,target));
        require(std::abs(after[12]-6)<.0001f && std::abs(take(world::read_transform(rt,target)).position.x-3)<.0001f,
            "Inherited shear disabled viewport movement or changed its coordinate space.");
        for(std::size_t i=0;i<12;++i) require(std::abs(before[i]-after[i])<.0001f,"Moving an affine child replaced its inherited linear frame.");
        changed(affine.undo());require(std::abs(take(world::world_matrix(rt,target))[12]-4)<.0001f,"Affine child movement Undo lost original placement.");
    }
    {
        editor::Session optional_space;auto& rt=optional_space.world().runtime();
        changed(optional_space.add_entity());const auto origin=optional_space.selection().entity;
        take(optional_space.set_transform(origin,{{10,0,0},{},{1,1,1}}));
        changed(optional_space.add_entity());const auto target=optional_space.selection().entity;
        take(optional_space.set_transform(target,{{2,0,0},{},{1,1,1}}));
        changed(optional_space.set_space_reference(target,origin,world::ReparentRule::keep_local));
        const auto before=take(optional_space.serialize());take(optional_space.remove_transform(target));
        require(!take(world::has_transform(rt,target)) && !take(world::space_reference(rt,target)),
            "Removing optional space from an Entity retained a hidden origin.");
        changed(optional_space.undo());
        require(take(world::read_transform(rt,target)).position.x==2 && take(world::space_reference(rt,target))==std::optional<ObjectId>{origin} &&
            take(world::world_matrix(rt,target))[12]==12 && take(optional_space.serialize())==before,
            "Optional space removal Undo lost the original TRS or explicit origin.");
        changed(optional_space.redo());take(optional_space.set_transform(target,{{3,0,0},{},{1,1,1}}));
        require(!take(world::space_reference(rt,target)) && take(world::world_matrix(rt,target))[12]==3,
            "Readding optional space resurrected a removed explicit origin.");
        changed(optional_space.undo());require(!take(world::has_transform(rt,target)) && !take(world::space_reference(rt,target)),
            "Undo of readded space retained a hidden origin.");
        changed(optional_space.redo());require(!take(world::space_reference(rt,target)) && take(world::world_matrix(rt,target))[12]==3,
            "Redo of readded space restored a retired origin.");
    }
    {
        editor::Session ordered;auto& rt=ordered.world().runtime();
        const auto add=[&] {changed(ordered.add_entity());return ordered.selection().entity;};
        const auto first=add(),second=add(),third=add(),child=add();
        changed(ordered.reparent(child,first,world::ReparentRule::keep_local));
        const auto roots=[&] {
            std::vector<ObjectId> values;
            for(const auto& member:take(ordered.world().members())) {
                if(take(world::parent_entity(rt,member.id()))==ObjectId{}) values.push_back(member.id());
            }
            return values;
        };
        const auto before_graph=take(ordered.serialize());const auto history=ordered.undo_count();
        changed(ordered.reparent(third,{},world::ReparentRule::keep_local,1));
        require(roots()==std::vector<ObjectId>{first,third,second} && ordered.undo_count()==history+1,
            "Root reparent index used flat World member order instead of the final root slot.");
        const auto after_graph=take(ordered.serialize());changed(ordered.undo());
        require(roots()==std::vector<ObjectId>{first,second,third} && take(ordered.serialize())==before_graph,
            "Root reorder Undo lost root order or recursive membership.");
        changed(ordered.redo());require(take(ordered.serialize())==after_graph,"Root reorder Redo changed document identities or ordering.");
        const auto reordered_history=ordered.undo_count();
        require(!take(ordered.reparent(third,{},world::ReparentRule::keep_local,1)).changed &&
            !take(ordered.reparent(third,{},world::ReparentRule::keep_local)).changed && ordered.undo_count()==reordered_history &&
            roots()==std::vector<ObjectId>{first,third,second},"Same root slot or default append changed ordering/history.");
        changed(ordered.reparent(third,{},world::ReparentRule::keep_local,2));
        require(roots()==std::vector<ObjectId>{first,second,third},"Explicit final root slot failed to move a root to the end.");
        changed(ordered.undo());require(take(ordered.serialize())==after_graph,"Moving root to end Undo failed to restore its prior slot.");
        changed(ordered.redo());require(take(ordered.serialize())==before_graph,"Moving root to end Redo lost recursive graph identity.");
    }
    {
        editor::Session organized;auto& rt=organized.world().runtime();
        const auto add=[&] {changed(organized.add_entity());return organized.selection().entity;};
        const auto first_parent=add();take(organized.set_transform(first_parent,{{10,2,0},{},{2,2,2}}));
        const auto second_parent=add();take(organized.set_transform(second_parent,{{-30,7,0},editor::from_euler_degrees({0,0,90}),{.5f,.5f,.5f}}));
        const auto logical=add();changed(organized.reparent(logical,first_parent,world::ReparentRule::keep_local));
        const auto nested_group=add();changed(organized.reparent(nested_group,logical,world::ReparentRule::keep_local));
        const auto first_branch=add();take(organized.set_transform(first_branch,{{1,2,3},editor::from_euler_degrees({30,0,0}),{1,2,3}}));
        changed(organized.reparent(first_branch,logical,world::ReparentRule::keep_local));
        const auto second_branch=add();take(organized.set_transform(second_branch,{{-4,5,6},editor::from_euler_degrees({0,45,0}),{3,2,1}}));
        changed(organized.reparent(second_branch,nested_group,world::ReparentRule::keep_local));
        const auto nested_spatial=add();take(organized.set_transform(nested_spatial,{{7,8,9},{},{1,1,1}}));
        changed(organized.reparent(nested_spatial,first_branch,world::ReparentRule::keep_local));
        const std::array spatial{first_branch,second_branch,nested_spatial};
        const auto poses=[&] {std::array<world::Trs,3> values;for(std::size_t i=0;i<spatial.size();++i) values[i]=take(world::read_transform(rt,spatial[i]));return values;};
        const auto matrices=[&] {std::array<math::Matrix4,3> values;for(std::size_t i=0;i<spatial.size();++i) values[i]=take(world::world_matrix(rt,spatial[i]));return values;};
        const auto before_local=poses();const auto before_world=matrices();const auto before_graph=take(organized.serialize());
        const auto history=organized.undo_count();changed(organized.reparent(logical,second_parent,world::ReparentRule::keep_world));
        const auto after_local=poses();const auto after_world=matrices();const auto after_graph=take(organized.serialize());
        require(organized.undo_count()==history+1 && !take(world::has_transform(rt,logical)) && !take(world::has_transform(rt,nested_group)) &&
            take(world::parent_entity(rt,logical))==second_parent && before_local[0]!=after_local[0] && before_local[1]!=after_local[1] && before_local[2]==after_local[2],
            "Non-spatial organization reparent failed to compensate both spatial frontiers in one history entry.");
        for(std::size_t object=0;object<spatial.size();++object) for(std::size_t lane=0;lane<16;++lane)
            require(std::abs(before_world[object][lane]-after_world[object][lane])<.001f,"Non-spatial reparent changed a branch world matrix.");
        changed(organized.undo());collect(organized);
        require(poses()==before_local && take(organized.serialize())==before_graph,
            "Non-spatial reparent Undo failed to restore all compensated local TRS and composition.");
        const auto undone_world=matrices();
        for(std::size_t object=0;object<spatial.size();++object) for(std::size_t lane=0;lane<16;++lane)
            require(std::abs(before_world[object][lane]-undone_world[object][lane])<.001f,"Non-spatial reparent Undo drifted a branch world matrix.");
        changed(organized.redo());
        require(poses()==after_local && take(organized.serialize())==after_graph,
            "Non-spatial reparent Redo failed to restore all compensated local TRS and composition.");
        const auto redone_world=matrices();
        for(std::size_t object=0;object<spatial.size();++object) for(std::size_t lane=0;lane<16;++lane)
            require(std::abs(before_world[object][lane]-redone_world[object][lane])<.001f,"Non-spatial reparent Redo drifted a branch world matrix.");
    }
    {
        editor::Session forest;auto& rt=forest.world().runtime();
        changed(forest.add_entity());const auto first_parent=forest.selection().entity;
        take(forest.set_transform(first_parent,{{10,0,0},{},{1,1,1}}));
        changed(forest.add_entity());const auto second_parent=forest.selection().entity;
        take(forest.set_transform(second_parent,{{100,0,0},{},{1,1,1}}));
        changed(forest.create_static_mesh(assets::mesh_assets().front(),{2,0,0}));const auto origin=forest.selection().entity;
        changed(forest.reparent(origin,second_parent,world::ReparentRule::keep_local));
        changed(forest.create_static_mesh(assets::mesh_assets().front(),{3,0,0}));const auto follower=forest.selection().entity;
        changed(forest.reparent(follower,first_parent,world::ReparentRule::keep_local));
        changed(forest.set_space_reference(follower,origin,world::ReparentRule::keep_local));
        changed(forest.create_static_mesh(assets::mesh_assets().front(),{4,0,0}));const auto descendant=forest.selection().entity;
        changed(forest.reparent(descendant,follower,world::ReparentRule::keep_local));
        const auto graph=[&] {auto value=nlohmann::json::parse(take(forest.serialize()));value.erase("next_id");return value;};
        const auto children=[&](ObjectId parent) {std::vector<ObjectId> ids;for(const auto& child:take(world::children(rt,parent))) ids.push_back(child.id());return ids;};
        const auto original_graph=graph();const auto original_count=take(forest.world().members()).size();
        const std::array selected{follower,origin};take(forest.select_entities(selected,origin));
        changed(forest.duplicate_selected());
        require(forest.selected_entities().size()==2,"Different-parent forest copy changed the selected roots.");
        const auto copied_follower=forest.selected_entities()[0],copied_origin=forest.selected_entities()[1];
        const auto copied_descendant=take(world::children(rt,copied_follower)).front().id();
        const auto copied_component=component(forest,copied_follower,forest.presentation_types().static_mesh_component);
        require(copied_follower!=follower && copied_origin!=origin && copied_descendant!=descendant &&
            take(world::parent_entity(rt,copied_follower))==first_parent && take(world::parent_entity(rt,copied_origin))==second_parent &&
            take(world::space_reference(rt,copied_follower))==std::optional<ObjectId>{copied_origin} &&
            take(world::space_reference(rt,follower))==std::optional<ObjectId>{origin} &&
            take(forest.world().members()).size()==original_count+3,
            "Forest copy lost composition parents or failed to remap an internal spatial origin.");
        require(take(world::world_matrix(rt,copied_follower))[12]==105 && take(world::world_matrix(rt,copied_descendant))[12]==109,
            "Copied cross-root space changed world placement.");
        const auto copied_graph=graph();const auto first_order=children(first_parent),second_order=children(second_parent);
        changed(forest.undo());collect(forest);
        require(graph()==original_graph,"Cross-root copy Undo left a partial forest or changed original identities.");
        changed(forest.redo());
        require(graph()==copied_graph && forest.selected_entities()[0]==copied_follower && forest.selected_entities()[1]==copied_origin &&
            component(forest,copied_follower,forest.presentation_types().static_mesh_component)==copied_component &&
            children(first_parent)==first_order && children(second_parent)==second_order,
            "Cross-root copy Redo lost identities, references, or original sibling order.");
        const std::array overlapping{copied_origin,copied_descendant,copied_follower};
        take(forest.select_entities(overlapping,copied_follower));changed(forest.erase_entities());collect(forest);
        require(graph()==original_graph,"Cross-parent batch deletion left a partial forest.");
        changed(forest.undo());
        require(graph()==copied_graph && take(world::space_reference(rt,copied_follower))==std::optional<ObjectId>{copied_origin} &&
            component(forest,copied_follower,forest.presentation_types().static_mesh_component)==copied_component &&
            children(first_parent)==first_order && children(second_parent)==second_order,
            "Forest deletion Undo lost spatial references, identity, or hierarchy order.");
        changed(forest.redo());require(graph()==original_graph,"Forest deletion Redo changed unrelated objects.");
        take(forest.select(origin));const auto history_count=forest.undo_count();
        require(!forest.erase_entities() && graph()==original_graph && forest.undo_count()==history_count &&
            forest.selection().entity==origin,"Rejected external spatial dependency partially deleted the document or changed history.");
    }
    {
        const auto folder=std::filesystem::current_path()/"out/editor-subtree-evidence"/std::to_string(GetTickCount64());
        auto project=take(editor::Project::create(folder));editor::Session graph;auto& rt=graph.world().runtime();
        changed(graph.add_entity());const auto original=graph.selection().entity;take(graph.rename(original,L"Reference Bundle"));
        changed(graph.create_static_mesh(assets::mesh_assets().front(),{1,0,0}));const auto first=graph.selection().entity;
        changed(graph.reparent(first,original,world::ReparentRule::keep_local));
        changed(graph.create_static_mesh(assets::mesh_assets().front(),{2,0,0}));const auto second=graph.selection().entity;
        changed(graph.reparent(second,original,world::ReparentRule::keep_local));
        take(graph.select(first));changed(graph.add_component(graph.physics_types().distance_constraint_component));const auto constraint=graph.selection().object;
        take(physics::set_constraint_target(rt,constraint,second));
        take(graph.select(first));changed(graph.add_component(graph.physics_types().scene_query_component));const auto query=graph.selection().object;
        const std::array excluded{second,first};take(physics::set_query_ignored_entities(rt,query,excluded));
        const auto target_field=take(rt.find_field(graph.physics_types().distance_constraint_component,"target"));
        const auto ignored_field=take(rt.find_field(graph.physics_types().scene_query_component,"ignored_entities"));
        require(take(rt.read_reference(constraint,target_field)).description().form==ValueForm::weak_reference &&
            take(rt.sequence_read(take(rt.reference_field(query,ignored_field)),0)).description().form==ValueForm::strong_reference,
            "Subtree acceptance did not exercise a real reflected weak reference and strong reference sequence.");
        const auto subtree=take(graph.serialize_subtree(original));const auto subtree_json=nlohmann::json::parse(subtree);
        const auto row=[](const nlohmann::json& source,const std::string& id)->const nlohmann::json& {
            for(const auto& entity:source.at("entities")) if(entity.at("id")==id) return entity;
            throw std::runtime_error("Expected stable Entity local ID is absent.");
        };
        const auto root_id=subtree_json.at("roots")[0].get<std::string>();const auto child_ids=row(subtree_json,root_id).at("children");
        bool ordinary_weak{},ordinary_sequence{};
        for(const auto& value:row(subtree_json,child_ids[0].get<std::string>()).at("components")) {
            if(value.at("type")=="physics.DistanceConstraintComponent") ordinary_weak=value.at("properties").at("target")==nlohmann::json{{"$ref",child_ids[1]}};
            if(value.at("type")=="physics.SceneQueryComponent") ordinary_sequence=value.at("properties").at("ignored_entities")==
                nlohmann::json::array({nlohmann::json{{"$ref",child_ids[1]}},nlohmann::json{{"$ref",child_ids[0]}}});
        }
        require(ordinary_weak && ordinary_sequence,"Subtree codec omitted ordinary reflected reference fields or persisted them outside properties.");
        changed(graph.instantiate_subtree(subtree,project));const auto imported_first=graph.selection().entity;
        changed(graph.instantiate_subtree(subtree,project));const auto imported_second=graph.selection().entity;
        const auto check_instance=[](editor::Session& instance,ObjectId root) {
            auto& runtime=instance.world().runtime();const auto children=take(world::children(runtime,root));
            require(children.size()==2,"Imported composition lost its child Entities.");
            const auto owner=children[0].id(),target=children[1].id();
            const auto restored_constraint=component(instance,owner,instance.physics_types().distance_constraint_component);
            const auto restored_query=component(instance,owner,instance.physics_types().scene_query_component);
            require(take(physics::constraint_target(runtime,restored_constraint))==target &&
                take(physics::query_ignored_entities(runtime,restored_query))==std::vector<ObjectId>{target,owner} &&
                take(runtime.type_of(restored_constraint))==instance.physics_types().distance_constraint_component &&
                take(runtime.type_of(restored_query))==instance.physics_types().scene_query_component &&
                take(runtime.type_of(component(instance,owner,instance.presentation_types().static_mesh_component)))==instance.presentation_types().static_mesh_component,
                "Imported ordinary references crossed instances, changed sequence order, or lost actual component types.");
            return std::array{root,owner,target,restored_constraint,restored_query,component(instance,owner,instance.presentation_types().static_mesh_component),
                component(instance,target,instance.presentation_types().static_mesh_component)};
        };
        std::unordered_set<ObjectId> identities;
        for(const auto root:{original,imported_first,imported_second}) for(const auto object:check_instance(graph,root))
            require(identities.insert(object).second,"Repeated composition import reused a Runtime object from another instance.");
        const auto before=nlohmann::json::parse(take(graph.serialize()));
        const auto imported_first_id=before.at("roots")[1].get<std::string>(),imported_second_id=before.at("roots")[2].get<std::string>();
        require(root_id!=imported_first_id && root_id!=imported_second_id && imported_first_id!=imported_second_id,
            "Repeated composition import reused a persistent local ID.");
        take(graph.rename(imported_first,L"Imported First"));take(graph.rename(imported_second,L"Imported Second"));
        changed(graph.reparent(imported_second,{},world::ReparentRule::keep_local,0));
        const auto after=nlohmann::json::parse(take(graph.serialize()));
        require(after.at("roots")==nlohmann::json::array({imported_second_id,root_id,imported_first_id}) && after.at("next_id")==before.at("next_id"),
            "Rename/reorder replaced root local IDs or allocated new identities.");
        for(auto entity:before.at("entities")) {
            if(entity.at("id")==imported_first_id) entity["name"]="Imported First";
            if(entity.at("id")==imported_second_id) entity["name"]="Imported Second";
            require(row(after,entity.at("id").get<std::string>())==entity,"Rename/reorder changed local IDs, child/component identities, or reflected references.");
        }
        const auto saved=take(graph.serialize());const auto path=take(project.resolve("Content/Scenes/Subtree-roundtrip.scene"));
        take(editor::write_file(path,saved));auto reopened=take(editor::Session::deserialize(take(assets::read_file(path)),project));
        require(take(reopened->serialize())==saved,"Complete save/reload changed stable local IDs or imported reference graphs.");
        std::size_t checked{};
        for(const auto& member:take(reopened->world().members())) if(take(world::parent_entity(reopened->world().runtime(),member.id()))==ObjectId{}) {
            (void)check_instance(*reopened,member.id());++checked;
        }
        require(checked==3,"Complete reload changed the number of independent imported compositions.");
        changed(graph.add_entity());const auto outside=graph.selection().entity;
        take(physics::set_constraint_target(rt,constraint,outside));const auto weak_external=take(graph.serialize());const auto weak_history=graph.undo_count();
        require(!graph.serialize_subtree(original) && take(graph.serialize())==weak_external && graph.undo_count()==weak_history,
            "Independent subtree export accepted an external weak business reference or mutated the document on rejection.");
        take(physics::set_constraint_target(rt,constraint,second));const std::array external_exclusions{second,outside};
        take(physics::set_query_ignored_entities(rt,query,external_exclusions));const auto strong_external=take(graph.serialize());const auto strong_history=graph.undo_count();
        require(!graph.serialize_subtree(original) && take(graph.serialize())==strong_external && graph.undo_count()==strong_history,
            "Independent subtree export accepted an external strong reference sequence or mutated the document on rejection.");
    }
    std::puts("Editor Entity composition acceptance: PASS (optional space, recursive trees, reparent compensation, forest history, subtree import/reference remap, stable-ID save/reload)");
}
