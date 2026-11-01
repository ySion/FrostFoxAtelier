void controller_relation_state() {
    PhysicsFixture f;
    take(f.scene->set_gravity({0, 0, 0}));
    auto origin = take(w::create_entity(f.runtime));
    take(f.world.set_transform(origin.id(), {{10, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}));
    committed(f.world.attach(origin.id()));
    auto settings = p::BodySettings{};
    settings.motion = p::MotionKind::kinematic;
    auto moving = f.make({5, 5, 10}, settings);
    auto motion = take(w::create_component(f.runtime, f.physics_types.kinematic_motion_component));
    take(f.runtime.with_native<p::KinematicMotionComponent>(motion.id(), [](auto& value) -> atelier::Result<void> {
        value.amplitude = {2, 0, 0};
        value.period = 1;
        return {};
    }));
    committed(f.world.attach_components(moving.id(), std::array{w::Attachment{motion.id()}}));
    f.start();
    f.advance(15);
    committed(f.world.reparent(moving.id(), origin.id(), w::ReparentRule::keep_world));
    f.advance(1);
    const auto expected = 5.f + 2.f * std::sin(16.f * fixed_delta * 6.283185307179586f);
    require(near(f.state(moving).position.x, expected, .002f), "Keep-world reparent reset the kinematic phase.");
    committed(f.world.stop());
}

void component_lifecycle() {
    controller_relation_state();
    PhysicsFixture f;
    auto ground = f.ground();
    auto entity = f.make({0, 3, 0}, {}, false);
    auto character = take(w::create_component(f.runtime, f.physics_types.character_component));
    committed(f.world.attach_components(entity.id(), std::array{w::Attachment{character.id()}}));
    f.start();
    step(f.world, *f.scene, 0);
    auto removal = take(f.world.remove_component(character.id()));
    require(!f.scene->character_handle(entity.id()), "Removing a controller retained component physics.");
    committed(f.world.restore(removal));
    step(f.world, *f.scene, 0);
    const auto restored = take(f.scene->character_handle(entity.id()));
    committed(f.world.stop());
    require(!f.scene->read_character_state(restored), "World stop retained a character.");
    committed(f.world.start());
    step(f.world, *f.scene, 0);
    require(take(f.scene->character_handle(entity.id())) != restored, "Restart retained the special handle.");
    committed(f.world.stop());
    committed(f.world.set_enabled(character.id(), false));

    auto doll = f.make({12, 4, 0}, {}, false);
    auto controller = take(w::create_component(f.runtime, f.physics_types.ragdoll_component));
    auto first_part = take(w::create_component(f.runtime, f.physics_types.ragdoll_part_component));
    auto second_part = take(w::create_component(f.runtime, f.physics_types.ragdoll_part_component));
    take(f.runtime.with_native<p::RagdollPartComponent>(first_part.id(), [](auto& value) -> atelier::Result<void> {
        value.rest_position = {.2f, 0, 0};
        return {};
    }));
    take(f.runtime.with_native<p::RagdollPartComponent>(second_part.id(), [](auto& value) -> atelier::Result<void> {
        value.parent_index = 0;
        value.rest_position = {0, .8f, 0};
        return {};
    }));
    auto first_entity = take(w::create_entity(f.runtime));
    auto second_entity = take(w::create_entity(f.runtime));
    take(f.world.set_transform(first_entity.id(), {}));
    take(f.world.set_transform(second_entity.id(), {}));
    committed(f.world.attach_components(first_entity.id(), std::array{w::Attachment{first_part.id()}}));
    committed(f.world.attach_components(second_entity.id(), std::array{w::Attachment{second_part.id()}}));
    committed(f.world.attach(first_entity.id(), w::append, doll.id()));
    committed(f.world.attach(second_entity.id(), w::append, doll.id()));
    committed(f.world.attach_components(doll.id(), std::array{w::Attachment{controller.id()}}));
    take(p::set_ragdoll_parts(f.runtime, controller.id(), std::array{first_entity.id(), second_entity.id()}));
    f.start();
    step(f.world, *f.scene, 0);
    const auto ragdoll = take(f.scene->ragdoll_handle(doll.id()));
    committed(f.world.set_enabled(second_part.id(), false));
    require(!f.scene->read_ragdoll_pose(ragdoll), "Disabling a ragdoll part retained its backend.");
    step(f.world, *f.scene, 0);
    require(!f.scene->ragdoll_handle(doll.id()), "Incomplete ragdoll recreated itself.");
    committed(f.world.set_enabled(second_part.id(), true));
    step(f.world, *f.scene, 0);
    const auto rebuilt = take(f.scene->ragdoll_handle(doll.id()));
    require(rebuilt != ragdoll, "Restoring a ragdoll part reused its generation.");
    committed(f.world.set_enabled(controller.id(), false));
    require(!f.scene->read_ragdoll_pose(rebuilt), "Disabling the ragdoll controller retained its bodies.");
    committed(f.world.stop());

    auto anchor = f.make({-12, 6, 0});
    auto bob = f.make({-12, 4, 0}, dynamic_body());
    auto joint = take(w::create_component(f.runtime, f.physics_types.distance_constraint_component));
    take(f.runtime.with_native<p::DistanceConstraintComponent>(joint.id(), [](auto& value) -> atelier::Result<void> {
        value.minimum = 2;
        value.maximum = 2;
        return {};
    }));
    committed(f.world.attach_components(bob.id(), std::array{w::Attachment{joint.id()}}));
    take(p::set_constraint_target(f.runtime, joint.id(), anchor.id()));
    f.start();
    f.advance(120);
    committed(f.world.set_enabled(anchor.entity.id(), false));
    f.advance(30);
    require(f.state(bob).position.y < 3.5f, "Disabling a constraint endpoint did not release and wake its body.");
    committed(f.world.stop());
}

void entity_physics_bindings() {
    PhysicsFixture f;
    take(f.world.set_failure_policy(w::FailurePolicy::halt));
    auto ground = f.ground();
    auto base = take(w::create_entity(f.runtime));
    take(f.world.set_transform(base.id(), {{10, 0, 0}, {0, .70710678f, 0, .70710678f}, {1, 1, 1}}));
    committed(f.world.attach(base.id()));
    auto character = f.make({0, 3, 2}, {}, false);
    committed(f.world.reparent(character.id(), base.id(), w::ReparentRule::keep_local));
    auto driver = take(w::create_component(f.runtime, f.physics_types.character_component));
    committed(f.world.attach_components(character.id(), std::array{w::Attachment{driver.id()}}));
    auto doll = f.make({20, 2, 0}, {}, false);
    auto controller = take(w::create_component(f.runtime, f.physics_types.ragdoll_component));
    committed(f.world.attach_components(doll.id(), std::array{w::Attachment{controller.id()}}));
    auto part = take(w::create_entity(f.runtime));
    take(f.world.set_transform(part.id(), {}));
    auto metadata = take(w::create_component(f.runtime, f.physics_types.ragdoll_part_component));
    auto mesh = take(w::create_component(f.runtime, f.presentation_types.static_mesh_component));
    take(pr::set_geometry(f.runtime, mesh.id(), atelier::geometry::cube_mesh()));
    committed(f.world.attach_components(part.id(), std::array{w::Attachment{metadata.id()}, w::Attachment{mesh.id()}}));
    committed(f.world.attach(part.id(), w::append, doll.id()));
    take(p::set_ragdoll_parts(f.runtime, controller.id(), std::array{part.id()}));
    // Off the controller, so only the part's own write-back can bring it onto the first body.
    take(f.world.set_transform(part.id(), {{1, 0, 0}, {0, 0, 0, 1}, {.35f, .35f, .15f}}));
    f.start();
    step(f.world, *f.scene, 0);
    const auto character_handle = take(f.scene->character_handle(character.id()));
    const auto initial = take(f.scene->read_character_state(character_handle));
    require(near(initial.position.x, 12) && near(initial.position.z, 0),
            "Nested Character used local pose as world pose.");
    f.advance(2);
    require(take(w::read_transform(f.runtime, part.id())).scale == Vector3{.35f, .35f, .15f},
            "Physics pose overwrote visual Entity scale.");
    committed(f.world.set_enabled(part.id(), false));
    require(!f.scene->ragdoll_handle(doll.id()), "Deactivating a bound Entity retained its controller.");
    step(f.world, *f.scene, 0);
    require(!f.scene->ragdoll_handle(doll.id()), "Incomplete binding recreated its controller.");
    committed(f.world.set_enabled(part.id(), true));
    step(f.world, *f.scene, 0);
    require(bool(f.scene->ragdoll_handle(doll.id())), "Reactivating a bound Entity did not recreate its controller.");
    committed(f.world.stop());
    // The part target cannot also be driven by its own ordinary rigid body.
    take(p::configure(f.runtime, mesh.id(), dynamic_body()));
    f.start();
    require(!f.world.update(0), "Two distinct producers were allowed to write one Entity pose.");
    committed(f.world.stop());
    require(f.scene->body_count() == 0, "Rejected pose bindings leaked ordinary bodies.");
}
