void trace_queries() {
    PhysicsFixture f;
    p::BodySettings touch;
    touch.category = 1;
    take(touch.set_response(7, p::Response::overlap));
    auto near_body = f.make({0, 0, 0}, touch);
    p::BodySettings block;
    block.category = 2;
    auto wall = f.make({0, 0, 3}, block);
    auto behind = f.make({0, 0, 6}, block);
    auto ignored_settings = block;
    take(ignored_settings.set_response(7, p::Response::ignore));
    auto ignored = f.make({0, 0, 9}, ignored_settings);
    f.start();
    step(f.world, *f.scene, 0);
    p::TraceSettings query;
    query.channel = 7;
    const Vector3 start{0, 0, -3}, end{0, 0, 12};
    p::QueryFilter legacy;
    legacy.category = 7;
    const auto closest = take(f.scene->raycast(start, end, legacy));
    require(closest.hits.size() == 1 && !closest.has_block, "Legacy closest query changed its blocking summary.");
    auto result = take(f.scene->trace(start, end, query));
    require(result.has_hit && result.has_block && result.hits.size() == 1 && result.hits[0].body.entity == wall.id(),
            "Channel single did not skip touches and select the first block.");
    require(near(result.hits[0].distance, 5.5f) && near(result.hits[0].point.z, 2.5f) &&
                near(result.hits[0].normal.z, -1),
            "Line trace distance, point or normal is incorrect.");
    query.mode = p::TraceMode::multi;
    result = take(f.scene->trace(start, end, query));
    require(result.hits.size() == 2 && !result.hits[0].blocking && result.hits[1].body.entity == wall.id(),
            "Channel multi did not stop after the first block.");
    query.capacity = 1;
    result = take(f.scene->trace(start, end, query));
    require(result.overflow && result.has_block && result.hits.size() == 1 && !result.hits[0].blocking,
            "Truncation lost the full trace blocking summary.");
    query.capacity = 64;
    query.mode = p::TraceMode::test;
    result = take(f.scene->trace(start, end, query));
    require(result.has_hit && result.has_block && result.hits.empty(),
            "Boolean trace retained payload or lost the blocking result.");
    const std::array ignored_entities{wall.id(), behind.id()};
    require(!take(f.scene->trace(start, end, query, {ignored_entities, {}})).has_hit,
            "Channel test accepted touches or ignored Entities.");
    const std::array ignored_components{wall.body.id(), behind.body.id()};
    require(!take(f.scene->trace(start, end, query, {{}, ignored_components})).has_hit,
            "Ignored Component list was not applied.");
    query.mode = p::TraceMode::multi;
    query.selection = p::TraceSelection::object_type;
    query.object_types = 1u << 2;
    result = take(f.scene->trace(start, end, query));
    require(result.hits.size() == 3 && result.hits.back().body.entity == ignored.id(),
            "Object-type trace stopped at a block or consulted channel responses.");
    query.object_types = 0;
    require(!take(f.scene->trace(start, end, query)).has_hit, "Empty object mask did not miss.");
    query.object_types = 0xffff;
    query.shape = p::TraceShape::sphere_sweep;
    query.radius = .25f;
    query.mode = p::TraceMode::single;
    result = take(f.scene->trace(start, end, query));
    require(result.hits.size() == 1 && near(result.hits[0].distance, 2.25f, .002f) &&
                near(result.hits[0].location.z, -.75f, .002f) && near(result.hits[0].point.z, -.5f, .002f),
            "Sphere trace confused shape location with surface contact.");
    result = take(f.scene->trace({0, 0, 0}, {0, 0, 2}, query));
    require(result.hits.size() == 1 && result.hits[0].initial_overlap && result.hits[0].distance == 0 &&
                result.hits[0].penetration_depth > 0,
            "Initial sphere penetration was not reported.");
    query.shape = p::TraceShape::sphere_overlap;
    query.mode = p::TraceMode::multi;
    result = take(f.scene->trace({0, 0, 0}, {}, query));
    require(result.hits.size() == 1 && result.hits[0].body.entity == near_body.id() &&
                result.hits[0].location == Vector3{},
            "Sphere overlap lost its center or identity.");
    query.shape = p::TraceShape::line;
    require(!f.scene->trace({}, {}, query), "Zero-length line trace was accepted.");
    const auto maximum = std::numeric_limits<float>::max();
    require(!f.scene->trace({maximum, 0, 0}, {-maximum, 0, 0}, query), "Overflowing finite endpoints reached Jolt.");
    query.channel = 16;
    require(!f.scene->trace(start, end, query), "Out-of-range channel was accepted.");
    query.channel = 7;
    query.capacity = 0;
    require(!f.scene->trace(start, end, query), "Zero result capacity was accepted.");
    query.capacity = 64;
    query.object_types = 0x10000;
    require(!f.scene->trace(start, end, query), "Out-of-range object mask was accepted.");
    query.object_types = 0xffff;
    query.mode = static_cast<p::TraceMode>(99);
    require(!f.scene->trace(start, end, query), "Unknown trace mode was accepted.");
    committed(f.world.stop());
    touch.sensor = true;
    take(touch.set_response(7, p::Response::block));
    take(p::configure(f.runtime, near_body.body.id(), touch));
    f.start();
    step(f.world, *f.scene, 0);
    query.mode = p::TraceMode::single;
    query.object_types = 1u << 1;
    require(take(f.scene->trace(start, end, query)).has_hit, "Object trace excluded a requested Sensor.");
    query.include_sensors = false;
    require(!take(f.scene->trace(start, end, query)).has_hit, "Sensor exclusion was ignored.");
}

void query_exclusions() {
    PhysicsFixture f;
    auto group = f.make({0, 0, -3}), part = f.make({0, 0, 0});
    auto target = f.make({0, 0, 4}), farther = f.make({0, 0, 7});
    committed(f.world.reparent(part.id(), group.id(), w::ReparentRule::keep_world));
    auto owner = take(w::create_entity(f.runtime));
    take(f.world.set_transform(owner.id(), {}));
    auto self_body = take(w::create_component(f.runtime, f.world_types.collision_component));
    auto query = take(w::create_component(f.runtime, f.physics_types.scene_query_component));
    committed(
        f.world.attach_components(owner.id(), std::array{w::Attachment{self_body.id()}, w::Attachment{query.id()}}));
    committed(f.world.attach(owner.id(), w::append, group.id()));
    const auto exposed = take(f.runtime.find_field(f.physics_types.scene_query_component, "ignored_entities"));
    const auto description = take(f.runtime.describe(f.physics_types.scene_query_component));
    const auto field = std::ranges::find(description->fields, exposed, &atelier::FieldInfo::id);
    require(field != description->fields.end() && field->value.form == atelier::ValueForm::strong_sequence,
            "Query exclusions are not public Entity references in the shared type definition.");
    f.start();
    step(f.world, *f.scene, 0);
    const auto initial = take(f.scene->execute_query(query.id()));
    require(initial.has_hit && initial.hits.front().body.entity == group.id(),
            "ignore_self guessed a grouping ancestor instead of the real owner Entity.");
    take(p::set_query_ignored_entities(f.runtime, query.id(), std::array{group.id(), part.id()}));
    require(take(p::query_ignored_entities(f.runtime, query.id())) == std::vector{group.id(), part.id()},
            "Persistent query exclusions did not preserve their explicit Entities.");
    const auto grouped = take(f.scene->execute_query(query.id()));
    require(grouped.has_hit && grouped.hits.front().body.entity == target.id() && grouped.start == Vector3{0, 0, -3} &&
                take(w::inspect_component(f.runtime, query.id())).owner == owner.id(),
            "Excluding a physical group changed query ownership, spatial origin, or filtering.");
    const auto edit_self = [&](bool value) {
        take(f.runtime.with_native<p::SceneQueryComponent>(query.id(), [&](auto& q) -> atelier::Result<void> {
            q.ignore_self = value;
            return {};
        }));
    };
    edit_self(false);
    require(take(f.scene->execute_query(query.id())).hits.front().body.entity == owner.id(),
            "Opting into actual self collision was overridden by group exclusions.");
    edit_self(true);
    const std::array caller_exclusions{target.id()};
    require(take(f.scene->execute_query(query.id(), {caller_exclusions, {}})).hits.front().body.entity == farther.id(),
            "Caller exclusions did not combine with the persistent group and actual self.");
    require(!p::set_query_ignored_entities(f.runtime, query.id(), std::array{group.id(), group.id()}) &&
                !p::set_query_ignored_entities(f.runtime, query.id(), std::array{atelier::ObjectId{}}),
            "Invalid query exclusion bindings were accepted.");
    require(take(f.scene->execute_query(query.id())).hits.front().body.entity == target.id(),
            "A rejected exclusion update changed the previous valid filter.");
    f.advance(1);
    require(take(f.scene->read_query_result(query.id())).hits.front().body.entity == target.id(),
            "Fixed-step query sampling skipped persistent Entity exclusions.");
    take(p::set_query_ignored_entities(f.runtime, query.id(), {}));
    require(take(f.scene->execute_query(query.id())).hits.front().body.entity == group.id(),
            "Clearing active query exclusions retained a guessed parent exclusion.");
    committed(f.world.stop());
}

void affine_query_space() {
    PhysicsFixture f;
    auto target = f.make({0, 0, 0});
    auto frame = take(w::create_entity(f.runtime));
    take(f.world.set_transform(frame.id(), {{0, 0, 0}, {0, 0, 0, 1}, {2, 1, 1}}));
    auto logical = take(w::create_entity(f.runtime)), owner = take(w::create_entity(f.runtime));
    take(f.world.set_transform(owner.id(), {{0, 0, -3}, {0, 0, .38268343f, .92387953f}, {1, 1, 1}}));
    committed(f.world.attach(frame.id()));
    committed(f.world.attach(logical.id()));
    committed(f.world.attach(owner.id(), w::append, logical.id()));
    committed(f.world.set_space_reference(owner.id(), frame.id(), w::ReparentRule::keep_local));
    require(!w::relative_transform(take(w::world_matrix(f.runtime, owner.id())), w::transform_matrix({})),
            "Affine query fixture did not create inherited shear.");
    auto query = take(w::create_component(f.runtime, f.physics_types.scene_query_component));
    take(f.runtime.with_native<p::SceneQueryComponent>(query.id(), [](auto& value) -> atelier::Result<void> {
        value.automatic = false;
        value.end = {0, 0, 10};
        return {};
    }));
    committed(f.world.attach_components(owner.id(), std::array{w::Attachment{query.id()}}));
    f.start();
    step(f.world, *f.scene, 0);
    const auto hit = take(f.scene->execute_query(query.id()));
    require(hit.has_hit && hit.hits.front().body.entity == target.id() && near(hit.hits.front().distance, 2.5f),
            "Query rejected a valid affine owner matrix or transformed its endpoints incorrectly.");
    take(f.runtime.with_native<p::SceneQueryComponent>(query.id(), [](auto& value) -> atelier::Result<void> {
        value.request_query();
        return {};
    }));
    committed(f.world.set_enabled(frame.id(), false));
    require(take(w::effective_enabled(f.runtime, owner.id())),
            "Independent spatial origin changed composition activity.");
    f.advance(1);
    const auto pending = take(f.scene->read_query_result(query.id()));
    require(pending.step_id == 1 && pending.has_hit && pending.hits.front().body.entity == target.id(),
            "Disabling an independent spatial origin discarded a pending query or stopped its active owner.");
    committed(f.world.set_enabled(logical.id(), false));
    step(f.world, *f.scene, 0);
    require(!f.scene->read_query_result(query.id()) && !f.scene->execute_query(query.id()),
            "Inactive composition owner retained an affine query.");
    committed(f.world.set_enabled(logical.id(), true));
    step(f.world, *f.scene, 0);
    require(take(f.scene->execute_query(query.id())).has_hit,
            "Active query did not resume with its independent origin still disabled.");
    committed(f.world.stop());
}

void query_components() {
    affine_query_space();
    PhysicsFixture f;
    auto target = f.make({0, 0, 0});
    auto emitter = f.make({0, 0, -3});
    auto parent = take(w::create_entity(f.runtime));
    take(f.world.set_transform(parent.id(), {}));
    committed(f.world.attach(parent.id()));
    committed(f.world.reparent(emitter.id(), parent.id(), w::ReparentRule::keep_local));
    auto query = take(w::create_component(f.runtime, f.physics_types.scene_query_component));
    committed(f.world.attach_components(emitter.id(), std::array{w::Attachment{query.id()}}));
    const auto edit = [&](auto callback) {
        take(f.runtime.with_native<p::SceneQueryComponent>(query.id(), [&](auto& value) -> atelier::Result<void> {
            callback(value);
            return {};
        }));
    };
    const auto ready = [&] {
        return take(f.runtime.with_native<const p::SceneQueryComponent>(
            query.id(), [](const auto& value) -> atelier::Result<bool> { return value.query_ready(); }));
    };
    f.start();
    step(f.world, *f.scene, 0);
    require(!f.scene->read_query_result(query.id()) && !ready(), "Unsampled component reported a result.");
    auto snapshot = take(f.scene->execute_query(query.id()));
    require(snapshot.hits.front().body.entity == target.id() && near(snapshot.hits.front().distance, 2.5f),
            "Component did not transform its ray or ignore its owner.");
    edit([](auto& q) { q.ignore_self = false; });
    require(take(f.scene->execute_query(query.id())).hits.front().body.entity == emitter.id(),
            "Opting into self detection did not hit the emitter.");
    edit([](auto& q) { q.ignore_self = true; });
    f.advance(1);
    require(take(f.scene->read_query_result(query.id())).step_id == 1,
            "Automatic query did not stamp the completed fixed step.");
    f.scene->set_paused(true);
    step(f.world, *f.scene, .1f);
    require(take(f.scene->read_query_result(query.id())).step_id == 1, "Paused component query advanced.");
    edit([](auto& q) {
        q.automatic = false;
        q.request_query();
        q.request_query();
    });
    f.scene->step_once();
    step(f.world, *f.scene, 0);
    require(take(f.scene->read_query_result(query.id())).step_id == 2,
            "Coalesced request was not sampled on the next fixed step.");
    f.scene->step_once();
    step(f.world, *f.scene, 0);
    require(take(f.scene->read_query_result(query.id())).step_id == 2, "One-shot request replayed.");
    edit([](auto& q) { q.settings.capacity = 0; });
    require(!f.scene->execute_query(query.id()) && !ready() && !f.scene->read_query_result(query.id()),
            "Failed query retained a stale success.");
    edit([](auto& q) {
        q.settings.capacity = 64;
        q.end = {0, 10, 0};
    });
    require(!take(f.scene->execute_query(query.id())).has_hit && ready(),
            "Miss was not published as a completed empty result.");
    edit([](auto& q) { q.end = {0, 0, 10}; });
    take(f.scene->execute_query(query.id()));
    committed(f.world.set_enabled(parent.id(), false));
    require(!ready() && !f.scene->read_query_result(query.id()), "Inactive ancestor retained a cached result.");
    step(f.world, *f.scene, 0);
    require(!f.scene->execute_query(query.id()), "Query executed under an inactive ancestor.");
    committed(f.world.set_enabled(parent.id(), true));
    step(f.world, *f.scene, 0);
    take(f.scene->execute_query(query.id()));
    auto removed = take(f.world.remove_component(query.id()));
    require(!ready() && !f.scene->read_query_result(query.id()), "Removed component retained query data.");
    committed(f.world.restore(removed));
    step(f.world, *f.scene, 0);
    take(f.scene->execute_query(query.id()));
    committed(f.world.set_enabled(target.body.id(), false));
    require(!f.scene->read_body_state(snapshot.hits.front().body),
            "Saved hit handle remained valid after body removal.");
    require(!take(f.scene->execute_query(query.id())).has_hit, "Removed target was still hittable.");
    w::World other{f.runtime, f.world_types};
    auto other_scene = take(p::PhysicsScene::create(other, f.physics_types));
    require(!other_scene->execute_query(query.id()) && !other_scene->read_query_result(query.id()),
            "Foreign scene exposed a component result.");
    committed(f.world.stop());
    require(!ready() && !f.scene->read_query_result(query.id()), "Stop retained component query state.");
    // A rotated child and a scaled ancestor use the actual World spatial chain.
    committed(f.world.set_enabled(target.body.id(), true));
    take(f.world.set_transform(parent.id(), w::Trs{{-5, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}));
    take(f.world.set_transform(emitter.id(), w::Trs{{0, 0, 0}, {0, .70710678f, 0, .70710678f}, {1, 1, 2}}));
    f.start();
    step(f.world, *f.scene, 0);
    auto transformed = take(f.scene->execute_query(query.id()));
    require(transformed.hits.front().body.entity == target.id() && near(transformed.start.x, -5) &&
                near(transformed.end.x, 15, .001f),
            "Nested rotation/scale was not applied to query endpoints.");
    committed(f.world.stop());
    // Stop also cancels a request made before this component's first reconcile.
    f.start();
    edit([](auto& q) { q.request_query(); });
    committed(f.world.stop());
    f.start();
    f.scene->set_paused(false);
    f.advance(1);
    require(!ready() && !f.scene->read_query_result(query.id()),
            "Unreconciled request survived deactivation and restart.");
    committed(f.world.stop());
}
