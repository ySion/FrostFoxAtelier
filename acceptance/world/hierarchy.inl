void hierarchical_lifecycle() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    source::Program program(runtime, types.counter);
    publish(program, "async void later() { yield; self.advance(50.0); }");
    const auto later = program.entry();
    w::World world(runtime, types);
    take(world.set_failure_policy(w::FailurePolicy::halt));
    auto root = take(w::create_entity(runtime));
    auto child = take(w::create_entity(runtime));
    auto leaf = take(w::create_entity(runtime));
    auto other = take(w::create_entity(runtime));
    auto a = take(w::create_component(runtime, types.counter));
    auto b = take(w::create_component(runtime, types.counter));
    auto c = take(w::create_component(runtime, types.counter));
    auto d = take(w::create_component(runtime, types.counter));
    const auto child_id = child.id();
    const auto leaf_id = leaf.id();
    const auto c_id = c.id();
    const auto d_id = d.id();
    const std::array root_components{w::Attachment{a.id()}, w::Attachment{b.id()}};
    const std::array child_components{w::Attachment{c_id}};
    const std::array leaf_components{w::Attachment{d_id}};
    committed(world.attach_components(root.id(), root_components));
    committed(world.attach_components(child_id, child_components));
    committed(world.attach_components(leaf_id, leaf_components));
    committed(world.reparent(leaf_id, child_id, w::ReparentRule::keep_local));
    committed(world.reparent(child_id, root.id(), w::ReparentRule::keep_local));
    using Event = std::pair<char, ObjectId>;
    std::vector<Event> events;
    std::vector<Event> registrations;
    std::vector<ObjectId> activations;
    std::vector<ObjectId> deactivations;
    ObjectId fail_begin;
    ObjectId fail_end;
    const auto hook = [&](char phase, ObjectId id, ObjectId failure) -> Result<void> {
        events.emplace_back(phase, id);
        if (id == failure)
            return std::unexpected(Error{ErrorCode::invocation_failed, "Intentional subtree lifecycle failure."});
        return {};
    };
    const w::World::Behavior behavior{.begin = [&](Runtime&, ObjectId id) { return hook('B', id, fail_begin); },
                                      .tick = [&](Runtime&, ObjectId id, float) { return hook('T', id, {}); },
                                      .end = [&](Runtime&, ObjectId id) { return hook('E', id, fail_end); }};
    take(world.bind_behavior(types.entity, behavior));
    take(world.bind_behavior(types.counter, behavior));
    take(world.observe_lifecycle([&](w::World& current, w::Lifecycle phase, ObjectId id) -> Result<void> {
        if (phase == w::Lifecycle::attached)
            registrations.emplace_back('R', id);
        if (phase == w::Lifecycle::detached)
            registrations.emplace_back('U', id);
        const auto entity = take(runtime.is_a(take(runtime.type_of(id)), types.entity));
        if (phase == w::Lifecycle::activated && entity)
            activations.push_back(id);
        if (phase == w::Lifecycle::deactivated && entity) {
            deactivations.push_back(id);
            require(take(current.query(types.counter)).size() == 4 && take(current.members()).size() == 4,
                    "Deactivated cleanup could not inspect complete registered graph, including close.");
            take(w::components(runtime, id));
            take(w::parent_entity(runtime, id));
        }
        return {};
    }));
    committed(world.attach(other.id()));
    committed(world.attach(root.id()));
    committed(world.start());
    require(events == std::vector<Event>{{'B', other.id()},
                                         {'B', root.id()},
                                         {'B', a.id()},
                                         {'B', b.id()},
                                         {'B', child_id},
                                         {'B', c_id},
                                         {'B', leaf_id},
                                         {'B', d_id}},
            "Begin did not follow Entity/components/children preorder.");
    events.clear();
    registrations.clear();
    committed(world.reparent(child_id, other.id(), w::ReparentRule::keep_local));
    take(world.start_task(c_id, [later](Runtime&, ObjectId id) { return later.start(id, {}); }));
    take(world.update(0));
    require(w::detail::Access::state(world).tasks.size() == 1, "Subtree sample did not own a suspended coroutine.");
    events.clear();
    deactivations.clear();
    committed(world.set_enabled(other.id(), false));
    require(deactivations == std::vector<ObjectId>{leaf_id, child_id, other.id()},
            "Effective subtree disable notifications did not follow reverse Entity order.");
    require(events.empty() && w::detail::Access::state(world).tasks.size() == 0 &&
                !take(w::effective_enabled(runtime, child_id)) && !take(w::effective_enabled(runtime, leaf_id)) &&
                take(w::inspect_entity(runtime, child_id)).enabled,
            "Ancestor disable altered local intent, repeated lifecycle or retained descendant tasks.");
    events.clear();
    activations.clear();
    committed(world.set_enabled(other.id(), true));
    require(activations == std::vector<ObjectId>{other.id(), child_id, leaf_id},
            "Effective subtree restore notifications did not follow Entity preorder.");
    require(events.empty() && take(w::effective_enabled(runtime, leaf_id)),
            "Ancestor restore repeated Begin/End or left stale effective state.");
    committed(world.reparent(child_id, root.id(), w::ReparentRule::keep_local));
    events.clear();
    registrations.clear();
    auto removal = take(world.detach(child_id));
    require(!removal.commit.notification_error &&
                events == std::vector<Event>{{'E', d_id}, {'E', leaf_id}, {'E', c_id}, {'E', child_id}} &&
                registrations == std::vector<Event>{{'U', d_id}, {'U', leaf_id}, {'U', c_id}, {'U', child_id}},
            "Subtree detach omitted descendants, affected unrelated objects or repeated cleanup.");
    require(take(world.members()).size() == 2 && take(w::parent_entity(runtime, child_id)) == ObjectId{} &&
                take(w::children(runtime, child_id)).front().id() == leaf_id,
            "Subtree detach left partial membership or destroyed its internal composition.");
    child.reset();
    leaf.reset();
    c.reset();
    d.reset();
    collect(runtime);
    require(take(w::inspect_entity(runtime, child_id)).state == w::EntityState::detached && runtime.type_of(d_id),
            "History failed to retain detached descendants or granted membership.");
    events.clear();
    registrations.clear();
    committed(world.restore(removal));
    require(take(world.members()).size() == 4 && take(w::parent_entity(runtime, child_id)) == root.id() &&
                events == std::vector<Event>{{'B', child_id}, {'B', c_id}, {'B', leaf_id}, {'B', d_id}},
            "Subtree restore changed identities, composition or participation interval.");
    committed(world.reparent(child_id, other.id(), w::ReparentRule::keep_local));
    const std::vector<Event> reverse{{'E', b.id()},  {'E', a.id()}, {'E', root.id()}, {'E', d_id},
                                     {'E', leaf_id}, {'E', c_id},   {'E', child_id},  {'E', other.id()}};
    events.clear();
    fail_end = c_id;
    const auto failed_stop = take(world.stop());
    require(failed_stop.notification_error && events == reverse && !world.running(),
            "End failure skipped remaining subtree/current-order cleanup.");
    events.clear();
    fail_end = {};
    fail_begin = c_id;
    const auto failed_start = take(world.start());
    require(
        failed_start.notification_error &&
            events ==
                std::vector<Event>{{'B', other.id()}, {'B', child_id}, {'B', c_id}, {'E', child_id}, {'E', other.id()}},
        "Subtree Begin failure did not unwind exactly the successful preorder prefix.");
    fail_begin = {};
    committed(world.start());
    registrations.clear();
    committed(world.close());
    std::vector<Event> unregister;
    for (const auto& event : reverse)
        unregister.emplace_back('U', event.second);
    require(registrations == unregister, "Close did not reverse the current composition registration order.");
    committed(w::destroy_entity(runtime, other.id()));
    require(!runtime.type_of(child_id) && !runtime.type_of(leaf_id) && !runtime.type_of(c_id) && !runtime.type_of(d_id),
            "Strong history prevented explicit subtree termination.");
    std::puts("[pass] Recursive lifecycle: subtree task cancellation, sole-root GC and complete failure cleanup");
}

void hierarchical_reference_lifetime() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    w::World world(runtime, types);
    auto parent = take(w::create_entity(runtime));
    auto child = take(w::create_entity(runtime));
    auto leaf = take(w::create_entity(runtime));
    auto component = take(w::create_component(runtime, types.counter));
    const auto parent_id = parent.id();
    const auto child_id = child.id();
    const auto leaf_id = leaf.id();
    const auto component_id = component.id();
    const std::array group{w::Attachment{component_id}};
    committed(world.attach_components(child_id, group));
    committed(world.reparent(leaf_id, child_id, w::ReparentRule::keep_local));
    committed(world.reparent(child_id, parent_id, w::ReparentRule::keep_local));
    parent.reset();
    child.reset();
    leaf.reset();
    collect(runtime);
    require(!runtime.type_of(parent_id) && runtime.type_of(child_id) && runtime.type_of(leaf_id),
            "A component failed to retain its owner/subtree or weak composition parent retained ancestors.");
    committed(w::destroy_entity(runtime, child_id));
    require(!runtime.type_of(child_id) && !runtime.type_of(leaf_id) && !runtime.type_of(component_id),
            "External strong component root blocked explicit subtree destruction.");
    component.reset();
    collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Subtree destruction retained hidden roots.");
    std::puts(
        "[pass] Recursive references: strong children/owner, weak parent, GC and explicit destruction remain distinct");
}

void hierarchical_space_boundaries() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    auto a = take(w::create_entity(runtime));
    auto b = take(w::create_entity(runtime));
    auto group = take(w::create_entity(runtime));
    auto x = take(w::create_entity(runtime));
    auto y = take(w::create_entity(runtime));
    take(world.set_transform(a.id(), {{10, 0, 0}, {}, {1, 1, 1}}));
    take(world.set_transform(b.id(), {{100, 0, 0}, {}, {1, 1, 1}}));
    take(world.set_transform(x.id(), {{1, 0, 0}, {}, {1, 1, 1}}));
    take(world.set_transform(y.id(), {{0, 2, 0}, {}, {1, 1, 1}}));
    committed(world.reparent(x.id(), group.id(), w::ReparentRule::keep_local));
    committed(world.reparent(y.id(), group.id(), w::ReparentRule::keep_local));
    committed(world.reparent(group.id(), a.id(), w::ReparentRule::keep_local));
    committed(world.attach(a.id()));
    committed(world.attach(b.id()));
    auto anchor = take(w::create_entity(runtime));
    auto probe = take(w::create_entity(runtime));
    take(world.set_transform(anchor.id(), {}));
    take(world.set_transform(probe.id(), {{3, 4, 5}, {}, {1, 1, 1}}));
    committed(world.set_space_reference(probe.id(), anchor.id(), w::ReparentRule::keep_local));
    committed(w::destroy_entity(runtime, anchor.id()));
    const auto probe_local = take(w::read_transform(runtime, probe.id()));
    committed(world.set_space_reference(probe.id(), ObjectId{}, w::ReparentRule::keep_local));
    require(take(w::space_reference(runtime, probe.id())) == std::optional<ObjectId>{ObjectId{}} &&
                take(w::read_transform(runtime, probe.id())) == probe_local,
            "Keep-local repair did not restore explicit world space with unchanged local TRS.");
    committed(world.attach(probe.id()));
    const auto probe_id = probe.id();
    take(world.remove_transform(probe_id));
    require(!take(w::has_transform(runtime, probe_id)) && !take(w::space_reference(runtime, probe_id)).has_value(),
            "Transform removal retained explicit world-space configuration.");
    committed(world.reparent(probe_id, a.id(), w::ReparentRule::keep_local));
    take(world.set_transform(probe_id, probe_local));
    const auto inherited_probe = take(w::world_matrix(runtime, probe_id));
    require(probe.id() == probe_id && take(w::spatial_parent(runtime, probe_id)) == a.id() &&
                near(inherited_probe[12], 13),
            "Re-added Transform restored hidden world-space configuration or replaced Entity identity.");
    auto stretched = take(w::create_entity(runtime));
    auto turned = take(w::create_entity(runtime));
    auto carried = take(w::create_entity(runtime));
    take(world.set_transform(stretched.id(), {{0, 0, 0}, {}, {2, 1, 1}}));
    take(world.set_transform(turned.id(), {{1, 0, 0}, {}, {.5f, 1, 1}}));
    take(world.set_transform(carried.id(), {{0, 1, 0}, {}, {1, 1, 1}}));
    committed(world.attach(stretched.id()));
    committed(world.attach(turned.id(), w::append, stretched.id()));
    committed(world.attach(carried.id(), w::append, turned.id()));
    committed(world.start());
    const std::array poses{w::SimulationPose{x.id(), {201, 3, 0}, {}}, w::SimulationPose{a.id(), {200, 0, 0}, {}}};
    take(world.apply_simulation_transforms(poses));
    const auto parent_world = take(w::world_matrix(runtime, a.id()));
    const auto child_world = take(w::world_matrix(runtime, x.id()));
    require(near(parent_world[12], 200) && near(child_world[12], 201) && near(child_world[13], 3),
            "Child-first physics pose batch used stale ancestor world pose.");
    // The child first converts against its parent's quarter-turn target. That turn under
    // the stretched grandparent would change the parent's authored scale, so the parent
    // is rejected and the child converts again against the parent's unmoved pose.
    const std::array conflicting{w::SimulationPose{carried.id(), {60, 5, 0}, {}},
                                 w::SimulationPose{turned.id(), {50, 0, 0}, {0, 0, .70710678f, .70710678f}}};
    take(world.apply_simulation_transforms(conflicting));
    const auto carried_world = take(w::world_matrix(runtime, carried.id()));
    require(near(carried_world[12], 60) && near(carried_world[13], 5) && near(carried_world[14], 0),
            "A pose kept after its parent's pose was rejected converted against the rejected target.");
    committed(world.stop());
    committed(world.close());
    std::puts("[pass] Recursive space: expired-origin repair and child-first physics batches");
}

void forest_publication() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    w::World world(runtime, types);
    auto p = take(w::create_entity(runtime));
    auto q = take(w::create_entity(runtime));
    auto existing = take(w::create_entity(runtime));
    take(world.set_transform(p.id(), {{100, 0, 0}, {}, {1, 1, 1}}));
    take(world.set_transform(q.id(), {{200, 0, 0}, {}, {1, 1, 1}}));
    committed(world.attach(p.id()));
    committed(world.attach(existing.id(), w::append, p.id()));
    committed(world.attach(q.id()));
    auto x = take(w::create_entity(runtime));
    auto y = take(w::create_entity(runtime));
    auto z = take(w::create_entity(runtime));
    auto cx = take(w::create_component(runtime, types.counter));
    auto cy = take(w::create_component(runtime, types.counter));
    take(world.set_transform(x.id(), {{1, 0, 0}, {}, {1, 1, 1}}));
    take(world.set_transform(y.id(), {{10, 0, 0}, {}, {1, 1, 1}}));
    take(world.set_transform(z.id(), {{2, 0, 0}, {}, {1, 1, 1}}));
    const std::array x_capabilities{w::Attachment{cx.id()}};
    const std::array y_capabilities{w::Attachment{cy.id()}};
    committed(world.attach_components(x.id(), x_capabilities));
    committed(world.attach_components(y.id(), y_capabilities));
    committed(world.reparent(z.id(), x.id(), w::ReparentRule::keep_local));
    committed(world.set_space_reference(x.id(), y.id(), w::ReparentRule::keep_local));
    using Event = std::pair<char, ObjectId>;
    std::vector<Event> events;
    const w::World::Behavior behavior{.begin = [&](Runtime&, ObjectId id) -> Result<void> {
                                          events.emplace_back('B', id);
                                          return {};
                                      },
                                      .end = [&](Runtime&, ObjectId id) -> Result<void> {
                                          events.emplace_back('E', id);
                                          return {};
                                      }};
    take(world.bind_behavior(types.entity, behavior));
    take(world.bind_behavior(types.counter, behavior));
    committed(world.start());
    events.clear();
    take(world.observe_lifecycle([&](w::World& current, w::Lifecycle phase, ObjectId id) -> Result<void> {
        if (phase == w::Lifecycle::detached) {
            events.emplace_back('U', id);
            return {};
        }
        if (phase != w::Lifecycle::attached)
            return {};
        events.emplace_back('R', id);
        require(take(current.members()).size() == 6 && take(current.query(types.counter)).size() == 2,
                "Forest observer saw only part of the member/capability publication.");
        require(take(w::parent_entity(runtime, x.id())) == p.id() &&
                    take(w::parent_entity(runtime, y.id())) == q.id() &&
                    near(take(w::world_matrix(runtime, x.id()))[12], 211) &&
                    near(take(w::world_matrix(runtime, z.id()))[12], 213),
                "Forest observer saw incomplete parent sequences or unresolved cross-root space.");
        return {};
    }));
    const std::array forest{w::EntityAttachment{x.id(), p.id(), 0}, w::EntityAttachment{y.id(), q.id()}};
    committed(world.attach_batch(forest));
    events.clear();
    const std::array reverse_selection{y.id(), x.id()};
    auto forest_history = take(world.detach_batch(reverse_selection));
    require(events == std::vector<Event>{{'E', cy.id()},
                                         {'E', y.id()},
                                         {'E', z.id()},
                                         {'E', cx.id()},
                                         {'E', x.id()},
                                         {'U', cy.id()},
                                         {'U', y.id()},
                                         {'U', z.id()},
                                         {'U', cx.id()},
                                         {'U', x.id()}},
            "Forest detach followed selection order instead of reverse current composition or repeated cleanup.");
    committed(world.stop());
    committed(world.close());
    std::puts("[pass] Forest publication: cross-root space and reverse batch cleanup");
}

void attachment_space_and_authoring() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    w::World world(runtime, types);
    source::Program task_program(runtime, types.counter);
    publish(task_program, "async void later() { yield; self.advance(50.0); }");
    const auto later = task_program.entry();
    auto root = take(w::create_entity(runtime));
    auto logical = take(w::create_entity(runtime));
    auto child = take(w::create_entity(runtime));
    take(world.set_transform(root.id(), {{10, 0, 0}, {}, {1, 1, 1}}));
    take(world.set_transform(child.id(), {{2, 0, 0}, {}, {1, 1, 1}}));
    committed(world.reparent(logical.id(), root.id(), w::ReparentRule::keep_local));
    committed(world.reparent(child.id(), logical.id(), w::ReparentRule::keep_local));
    committed(world.attach(root.id()));
    const auto child_local = take(w::read_transform(runtime, child.id()));
    take(world.set_transform(logical.id(), {{3, 0, 0}, {}, {1, 1, 1}}));
    take(world.remove_transform(logical.id()));
    require(!take(w::has_transform(runtime, logical.id())) &&
                take(w::read_transform(runtime, child.id())) == child_local &&
                near(take(w::world_matrix(runtime, child.id()))[12], 12),
            "Transform undo blocked default spatial inheritance or changed local TRS.");
    take(world.remove_transform(root.id()));
    require(near(take(w::world_matrix(runtime, child.id()))[12], 2),
            "Transform removal did not pass through nonspatial ancestors.");
    take(world.set_transform(root.id(), {{10, 0, 0}, {}, {1, 1, 1}}));
    committed(world.set_space_reference(child.id(), root.id(), w::ReparentRule::keep_local));
    auto counter = take(w::create_component(runtime, types.counter));
    const std::array counter_parts{w::Attachment{counter.id()}};
    committed(world.attach_components(child.id(), counter_parts));
    ObjectId a_id{};
    ObjectId b_id{};
    take(world.bind_behavior(types.entity, {.begin = [&](Runtime&, ObjectId object) -> Result<void> {
                                 if (object == a_id || object == b_id) {
                                     require(take(w::space_reference(runtime, a_id)) == std::optional<ObjectId>{b_id} &&
                                                 take(w::space_reference(runtime, b_id)) ==
                                                     std::optional<ObjectId>{ObjectId{}} &&
                                                 near(take(w::world_matrix(runtime, a_id))[12], 7),
                                             "Begin saw a temporary spatial cycle or incomplete origins.");
                                 }
                                 return {};
                             }}));
    committed(world.start());
    take(world.start_task(counter.id(), [later](Runtime&, ObjectId receiver) { return later.start(receiver, {}); }));
    take(world.update(0));
    require(w::detail::Access::state(world).tasks.size() == 1, "Space continuity fixture did not suspend a real task.");
    committed(world.set_space_reference(child.id(), ObjectId{}, w::ReparentRule::keep_world));
    require(take(w::space_reference(runtime, child.id())) == std::optional<ObjectId>{ObjectId{}} &&
                w::detail::Access::state(world).tasks.size() == 1 &&
                take(w::inspect_component(runtime, counter.id())).active,
            "Running origin change cancelled a task.");
    committed(world.set_space_reference(child.id(), std::nullopt, w::ReparentRule::keep_world));
    take(world.update(0));
    require(w::detail::Access::state(world).tasks.size() == 0 &&
                std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 50.f,
            "Origin changes interrupted the suspended task's next execution.");
    auto a = take(w::create_entity(runtime));
    auto b = take(w::create_entity(runtime));
    a_id = a.id();
    b_id = b.id();
    take(world.set_transform(a_id, {{2, 0, 0}, {}, {1, 1, 1}}));
    take(world.set_transform(b_id, {{5, 0, 0}, {}, {1, 1, 1}}));
    committed(world.reparent(b_id, a_id, w::ReparentRule::keep_local));
    const std::array forest{w::EntityAttachment{a_id}};
    const std::array complete{w::SpaceReference{a_id, b_id}, w::SpaceReference{b_id, ObjectId{}}};
    const auto matrices = take(world.attachment_world_matrices(forest, complete));
    require(matrices.size() == 2 && near(matrices.front().second[12], 7) && !take(w::space_reference(runtime, a_id)),
            "Prospective preview changed live relations or computed a temporary graph.");
    committed(world.attach_batch(forest, complete));
    committed(world.close());
    std::puts("[pass] Final space plans: preview matrices, inherited Transform removal and real task continuity");
}

void mirrored_and_degenerate_space() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    const auto item = [&](ObjectId entity) -> std::optional<p::RenderItem> {
        for (const auto& value : take(scene.render_items()))
            if (value.entity == entity)
                return value;
        return std::nullopt;
    };
    // A negative axis mirrors: S*R*T keeps its sign and the matrix turns left-handed.
    auto mirror = take(w::create_entity(runtime));
    auto mirror_shape = add_shape(runtime, types, world, mirror.id());
    take(world.set_transform(mirror.id(), {{1, 2, 3}, {}, {-2, 3, 4}}));
    committed(world.attach(mirror.id()));
    const auto mirrored = item(mirror.id());
    require(mirrored && mirrored->world[0] == -2 && mirrored->world[5] == 3 && mirrored->world[10] == 4 &&
                mirrored->world[12] == 1,
            "Negative scale did not produce a mirrored S*R*T matrix.");
    // Decomposition keeps a proper rotation: X carries the reflection unless the
    // object's own sign pattern has the same parity.
    const auto inverted = w::transform_matrix({{}, {}, {-1, -1, -1}});
    const auto split = take(w::relative_transform(inverted, w::transform_matrix({})));
    const auto kept = take(w::relative_transform(inverted, w::transform_matrix({}), {-1, -1, -1}));
    require(near(split.scale.x, -1) && near(split.scale.y, 1) && near(split.scale.z, 1) &&
                near(std::abs(split.rotation.x), 1) && near(kept.scale.x, -1) && near(kept.scale.y, -1) &&
                near(kept.scale.z, -1) && near(std::abs(kept.rotation.w), 1),
            "Mirrored decomposition lost the preferred sign pattern.");
    // Rotating a zero-scale axis cannot restore the matrix's rank.
    const auto turn = [](float degrees, math::Vector3 axis) {
        const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        const float angle = degrees * 3.14159265f / 360;
        const float s = std::sin(angle) / length;
        return math::Quaternion{axis.x * s, axis.y * s, axis.z * s, std::cos(angle)};
    };
    auto tilted = take(w::create_entity(runtime));
    auto leaf = take(w::create_entity(runtime));
    auto tilted_shape = add_shape(runtime, types, world, tilted.id());
    auto leaf_shape = add_shape(runtime, types, world, leaf.id());
    take(world.set_transform(tilted.id(), {{}, turn(17, {1, 2, 3}), {0, 1, 1}}));
    take(world.set_transform(leaf.id(), {{}, turn(29, {1, 2, 3}), {1, 1, 1}}));
    committed(world.reparent(leaf.id(), tilted.id(), w::ReparentRule::keep_local));
    committed(world.attach(tilted.id()));
    require(!item(leaf.id()) && !item(tilted.id()), "A mesh under a rotated zero scale axis was drawn.");
    // Recomposing only the child reuses the parent's entry from this pass.
    take(world.set_transform(leaf.id(), {{}, turn(41, {1, 2, 3}), {1, 1, 1}}));
    require(!item(leaf.id()), "A child recomposed under a cached zero scale axis was drawn.");
    (void)take(world.detach(tilted.id()));
    committed(world.close());
    std::puts("[pass] Mirrored and degenerate space: signed S*R*T, proper-rotation decomposition and zero-axis cache");
}
