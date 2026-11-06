struct LifecycleEntity : w::Entity {
    float value{};

    void advance(float amount) noexcept { value += amount; }
};

struct DerivedLifecycleEntity : LifecycleEntity {};

void entity_lifecycle_and_detached_graph() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    const auto entity_type =
        take(NativeRegistration<LifecycleEntity>{runtime, "worldAcceptance.LifecycleEntity", TypeKind::object}
                 .base<w::Entity>()
                 .field<&LifecycleEntity::value>("value")
                 .method<&LifecycleEntity::advance>("advance")
                 .publish());
    const auto derived_type = take(
        NativeRegistration<DerivedLifecycleEntity>{runtime, "worldAcceptance.DerivedLifecycleEntity", TypeKind::object}
            .base<LifecycleEntity>()
            .publish());
    const auto value_field = take(runtime.find_field(entity_type, "value"));
    w::World world(runtime, types);
    w::World foreign(runtime, types);
    take(world.set_failure_policy(w::FailurePolicy::halt));
    auto entity = take(w::create_entity(runtime, derived_type));
    auto second = take(w::create_entity(runtime, entity_type));
    auto a = take(w::create_component(runtime, types.counter));
    auto b = take(w::create_component(runtime, types.counter));
    auto inserted = take(w::create_component(runtime, types.counter));
    using Event = std::pair<char, ObjectId>;
    std::vector<Event> events;
    std::vector<std::pair<w::Lifecycle, ObjectId>> registrations;
    ObjectId fail_begin;
    const auto hook = [&](char phase, ObjectId id, ObjectId failure) -> Result<void> {
        events.emplace_back(phase, id);
        if (id == failure)
            return std::unexpected(Error{ErrorCode::invocation_failed, "Intentional Entity lifecycle failure."});
        return {};
    };
    const w::World::Behavior behavior{.begin = [&](Runtime&, ObjectId id) { return hook('B', id, fail_begin); },
                                      .tick = [&](Runtime&, ObjectId id, float) { return hook('T', id, {}); },
                                      .end = [&](Runtime&, ObjectId id) { return hook('E', id, {}); }};
    take(world.bind_behavior(entity_type, behavior));
    take(world.bind_behavior(types.counter, behavior));
    take(world.observe_lifecycle([&](w::World&, w::Lifecycle phase, ObjectId id) -> Result<void> {
        if (phase == w::Lifecycle::attached || phase == w::Lifecycle::detached)
            registrations.emplace_back(phase, id);
        return {};
    }));
    const std::array composition{w::Attachment{a.id()}, w::Attachment{b.id()}};
    committed(world.attach_components(entity.id(), composition));
    committed(world.set_enabled(entity.id(), false));
    committed(world.set_enabled(a.id(), false));
    committed(world.attach(entity.id()));
    committed(world.attach(second.id()));
    const std::array foreign_addition{w::Attachment{inserted.id()}};
    committed(world.start());
    events.clear();
    committed(world.set_enabled(entity.id(), true));
    committed(world.set_enabled(a.id(), true));
    require(events.empty(), "Entity enable repeated Begin or End.");
    committed(world.attach_components(entity.id(), foreign_addition, 1));
    require(events == std::vector<Event>{{'B', inserted.id()}}, "Dynamic Component insertion repeated Entity Begin.");
    auto program = take(source::Program::create(runtime, entity_type));
    publish(*program, "async void later() { yield; self.advance(50.0); }");
    const auto later = program->entry();
    take(world.start_task(entity.id(), [later](Runtime&, ObjectId id) { return later.start(id, {}); }));
    take(world.update(0));
    require(w::detail::Access::state(world).tasks.size() == 1, "Entity task was not retained after suspension.");
    committed(world.set_enabled(entity.id(), false));
    require(w::detail::Access::state(world).tasks.size() == 0, "Disabling Entity did not cancel its own task.");
    take(world.update(0));
    require(std::get<float>(take(runtime.read(entity.id(), value_field))) == 0.f, "Cancelled Entity task resumed.");
    committed(world.set_enabled(entity.id(), true));
    events.clear();
    committed(world.stop());
    const std::vector<Event> reverse{
        {'E', second.id()}, {'E', b.id()}, {'E', inserted.id()}, {'E', a.id()}, {'E', entity.id()}};
    require(events == reverse, "Entity End did not follow reverse member/component cleanup.");
    events.clear();
    fail_begin = entity.id();
    auto failure = take(world.start());
    require(failure.notification_error && !world.running() && events == std::vector<Event>{{'B', entity.id()}},
            "Failed Entity Begin started components or received an unmatched End.");
    registrations.clear();
    committed(world.close());
    require(registrations == std::vector<std::pair<w::Lifecycle, ObjectId>>{{w::Lifecycle::detached, second.id()},
                                                                            {w::Lifecycle::detached, b.id()},
                                                                            {w::Lifecycle::detached, inserted.id()},
                                                                            {w::Lifecycle::detached, a.id()},
                                                                            {w::Lifecycle::detached, entity.id()}},
            "Whole Entity deregistration was not symmetric with registration.");

    auto garbage = take(w::create_entity(runtime));
    auto garbage_component = take(w::create_component(runtime, types.component));
    const auto garbage_id = garbage.id();
    const auto garbage_component_id = garbage_component.id();
    const std::array garbage_shape{w::Attachment{garbage_component.id()}};
    committed(foreign.attach_components(garbage.id(), garbage_shape));
    garbage.reset();
    garbage_component.reset();
    collect(runtime);
    require(!runtime.type_of(garbage_id) && !runtime.type_of(garbage_component_id),
            "Never registered detached composition cycle did not collect.");
    committed(foreign.close());
    std::puts("[pass] Entity lifetime: JIT task cancellation, failed Begin, reverse cleanup and detached GC");
}
