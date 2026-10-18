void frame_phases() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    w::World world(runtime, types);
    using Event = std::tuple<char, ObjectId, float>;
    std::vector<Event> events;
    const auto record = [&events](char phase) {
        return [&events, phase](Runtime&, ObjectId id, float dt) -> Result<void> { events.emplace_back(phase, id, dt); return {}; };
    };
    const auto ended = [&events](Runtime&, ObjectId id) -> Result<void> { events.emplace_back('E', id, 0.f); return {}; };
    const auto at = [&events](char phase, ObjectId id) {
        const auto found = std::ranges::find_if(events, [&](const Event& e) { return std::get<0>(e) == phase && std::get<1>(e) == id; });
        require(found != events.end(), "An expected Tick did not run.");
        return found - events.begin();
    };
    const auto count = [&events](char phase, ObjectId id) {
        return std::ranges::count_if(events, [&](const Event& e) { return std::get<0>(e) == phase && std::get<1>(e) == id; });
    };
    auto a = take(w::create_entity(runtime)), b = take(w::create_entity(runtime));
    auto c = take(w::create_entity(runtime)), d = take(w::create_entity(runtime));
    for (const auto* entity : {&a, &b, &c, &d}) committed(world.attach(entity->id()));
    auto k = take(w::create_component(runtime, types.counter));
    committed(world.attach_components(c.id(), std::array{w::Attachment{k.id()}}));
    take(world.bind_behavior(types.entity, {.tick = record('P'), .end = ended, .post_physics_tick = record('Q'), .late_tick = record('L')}));
    take(world.bind_behavior(types.counter, {.tick = record('K')}));
    take(world.add_simulation({[&](float dt) -> Result<void> { events.emplace_back('S', ObjectId{}, dt); return {}; },
        [](ObjectId) { return false; }}));
    committed(world.start());
    take(world.update(.25f));
    const auto rank = [](const Event& e) {
        const auto phase = std::get<0>(e);
        return phase == 'P' || phase == 'K' ? 0 : phase == 'S' ? 1 : phase == 'Q' ? 2 : 3;
    };
    require(events.size() == 14 && std::ranges::is_sorted(events, {}, rank) &&
        std::ranges::all_of(events, [](const Event& e) { return std::get<2>(e) == .25f; }) &&
        std::ranges::all_of(std::array{a.id(), b.id(), c.id(), d.id()}, [&](ObjectId id) {
            return count('P', id) == 1 && count('Q', id) == 1 && count('L', id) == 1; }) && count('K', k.id()) == 1,
        "Frame phases did not run pre_physics, simulation, post_physics and late once each with the whole frame dt.");

    // b -> k -> a orders a after b in every phase, including those where k has no Tick.
    take(world.add_tick_prerequisite(k.id(), b.id()));
    take(world.add_tick_prerequisite(a.id(), k.id()));
    reject(world.add_tick_prerequisite(b.id(), a.id()), "A tick prerequisite cycle through a component was accepted.");
    reject(world.add_tick_prerequisite(a.id(), a.id()), "An object became its own tick prerequisite.");
    events.clear(); take(world.update(.25f));
    require(at('P', b.id()) < at('K', k.id()) && at('K', k.id()) < at('P', a.id()) &&
        at('Q', b.id()) < at('Q', a.id()) && at('L', b.id()) < at('L', a.id()),
        "Tick prerequisites did not order their transitive closure in every phase.");

    // A running rebind takes effect from the next phase without replaying Begin.
    // Requests queued by a Tick commit when its phase ends.
    const auto d_id = d.id();
    bool queued = false, nested_bind = false;
    take(world.bind_behavior(types.entity, {
        .begin = [&](Runtime&, ObjectId id) -> Result<void> { events.emplace_back('B', id, 0.f); return {}; },
        .tick = [&](Runtime&, ObjectId id, float dt) -> Result<void> {
            events.emplace_back('p', id, dt);
            if (id != a.id() || queued) return {};
            queued = true;
            nested_bind = !world.bind_behavior(types.counter, {});
            take(world.request({.kind = w::RequestKind::destroy_entity, .target = d_id}));
            take(world.request({.kind = w::RequestKind::entity_enabled, .target = d_id, .enabled = false}));
            return {};
        },
        .end = ended, .post_physics_tick = record('Q'), .late_tick = record('L')}));
    events.clear(); take(world.update(.25f));
    require(count('B', a.id()) == 0 && count('P', a.id()) == 0 && count('p', a.id()) == 1 && nested_bind,
        "A running rebind replayed Begin, kept the old Tick or was allowed inside Tick dispatch.");
    require(count('E', d_id) == 1 && at('E', d_id) < at('S', {}) && count('Q', d_id) == 0 && count('L', d_id) == 0 &&
        !runtime.type_of(d_id), "A Tick-queued destroy did not commit with paired End when its phase ended.");
    const auto diagnostics = world.take_diagnostics();
    require(diagnostics.size() == 1 && diagnostics[0].kind == w::DiagnosticKind::command_rejected &&
        diagnostics[0].severity == w::Severity::warning && diagnostics[0].phase == w::DiagnosticPhase::command &&
        diagnostics[0].object == d_id && world.running(),
        "A stale queued request was not reported as a rejected command on its target.");

    // Source requests use the same queue.
    source::Program program(runtime, types.counter);
    publish(program, "void tick(float dt) {\n    self.advance(dt);\n"
        "    if (self.steps == 1) { self.set_enabled(false); } else { self.destroy(); }\n}\n");
    const auto entry = take(program.bind<void, float>());
    take(world.bind_behavior(types.counter, {.tick = [entry](Runtime&, ObjectId receiver, float amount) -> Result<void> {
        const std::array arguments{Scalar{amount}};
        auto result = entry.invoke(receiver, arguments);
        if (!result) return std::unexpected(result.error());
        return {};
    }}));
    const auto steps = [&] { return take(runtime.with_native<Counter>(k.id(), [](Counter& value) -> Result<std::uint32_t> { return value.steps; })); };
    take(world.update(1.f));
    require(steps() == 1 && !take(w::inspect_component(runtime, k.id())).active, "Source set_enabled did not commit at the end of its phase.");
    take(world.update(1.f));
    require(steps() == 1, "A component disabled by Source still ticked.");
    committed(world.set_enabled(k.id(), true));
    const auto k_id = k.id();
    take(world.update(1.f));
    require(!runtime.type_of(k_id) && take(w::components(runtime, c.id())).empty(), "Source destroy did not commit at the end of its phase.");
    events.clear(); take(world.update(.25f));
    require(count('p', a.id()) == 1 && count('L', a.id()) == 1 && count('L', b.id()) == 1, "A destroyed prerequisite left a dangling tick edge.");

    events.clear(); committed(world.stop());
    require(count('E', a.id()) == 1 && count('B', a.id()) == 0, "Running rebind did not pair End through the current binding.");
    committed(world.close());

    // Presentation buckets are registered per World and re-evaluated on demand.
    w::World shown(runtime, types);
    auto owner = take(w::create_entity(runtime));
    committed(shown.attach(owner.id()));
    auto lit = take(w::create_component(runtime, types.counter)), dark = take(w::create_component(runtime, types.counter));
    committed(shown.attach_components(owner.id(), std::array{w::Attachment{lit.id()}, w::Attachment{dark.id()}}));
    take(runtime.write(lit.id(), types.counter_value, Scalar{1.f}));
    const auto glowing = take(shown.register_presentation(types.counter, [&](Runtime& rt, ObjectId id) -> Result<bool> {
        auto value = rt.read(id, types.counter_value);
        if (!value) return std::unexpected(value.error());
        return std::get<float>(*value) > 0.f;
    }));
    const auto bucket = [&] {
        const auto items = take(shown.presented(glowing));
        return std::vector<w::PresentedComponent>(items.begin(), items.end());
    };
    auto items = bucket();
    require(items.size() == 1 && items[0].component == lit.id() && items[0].entity == owner.id() && items[0].active,
        "A presentation registration did not evaluate existing members.");
    take(runtime.write(dark.id(), types.counter_value, Scalar{2.f}));
    require(bucket().size() == 1, "A presentation bucket changed before re-evaluation.");
    take(shown.reevaluate_presentation(dark.id()));
    committed(shown.set_enabled(lit.id(), false));
    items = bucket();
    require(items.size() == 2 && items[0].component == lit.id() && !items[0].active && items[1].component == dark.id() && items[1].active,
        "A presentation bucket lost composition order, an inactive member or a re-evaluated component.");
    reject(shown.register_presentation(types.entity), "An Entity type was registered for presentation.");

    // Admissions see attach candidates and edited values; a rejected edit is restored.
    std::vector<ObjectId> admitted;
    take(shown.add_admission(types.counter, [&](const w::World& world, std::span<const ObjectId> candidates) -> Result<void> {
        admitted.assign(candidates.begin(), candidates.end());
        for (const auto id : candidates) {
            auto value = world.runtime().read(id, types.counter_value);
            if (!value) return std::unexpected(value.error());
            if (std::get<float>(*value) > 10.f) return std::unexpected(Error{ErrorCode::argument_mismatch, "Counter exceeds 10."});
        }
        return {};
    }));
    reject(shown.add_admission(types.entity, [](const w::World&, std::span<const ObjectId>) -> Result<void> { return {}; }),
        "An Entity type was registered for admission.");
    auto loud = take(w::create_component(runtime, types.counter));
    take(runtime.write(loud.id(), types.counter_value, Scalar{11.f}));
    reject(shown.attach_components(owner.id(), std::array{w::Attachment{loud.id()}}), "An admission did not reject an attached component.");
    require(admitted == std::vector{loud.id()} && take(w::components(runtime, owner.id())).size() == 2,
        "A rejected component attach saw other candidates or was published.");
    auto guest = take(w::create_entity(runtime));
    admitted.clear();
    committed(shown.attach_components(guest.id(), std::array{w::Attachment{loud.id()}}));
    require(admitted.empty(), "Detached composition ran an admission.");
    reject(shown.attach(guest.id()), "An admission did not reject an Entity attach.");
    require(!shown.contains(guest.id()) && admitted == std::vector{loud.id()}, "A rejected Entity attach was published.");
    admitted.clear();
    reject(shown.write(dark.id(), std::array{types.counter_value}, Scalar{12.f}), "An admission did not reject an author edit.");
    require(admitted == std::vector{dark.id()} && std::get<float>(take(runtime.read(dark.id(), types.counter_value))) == 2.f,
        "A rejected author edit was not checked on its written value or not restored.");
    take(shown.write(dark.id(), std::array{types.counter_value}, Scalar{5.f}));
    require(std::get<float>(take(runtime.read(dark.id(), types.counter_value))) == 5.f, "An admitted author edit was not kept.");
    committed(shown.close());
    std::puts("[pass] Frame phases: whole-frame dt per phase, simulation step, transitive prerequisites, phase-end requests, running rebind, Source requests, presentation buckets, admissions");
}
