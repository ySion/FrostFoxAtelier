struct LifecycleActor : w::Actor {
    float value{};
    void advance(float amount) noexcept { value += amount; }
};
struct DerivedLifecycleActor : LifecycleActor {};

void actor_lifecycle_and_detached_graph() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    const auto actor_type = take(NativeRegistration<LifecycleActor>{runtime, "worldAcceptance.LifecycleActor", TypeKind::object}
        .base<w::Actor>().field<&LifecycleActor::value>("value").method<&LifecycleActor::advance>("advance").publish());
    const auto derived_type = take(NativeRegistration<DerivedLifecycleActor>{runtime, "worldAcceptance.DerivedLifecycleActor", TypeKind::object}
        .base<LifecycleActor>().publish());
    const auto value_field = take(runtime.find_field(actor_type, "value"));
    w::World world(runtime, types), foreign(runtime, types);
    auto actor = take(w::create_actor(runtime, derived_type));
    auto second = take(w::create_actor(runtime, actor_type));
    auto a = take(w::create_component(runtime, types.counter));
    auto b = take(w::create_component(runtime, types.counter));
    auto inserted = take(w::create_component(runtime, types.counter));
    using Event = std::pair<char, ObjectId>;
    std::vector<Event> events;
    std::vector<std::pair<w::Lifecycle,ObjectId>> registrations;
    ObjectId fail_begin, fail_tick, fail_end;
    bool queue_disable{};
    bool fail_registration{true};
    const auto hook = [&](char phase, ObjectId id, ObjectId failure) -> Result<void> {
        events.emplace_back(phase,id);
        if (queue_disable && phase == 'T' && id == actor.id()) {
            queue_disable = false;
            reject(world.stop(), "Actor Tick reentered World stop.");
            reject(world.set_enabled(id,false), "Actor Tick reentered enable mutation.");
            take(world.request({.kind=w::RequestKind::actor_enabled,.target=id,.enabled=false}));
        }
        if (id == failure) return std::unexpected(Error{ErrorCode::invocation_failed,"Intentional Actor lifecycle failure."});
        return {};
    };
    const w::World::Behavior behavior{
        .begin=[&](Runtime&, ObjectId id) { return hook('B',id,fail_begin); },
        .tick=[&](Runtime&, ObjectId id,float) { return hook('T',id,fail_tick); },
        .end=[&](Runtime&, ObjectId id) { return hook('E',id,fail_end); }
    };
    take(world.bind_behavior(actor_type,behavior));
    take(world.bind_behavior(types.counter,behavior));
    take(world.observe_lifecycle([&](w::World&,w::Lifecycle phase,ObjectId id)->Result<void> {
        if (phase == w::Lifecycle::attached || phase == w::Lifecycle::detached) registrations.emplace_back(phase,id);
        if (fail_registration && phase == w::Lifecycle::attached && id == actor.id()) {
            fail_registration = false;
            return std::unexpected(Error{ErrorCode::invocation_failed,"Intentional whole-group registration observer failure."});
        }
        return {};
    }));
    const std::array composition{w::Attachment{a.id()},w::Attachment{b.id()}};
    committed(world.attach_components(actor.id(),composition));
    committed(world.set_enabled(actor.id(),false));
    committed(world.set_enabled(a.id(),false));
    require(events.empty() && registrations.empty() && take(world.members()).empty() && take(world.query(types.component)).empty(),
        "Detached composition created World participation.");
    const auto registration_failure = take(world.attach(actor.id()));
    require(registration_failure.changed && registration_failure.notification_error && events.empty() &&
        take(world.members()).size() == 1 && take(w::components(runtime,actor.id())).size() == 2,
        "Whole-group observer failure rolled back the committed graph or ran Begin in a stopped World.");
    committed(world.attach(second.id()));
    require(registrations == std::vector<std::pair<w::Lifecycle,ObjectId>>{
        {w::Lifecycle::attached,actor.id()},{w::Lifecycle::attached,a.id()},{w::Lifecycle::attached,b.id()},{w::Lifecycle::attached,second.id()}},
        "Whole Actor registration did not include its detached composition in order.");
    reject(foreign.set_enabled(actor.id(),true), "Foreign World changed Actor enabled state.");
    reject(foreign.set_enabled(a.id(),true), "Foreign World changed owned Component enabled state.");
    const std::array foreign_addition{w::Attachment{inserted.id()}};
    reject(foreign.attach_components(actor.id(),foreign_addition), "Foreign World extended an Actor composition.");
    committed(world.start());
    require(events == std::vector<Event>{{'B',actor.id()},{'B',a.id()},{'B',b.id()},{'B',second.id()}},
        "Inherited Actor Begin did not precede disabled components.");
    events.clear(); take(world.update(1));
    require(events == std::vector<Event>{{'T',second.id()}}, "Disabled Actor did not gate its own and Component Tick.");
    events.clear(); committed(world.set_enabled(actor.id(),true)); committed(world.set_enabled(a.id(),true));
    require(events.empty(), "Actor enable repeated Begin or End.");
    committed(world.attach_components(actor.id(),foreign_addition,1));
    require(events == std::vector<Event>{{'B',inserted.id()}}, "Dynamic Component insertion repeated Actor Begin.");
    events.clear(); take(world.update(1));
    require(events == std::vector<Event>{{'T',actor.id()},{'T',a.id()},{'T',inserted.id()},{'T',b.id()},{'T',second.id()}},
        "Actor Tick did not precede the current composition order.");
    source::Program program(runtime,actor_type);
    publish(program,"async void later() { yield; self.advance(50.0); }");
    const auto later = take(program.bind<void>(FunctionExecution::coroutine));
    take(world.start_task(actor.id(),[later](Runtime&,ObjectId id) { return later.start(id,{}); }));
    take(world.update(0)); require(world.activity().tasks == 1,"Actor task was not retained after suspension.");
    committed(world.set_enabled(actor.id(),false));
    require(world.activity().tasks == 0,"Disabling Actor did not cancel its own task.");
    take(world.update(0));
    require(std::get<float>(take(runtime.read(actor.id(),value_field))) == 0.f,"Cancelled Actor task resumed.");
    committed(world.set_enabled(actor.id(),true)); events.clear();
    queue_disable = true; take(world.update(1));
    const auto requests = take(world.flush());
    require(requests.size() == 1 && requests.front().result && !take(w::inspect_actor(runtime,actor.id())).enabled,
        "Actor Tick deferred request failed to commit at the boundary.");
    committed(world.set_enabled(actor.id(),true)); events.clear(); committed(world.stop());
    const std::vector<Event> reverse{{'E',second.id()},{'E',b.id()},{'E',inserted.id()},{'E',a.id()},{'E',actor.id()}};
    require(events == reverse,"Actor End did not follow reverse member/component cleanup.");
    events.clear(); fail_begin = actor.id();
    auto failure = take(world.start());
    require(failure.notification_error && !world.running() && events == std::vector<Event>{{'B',actor.id()}},
        "Failed Actor Begin started components or received an unmatched End.");
    events.clear(); fail_begin = b.id(); failure = take(world.start());
    require(failure.notification_error && events == std::vector<Event>{{'B',actor.id()},{'B',a.id()},{'B',inserted.id()},{'B',b.id()},
        {'E',inserted.id()},{'E',a.id()},{'E',actor.id()}},"Component Begin failure did not unwind its begun Actor.");
    fail_begin = {}; events.clear(); committed(world.start()); events.clear(); fail_tick = actor.id();
    reject(world.update(1),"Actor Tick failure was swallowed.");
    auto tick_failure = std::vector<Event>{{'T',actor.id()}}; tick_failure.insert(tick_failure.end(),reverse.begin(),reverse.end());
    require(events == tick_failure && !world.running(),"Actor Tick failure left World participation alive.");
    fail_tick = {}; events.clear(); committed(world.start()); events.clear(); fail_end = second.id(); failure = take(world.stop());
    require(failure.notification_error && events == reverse,"Actor End failure abandoned remaining cleanup.");
    fail_end = {}; events.clear(); take(world.bind_behavior(derived_type,{}));
    committed(world.start()); take(world.update(1)); committed(world.stop());
    require(std::ranges::none_of(events,[&](const Event& e) { return e.second == actor.id(); }),
        "Derived Actor empty binding did not replace the whole inherited behavior.");
    registrations.clear(); committed(world.close());
    require(registrations == std::vector<std::pair<w::Lifecycle,ObjectId>>{
        {w::Lifecycle::detached,second.id()},{w::Lifecycle::detached,b.id()},{w::Lifecycle::detached,inserted.id()},
        {w::Lifecycle::detached,a.id()},{w::Lifecycle::detached,actor.id()}},"Whole Actor deregistration was not symmetric with registration.");

    // Detached spatial assembly validates the complete batch before publishing relations.
    auto detached = take(w::create_actor(runtime));
    auto root = take(w::create_component(runtime,types.scene_component));
    auto child = take(w::create_component(runtime,types.static_mesh_component));
    const std::array missing_root{w::Attachment{child.id()}};
    reject(foreign.attach_components(detached.id(),missing_root),"Detached SceneComponent accepted a missing root.");
    const std::array cycle{w::Attachment{root.id(),{},true},w::Attachment{child.id(),child.id()}};
    reject(foreign.attach_components(detached.id(),cycle),"Detached spatial cycle was accepted.");
    auto cross_actor = take(w::create_actor(runtime));
    auto cross_root = take(w::create_component(runtime,types.scene_component));
    const std::array cross_shape{w::Attachment{cross_root.id(),{},true}};
    committed(foreign.attach_components(cross_actor.id(),cross_shape));
    const std::array cross_owner{w::Attachment{root.id(),{},true},w::Attachment{child.id(),cross_root.id()}};
    reject(foreign.attach_components(detached.id(),cross_owner),"Detached graph accepted a foreign owner parent.");
    require(take(w::components(runtime,detached.id())).empty() && take(w::inspect_component(runtime,root.id())).owner == ObjectId{} &&
        take(w::inspect_component(runtime,child.id())).owner == ObjectId{},"Rejected detached batch partially published relations.");
    take(foreign.set_transform(root.id(),{{3,0,0},{},{1,1,1}}));
    const std::array shape{w::Attachment{child.id(),root.id()},w::Attachment{root.id(),{},true}};
    committed(foreign.attach_components(detached.id(),shape));
    take(foreign.set_transform(child.id(),{{0,2,0},{},{1,1,1}}));
    take(w::set_geometry(runtime,child.id(),geometry::cube_mesh()));
    const auto matrix = take(w::world_matrix(runtime,child.id()));
    require(near(matrix[12],3) && near(matrix[13],2) && take(foreign.render_items()).empty(),"Detached tree lost transforms or entered rendering.");
    committed(foreign.attach(detached.id()));
    require(take(foreign.render_items()).size() == 1,"Preassembled graph did not enter presentation on attach.");
    w::World outsider(runtime,types);
    reject(outsider.set_transform(child.id(),{}),"Foreign World edited a member Transform.");
    auto detached_history = take(foreign.detach(detached.id())); detached_history.root.reset();
    committed(w::destroy_actor(runtime,detached.id()));
    require(!runtime.type_of(root.id()) && !runtime.type_of(child.id()),"Detached graph destruction retained spatial children.");
    auto garbage = take(w::create_actor(runtime));
    auto garbage_root = take(w::create_component(runtime,types.scene_component));
    const auto garbage_id = garbage.id(), garbage_root_id = garbage_root.id();
    const std::array garbage_shape{w::Attachment{garbage_root.id(),{},true}};
    committed(foreign.attach_components(garbage.id(),garbage_shape));
    garbage.reset(); garbage_root.reset(); collect(runtime);
    require(!runtime.type_of(garbage_id) && !runtime.type_of(garbage_root_id),"Never registered detached composition cycle did not collect.");
    std::puts("[pass] Actor lifecycle and detached assembly: inherited hooks, ordering, tasks, failures, requests, symmetric registration, atomic graph and GC");
}
