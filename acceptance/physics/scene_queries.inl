void trace_queries() {
    PhysicsFixture f;
    p::BodySettings touch;
    touch.category = 1;
    touch.overlap_categories = 1u << 7;
    auto near_body = f.make({0, 0, 0}, touch);
    p::BodySettings block;
    block.category = 2;
    auto wall = f.make({0, 0, 3}, block);
    f.start();
    step(f.world, *f.scene, 0);
    p::TraceSettings query;
    query.channel = 7;
    const Vector3 start{0, 0, -3};
    const Vector3 end{0, 0, 12};
    auto result = take(f.scene->trace(start, end, query));
    require(result.has_hit && result.has_block && result.hits.size() == 1 && result.hits[0].body.entity == wall.id(),
            "Channel single did not skip touches and select the first block.");
    require(near(result.hits[0].distance, 5.5f) && near(result.hits[0].point.z, 2.5f) &&
                near(result.hits[0].normal.z, -1),
            "Line trace distance, point or normal is incorrect.");
    query.selection = p::TraceSelection::object_type;
    query.shape = p::TraceShape::sphere_sweep;
    query.radius = .25f;
    query.mode = p::TraceMode::single;
    result = take(f.scene->trace(start, end, query));
    require(result.hits.size() == 1 && near(result.hits[0].distance, 2.25f, .002f) &&
                near(result.hits[0].location.z, -.75f, .002f) && near(result.hits[0].point.z, -.5f, .002f),
            "Sphere trace confused shape location with surface contact.");
    query.shape = p::TraceShape::sphere_overlap;
    query.mode = p::TraceMode::multi;
    result = take(f.scene->trace({0, 0, 0}, {}, query));
    require(result.hits.size() == 1 && result.hits[0].body.entity == near_body.id() &&
                result.hits[0].location == Vector3{},
            "Sphere overlap lost its center or identity.");
}

void query_components() {
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
    edit([](auto& q) { q.automatic = false; });
    take(f.scene->execute_query(query.id()));
    auto removed = take(f.world.remove_component(query.id()));
    require(!ready() && !f.scene->read_query_result(query.id()), "Removed component retained query data.");
    committed(f.world.restore(removed));
    step(f.world, *f.scene, 0);
    take(f.scene->execute_query(query.id()));
    committed(f.world.stop());
    require(!ready() && !f.scene->read_query_result(query.id()), "Stop retained component query state.");
    // A rotated child and a scaled ancestor use the actual World spatial chain.
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
    f.advance(1);
    require(!ready() && !f.scene->read_query_result(query.id()),
            "Unreconciled request survived deactivation and restart.");
    committed(f.world.stop());
}
