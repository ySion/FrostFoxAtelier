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
    auto a = take(w::create_entity(runtime)), b = take(w::create_entity(runtime));
    auto c = take(w::create_entity(runtime)), d = take(w::create_entity(runtime));
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
    const auto rank = [](const Event& e) {
        const auto phase = std::get<0>(e);
        return phase == 'P' || phase == 'K' ? 0 : phase == 'S' ? 1 : phase == 'Q' ? 2 : 3;
    };
    require(
        events.size() == 14 && std::ranges::is_sorted(events, {}, rank) &&
            std::ranges::all_of(events, [](const Event& e) { return std::get<2>(e) == .25f; }) &&
            std::ranges::all_of(
                std::array{a.id(), b.id(), c.id(), d.id()},
                [&](ObjectId id) { return count('P', id) == 1 && count('Q', id) == 1 && count('L', id) == 1; }) &&
            count('K', k.id()) == 1,
        "Frame phases did not run pre_physics, simulation, post_physics and late once each with the whole frame dt.");

    // b -> k -> a orders a after b in every phase, including those where k has no Tick.
    take(world.add_tick_prerequisite(k.id(), b.id()));
    take(world.add_tick_prerequisite(a.id(), k.id()));
    reject(world.add_tick_prerequisite(b.id(), a.id()), "A tick prerequisite cycle through a component was accepted.");
    reject(world.add_tick_prerequisite(a.id(), a.id()), "An object became its own tick prerequisite.");
    events.clear();
    take(world.update(.25f));
    require(at('P', b.id()) < at('K', k.id()) && at('K', k.id()) < at('P', a.id()) &&
                at('Q', b.id()) < at('Q', a.id()) && at('L', b.id()) < at('L', a.id()),
            "Tick prerequisites did not order their transitive closure in every phase.");

    // A running rebind takes effect from the next phase without replaying Begin.
    // Requests queued by a Tick commit when its phase ends.
    const auto d_id = d.id();
    bool queued = false, nested_bind = false;
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
    const auto diagnostics = world.take_diagnostics();
    require(diagnostics.size() == 1 && diagnostics[0].kind == w::DiagnosticKind::command_rejected &&
                diagnostics[0].severity == w::Severity::warning &&
                diagnostics[0].phase == w::DiagnosticPhase::command && diagnostics[0].object == d_id && world.running(),
            "A stale queued request was not reported as a rejected command on its target.");

    // Source requests use the same queue.
    source::Program program(runtime, types.counter);
    publish(program, "void tick(float dt) {\n    self.advance(dt);\n"
                     "    if (self.steps == 1) { self.set_enabled(false); } else { self.destroy(); }\n}\n");
    const auto entry = take(program.bind<void, float>());
    take(
        world.bind_behavior(types.counter, {.tick = [entry](Runtime&, ObjectId receiver, float amount) -> Result<void> {
                                const std::array arguments{Scalar{amount}};
                                auto result = entry.invoke(receiver, arguments);
                                if (!result)
                                    return std::unexpected(result.error());
                                return {};
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
        bool gc_blocked = false, dead_ticked = false;
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

    // Capability buckets are registered per World and re-evaluated on demand.
    w::World shown(runtime, types);
    auto owner = take(w::create_entity(runtime));
    committed(shown.attach(owner.id()));
    auto lit = take(w::create_component(runtime, types.counter)),
         dark = take(w::create_component(runtime, types.counter));
    committed(shown.attach_components(owner.id(), std::array{w::Attachment{lit.id()}, w::Attachment{dark.id()}}));
    take(runtime.write(lit.id(), types.counter_value, Scalar{1.f}));
    const auto glowing = take(shown.register_capability(types.counter, [&](Runtime& rt, ObjectId id) -> Result<bool> {
        auto value = rt.read(id, types.counter_value);
        if (!value)
            return std::unexpected(value.error());
        return std::get<float>(*value) > 0.f;
    }));
    const auto bucket = [&] {
        const auto items = take(shown.members(glowing));
        return std::vector<w::CapabilityMember>(items.begin(), items.end());
    };
    auto items = bucket();
    require(items.size() == 1 && items[0].component == lit.id() && items[0].entity == owner.id() && items[0].active,
            "A capability registration did not evaluate existing members.");
    take(runtime.write(dark.id(), types.counter_value, Scalar{2.f}));
    require(bucket().size() == 1, "A capability bucket changed before re-evaluation.");
    take(shown.reevaluate_capabilities(dark.id()));
    committed(shown.set_enabled(lit.id(), false));
    items = bucket();
    require(items.size() == 2 && items[0].component == lit.id() && !items[0].active &&
                items[1].component == dark.id() && items[1].active,
            "A capability bucket lost composition order, an inactive member or a re-evaluated component.");
    reject(shown.register_capability(types.entity), "An Entity type was registered as a capability.");
    // A host recreated on one World reuses its predicate-free registration; a predicate always adds one.
    const auto plain = take(shown.register_capability(types.counter));
    require(take(shown.register_capability(types.counter)) == plain && !(plain == glowing) &&
                take(shown.members(plain)).size() == 2,
            "A repeated predicate-free capability registration was not reused or missed members.");

    // Admissions see attach candidates and edited values; a rejected edit is restored.
    std::vector<ObjectId> admitted;
    take(shown.add_admission(
        types.counter,
        [&](const w::World& world, std::span<const w::World::AdmissionCandidate> candidates) -> Result<void> {
            admitted.clear();
            for (const auto& candidate : candidates)
                admitted.push_back(candidate.component);
            for (const auto& candidate : candidates) {
                require(candidate.owner != ObjectId{}, "Admission did not receive a proposed owner.");
                auto value = world.runtime().read(candidate.component, types.counter_value);
                if (!value)
                    return std::unexpected(value.error());
                if (std::get<float>(*value) > 10.f)
                    return std::unexpected(Error{ErrorCode::argument_mismatch, "Counter exceeds 10."});
            }
            return {};
        }));
    reject(shown.add_admission(
               types.entity,
               [](const w::World&, std::span<const w::World::AdmissionCandidate>) -> Result<void> { return {}; }),
           "An Entity type was registered for admission.");
    auto loud = take(w::create_component(runtime, types.counter));
    take(runtime.write(loud.id(), types.counter_value, Scalar{11.f}));
    reject(shown.attach_components(owner.id(), std::array{w::Attachment{loud.id()}}),
           "An admission did not reject an attached component.");
    require(admitted == std::vector{loud.id()} && take(w::components(runtime, owner.id())).size() == 2,
            "A rejected component attach saw other candidates or was published.");
    auto guest = take(w::create_entity(runtime));
    admitted.clear();
    committed(shown.attach_components(guest.id(), std::array{w::Attachment{loud.id()}}));
    require(admitted.empty(), "Detached composition ran an admission.");
    reject(shown.attach(guest.id()), "An admission did not reject an Entity attach.");
    require(!shown.contains(guest.id()) && admitted == std::vector{loud.id()},
            "A rejected Entity attach was published.");
    admitted.clear();
    reject(shown.write(dark.id(), std::array{types.counter_value}, Scalar{12.f}),
           "An admission did not reject an author edit.");
    require(admitted == std::vector{dark.id()} &&
                std::get<float>(take(runtime.read(dark.id(), types.counter_value))) == 2.f,
            "A rejected author edit was not checked on its written value or not restored.");
    take(shown.write(dark.id(), std::array{types.counter_value}, Scalar{5.f}));
    require(std::get<float>(take(runtime.read(dark.id(), types.counter_value))) == 5.f,
            "An admitted author edit was not kept.");
    // An admission may read the World but not edit it or register admissions while it runs; one
    // that throws rejects the edit like a returned error, and the previous value is restored.
    enum class Probe { none, reenter, raise } probe{};
    std::optional<Result<void>> nested_write, nested_admission;
    take(shown.add_admission(
        types.counter, [&](const w::World&, std::span<const w::World::AdmissionCandidate> candidates) -> Result<void> {
            if (probe == Probe::reenter) {
                nested_write = shown.write(candidates.front().component, std::array{types.counter_value}, Scalar{9.f});
                nested_admission = shown.add_admission(
                    types.counter,
                    [](const w::World&, std::span<const w::World::AdmissionCandidate>) -> Result<void> { return {}; });
            }
            if (probe == Probe::raise)
                throw std::runtime_error("Admission probe threw.");
            return {};
        }));
    probe = Probe::reenter;
    take(shown.write(dark.id(), std::array{types.counter_value}, Scalar{3.f}));
    require(nested_write && !*nested_write && nested_write->error().code == ErrorCode::busy && nested_admission &&
                !*nested_admission && nested_admission->error().code == ErrorCode::busy &&
                std::get<float>(take(runtime.read(dark.id(), types.counter_value))) == 3.f,
            "An admission edited the World or registered an admission while it ran.");
    probe = Probe::raise;
    const auto raised = shown.write(dark.id(), std::array{types.counter_value}, Scalar{7.f});
    require(!raised && raised.error().code == ErrorCode::invocation_failed &&
                raised.error().message == "Admission probe threw." &&
                std::get<float>(take(runtime.read(dark.id(), types.counter_value))) == 3.f,
            "A throwing admission did not reject the edit and restore the previous value.");
    probe = Probe::none;
    // A rejected edit returns the field to its exact previous bytes, even a value that bypassed
    // the write path: NaN keeps its payload and an out-of-bounds value is not clamped.
    bool deny = false, raise = false;
    const auto denied = [&](const w::World&, std::span<const w::World::AdmissionCandidate>) -> Result<void> {
        if (deny)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Restore probe denied the edit."});
        if (raise)
            throw std::runtime_error("Restore probe threw.");
        return {};
    };
    take(shown.add_admission(types.light_component, denied));
    auto lamp = take(p::create_static_mesh_entity(shown, {geometry::cube_mesh(), {{}, {}, {1, 1, 1}}}));
    auto bulb = take(w::create_component(runtime, types.light_component));
    committed(shown.attach_components(lamp.id(), std::array{w::Attachment{bulb.id()}}));
    committed(shown.attach(lamp.id()));
    const auto field = [&](TypeId type, std::string_view name) { return take(runtime.find_field(type, name)); };
    const auto settings = field(types.light_component, "settings"),
               intensity = field(take(runtime.native_type<p::LightSettings>()), "intensity");
    const auto color = field(types.light_settings, "color");
    const auto x = field(take(runtime.native_type<math::Vector3>()), "x");
    const auto rejected = [&](Result<void> result) {
        return !result && result.error().message == "Restore probe denied the edit.";
    };
    const auto quiet_nan = std::bit_cast<float>(0x7fc12345u);
    take(runtime.with_native<p::LightComponent>(bulb.id(), [&](p::LightComponent& light) -> Result<void> {
        light.settings.intensity = quiet_nan;
        return {};
    }));
    deny = true;
    require(rejected(shown.write(bulb.id(), std::array{settings, intensity}, Scalar{1.f})) &&
                std::bit_cast<std::uint32_t>(take(p::read_light(runtime, bulb.id())).intensity) == 0x7fc12345u,
            "A rejected scalar edit did not restore a non-finite previous value bit for bit.");
    deny = false;
    take(runtime.with_native<p::LightComponent>(bulb.id(), [&](p::LightComponent& light) -> Result<void> {
        light.settings.intensity = -2.f;
        light.settings.color.x = -2.f;
        return {};
    }));
    const auto bypassed = take(p::read_light(runtime, bulb.id()));
    deny = true;
    require(rejected(shown.write(bulb.id(), std::array{settings, intensity}, Scalar{.5f})) &&
                rejected(shown.write(bulb.id(), std::array{settings, color, x}, Scalar{.5f})) &&
                take(p::read_light(runtime, bulb.id())) == bypassed,
            "A rejected scalar or vector-component edit clamped an out-of-bounds previous value.");
    deny = false;
    take(runtime.with_native<p::LightComponent>(bulb.id(), [&](p::LightComponent& light) -> Result<void> {
        light.settings.source_radius = quiet_nan;
        return {};
    }));
    const auto settings_bytes = [&] {
        return take(runtime.with_native<const p::LightComponent>(
            bulb.id(), [](const auto& light) -> Result<std::vector<std::byte>> {
                const auto bytes = std::as_bytes(std::span{&light.settings, 1});
                return std::vector<std::byte>{bytes.begin(), bytes.end()};
            }));
    };
    const auto corrupted = settings_bytes();
    const p::LightSettings clean{};
    deny = true;
    require(rejected(shown.write(bulb.id(), std::array{settings}, ValueInput{types.light_settings, &clean})),
            "A rejected whole-value edit was not reported with its admission error.");
    require(settings_bytes() == corrupted,
            "A rejected whole-value edit did not restore a record holding NaN bit for bit.");
    // The typed entries share write's admissions and exact restore: a denying or throwing admission
    // fails the edit with its own error and leaves the NaN-holding value as it was.
    const auto unchanged = [&] { return settings_bytes() == corrupted; };
    p::LightSettings brighter;
    brighter.intensity = 2.f;
    require(rejected(p::set_light(shown, bulb.id(), brighter)) && unchanged(),
            "A denied typed edit did not fail with the admission error or changed its field.");
    deny = false;
    raise = true;
    const auto thrown = [](Result<void> result) {
        return !result && result.error().code == ErrorCode::invocation_failed &&
               result.error().message == "Restore probe threw.";
    };
    require(thrown(p::set_light(shown, bulb.id(), brighter)) && unchanged(),
            "A throwing admission did not fail a typed edit or changed its field.");
    raise = false;
    take(p::set_light(shown, bulb.id(), brighter));
    require(take(p::read_light(runtime, bulb.id())) == brighter, "An admitted typed edit was not kept.");
    committed(shown.close());
    std::puts(
        "[pass] Frame phases: whole-frame dt per phase, simulation step, transitive prerequisites, phase-end requests, running rebind, Source requests, capability buckets, admissions (no reentry, exceptions restore, rejected edits and typed entries restore exact bytes)");
}
