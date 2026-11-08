void primitive_composition() {
    PhysicsFixture f;
    auto entity = take(w::create_entity(f.runtime));
    auto parent = take(w::create_component(f.runtime, f.presentation_types.static_mesh_component));
    auto child_entity = take(w::create_entity(f.runtime));
    auto child = take(w::create_component(f.runtime, f.world_types.collision_component));
    take(pr::set_geometry(f.runtime, parent.id(), atelier::geometry::cube_mesh()));
    take(f.world.set_transform(entity.id(), {{10, 2, 0}, {0, .70710677f, 0, .70710677f}, {1, 1, 1}}));
    take(f.world.set_transform(child_entity.id(), {{4, 2, 0}, {0, 0, 0, 1}, {1, 1, 1}}));
    take(p::configure(f.runtime, parent.id(), dynamic_body()));
    take(p::configure(f.runtime, child.id(), dynamic_body()));
    committed(f.world.attach_components(child_entity.id(), std::array{w::Attachment{child.id()}}));
    committed(f.world.attach_components(entity.id(), std::array{w::Attachment{parent.id()}}));
    committed(f.world.reparent(child_entity.id(), entity.id(), w::ReparentRule::keep_local));
    committed(f.world.attach(entity.id()));
    f.start();
    take(f.scene->set_gravity({0, 0, 0}));
    const auto b = take(f.scene->body_handle(child.id()));
    const auto before = take(f.scene->read_body_state(b));
    require(near(before.position.x, 10) && near(before.position.y, 4) && near(before.position.z, -4),
            "Child body was created from local rather than world pose.");
    committed(f.world.stop());
}

void primitive_authoring() {
    PhysicsFixture f;
    auto mesh_entity = take(pr::create_static_mesh_entity(f.world, {.mesh = atelier::geometry::cube_mesh()}));
    const auto mesh = mesh_component(f.runtime, mesh_entity.id());
    const auto body_field = take(f.runtime.find_field(f.world_types.primitive_component, "body"));
    const auto settings_type = take(f.runtime.native_type<p::BodySettings>());
    const auto enabled_field = take(f.runtime.find_field(settings_type, "enabled"));
    take(f.world.write(mesh, std::array{body_field, enabled_field}, true));
    take(f.world.set_transform(mesh_entity.id(), {{0, 0, 0}, {0, 0, 0, 1}, {4, 2, 1}}));
    auto trigger = take(w::create_component(f.runtime, f.world_types.collision_component));
    auto trigger_entity = take(w::create_entity(f.runtime));
    auto owner = take(w::create_entity(f.runtime));
    auto settings = p::BodySettings{};
    settings.motion = p::MotionKind::kinematic;
    settings.sensor = true;
    settings.shape = p::ShapeKind::sphere;
    take(p::configure(f.runtime, trigger.id(), settings));
    committed(f.world.attach_components(trigger_entity.id(), std::array{w::Attachment{trigger.id()}}));
    take(f.world.set_transform(owner.id(), {{10, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}));
    take(f.world.set_transform(trigger_entity.id(), {}));
    committed(f.world.reparent(trigger_entity.id(), owner.id(), w::ReparentRule::keep_local));
    committed(f.world.attach(owner.id()));
    committed(f.world.attach(mesh_entity.id()));
    f.start();
    const auto scaled = take(f.scene->raycast({3, 0, 0}, {0, 0, 0}));
    require(scaled.has_hit && near(scaled.hits.front().point.x, 2), "Box collision ignored Primitive world scale.");
    const auto handle = take(f.scene->body_handle(trigger.id()));
    take(f.scene->set_kinematic_target(handle, {12, 0, 0}, {0, 0, 0, 1}));
    step(f.world, *f.scene, 3 * fixed_delta);
    const auto state = take(f.scene->read_body_state(handle));
    require(near(state.position.x, 12) && near(speed(state.linear_velocity), 0),
            "Kinematic body drifted past a one-shot target.");
    require(near(take(w::read_transform(f.runtime, trigger_entity.id())).position.x, 2),
            "Kinematic child pose was not converted back to local space.");
    committed(f.world.stop());
}

void mirrored_scale() {
    PhysicsFixture f;
    take(f.world.set_failure_policy(w::FailurePolicy::halt));
    const float n = 1 / std::sqrt(5.f);
    const auto vertex = [&](float x, float y, float z) {
        atelier::geometry::Vertex v{};
        v.position[0] = x;
        v.position[1] = y;
        v.position[2] = z;
        v.normal[0] = 2 * n;
        v.normal[1] = n;
        return v;
    };
    // An asymmetric ramp over x in [1,2] whose front faces +X and up: mirroring X must move it and turn its front to -X.
    const auto ramp = take(atelier::geometry::Mesh::create(std::vector{
        vertex(1, 2, -1), vertex(2, 0, -1), vertex(2, 0, 1), vertex(1, 2, -1), vertex(2, 0, 1), vertex(1, 2, 1)}));
    const Vector3 mirror{-1, 1, 1};
    auto entity = take(pr::create_static_mesh_entity(
        f.world, {.mesh = ramp, .transform = {{0, 0, 0}, {0, 0, 0, 1}, mirror}, .mesh_source = "acceptance/ramp"}));
    p::BodySettings settings;
    settings.shape = p::ShapeKind::triangle_mesh;
    take(p::configure(f.runtime, mesh_component(f.runtime, entity.id()), settings));
    committed(f.world.attach(entity.id()));
    // A uniform reflection (-1,-1,-1) decomposes as (-1,1,1) and a half turn; writeback must restore the authored TRS.
    const atelier::math::Quaternion turn{0, .38268343f, 0, .92387953f};
    const w::Trs child_pose{{2, 0, 0}, turn, {1, 1, 1}};
    auto inverted_parent = take(w::create_entity(f.runtime));
    take(f.world.set_transform(inverted_parent.id(), {{0, 0, 60}, {0, 0, 0, 1}, {-1, -1, -1}}));
    committed(f.world.attach(inverted_parent.id()));
    auto inverted_child = f.make({}, dynamic_body());
    take(f.world.set_transform(inverted_child.id(), child_pose));
    committed(f.world.reparent(inverted_child.id(), inverted_parent.id(), w::ReparentRule::keep_local));
    f.start();
    take(f.scene->set_gravity({0, 0, 0}));
    step(f.world, *f.scene, 0);

    const auto hit = take(f.scene->raycast({-1.5f, 10, 0}, {-1.5f, -10, 0}));
    require(hit.has_hit && near(hit.hits.front().point.y, 1) && hit.hits.front().normal.x < 0,
            "A mirrored triangle mesh lost its front face or its reflected position.");
    require(!take(f.scene->raycast({1.5f, 10, 0}, {1.5f, -10, 0})).has_hit,
            "A mirrored triangle mesh kept its unmirrored position.");

    take(f.scene->add_impulse(f.handle(inverted_child), {300, 0, 0}));
    f.advance(3);
    const auto same_rotation = [](atelier::math::Quaternion a, atelier::math::Quaternion b) {
        return std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) > 1 - 1e-5f;
    };
    const auto child_local = take(w::read_transform(f.runtime, inverted_child.id()));
    // The parent mirrors X, so moving along world +X moves the child along local -X.
    require(child_local.scale == child_pose.scale && same_rotation(child_local.rotation, child_pose.rotation) &&
                child_local.position.x < 1.99f && near(child_local.position.z, 0),
            "A body under a (-1,-1,-1) parent did not round-trip its local TRS.");
    committed(f.world.stop());
}

void pose_publication() {
    PhysicsFixture f;
    auto falling = f.make({4, 60, 0}, dynamic_body());
    f.start();
    const auto& z = f.world_types.position_fields[2];
    const auto local = [&](const auto& entity) { return take(w::read_transform(f.runtime, entity.id())); };
    f.scene->on_before_step([&](std::uint64_t, float) -> atelier::Result<void> {
        return f.runtime.write(falling.id(), z, atelier::Scalar{3.f});
    });
    f.advance(1);
    f.scene->on_before_step({});
    require(near(local(falling).position.z, 3, 1e-4f) && near(f.state(falling).position.z, 3, 1e-4f),
            "A TRS a step handler wrote did not move its body within that step.");
    committed(f.world.stop());
}

void contact_capacity() {
    // A resting contact whose cached solve was dropped while the buffer was full is reported again once
    // there is room. Falling bodies must precede the resting ones in Jolt's active list.
    {
        PhysicsFixture f;
        f.scene.reset();
        f.scene =
            take(p::PhysicsScene::create(f.world, f.physics_types, pr::read_mesh_source, {.contact_constraints = 4}));
        auto floor = f.ground();
        std::vector<PhysicsFixture::Entity> falling;
        std::vector<PhysicsFixture::Entity> resting;
        for (unsigned i = 0; i < 28; ++i)
            falling.push_back(f.make({10 + float(i % 7) * 1.5f, .59f, float(i / 7) * 1.5f}, dynamic_body()));
        for (unsigned i = 0; i < 4; ++i)
            resting.push_back(f.make({-10 + float(i) * 1.5f, .5f, 0}, dynamic_body()));
        std::vector<atelier::ObjectId> touching;
        f.scene->on_contact([&](const p::ContactEvent& event) -> atelier::Result<void> {
            if (event.phase != p::ContactPhase::ended &&
                (event.first.entity == floor.id() || event.second.entity == floor.id()))
                touching.push_back(event.first.entity == floor.id() ? event.second.entity : event.first.entity);
            return {};
        });
        f.start();
        unsigned quiet = 0;
        while (quiet < 15 && !step(f.world, *f.scene, fixed_delta).contact_overflows)
            ++quiet;
        require(quiet >= 3 && quiet < 15,
                "The resting contacts did not settle before the falling boxes filled the contact buffer.");
        f.advance(2);
        for (const auto& box : falling)
            committed(f.world.set_enabled(box.id(), false));
        f.advance(4);
        touching.clear();
        require(!step(f.world, *f.scene, fixed_delta).contact_overflows,
                "The contact buffer stayed full after the falling boxes left.");
        for (const auto& box : resting)
            require(std::ranges::find(touching, box.id()) != touching.end() && near(f.state(box).position.y, .5f, .05f),
                    "A resting contact dropped while the buffer was full was not reported again once there was room.");
        committed(f.world.stop());
    }
}

void render_interpolation() {
    PhysicsFixture f;
    auto falling = f.make({4, 60, 0}, dynamic_body());
    const auto visual = [&](atelier::ObjectId parent) {
        auto entity = take(pr::create_static_mesh_entity(
            f.world, {atelier::geometry::cube_mesh(), {{0, 2, 0}, {0, 0, 0, 1}, {1, 1, 1}}}));
        committed(f.world.attach(entity.id()));
        committed(f.world.reparent(entity.id(), parent, w::ReparentRule::keep_local));
        return entity;
    };
    auto above = visual(falling.id());
    f.start();
    const auto slot = [&](const atelier::ObjectRoot& entity) -> const pr::MeshSlot& {
        for (const auto& value : f.shown.mesh_slots())
            if (value.item && value.item->entity == entity.id())
                return value;
        atelier::fatal("A mesh lost its presentation.");
    };
    const auto drawn = [&](const atelier::ObjectRoot& entity) {
        const auto& value = slot(entity);
        return value.presented ? *value.presented : value.item->world;
    };
    const auto frame = [&](float elapsed) {
        const auto result = step(f.world, *f.scene, elapsed);
        take(f.shown.update(f.scene->presented_poses()));
        return result.steps;
    };
    constexpr float half = 1.f / 120;
    frame(half);
    frame(half);
    take(f.runtime.write(falling.id(), f.world_types.position_fields[0], atelier::Scalar{-40.f}));
    frame(fixed_delta);
    require(std::abs(drawn(above)[12] + 40) < 1e-3f, "A body moved past World was drawn trailing from its old pose.");
    committed(f.world.stop());
}
