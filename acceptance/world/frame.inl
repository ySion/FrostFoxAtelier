void frame_phases() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    w::World world(runtime, types);
    using Event = std::tuple<char, ObjectId, float>;
    std::vector<Event> events;
    const auto record = [&events](char phase) {
        return [&events, phase](Runtime&, ObjectId id, float dt) -> Result<void> {
            events.emplace_back(phase, id, dt);
            return {};
        };
    };
    const auto ended = [&events](Runtime&, ObjectId id) -> Result<void> {
        events.emplace_back('E', id, 0.f);
        return {};
    };
    const auto at = [&events](char phase, ObjectId id) {
        const auto found = std::ranges::find_if(
            events, [&](const Event& e) { return std::get<0>(e) == phase && std::get<1>(e) == id; });
        require(found != events.end(), "An expected Tick did not run.");
        return found - events.begin();
    };
    const auto count = [&events](char phase, ObjectId id) {
        return std::ranges::count_if(events,
                                     [&](const Event& e) { return std::get<0>(e) == phase && std::get<1>(e) == id; });
    };
    auto a = take(w::create_entity(runtime));
    auto b = take(w::create_entity(runtime));
    auto c = take(w::create_entity(runtime));
    auto d = take(w::create_entity(runtime));
    for (const auto* entity : {&a, &b, &c, &d})
        committed(world.attach(entity->id()));
    auto k = take(w::create_component(runtime, types.counter));
    committed(world.attach_components(c.id(), std::array{w::Attachment{k.id()}}));
    take(world.bind_behavior(
        types.entity, {.tick = record('P'), .end = ended, .post_physics_tick = record('Q'), .late_tick = record('L')}));
    take(world.bind_behavior(types.counter, {.tick = record('K')}));
    take(world.add_simulation({.step = [&](float dt) -> Result<void> {
                                   events.emplace_back('S', ObjectId{}, dt);
                                   return {};
                               },
                               .writes_pose = [](ObjectId) { return false; },
                               .relocated = {}}));
    committed(world.start());
    take(world.update(.25f));

    // b -> k -> a also exercises removal of a transitive prerequisite below.
    take(world.add_tick_prerequisite(k.id(), b.id()));
    take(world.add_tick_prerequisite(a.id(), k.id()));

    // Requests queued by a Tick commit when its phase ends.
    const auto d_id = d.id();
    bool queued = false;
    bool nested_bind = false;
    take(world.bind_behavior(types.entity,
                             {.begin = [&](Runtime&, ObjectId id) -> Result<void> {
                                  events.emplace_back('B', id, 0.f);
                                  return {};
                              },
                              .tick = [&](Runtime&, ObjectId id, float dt) -> Result<void> {
                                  events.emplace_back('p', id, dt);
                                  if (id != a.id() || queued)
                                      return {};
                                  queued = true;
                                  nested_bind = !world.bind_behavior(types.counter, {});
                                  take(world.request({.kind = w::RequestKind::destroy_entity, .target = d_id}));
                                  take(world.request(
                                      {.kind = w::RequestKind::entity_enabled, .target = d_id, .enabled = false}));
                                  return {};
                              },
                              .end = ended,
                              .post_physics_tick = record('Q'),
                              .late_tick = record('L')}));
    events.clear();
    take(world.update(.25f));
    require(count('B', a.id()) == 0 && count('P', a.id()) == 0 && count('p', a.id()) == 1 && nested_bind,
            "A running rebind replayed Begin, kept the old Tick or was allowed inside Tick dispatch.");
    require(count('E', d_id) == 1 && at('E', d_id) < at('S', {}) && count('Q', d_id) == 0 && count('L', d_id) == 0 &&
                !runtime.type_of(d_id),
            "A Tick-queued destroy did not commit with paired End when its phase ended.");

    // Source requests use the same queue.
    auto program = take(source::Program::create(runtime, types.counter));
    publish(*program, "void tick(float dt) {\n    self.advance(dt);\n"
                      "    if (self.steps == 1) { self.set_enabled(false); } else { self.destroy(); }\n}\n");
    const auto entry = take(program->entry().bind<void, float>());
    take(
        world.bind_behavior(types.counter, {.tick = [entry](Runtime&, ObjectId receiver, float amount) -> Result<void> {
                                return entry.invoke(receiver, amount);
                            }}));
    const auto steps = [&] {
        return take(
            runtime.with_native<Counter>(k.id(), [](Counter& value) -> Result<std::uint32_t> { return value.steps; }));
    };
    take(world.update(1.f));
    require(steps() == 1 && !take(w::inspect_component(runtime, k.id())).active,
            "Source set_enabled did not commit at the end of its phase.");
    take(world.update(1.f));
    require(steps() == 1, "A component disabled by Source still ticked.");
    committed(world.set_enabled(k.id(), true));
    const auto k_id = k.id();
    take(world.update(1.f));
    require(!runtime.type_of(k_id) && take(w::components(runtime, c.id())).empty(),
            "Source destroy did not commit at the end of its phase.");
    events.clear();
    take(world.update(.25f));
    require(count('p', a.id()) == 1 && count('L', a.id()) == 1 && count('L', b.id()) == 1,
            "A destroyed prerequisite left a dangling tick edge.");

    events.clear();
    committed(world.stop());
    require(count('E', a.id()) == 1 && count('B', a.id()) == 0,
            "Running rebind did not pair End through the current binding.");
    committed(world.close());

    // Execution prevents GC, not explicit native destruction. The next Tick must
    // still reject a dead receiver before entering even a callback that ignores it.
    {
        w::World invalidated(runtime, types);
        auto owner = take(w::create_entity(runtime));
        auto first = take(w::create_component(runtime, types.counter));
        auto next = take(w::create_component(runtime, types.counter));
        committed(
            invalidated.attach_components(owner.id(), std::array{w::Attachment{first.id()}, w::Attachment{next.id()}}));
        committed(invalidated.attach(owner.id()));
        bool gc_blocked = false;
        bool dead_ticked = false;
        take(invalidated.bind_behavior(types.counter, {.tick = [&](Runtime& rt, ObjectId id, float) -> Result<void> {
                                           if (id == first.id()) {
                                               const auto gc = rt.begin_collection();
                                               gc_blocked = !gc && gc.error().code == ErrorCode::busy;
                                               return rt.destroy(next.id());
                                           }
                                           dead_ticked = true;
                                           return {};
                                       }}));
        take(invalidated.add_tick_prerequisite(next.id(), first.id()));
        committed(invalidated.start());
        const auto updated = invalidated.update(.25f);
        const bool stopped = !invalidated.running();
        // Close before assertions so the intentionally invalid component cannot
        // hide an assertion failure in fixture teardown. The owner remains live.
        const auto closed = invalidated.close();
        require(!updated && updated.error().code == ErrorCode::invalid_object && stopped && !dead_ticked && gc_blocked,
                "Tick lost its live receiver check or execution-time GC barrier.");
        require(bool(closed) && invalidated.closed(), "A stopped World with a dead Tick receiver did not close.");
    }
    std::puts("[pass] Frame lifetime: phase-end native/Source destruction and execution-time GC barrier");
}
