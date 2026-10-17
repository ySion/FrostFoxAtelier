struct LifecycleEntity : w::Entity {
    float value{};
    void advance(float amount) noexcept { value += amount; }
};
struct DerivedLifecycleEntity : LifecycleEntity {};

void entity_lifecycle_and_detached_graph() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    const auto entity_type = take(NativeRegistration<LifecycleEntity>{runtime, "worldAcceptance.LifecycleEntity", TypeKind::object}
        .base<w::Entity>().field<&LifecycleEntity::value>("value").method<&LifecycleEntity::advance>("advance").publish());
    const auto derived_type = take(NativeRegistration<DerivedLifecycleEntity>{runtime, "worldAcceptance.DerivedLifecycleEntity", TypeKind::object}
        .base<LifecycleEntity>().publish());
    const auto value_field = take(runtime.find_field(entity_type, "value"));
    w::World world(runtime, types), foreign(runtime, types);
    take(world.set_failure_policy(w::FailurePolicy::halt));
    auto entity = take(w::create_entity(runtime, derived_type));
    auto second = take(w::create_entity(runtime, entity_type));
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
        if (queue_disable && phase == 'T' && id == entity.id()) {
            queue_disable = false;
            reject(world.stop(), "Entity Tick reentered World stop.");
            reject(world.set_enabled(id,false), "Entity Tick reentered enable mutation.");
            take(world.request({.kind=w::RequestKind::entity_enabled,.target=id,.enabled=false}));
        }
        if (id == failure) return std::unexpected(Error{ErrorCode::invocation_failed,"Intentional Entity lifecycle failure."});
        return {};
    };
    const w::World::Behavior behavior{
        .begin=[&](Runtime&, ObjectId id) { return hook('B',id,fail_begin); },
        .tick=[&](Runtime&, ObjectId id,float) { return hook('T',id,fail_tick); },
        .end=[&](Runtime&, ObjectId id) { return hook('E',id,fail_end); }
    };
    take(world.bind_behavior(entity_type,behavior));
    take(world.bind_behavior(types.counter,behavior));
    take(world.observe_lifecycle([&](w::World&,w::Lifecycle phase,ObjectId id)->Result<void> {
        if (phase == w::Lifecycle::attached || phase == w::Lifecycle::detached) registrations.emplace_back(phase,id);
        if (fail_registration && phase == w::Lifecycle::attached && id == entity.id()) {
            fail_registration = false;
            return std::unexpected(Error{ErrorCode::invocation_failed,"Intentional whole-group registration observer failure."});
        }
        return {};
    }));
    const std::array composition{w::Attachment{a.id()},w::Attachment{b.id()}};
    committed(world.attach_components(entity.id(),composition));
    committed(world.set_enabled(entity.id(),false));
    committed(world.set_enabled(a.id(),false));
    require(events.empty() && registrations.empty() && take(world.members()).empty() && take(world.query(types.component)).empty(),
        "Detached composition created World participation.");
    const auto registration_failure = take(world.attach(entity.id()));
    require(registration_failure.changed && registration_failure.notification_error && events.empty() &&
        take(world.members()).size() == 1 && take(w::components(runtime,entity.id())).size() == 2,
        "Whole-group observer failure rolled back the committed graph or ran Begin in a stopped World.");
    committed(world.attach(second.id()));
    require(registrations == std::vector<std::pair<w::Lifecycle,ObjectId>>{
        {w::Lifecycle::attached,entity.id()},{w::Lifecycle::attached,a.id()},{w::Lifecycle::attached,b.id()},{w::Lifecycle::attached,second.id()}},
        "Whole Entity registration did not include its detached composition in order.");
    reject(foreign.set_enabled(entity.id(),true), "Foreign World changed Entity enabled state.");
    reject(foreign.set_enabled(a.id(),true), "Foreign World changed owned Component enabled state.");
    const std::array foreign_addition{w::Attachment{inserted.id()}};
    reject(foreign.attach_components(entity.id(),foreign_addition), "Foreign World extended an Entity composition.");
    committed(world.start());
    require(events == std::vector<Event>{{'B',entity.id()},{'B',a.id()},{'B',b.id()},{'B',second.id()}},
        "Inherited Entity Begin did not precede disabled components.");
    events.clear(); take(world.update(1));
    require(events == std::vector<Event>{{'T',second.id()}}, "Disabled Entity did not gate its own and Component Tick.");
    events.clear(); committed(world.set_enabled(entity.id(),true)); committed(world.set_enabled(a.id(),true));
    require(events.empty(), "Entity enable repeated Begin or End.");
    committed(world.attach_components(entity.id(),foreign_addition,1));
    require(events == std::vector<Event>{{'B',inserted.id()}}, "Dynamic Component insertion repeated Entity Begin.");
    events.clear(); take(world.update(1));
    require(events == std::vector<Event>{{'T',entity.id()},{'T',a.id()},{'T',inserted.id()},{'T',b.id()},{'T',second.id()}},
        "Entity Tick did not precede the current composition order.");
    source::Program program(runtime,entity_type);
    publish(program,"async void later() { yield; self.advance(50.0); }");
    const auto later = take(program.bind<void>(FunctionExecution::coroutine));
    take(world.start_task(entity.id(),[later](Runtime&,ObjectId id) { return later.start(id,{}); }));
    take(world.update(0)); require(world.activity().tasks == 1,"Entity task was not retained after suspension.");
    committed(world.set_enabled(entity.id(),false));
    require(world.activity().tasks == 0,"Disabling Entity did not cancel its own task.");
    take(world.update(0));
    require(std::get<float>(take(runtime.read(entity.id(),value_field))) == 0.f,"Cancelled Entity task resumed.");
    committed(world.set_enabled(entity.id(),true)); events.clear();
    queue_disable = true; take(world.update(1));
    const auto requests = take(world.flush());
    require(requests.size() == 1 && requests.front().result && !take(w::inspect_entity(runtime,entity.id())).enabled,
        "Entity Tick deferred request failed to commit at the boundary.");
    committed(world.set_enabled(entity.id(),true)); events.clear(); committed(world.stop());
    const std::vector<Event> reverse{{'E',second.id()},{'E',b.id()},{'E',inserted.id()},{'E',a.id()},{'E',entity.id()}};
    require(events == reverse,"Entity End did not follow reverse member/component cleanup.");
    events.clear(); fail_begin = entity.id();
    auto failure = take(world.start());
    require(failure.notification_error && !world.running() && events == std::vector<Event>{{'B',entity.id()}},
        "Failed Entity Begin started components or received an unmatched End.");
    events.clear(); fail_begin = b.id(); failure = take(world.start());
    require(failure.notification_error && events == std::vector<Event>{{'B',entity.id()},{'B',a.id()},{'B',inserted.id()},{'B',b.id()},
        {'E',inserted.id()},{'E',a.id()},{'E',entity.id()}},"Component Begin failure did not unwind its begun Entity.");
    fail_begin = {}; events.clear(); committed(world.start()); events.clear(); fail_tick = entity.id();
    reject(world.update(1),"Entity Tick failure was swallowed.");
    auto tick_failure = std::vector<Event>{{'T',entity.id()}}; tick_failure.insert(tick_failure.end(),reverse.begin(),reverse.end());
    require(events == tick_failure && !world.running(),"Entity Tick failure left World participation alive.");
    fail_tick = {}; events.clear(); committed(world.start()); events.clear(); fail_end = second.id(); failure = take(world.stop());
    require(failure.notification_error && events == reverse,"Entity End failure abandoned remaining cleanup.");
    fail_end = {}; events.clear(); take(world.bind_behavior(derived_type,{}));
    committed(world.start()); take(world.update(1)); committed(world.stop());
    require(std::ranges::none_of(events,[&](const Event& e) { return e.second == entity.id(); }),
        "Derived Entity empty binding did not replace the whole inherited behavior.");
    registrations.clear(); committed(world.close());
    require(registrations == std::vector<std::pair<w::Lifecycle,ObjectId>>{
        {w::Lifecycle::detached,second.id()},{w::Lifecycle::detached,b.id()},{w::Lifecycle::detached,inserted.id()},
        {w::Lifecycle::detached,a.id()},{w::Lifecycle::detached,entity.id()}},"Whole Entity deregistration was not symmetric with registration.");

    // Detached capability assembly validates the complete batch before publishing owners.
    auto detached = take(w::create_entity(runtime));
    auto sibling = take(w::create_component(runtime,types.component));
    auto mesh = take(w::create_component(runtime,types.static_mesh_component));
    const std::array invalid{w::Attachment{sibling.id()},w::Attachment{detached.id()}};
    reject(foreign.attach_components(detached.id(),invalid),"Detached assembly accepted an Entity as Component.");
    const std::array duplicate{w::Attachment{mesh.id()},w::Attachment{mesh.id()}};
    reject(foreign.attach_components(detached.id(),duplicate),"Detached assembly accepted duplicate identities.");
    require(take(w::components(runtime,detached.id())).empty() && take(w::inspect_component(runtime,sibling.id())).owner == ObjectId{} &&
        take(w::inspect_component(runtime,mesh.id())).owner == ObjectId{},"Rejected detached batch partially published owners.");
    take(foreign.set_transform(detached.id(),{{3,2,0},{},{1,1,1}}));
    const std::array shape{w::Attachment{mesh.id()},w::Attachment{sibling.id()}};
    committed(foreign.attach_components(detached.id(),shape));
    take(w::set_geometry(runtime,mesh.id(),geometry::cube_mesh()));
    const auto matrix = take(w::world_matrix(runtime,detached.id()));
    require(near(matrix[12],3) && near(matrix[13],2) && take(foreign.render_items()).empty(),"Detached graph entered rendering or lost Entity TRS.");
    committed(foreign.attach(detached.id()));
    require(take(foreign.render_items()).size() == 1,"Preassembled graph did not enter presentation on attach.");
    w::World outsider(runtime,types);
    reject(outsider.set_transform(detached.id(),{}),"Foreign World edited a member Transform.");
    auto detached_history = take(foreign.detach(detached.id())); detached_history.root.reset();
    committed(w::destroy_entity(runtime,detached.id()));
    require(!runtime.type_of(sibling.id()) && !runtime.type_of(mesh.id()),"Detached destruction retained capabilities.");
    auto garbage = take(w::create_entity(runtime));
    auto garbage_component = take(w::create_component(runtime,types.component));
    const auto garbage_id = garbage.id(), garbage_component_id = garbage_component.id();
    const std::array garbage_shape{w::Attachment{garbage_component.id()}};
    committed(foreign.attach_components(garbage.id(),garbage_shape));
    garbage.reset(); garbage_component.reset(); collect(runtime);
    require(!runtime.type_of(garbage_id) && !runtime.type_of(garbage_component_id),"Never registered detached composition cycle did not collect.");
    auto pending=take(w::create_entity(runtime));
    auto pending_mesh=take(w::create_component(runtime,types.static_mesh_component));
    take(w::set_geometry(runtime,pending_mesh.id(),geometry::cube_mesh()));
    const std::array pending_shape{w::Attachment{pending_mesh.id()}};
    committed(foreign.attach_components(pending.id(),pending_shape));
    std::size_t pending_begins{};
    take(foreign.bind_behavior(types.entity,{.begin=[&](Runtime&,ObjectId)->Result<void>{++pending_begins;return {};}}));
    require(!take(w::has_transform(runtime,pending.id())) && take(w::components(runtime,pending.id())).size()==1,
        "Detached capability assembly implicitly created spatial identity.");
    reject(foreign.attach(pending.id()),"Incomplete Detached spatial owner entered World.");
    require(take(foreign.members()).empty() && pending_begins==0 && take(foreign.render_items()).empty(),
        "Failed incomplete-owner publication registered members or invoked Begin.");
    take(foreign.set_transform(pending.id(),{}));committed(foreign.attach(pending.id()));committed(foreign.start());
    require(pending_begins==1 && take(foreign.render_items()).size()==1,
        "Explicit spatial completion did not publish the same capability graph.");
    committed(foreign.stop());committed(w::destroy_entity(runtime,pending.id()));committed(foreign.close());
    std::puts("[pass] Entity lifecycle and detached assembly: inherited hooks, ordering, tasks, failures, requests, symmetric registration, atomic graph and GC");
}
