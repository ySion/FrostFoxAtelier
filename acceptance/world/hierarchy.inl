void hierarchical_lifecycle() {
    Runtime runtime;
    const auto types=register_test_types(runtime);
    source::Program program(runtime,types.counter);
    publish(program,"async void later() { yield; self.advance(50.0); }");
    const auto later=take(program.bind<void>(FunctionExecution::coroutine));
    w::World world(runtime,types);
    take(world.set_failure_policy(w::FailurePolicy::halt));
    auto root=take(w::create_entity(runtime)), child=take(w::create_entity(runtime));
    auto leaf=take(w::create_entity(runtime)), other=take(w::create_entity(runtime));
    auto a=take(w::create_component(runtime,types.counter)), b=take(w::create_component(runtime,types.counter));
    auto c=take(w::create_component(runtime,types.counter)), d=take(w::create_component(runtime,types.counter));
    const auto child_id=child.id(),leaf_id=leaf.id(),c_id=c.id(),d_id=d.id();
    const std::array root_components{w::Attachment{a.id()},w::Attachment{b.id()}};
    const std::array child_components{w::Attachment{c_id}},leaf_components{w::Attachment{d_id}};
    committed(world.attach_components(root.id(),root_components));
    committed(world.attach_components(child_id,child_components));
    committed(world.attach_components(leaf_id,leaf_components));
    committed(world.reparent(leaf_id,child_id,w::ReparentRule::keep_local));
    committed(world.reparent(child_id,root.id(),w::ReparentRule::keep_local));
    using Event=std::pair<char,ObjectId>;
    std::vector<Event> events,registrations;
    std::vector<ObjectId> activations,deactivations;
    ObjectId fail_begin,fail_end;
    std::size_t phantom_activation{};
    const auto hook=[&](char phase,ObjectId id,ObjectId failure)->Result<void>{
        events.emplace_back(phase,id);
        if(id==failure) return std::unexpected(Error{ErrorCode::invocation_failed,"Intentional subtree lifecycle failure."});
        return {};
    };
    const w::World::Behavior behavior{
        .begin=[&](Runtime&,ObjectId id){return hook('B',id,fail_begin);},
        .tick=[&](Runtime&,ObjectId id,float){return hook('T',id,{});},
        .end=[&](Runtime&,ObjectId id){return hook('E',id,fail_end);}
    };
    take(world.bind_behavior(types.entity,behavior));take(world.bind_behavior(types.counter,behavior));
    take(world.observe_lifecycle([&](w::World& current,w::Lifecycle phase,ObjectId id)->Result<void>{
        if(phase==w::Lifecycle::attached) registrations.emplace_back('R',id);
        if(phase==w::Lifecycle::detached) registrations.emplace_back('U',id);
        const auto entity=take(runtime.is_a(take(runtime.type_of(id)),types.entity));
        if(phase==w::Lifecycle::activated && entity) activations.push_back(id);
        if(phase==w::Lifecycle::deactivated && entity) {
            deactivations.push_back(id);
            require(take(current.query(types.counter)).size()==4 && take(current.members()).size()==4,
                "Deactivated cleanup could not inspect complete registered graph, including close.");
            take(w::components(runtime,id));take(w::parent_entity(runtime,id));
            reject(current.set_enabled(id,true),"Deactivated observer reentered structural state mutation.");
            reject(w::destroy_entity(runtime,id),"Deactivated observer reentered explicit destruction.");
        }
        if(phase==w::Lifecycle::activated && id==c_id) ++phantom_activation;
        return {};
    }));
    committed(world.attach(other.id()));committed(world.attach(root.id()));
    require(take(world.members()).size()==4 && take(world.query(types.counter)).size()==4,
        "Subtree attach duplicated or omitted member/component registrations.");
    require(registrations==std::vector<Event>{{'R',other.id()},{'R',root.id()},{'R',a.id()},{'R',b.id()},
        {'R',child_id},{'R',c_id},{'R',leaf_id},{'R',d_id}},"Subtree registration did not follow complete composition preorder exactly once.");
    reject(world.attach(child_id),"Attached subtree entered World a second time.");
    committed(world.start());
    require(events==std::vector<Event>{{'B',other.id()},{'B',root.id()},{'B',a.id()},{'B',b.id()},
        {'B',child_id},{'B',c_id},{'B',leaf_id},{'B',d_id}},"Begin did not follow Entity/components/children preorder.");
    events.clear();registrations.clear();
    committed(world.reparent(child_id,other.id(),w::ReparentRule::keep_local));
    require(events.empty() && registrations.empty() && take(world.members()).size()==4,
        "Stable running reparent repeated Begin/End or member registration.");
    take(world.update(0));
    require(events==std::vector<Event>{{'T',other.id()},{'T',child_id},{'T',c_id},{'T',leaf_id},{'T',d_id},
        {'T',root.id()},{'T',a.id()},{'T',b.id()}},"Reparent retained stale flattened Tick order.");
    take(world.start_task(c_id,[later](Runtime&,ObjectId id){return later.start(id,{});}));
    take(world.update(0));require(world.activity().tasks==1,"Subtree sample did not own a suspended coroutine.");
    events.clear();deactivations.clear();committed(world.set_enabled(other.id(),false));
    require(deactivations==std::vector<ObjectId>{leaf_id,child_id,other.id()},
        "Effective subtree disable notifications did not follow reverse Entity order.");
    require(events.empty() && world.activity().tasks==0 && !take(w::effective_enabled(runtime,child_id)) &&
        !take(w::effective_enabled(runtime,leaf_id)) && take(w::inspect_entity(runtime,child_id)).enabled,
        "Ancestor disable altered local intent, repeated lifecycle or retained descendant tasks.");
    take(world.update(0));
    require(events==std::vector<Event>{{'T',root.id()},{'T',a.id()},{'T',b.id()}},"Disabled ancestor did not suppress its complete subtree.");
    phantom_activation=0;committed(world.set_enabled(c_id,false));committed(world.set_enabled(c_id,true));
    require(phantom_activation==0 && !take(w::inspect_component(runtime,c_id)).active,
        "Component activation ignored a disabled ancestor.");
    events.clear();activations.clear();committed(world.set_enabled(other.id(),true));
    require(activations==std::vector<ObjectId>{other.id(),child_id,leaf_id},
        "Effective subtree restore notifications did not follow Entity preorder.");
    require(events.empty() && take(w::effective_enabled(runtime,leaf_id)),"Ancestor restore repeated Begin/End or left stale effective state.");
    committed(world.reparent(child_id,root.id(),w::ReparentRule::keep_local));
    events.clear();registrations.clear();
    auto removal=take(world.detach(child_id));
    require(!removal.commit.notification_error && events==std::vector<Event>{{'E',d_id},{'E',leaf_id},{'E',c_id},{'E',child_id}} &&
        registrations==std::vector<Event>{{'U',d_id},{'U',leaf_id},{'U',c_id},{'U',child_id}},
        "Subtree detach omitted descendants, affected unrelated objects or repeated cleanup.");
    require(take(world.members()).size()==2 && take(w::parent_entity(runtime,child_id))==ObjectId{} &&
        take(w::children(runtime,child_id)).front().id()==leaf_id,"Subtree detach left partial membership or destroyed its internal composition.");
    child.reset();leaf.reset();c.reset();d.reset();collect(runtime);
    require(take(w::inspect_entity(runtime,child_id)).state==w::EntityState::detached && runtime.type_of(d_id),
        "History failed to retain detached descendants or granted membership.");
    events.clear();registrations.clear();committed(world.restore(removal));
    require(take(world.members()).size()==4 && take(w::parent_entity(runtime,child_id))==root.id() &&
        events==std::vector<Event>{{'B',child_id},{'B',c_id},{'B',leaf_id},{'B',d_id}},"Subtree restore changed identities, composition or participation interval.");
    committed(world.reparent(child_id,other.id(),w::ReparentRule::keep_local));
    const std::vector<Event> reverse{{'E',b.id()},{'E',a.id()},{'E',root.id()},{'E',d_id},{'E',leaf_id},{'E',c_id},{'E',child_id},{'E',other.id()}};
    events.clear();fail_end=c_id;const auto failed_stop=take(world.stop());
    require(failed_stop.notification_error && events==reverse && !world.running(),
        "End failure skipped remaining subtree/current-order cleanup.");
    events.clear();committed(world.stop());require(events.empty(),"Repeated subtree stop repeated End.");
    fail_end={};fail_begin=c_id;const auto failed_start=take(world.start());
    require(failed_start.notification_error && events==std::vector<Event>{{'B',other.id()},{'B',child_id},{'B',c_id},{'E',child_id},{'E',other.id()}},
        "Subtree Begin failure did not unwind exactly the successful preorder prefix.");
    fail_begin={};committed(world.start());
    registrations.clear();committed(world.close());
    std::vector<Event> unregister;for(const auto& event:reverse) unregister.emplace_back('U',event.second);
    require(registrations==unregister,"Close did not reverse the current composition registration order.");
    committed(w::destroy_entity(runtime,other.id()));
    require(!runtime.type_of(child_id) && !runtime.type_of(leaf_id) && !runtime.type_of(c_id) && !runtime.type_of(d_id),
        "Strong history prevented explicit subtree termination.");
    reject(world.restore(removal),"Closed history restored terminated descendants.");
    std::puts("[pass] Recursive lifecycle: one member directory, preorder after reparent, effective enable/tasks, subtree history and complete failure cleanup");
}

void hierarchical_reference_lifetime() {
    Runtime runtime;const auto types=register_test_types(runtime);w::World world(runtime,types);
    auto parent=take(w::create_entity(runtime)),child=take(w::create_entity(runtime)),leaf=take(w::create_entity(runtime));
    auto component=take(w::create_component(runtime,types.counter));
    const auto parent_id=parent.id(),child_id=child.id(),leaf_id=leaf.id(),component_id=component.id();
    const std::array group{w::Attachment{component_id}};committed(world.attach_components(child_id,group));
    committed(world.reparent(leaf_id,child_id,w::ReparentRule::keep_local));
    committed(world.reparent(child_id,parent_id,w::ReparentRule::keep_local));
    parent.reset();child.reset();leaf.reset();collect(runtime);
    require(!runtime.type_of(parent_id) && runtime.type_of(child_id) && runtime.type_of(leaf_id),
        "A component failed to retain its owner/subtree or weak composition parent retained ancestors.");
    committed(w::destroy_entity(runtime,child_id));
    require(!runtime.type_of(child_id) && !runtime.type_of(leaf_id) && !runtime.type_of(component_id),
        "External strong component root blocked explicit subtree destruction.");
    component.reset();collect(runtime);require(runtime.live_objects()==0 && runtime.root_count()==0,"Subtree destruction retained hidden roots.");
    std::puts("[pass] Recursive references: strong children/owner, weak parent, GC and explicit destruction remain distinct");
}

void hierarchical_space_boundaries() {
    Runtime runtime,foreign_runtime;
    const auto types=register_all_types(runtime);
    (void)register_all_types(foreign_runtime);
    w::World world(runtime,types),other(runtime,types);
    auto a=take(w::create_entity(runtime)),b=take(w::create_entity(runtime)),group=take(w::create_entity(runtime));
    auto x=take(w::create_entity(runtime)),y=take(w::create_entity(runtime));
    auto foreign_world_entity=take(w::create_entity(runtime)),foreign=take(w::create_entity(foreign_runtime));
    auto ability=take(w::create_component(runtime,types.component));
    take(world.set_transform(a.id(),{{10,0,0},{},{1,1,1}}));take(world.set_transform(b.id(),{{100,0,0},{},{1,1,1}}));
    take(world.set_transform(x.id(),{{1,0,0},{},{1,1,1}}));take(world.set_transform(y.id(),{{0,2,0},{},{1,1,1}}));
    committed(world.reparent(x.id(),group.id(),w::ReparentRule::keep_local));
    committed(world.reparent(y.id(),group.id(),w::ReparentRule::keep_local));
    committed(world.reparent(group.id(),a.id(),w::ReparentRule::keep_local));
    committed(world.attach(a.id()));committed(world.attach(b.id()));committed(other.attach(foreign_world_entity.id()));
    const auto before_x=take(w::world_matrix(runtime,x.id())),before_y=take(w::world_matrix(runtime,y.id()));
    const auto equal_matrix=[](const auto& left,const auto& right){for(std::size_t i=0;i<16;++i) if(!near(left[i],right[i])) return false;return true;};
    committed(world.reparent(group.id(),b.id(),w::ReparentRule::keep_world));
    require(!take(w::has_transform(runtime,group.id())) && equal_matrix(before_x,take(w::world_matrix(runtime,x.id()))) &&
        equal_matrix(before_y,take(w::world_matrix(runtime,y.id()))),"Nonspatial-root keep-world missed a spatial frontier or fabricated Transform.");
    reject(world.reparent(b.id(),x.id(),w::ReparentRule::keep_local),"Composition cycle through a nonspatial ancestor was accepted.");
    reject(world.reparent(group.id(),foreign_world_entity.id(),w::ReparentRule::keep_local),"Composition reparent silently crossed Worlds.");
    reject(world.reparent(group.id(),foreign.id(),w::ReparentRule::keep_local),"Composition reparent crossed Runtimes.");
    reject(world.set_space_reference(x.id(),foreign.id(),w::ReparentRule::keep_local),"Spatial origin crossed Runtimes.");
    reject(world.set_space_reference(x.id(),foreign_world_entity.id(),w::ReparentRule::keep_local),"Spatial origin crossed Worlds.");
    reject(world.set_space_reference(x.id(),ability.id(),w::ReparentRule::keep_local),"Capability Component was accepted as spatial origin.");
    reject(world.set_space_reference(x.id(),x.id(),w::ReparentRule::keep_local),"Self spatial cycle was accepted.");
    committed(world.set_space_reference(x.id(),y.id(),w::ReparentRule::keep_world));
    const auto y_reference=take(w::space_reference(runtime,y.id()));
    reject(world.set_space_reference(y.id(),x.id(),w::ReparentRule::keep_world),"Independent spatial reference cycle was accepted.");
    require(take(w::space_reference(runtime,y.id()))==y_reference,"Rejected spatial cycle changed origin.");
    committed(world.set_space_reference(x.id(),std::nullopt,w::ReparentRule::keep_world));
    auto external=take(w::create_entity(runtime));take(world.set_transform(external.id(),{{20,0,0},{},{1,1,1}}));
    committed(world.attach(external.id()));committed(world.set_space_reference(external.id(),x.id(),w::ReparentRule::keep_world));
    const auto members=take(world.members()).size();
    reject(world.detach(group.id()),"Subtree detach left an external spatial dependent in World.");
    reject(w::destroy_entity(runtime,group.id()),"Subtree destruction silently invalidated an external spatial dependent.");
    reject(world.remove_transform(x.id()),"Transform removal ignored an explicit external origin dependency.");
    require(take(world.members()).size()==members && take(w::parent_entity(runtime,group.id()))==b.id() &&
        take(w::space_reference(runtime,external.id()))==std::optional<ObjectId>{x.id()},"Rejected external-dependency mutation partially committed.");
    committed(world.set_space_reference(external.id(),ObjectId{},w::ReparentRule::keep_world));
    require(take(w::space_reference(runtime,external.id()))==std::optional<ObjectId>{ObjectId{}},"Explicit world space was confused with inherited space.");
    committed(world.reparent(group.id(),a.id(),w::ReparentRule::keep_world));
    take(world.set_transform(b.id(),{{100,0,0},{},{2,1,1}}));
    take(world.set_transform(y.id(),{{0,2,0},{0,0,std::sin(.4f),std::cos(.4f)},{1,1,1}}));
    const auto local_x=take(w::read_transform(runtime,x.id())),local_y=take(w::read_transform(runtime,y.id()));
    reject(world.reparent(group.id(),b.id(),w::ReparentRule::keep_world),"Nonspatial-root keep-world discarded shear in a later frontier.");
    require(take(w::parent_entity(runtime,group.id()))==a.id() && take(w::read_transform(runtime,x.id()))==local_x &&
        take(w::read_transform(runtime,y.id()))==local_y,"Failed frontier batch changed the first frontier or composition parent.");
    auto anchor=take(w::create_entity(runtime)),probe=take(w::create_entity(runtime));
    take(world.set_transform(anchor.id(),{}));take(world.set_transform(probe.id(),{{3,4,5},{},{1,1,1}}));
    committed(world.set_space_reference(probe.id(),anchor.id(),w::ReparentRule::keep_local));
    committed(w::destroy_entity(runtime,anchor.id()));
    reject(w::world_matrix(runtime,probe.id()),"Stale explicit origin returned a fabricated world matrix.");
    reject(world.attach(probe.id()),"Subtree attach accepted an invalid explicit spatial origin.");
    require(take(world.members()).size()==members,"Failed stale-origin attach registered partial membership.");
    const auto probe_local=take(w::read_transform(runtime,probe.id()));
    reject(world.set_space_reference(probe.id(),ObjectId{},w::ReparentRule::keep_world),"Keep-world fabricated a pose from an expired origin.");
    reject(world.set_space_reference(probe.id(),anchor.id(),w::ReparentRule::keep_local),"Keep-local repair accepted an expired replacement origin.");
    reject(w::world_matrix(runtime,probe.id()),"Rejected stale-origin repair changed spatial reference.");
    require(take(w::read_transform(runtime,probe.id()))==probe_local,"Rejected stale-origin repair changed local TRS.");
    committed(world.set_space_reference(probe.id(),ObjectId{},w::ReparentRule::keep_local));
    require(take(w::space_reference(runtime,probe.id()))==std::optional<ObjectId>{ObjectId{}} &&
        take(w::read_transform(runtime,probe.id()))==probe_local &&
        equal_matrix(take(w::world_matrix(runtime,probe.id())),w::transform_matrix(probe_local)),
        "Keep-local repair did not restore explicit world space with unchanged local TRS.");
    committed(world.attach(probe.id()));
    const auto probe_id=probe.id();
    take(world.remove_transform(probe_id));
    require(!take(w::has_transform(runtime,probe_id)) && !take(w::space_reference(runtime,probe_id)).has_value(),
        "Transform removal retained explicit world-space configuration.");
    committed(world.reparent(probe_id,a.id(),w::ReparentRule::keep_local));
    take(world.set_transform(probe_id,probe_local));
    const auto inherited_probe=take(w::world_matrix(runtime,probe_id));
    require(probe.id()==probe_id && take(w::spatial_parent(runtime,probe_id))==a.id() && near(inherited_probe[12],13),
        "Re-added Transform restored hidden world-space configuration or replaced Entity identity.");
    auto optional_space=take(w::create_entity(runtime)),temporary_origin=take(w::create_entity(runtime));
    auto environment=take(w::create_component(runtime,types.environment_component));
    const auto optional_id=optional_space.id();
    take(world.set_transform(optional_id,probe_local));take(world.set_transform(temporary_origin.id(),{}));
    const std::array environment_group{w::Attachment{environment.id()}};
    committed(world.attach_components(optional_id,environment_group));
    committed(world.set_space_reference(optional_id,temporary_origin.id(),w::ReparentRule::keep_local));
    take(world.remove_transform(optional_id));
    require(!take(w::has_transform(runtime,optional_id)) && !take(w::space_reference(runtime,optional_id)).has_value() &&
        take(w::components(runtime,optional_id)).front().id()==environment.id(),
        "A nonspatial component prevented space removal or lost its Entity/component identity.");
    committed(w::destroy_entity(runtime,temporary_origin.id()));
    committed(world.attach(optional_id));
    take(world.set_transform(optional_id,probe_local));
    require(optional_space.id()==optional_id && !take(w::space_reference(runtime,optional_id)).has_value() &&
        equal_matrix(take(w::world_matrix(runtime,optional_id)),w::transform_matrix(probe_local)),
        "Re-added Transform reactivated an expired hidden origin.");
    auto stretched=take(w::create_entity(runtime)),turned=take(w::create_entity(runtime)),carried=take(w::create_entity(runtime));
    take(world.set_transform(stretched.id(),{{0,0,0},{},{2,1,1}}));
    take(world.set_transform(turned.id(),{{1,0,0},{},{.5f,1,1}}));
    take(world.set_transform(carried.id(),{{0,1,0},{},{1,1,1}}));
    committed(world.attach(stretched.id()));committed(world.attach(turned.id(),w::append,stretched.id()));
    committed(world.attach(carried.id(),w::append,turned.id()));
    committed(world.start());
    const std::array poses{w::SimulationPose{x.id(),{201,3,0},{}},w::SimulationPose{a.id(),{200,0,0},{}}};
    take(world.apply_simulation_transforms(poses));
    const auto parent_world=take(w::world_matrix(runtime,a.id())),child_world=take(w::world_matrix(runtime,x.id()));
    require(near(parent_world[12],200) && near(child_world[12],201) && near(child_world[13],3),
        "Child-first physics pose batch used stale ancestor world pose.");
    const std::array invalid_poses{w::SimulationPose{a.id(),{300,0,0},{}},
        w::SimulationPose{x.id(),{std::numeric_limits<float>::infinity(),0,0},{}}};
    // Isolate rejects only the invalid pose; its Entity keeps its local pose under the moved parent.
    take(world.apply_simulation_transforms(invalid_poses));
    const auto rejected_pose=world.take_diagnostics();
    require(near(take(w::world_matrix(runtime,a.id()))[12],300) && near(take(w::world_matrix(runtime,x.id()))[12],301) &&
        rejected_pose.size()==1 && rejected_pose[0].object==x.id() && rejected_pose[0].kind==w::DiagnosticKind::command_rejected &&
        rejected_pose[0].phase==w::DiagnosticPhase::physics && rejected_pose[0].severity==w::Severity::warning,
        "Isolated simulation batch did not reject exactly the invalid pose.");
    // The child first converts against its parent's quarter-turn target. That turn under
    // the stretched grandparent would change the parent's authored scale, so the parent
    // is rejected and the child converts again against the parent's unmoved pose.
    const auto turned_world=take(w::world_matrix(runtime,turned.id()));
    const std::array conflicting{w::SimulationPose{carried.id(),{60,5,0},{}},
        w::SimulationPose{turned.id(),{50,0,0},{0,0,.70710678f,.70710678f}}};
    take(world.apply_simulation_transforms(conflicting));
    const auto conflict=world.take_diagnostics();
    const auto carried_world=take(w::world_matrix(runtime,carried.id()));
    require(conflict.size()==1 && conflict[0].object==turned.id() && conflict[0].kind==w::DiagnosticKind::command_rejected &&
        equal_matrix(turned_world,take(w::world_matrix(runtime,turned.id()))) &&
        near(carried_world[12],60) && near(carried_world[13],5) && near(carried_world[14],0),
        "A pose kept after its parent's pose was rejected converted against the rejected target.");
    committed(world.stop());take(world.set_failure_policy(w::FailurePolicy::halt));committed(world.start());
    take(world.apply_simulation_transforms(poses));
    reject(world.apply_simulation_transforms(invalid_poses),"Invalid final simulation pose was accepted.");
    require(equal_matrix(parent_world,take(w::world_matrix(runtime,a.id()))) && equal_matrix(child_world,take(w::world_matrix(runtime,x.id()))),
        "Failed simulation batch partially moved an ancestor.");
    const std::array duplicate_poses{poses[0],poses[0]};reject(world.apply_simulation_transforms(duplicate_poses),"Duplicate simulation entity identities were accepted.");
    committed(world.close());committed(other.close());
    std::puts("[pass] Recursive space: multiple nonspatial frontiers, independent origins, external dependencies, atomic TRS failure and parent-first physics batches");
}

void forest_publication() {
    Runtime runtime;const auto types=register_test_types(runtime);w::World world(runtime,types);
    auto p=take(w::create_entity(runtime)),q=take(w::create_entity(runtime)),existing=take(w::create_entity(runtime));
    take(world.set_transform(p.id(),{{100,0,0},{},{1,1,1}}));
    take(world.set_transform(q.id(),{{200,0,0},{},{1,1,1}}));
    committed(world.attach(p.id()));committed(world.attach(existing.id(),w::append,p.id()));committed(world.attach(q.id()));
    auto x=take(w::create_entity(runtime)),y=take(w::create_entity(runtime)),z=take(w::create_entity(runtime));
    auto cx=take(w::create_component(runtime,types.counter)),cy=take(w::create_component(runtime,types.counter));
    take(world.set_transform(x.id(),{{1,0,0},{},{1,1,1}}));
    take(world.set_transform(y.id(),{{10,0,0},{},{1,1,1}}));
    take(world.set_transform(z.id(),{{2,0,0},{},{1,1,1}}));
    const std::array x_capabilities{w::Attachment{cx.id()}},y_capabilities{w::Attachment{cy.id()}};
    committed(world.attach_components(x.id(),x_capabilities));committed(world.attach_components(y.id(),y_capabilities));
    committed(world.reparent(z.id(),x.id(),w::ReparentRule::keep_local));
    committed(world.set_space_reference(x.id(),y.id(),w::ReparentRule::keep_local));
    using Event=std::pair<char,ObjectId>;std::vector<Event> events;
    const w::World::Behavior behavior{
        .begin=[&](Runtime&,ObjectId id)->Result<void>{events.emplace_back('B',id);return {};},
        .end=[&](Runtime&,ObjectId id)->Result<void>{events.emplace_back('E',id);return {};}
    };
    take(world.bind_behavior(types.entity,behavior));take(world.bind_behavior(types.counter,behavior));
    committed(world.start());events.clear();
    bool verify_publication{true};
    take(world.observe_lifecycle([&](w::World& current,w::Lifecycle phase,ObjectId id)->Result<void>{
        if(phase==w::Lifecycle::detached){events.emplace_back('U',id);return {};}
        if(phase!=w::Lifecycle::attached)return {};
        events.emplace_back('R',id);
        if(verify_publication) {
            require(take(current.members()).size()==6 && take(current.query(types.counter)).size()==2,
                "Forest observer saw only part of the member/capability publication.");
            require(take(w::parent_entity(runtime,x.id()))==p.id() && take(w::parent_entity(runtime,y.id()))==q.id() &&
                near(take(w::world_matrix(runtime,x.id()))[12],211) && near(take(w::world_matrix(runtime,z.id()))[12],213),
                "Forest observer saw incomplete parent sequences or unresolved cross-root space.");
        }
        return {};
    }));
    const std::array invalid{w::EntityAttachment{x.id(),p.id(),0},w::EntityAttachment{y.id(),q.id(),1}};
    reject(world.attach_batch(invalid),"Forest accepted an invalid later-parent insertion index.");
    require(events.empty() && take(world.members()).size()==3 && take(w::children(runtime,p.id())).front().id()==existing.id() &&
        take(w::children(runtime,q.id())).empty() && take(w::parent_entity(runtime,x.id()))==ObjectId{} &&
        take(w::parent_entity(runtime,y.id()))==ObjectId{},"Failed forest validation partially published a parent sequence or lifecycle.");
    const std::array new_parent{w::EntityAttachment{x.id(),y.id()},w::EntityAttachment{y.id(),q.id()}};
    reject(world.attach_batch(new_parent),"Forest accepted a same-batch parent instead of requiring detached assembly.");
    const std::array overlap{w::EntityAttachment{x.id(),p.id()},w::EntityAttachment{z.id(),q.id()}};
    reject(world.attach_batch(overlap),"Forest accepted overlapping composition roots.");
    require(events.empty() && take(world.members()).size()==3,"Rejected forest emitted notifications or partially registered descendants.");
    const std::array forest{w::EntityAttachment{x.id(),p.id(),0},w::EntityAttachment{y.id(),q.id()}};
    committed(world.attach_batch(forest));
    require(events==std::vector<Event>{{'R',x.id()},{'R',cx.id()},{'R',z.id()},{'R',y.id()},{'R',cy.id()},
        {'B',x.id()},{'B',cx.id()},{'B',z.id()},{'B',y.id()},{'B',cy.id()}},
        "Different-parent forest repeated registration/Begin or violated current composition preorder.");
    events.clear();
    reject(world.detach(y.id()),"Individual departure invalidated a retained Entity's spatial origin.");
    require(events.empty() && take(world.members()).size()==6,"Rejected single-root departure partially ended the forest.");
    const std::array reverse_selection{y.id(),x.id()};auto forest_history=take(world.detach_batch(reverse_selection));
    require(forest_history.size()==2 && forest_history[0].id()==y.id() && forest_history[1].id()==x.id() &&
        !forest_history[0].commit.notification_error && !forest_history[1].commit.notification_error,
        "Forest histories did not retain input selection order and shared successful commit.");
    require(events==std::vector<Event>{{'E',cy.id()},{'E',y.id()},{'E',z.id()},{'E',cx.id()},{'E',x.id()},
        {'U',cy.id()},{'U',y.id()},{'U',z.id()},{'U',cx.id()},{'U',x.id()}},
        "Forest detach followed selection order instead of reverse current composition or repeated cleanup.");
    require(take(world.members()).size()==3 && take(w::children(runtime,p.id())).front().id()==existing.id() &&
        take(w::children(runtime,q.id())).empty() && take(w::space_reference(runtime,x.id()))==std::optional<ObjectId>{y.id()} &&
        take(w::parent_entity(runtime,z.id()))==x.id(),"Forest departure discarded internal references or left partial parent/membership publication.");
    events.clear();reject(world.restore(forest_history[1]),"One-root history restored before its detached spatial origin.");
    require(events.empty() && take(world.members()).size()==3,"Rejected partial restore published members or Begin.");
    committed(world.restore(std::span<const w::EntityRemoval>{forest_history}));
    require(events==std::vector<Event>{{'R',x.id()},{'R',cx.id()},{'R',z.id()},{'R',y.id()},{'R',cy.id()},
        {'B',x.id()},{'B',cx.id()},{'B',z.id()},{'B',y.id()},{'B',cy.id()}} &&
        near(take(w::world_matrix(runtime,x.id()))[12],211),"Forest history restore changed identities, spatial refs, parents or Begin order.");
    verify_publication=false;
    auto first=take(w::create_entity(runtime)),second=take(w::create_entity(runtime));
    const std::array legacy{first.id(),second.id()};committed(world.attach_batch(legacy,0,p.id()));
    auto siblings=take(w::children(runtime,p.id()));
    require(siblings.size()==4 && siblings[0].id()==first.id() && siblings[1].id()==second.id() &&
        siblings[2].id()==x.id() && siblings[3].id()==existing.id(),"Legacy same-parent batch changed equal-index insertion order.");
    committed(world.stop());
    committed(world.set_space_reference(x.id(),std::nullopt,w::ReparentRule::keep_world));
    auto removed=take(world.detach(x.id()));committed(world.restore(removed));
    siblings=take(w::children(runtime,p.id()));
    require(siblings[2].id()==x.id() && take(w::parent_entity(runtime,z.id()))==x.id(),"Subtree restore lost its old child insertion index through the forest entry.");
    auto last=take(w::create_entity(runtime));committed(world.attach(last.id()));
    std::vector<ObjectId> original_order;for(const auto& member:take(world.members()))original_order.push_back(member.id());
    const std::array mixed_roots{q.id(),x.id()};auto mixed_history=take(world.detach_batch(mixed_roots));
    require(mixed_history[0].member_count==2 && mixed_history[1].member_count==2,
        "Forest history omitted original subtree member counts.");
    committed(world.restore(std::span<const w::EntityRemoval>{mixed_history}));
    std::vector<ObjectId> restored_order;for(const auto& member:take(world.members()))restored_order.push_back(member.id());
    require(restored_order==original_order,"Mixed World-root/child history did not rebase collapsed member positions.");
    committed(world.close());
    std::puts("[pass] Forest publication: distinct existing parents, cross-root space, complete notifications, atomic late rejection and legacy insertion/history");
}

void attachment_space_and_authoring() {
    Runtime runtime;const auto types=register_test_types(runtime);w::World world(runtime,types);
    source::Program task_program(runtime,types.counter);
    publish(task_program,"async void later() { yield; self.advance(50.0); }");
    const auto later=take(task_program.bind<void>(FunctionExecution::coroutine));
    auto root=take(w::create_entity(runtime)),logical=take(w::create_entity(runtime)),child=take(w::create_entity(runtime));
    take(world.set_transform(root.id(),{{10,0,0},{},{1,1,1}}));
    take(world.set_transform(child.id(),{{2,0,0},{},{1,1,1}}));
    committed(world.reparent(logical.id(),root.id(),w::ReparentRule::keep_local));
    committed(world.reparent(child.id(),logical.id(),w::ReparentRule::keep_local));
    committed(world.attach(root.id()));
    const auto child_local=take(w::read_transform(runtime,child.id()));
    take(world.set_transform(logical.id(),{{3,0,0},{},{1,1,1}}));
    take(world.remove_transform(logical.id()));
    require(!take(w::has_transform(runtime,logical.id())) && take(w::read_transform(runtime,child.id()))==child_local &&
        near(take(w::world_matrix(runtime,child.id()))[12],12),"Transform undo blocked default spatial inheritance or changed local TRS.");
    take(world.remove_transform(root.id()));
    require(near(take(w::world_matrix(runtime,child.id()))[12],2),"Transform removal did not pass through nonspatial ancestors.");
    take(world.set_transform(root.id(),{{10,0,0},{},{1,1,1}}));
    committed(world.set_space_reference(child.id(),root.id(),w::ReparentRule::keep_local));
    reject(world.remove_transform(root.id()),"Transform removal accepted an explicit spatial dependent.");
    require(take(w::has_transform(runtime,root.id())) && take(w::read_transform(runtime,child.id()))==child_local,
        "Rejected Transform removal changed author values.");
    committed(world.set_space_reference(child.id(),std::nullopt,w::ReparentRule::keep_local));
    auto marker=take(w::create_entity(runtime));
    reject(world.attach(marker.id(),1),"World-root insertion accepted an index inside an existing subtree.");
    require(take(world.members()).size()==3 && take(w::inspect_entity(runtime,marker.id())).state==w::EntityState::detached,
        "Invalid root boundary partially published a member.");
    committed(world.attach(marker.id(),3));

    auto counter=take(w::create_component(runtime,types.counter));
    const std::array counter_parts{w::Attachment{counter.id()}};
    committed(world.attach_components(child.id(),counter_parts));
    unsigned begins{},ends{},lifecycle_events{};bool space_reentry_rejected{};ObjectId prepared_id{},a_id{},b_id{};
    take(world.bind_behavior(types.entity,{.begin=[&](Runtime&,ObjectId object)->Result<void>{
        if(object==child.id()) {
            const auto rejected=world.set_space_reference(child.id(),ObjectId{},w::ReparentRule::keep_local);
            space_reentry_rejected=!rejected && rejected.error().code==ErrorCode::busy;
        }
        if(object==a_id || object==b_id) {
            require(take(w::space_reference(runtime,a_id))==std::optional<ObjectId>{b_id} &&
                take(w::space_reference(runtime,b_id))==std::optional<ObjectId>{ObjectId{}} &&
                near(take(w::world_matrix(runtime,a_id))[12],7),"Begin saw a temporary spatial cycle or incomplete origins.");
            ++begins;
        }
        return {};
    },.end=[&](Runtime&,ObjectId)->Result<void>{++ends;return {};}}));
    take(world.observe_lifecycle([&](w::World&,w::Lifecycle,ObjectId)->Result<void>{++lifecycle_events;return {};}));
    committed(world.start());
    require(space_reentry_rejected && !take(w::space_reference(runtime,child.id())),"Running origin change reentered a Begin callback.");
    take(world.set_transform(root.id(),take(w::read_transform(runtime,root.id()))));
    take(world.start_task(counter.id(),[later](Runtime&,ObjectId receiver){return later.start(receiver,{});}));
    take(world.update(0));
    const auto running_pose=take(w::world_matrix(runtime,child.id()));
    const auto events_before=lifecycle_events,ends_before=ends,begins_before=begins;
    require(world.activity().tasks==1,"Space continuity fixture did not suspend a real task.");
    committed(world.set_space_reference(child.id(),ObjectId{},w::ReparentRule::keep_world));
    require(take(w::space_reference(runtime,child.id()))==std::optional<ObjectId>{ObjectId{}} &&
        take(w::world_matrix(runtime,child.id())).elements==running_pose.elements && world.activity().tasks==1 &&
        take(w::inspect_component(runtime,counter.id())).active && lifecycle_events==events_before && ends==ends_before && begins==begins_before,
        "Running origin change lost world pose, cancelled a task or emitted lifecycle transitions.");
    committed(world.set_space_reference(child.id(),std::nullopt,w::ReparentRule::keep_world));
    const auto root_local=take(w::read_transform(runtime,root.id()));
    reject(world.set_space_reference(root.id(),child.id(),w::ReparentRule::keep_world),"Running origin change accepted a spatial cycle.");
    require(!take(w::space_reference(runtime,root.id())) && take(w::read_transform(runtime,root.id()))==root_local &&
        take(w::world_matrix(runtime,child.id())).elements==running_pose.elements && world.activity().tasks==1 &&
        lifecycle_events==events_before && ends==ends_before && begins==begins_before,
        "Rejected running spatial cycle changed the graph, tasks or lifecycle state.");
    take(world.update(0));
    require(world.activity().tasks==0 && std::get<float>(take(runtime.read(counter.id(),types.counter_value)))==50.f &&
        lifecycle_events==events_before && ends==ends_before && begins==begins_before,
        "Origin changes interrupted the suspended task's next execution or replayed lifecycle.");
    take(world.write(root.id(),types.position_fields[0],take(runtime.read(root.id(),types.position_fields[0]))));
    auto prepared=take(w::create_entity(runtime));prepared_id=prepared.id();
    take(world.set_transform(prepared_id,{{1,0,0},{},{1,1,1}}));
    take(world.write(prepared_id,types.position_fields[0],Scalar{3.f}));
    take(world.remove_transform(prepared_id));take(world.set_transform(prepared_id,{{3,0,0},{},{1,1,1}}));
    committed(world.set_space_reference(prepared_id,ObjectId{},w::ReparentRule::keep_local));
    auto mesh=take(w::create_component(runtime,types.static_mesh_component));
    auto light=take(w::create_component(runtime,types.light_component));
    const auto nonspatial=take(w::create_component(runtime,types.environment_component));
    require(take(w::component_requires_space(runtime,mesh.id())) && take(w::component_requires_space(runtime,light.id())) &&
        !take(w::component_requires_space(runtime,nonspatial.id())),"SpatialComponent did not decide the space requirement.");
    take(p::set_material(world,mesh.id(),{}));const std::array capabilities{w::Attachment{mesh.id()},w::Attachment{light.id()}};
    committed(world.attach_components(prepared_id,capabilities));
    take(p::set_material(world,mesh.id(),{}));take(p::set_light(world,light.id(),{}));
    committed(world.attach(prepared_id));
    take(p::set_material(world,mesh.id(),{}));take(p::set_light(world,light.id(),{}));

    auto a=take(w::create_entity(runtime)),b=take(w::create_entity(runtime));a_id=a.id();b_id=b.id();
    take(world.set_transform(a_id,{{2,0,0},{},{1,1,1}}));take(world.set_transform(b_id,{{5,0,0},{},{1,1,1}}));
    committed(world.reparent(b_id,a_id,w::ReparentRule::keep_local));
    const std::array forest{w::EntityAttachment{a_id}};
    const std::array cycle{w::SpaceReference{a_id,b_id}};
    const auto size=take(world.members()).size();
    reject(world.attachment_world_matrices(forest,cycle),"Prospective preview accepted a spatial cycle.");
    reject(world.attach_batch(forest,cycle),"Forest accepted an incomplete final origin plan.");
    const std::array duplicates{w::SpaceReference{a_id,ObjectId{}},w::SpaceReference{a_id,std::nullopt}};
    reject(world.attach_batch(forest,duplicates),"Forest accepted duplicate origin plans.");
    const std::array unrelated{w::SpaceReference{root.id(),ObjectId{}}};
    reject(world.attach_batch(forest,unrelated),"Origin plan modified an existing World member.");
    require(take(world.members()).size()==size && begins==0 && !take(w::space_reference(runtime,a_id)),
        "Invalid final graph plans changed members, references or Begin.");
    const std::array complete{w::SpaceReference{a_id,b_id},w::SpaceReference{b_id,ObjectId{}}};
    const auto matrices=take(world.attachment_world_matrices(forest,complete));
    require(matrices.size()==2 && near(matrices.front().second[12],7) && !take(w::space_reference(runtime,a_id)),
        "Prospective preview changed live relations or computed a temporary graph.");
    committed(world.attach_batch(forest,complete));require(begins==2,"Final graph publication did not begin each new Entity once.");
    auto follower=take(w::create_entity(runtime));take(world.set_transform(follower.id(),{{4,0,0},{},{1,1,1}}));
    const std::array followers{w::EntityAttachment{follower.id()}};
    const std::array external{w::SpaceReference{follower.id(),root.id()}};
    require(near(take(world.attachment_world_matrices(followers,external)).front().second[12],14),
        "Detached forest preview rejected an existing member spatial origin.");
    committed(world.attach_batch(followers,external));
    require(near(take(w::world_matrix(runtime,follower.id()))[12],14),"Final graph failed to publish an external origin.");
    const auto follower_local=take(w::read_transform(runtime,follower.id()));
    const auto follower_matrix=take(w::world_matrix(runtime,follower.id()));
    const auto complete_members=take(world.members()).size();
    reject(world.detach(root.id()),"Departure accepted a retained member's incoming spatial dependency.");
    require(take(world.members()).size()==complete_members,"Incoming dependency rejection changed World membership.");
    auto removed=take(world.detach(follower.id()));
    require(!removed.commit.notification_error && take(world.members()).size()==complete_members-1 &&
        take(w::space_reference(runtime,follower.id()))==std::optional<ObjectId>{root.id()} &&
        take(w::read_transform(runtime,follower.id()))==follower_local,
        "Outgoing dependency departure cleared its author origin or compensated local TRS.");
    reject(w::world_matrix(runtime,follower.id()),"Detached outward spatial reference fabricated a cross-membership matrix.");
    w::World other(runtime,types);
    reject(other.attach(follower.id()),"Detached outgoing origin crossed into another World.");
    require(take(other.members()).empty() && take(w::space_reference(runtime,follower.id()))==std::optional<ObjectId>{root.id()},
        "Rejected cross-World attachment changed the retained origin.");
    committed(world.restore(removed));
    require(take(world.members()).size()==complete_members && take(w::read_transform(runtime,follower.id()))==follower_local &&
        take(w::world_matrix(runtime,follower.id())).elements==follower_matrix.elements,
        "Restoring an outgoing-origin history changed authored space or its effective matrix.");
    auto huge_parent=take(w::create_entity(runtime)),huge_child=take(w::create_entity(runtime));
    const w::Trs huge{{},{},{1e30f,1e30f,1e30f}};
    take(world.set_transform(huge_parent.id(),huge));take(world.set_transform(huge_child.id(),huge));
    committed(world.reparent(huge_child.id(),huge_parent.id(),w::ReparentRule::keep_local));
    require(take(w::parent_entity(runtime,huge_child.id()))==huge_parent.id() &&
        take(w::read_transform(runtime,huge_child.id()))==huge,
        "Detached keep-local assembly evaluated an unfinished spatial graph or changed its local values.");
    reject(world.reparent(huge_parent.id(),huge_child.id(),w::ReparentRule::keep_local),
        "Deferred Detached space validation also deferred a composition cycle.");
    reject(world.reparent(huge_child.id(),{},w::ReparentRule::keep_world),
        "Detached keep-world accepted an invalid current spatial matrix.");
    const auto before_huge=take(world.members()).size();
    reject(world.attach(huge_parent.id()),"Final publication accepted overflowing inherited space.");
    require(take(world.members()).size()==before_huge && take(w::parent_entity(runtime,huge_child.id()))==huge_parent.id() &&
        take(w::read_transform(runtime,huge_child.id()))==huge,"Rejected unfinished space partially published or changed composition.");
    const std::array huge_forest{w::EntityAttachment{huge_parent.id()}};
    const std::array huge_spaces{w::SpaceReference{huge_child.id(),ObjectId{}}};
    const auto huge_matrices=take(world.attachment_world_matrices(huge_forest,huge_spaces));
    require(huge_matrices.size()==2 && std::ranges::all_of(huge_matrices,[](const auto& item){
        return std::ranges::all_of(item.second.elements,[](float value){return std::isfinite(value);});
    }),"Final explicit world origin did not repair the temporary inherited-space overflow.");
    committed(world.attach_batch(huge_forest,huge_spaces));
    require(take(w::world_matrix(runtime,huge_child.id()))[0]==1e30f,
        "Final explicit world space multiplied the child's author scale by its composition parent.");
    committed(world.close());
    std::puts("[pass] Final space plans: atomic origin publication, preview/Cook matrices, root boundaries, inherited Transform removal and Detached running authoring");
}
void mirrored_and_degenerate_space() {
    Runtime runtime;const auto types=register_all_types(runtime);w::World world(runtime,types);const auto scene=take(p::Scene::create(world,types));
    const auto same=[](const math::Matrix4& left,const math::Matrix4& right){for(std::size_t i=0;i<16;++i) if(std::abs(left[i]-right[i])>1e-4f) return false;return true;};
    const auto item=[&](ObjectId entity)->std::optional<p::RenderItem>{
        for(const auto& value:take(scene.render_items())) if(value.entity==entity) return value;
        return std::nullopt;
    };
    const float half=std::sqrt(.5f);
    // A negative axis mirrors: S*R*T keeps its sign and the matrix turns left-handed.
    auto mirror=take(w::create_entity(runtime));auto mirror_shape=add_shape(runtime,types,world,mirror.id());
    take(world.set_transform(mirror.id(),{{1,2,3},{},{-2,3,4}}));committed(world.attach(mirror.id()));
    const auto mirrored=item(mirror.id());
    require(mirrored && mirrored->world[0]==-2 && mirrored->world[5]==3 && mirrored->world[10]==4 && mirrored->world[12]==1,
        "Negative scale did not produce a mirrored S*R*T matrix.");
    // Decomposition keeps a proper rotation: X carries the reflection unless the
    // object's own sign pattern has the same parity.
    const auto inverted=w::transform_matrix({{},{},{-1,-1,-1}});
    const auto split=take(w::relative_transform(inverted,w::transform_matrix({})));
    const auto kept=take(w::relative_transform(inverted,w::transform_matrix({}),{-1,-1,-1}));
    require(near(split.scale.x,-1) && near(split.scale.y,1) && near(split.scale.z,1) && near(std::abs(split.rotation.x),1) &&
        near(kept.scale.x,-1) && near(kept.scale.y,-1) && near(kept.scale.z,-1) && near(std::abs(kept.rotation.w),1) &&
        same(w::transform_matrix(split),inverted) && same(w::transform_matrix(kept),inverted),
        "Mirrored decomposition lost the matrix or the preferred sign pattern.");
    auto a=take(w::create_entity(runtime)),d=take(w::create_entity(runtime)),b=take(w::create_entity(runtime)),c=take(w::create_entity(runtime));
    take(world.set_transform(a.id(),{{5,0,0},{0,0,half,half},{2,2,2}}));
    take(world.set_transform(d.id(),{{0,5,0},{half,0,0,half},{4,4,4}}));
    take(world.set_transform(b.id(),{{0,0,5},{0,half,0,half},{-1,1,1}}));
    take(world.set_transform(c.id(),{{1,0,0},{0,std::sin(.2f),0,std::cos(.2f)},{1,-1,1}}));
    committed(world.reparent(c.id(),a.id(),w::ReparentRule::keep_local));
    committed(world.attach(a.id()));committed(world.attach(d.id()));committed(world.attach(b.id()));
    const auto placed=take(w::world_matrix(runtime,c.id()));
    committed(world.reparent(c.id(),d.id(),w::ReparentRule::keep_world));
    auto local=take(w::read_transform(runtime,c.id()));
    require(same(take(w::world_matrix(runtime,c.id())),placed) && local.scale.x>0 && local.scale.y<0 && local.scale.z>0,
        "Keep-world under an unmirrored parent lost the world pose or the Entity's own mirrored axis.");
    committed(world.reparent(c.id(),b.id(),w::ReparentRule::keep_world));
    local=take(w::read_transform(runtime,c.id()));
    require(same(take(w::world_matrix(runtime,c.id())),placed) && local.scale.x>0 && local.scale.y>0 && local.scale.z>0,
        "Keep-world under a mirrored parent lost the world pose or kept a now-redundant reflection.");
    // A zero axis is stored; its subtree is skipped and reported once, on the cause.
    auto flat=take(w::create_entity(runtime)),u=take(w::create_entity(runtime)),v=take(w::create_entity(runtime));
    auto u_shape=add_shape(runtime,types,world,u.id()),v_shape=add_shape(runtime,types,world,v.id());
    take(world.set_transform(flat.id(),{{0,0,0},{},{1,0,1}}));
    committed(world.reparent(u.id(),flat.id(),w::ReparentRule::keep_local));committed(world.reparent(v.id(),u.id(),w::ReparentRule::keep_local));
    committed(world.attach(flat.id()));
    (void)world.take_diagnostics();
    require(!item(u.id()) && !item(v.id()) && item(mirror.id()),"Degenerate space was drawn or hid an unrelated Entity.");
    auto report=world.take_diagnostics();
    require(report.size()==1 && report[0].object==flat.id() && report[0].kind==w::DiagnosticKind::degenerate_value &&
        report[0].phase==w::DiagnosticPhase::presentation && report[0].severity==w::Severity::warning &&
        report[0].field==std::vector<std::string>{"scale"},
        "Degenerate space was not reported once on its zero-scale cause.");
    require(!item(u.id()) && world.take_diagnostics().empty(),"A persisting degenerate space was reported again.");
    take(world.set_failure_policy(w::FailurePolicy::halt));
    require(!item(v.id()) && item(mirror.id()),"Halt treated a degenerate space as an extraction failure.");
    take(world.set_failure_policy(w::FailurePolicy::isolate));
    reject(world.reparent(v.id(),mirror.id(),w::ReparentRule::keep_world),"Keep-world accepted a singular world pose.");
    reject(world.reparent(b.id(),flat.id(),w::ReparentRule::keep_world),"Keep-world accepted a singular parent.");
    take(world.set_transform(flat.id(),{{0,0,0},{},{1,1,1}}));
    require(item(u.id()) && item(v.id()),"Restoring the zero axis did not restore its subtree.");
    take(world.set_transform(flat.id(),{{0,0,0},{},{0,1,1}}));(void)item(u.id());
    require(world.take_diagnostics().size()==1,"A degenerate space that recurred after resolving was not reported again.");
    // Under a rotated zero axis, a rotated child's FP32 matrix keeps a rounding
    // residue in its determinant; the space is still degenerate.
    const auto turn=[](float degrees,math::Vector3 axis){
        const float length=std::sqrt(axis.x*axis.x+axis.y*axis.y+axis.z*axis.z),angle=degrees*3.14159265f/360,s=std::sin(angle)/length;
        return math::Quaternion{axis.x*s,axis.y*s,axis.z*s,std::cos(angle)};
    };
    bool residue=false;
    for(const float degrees:{17.f,37.f,53.f,71.f}) {
        auto tilted=take(w::create_entity(runtime)),leaf=take(w::create_entity(runtime));
        auto tilted_shape=add_shape(runtime,types,world,tilted.id()),leaf_shape=add_shape(runtime,types,world,leaf.id());
        take(world.set_transform(tilted.id(),{{},turn(degrees,{1,2,3}),{0,1,1}}));
        take(world.set_transform(leaf.id(),{{},turn(29,{1,2,3}),{1,1,1}}));
        committed(world.reparent(leaf.id(),tilted.id(),w::ReparentRule::keep_local));committed(world.attach(tilted.id()));
        residue|=!w::singular(take(w::world_matrix(runtime,leaf.id())));
        (void)world.take_diagnostics();
        require(!item(leaf.id()) && !item(tilted.id()),"A mesh under a rotated zero scale axis was drawn.");
        report=world.take_diagnostics();
        require(report.size()==1 && report[0].object==tilted.id() && report[0].kind==w::DiagnosticKind::degenerate_value,
            "A rotated zero scale axis was not reported on its cause.");
        // Recomposing only the child reuses the parent's entry from this pass.
        take(world.set_transform(leaf.id(),{{},turn(41,{1,2,3}),{1,1,1}}));
        require(!item(leaf.id()) && world.take_diagnostics().empty(),"A child recomposed under a cached zero scale axis was drawn.");
        (void)take(world.detach(tilted.id()));
    }
    require(residue,"No rotated zero axis left an FP32 rounding residue; the case no longer exercises it.");
    // Light frames ignore scale magnitudes and keep signs: under a uniform mirror
    // they follow the mirrored local axes, with right negated to stay right-handed.
    std::vector<atelier::ObjectRoot> lights;
    const auto light=[&](ObjectId parent,p::LightKind kind,math::Quaternion rotation) {
        auto entity=take(w::create_entity(runtime));auto component=take(w::create_component(runtime,types.light_component));
        p::LightSettings settings;settings.kind=kind;take(p::set_light(world,component.id(),settings));
        committed(world.attach_components(entity.id(),std::array{w::Attachment{component.id()}}));
        take(world.set_transform(entity.id(),{{1,2,3},rotation,{1,1,1}}));
        committed(world.attach(entity.id(),w::append,parent));
        const auto id=component.id();lights.push_back(std::move(component));lights.push_back(std::move(entity));return id;
    };
    const auto axis=[](const math::Matrix4& m,std::size_t row) {
        const math::Vector3 value{m[row*4],m[row*4+1],m[row*4+2]};return value*(1/std::sqrt(math::dot(value,value)));
    };
    const math::Quaternion tilt{std::sin(.3f)*.6f,std::sin(.3f)*.8f,0,std::cos(.3f)},roll{0,std::sin(.5f),0,std::cos(.5f)};
    for(const auto& [scale,odd]:{std::pair{math::Vector3{-1,1,1},true},std::pair{math::Vector3{-1,-1,1},false}}) {
        auto parent=take(w::create_entity(runtime));
        take(world.set_transform(parent.id(),{{0,0,20},roll,scale}));committed(world.attach(parent.id()));
        const auto rectangle=light(parent.id(),p::LightKind::rectangle,tilt),sun=light(parent.id(),p::LightKind::directional,tilt);
        for(const auto& value:take(scene.light_items())) {
            if(value.component!=rectangle && value.component!=sun) continue;
            const auto m=take(w::world_matrix(runtime,value.entity));
            const auto right=axis(m,0)*(odd?-1.f:1.f);
            require(near(math::dot(value.direction,axis(m,2)),1) && near(math::dot(value.up,axis(m,1)),1) &&
                near(math::dot(value.right,right),1) && near(math::dot(math::cross(value.right,value.up),value.direction),1),
                "A light under a mirrored parent did not follow its mirrored local axes in a right-handed frame.");
        }
        lights.push_back(std::move(parent));
    }
    committed(world.close());
    std::puts("[pass] Mirrored and degenerate space: signed S*R*T, proper-rotation decomposition, sign-preserving keep-world, one diagnostic per zero-scale cause, mirrored light frames");
}