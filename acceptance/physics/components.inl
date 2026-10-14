void reject_bound_native_controllers(PhysicsFixture& fixture,atelier::ObjectId target) {
    const auto before=take(w::read_transform(fixture.runtime,target));const auto body_count=fixture.scene->body_count();
    const auto world_pose=take(w::relative_transform(take(w::world_matrix(fixture.runtime,target)),w::transform_matrix({})));
    require(near(world_pose.scale.x,1) && near(world_pose.scale.y,1) && near(world_pose.scale.z,1) && !fixture.scene->body_handle(target),
        "Bound-writer rejection fixture must have unit world scale and no ordinary body.");
    const std::array parts{p::RagdollPart{"Blocked root",-1,{0,0,0},.2f,.25f}};
    require(!fixture.scene->create_character(target) && !fixture.scene->create_ragdoll(target,parts),
        "Manual native controller stole a bound part/wheel Entity's simulation pose.");
    require(!fixture.scene->character_handle(target) && !fixture.scene->ragdoll_handle(target) &&
        fixture.scene->body_count()==body_count && take(w::read_transform(fixture.runtime,target))==before,
        "Rejected native controller partially changed backend ownership or the bound Entity.");
}

void controller_relation_state() {
    PhysicsFixture f;take(f.scene->set_gravity({0,0,0}));
    auto origin=take(w::create_entity(f.runtime));take(f.world.set_transform(origin.id(),{{10,0,0},{0,0,0,1},{1,1,1}}));
    committed(f.world.attach(origin.id()));
    auto alternate=take(w::create_entity(f.runtime));take(f.world.set_transform(alternate.id(),{{-7,0,0},{0,0,0,1},{1,1,1}}));
    committed(f.world.attach(alternate.id()));
    auto logical=take(w::create_entity(f.runtime));committed(f.world.attach(logical.id()));
    auto forced=f.make({0,5,0},dynamic_body());
    auto force=take(w::create_component(f.runtime,f.physics_types.force_component));
    take(f.runtime.with_native<p::ForceComponent>(force.id(),[](auto& value)->atelier::Result<void>{value.initial_impulse={10,0,0};return {};}));
    committed(f.world.attach_components(forced.id(),std::array{w::Attachment{force.id()}}));
    auto settings=p::BodySettings{};settings.motion=p::MotionKind::kinematic;
    auto moving=f.make({5,5,10},settings);auto motion=take(w::create_component(f.runtime,f.physics_types.kinematic_motion_component));
    take(f.runtime.with_native<p::KinematicMotionComponent>(motion.id(),[](auto& value)->atelier::Result<void>{value.amplitude={2,0,0};value.period=1;return {};}));
    committed(f.world.attach_components(moving.id(),std::array{w::Attachment{motion.id()}}));
    f.start();f.advance(15);
    const auto force_handle=f.handle(forced),motion_handle=f.handle(moving);const auto velocity=f.state(forced).linear_velocity.x;
    committed(f.world.reparent(forced.id(),origin.id(),w::ReparentRule::keep_world));
    committed(f.world.reparent(moving.id(),origin.id(),w::ReparentRule::keep_world));
    f.advance(1);
    const auto expected=5.f+2.f*std::sin(16.f*fixed_delta*6.283185307179586f);
    require(f.handle(forced)==force_handle && f.handle(moving)==motion_handle && velocity>0 &&
        f.state(forced).linear_velocity.x<=velocity*1.01f && f.state(forced).linear_velocity.x>=velocity*.95f &&
        near(f.state(moving).position.x,expected,.002f),
        "Keep-world reparent replaced a body, replayed initial impulse, or reset the kinematic phase.");
    const auto before_force=f.state(forced),before_motion=f.state(moving);
    for(const auto target:{forced.id(),moving.id()})
        committed(f.world.set_space_reference(target,alternate.id(),w::ReparentRule::keep_world));
    take(f.scene->update(0));
    require(f.handle(forced)==force_handle && f.handle(moving)==motion_handle &&
        f.state(forced).position==before_force.position && f.state(moving).position==before_motion.position,
        "Running keep-world spatial reference change replaced a body or moved its backend pose.");
    f.advance(1);
    require(near(f.state(moving).position.x,5.f+2.f*std::sin(17.f*fixed_delta*6.283185307179586f),.002f) &&
        f.state(forced).linear_velocity.x<=velocity*1.01f && f.state(forced).linear_velocity.x>=velocity*.95f,
        "Running spatial reference change replayed initial impulse or reset the kinematic phase.");
    // Composition and space are separate: both owners become independent roots
    // while retaining their explicitly selected frame.
    for(const auto target:{forced.id(),moving.id()}) {
        committed(f.world.reparent(target,{},w::ReparentRule::keep_world));
    }
    committed(f.world.set_enabled(alternate.id(),false));take(f.scene->update(0));
    require(take(w::effective_enabled(f.runtime,forced.id())) && take(w::effective_enabled(f.runtime,moving.id())) &&
        f.handle(forced)==force_handle && f.handle(moving)==motion_handle,
        "Disabling an independent spatial origin disabled or recreated active physical owners.");
    f.advance(1);
    require(near(f.state(moving).position.x,5.f+2.f*std::sin(18.f*fixed_delta*6.283185307179586f),.002f) &&
        f.state(forced).linear_velocity.x<=velocity*1.01f,"Inactive origin reset a controller's continuous state.");
    committed(f.world.reparent(forced.id(),logical.id(),w::ReparentRule::keep_world));
    take(f.scene->update(0));committed(f.world.set_enabled(logical.id(),false));
    require(!take(w::effective_enabled(f.runtime,forced.id())) && !f.scene->read_body_state(force_handle) && f.handle(moving)==motion_handle,
        "Real composition deactivation failed to clear its own physics or affected an independent owner.");
    committed(f.world.set_enabled(logical.id(),true));take(f.scene->update(0));
    require(f.handle(forced)!=force_handle,"Real owner reactivation retained an obsolete body generation.");
    committed(f.world.stop());
}

void component_lifecycle() {
    controller_relation_state();
    PhysicsFixture f;auto ground=f.ground();
    auto entity=f.make({0,3,0},{},false);
    auto character=take(w::create_component(f.runtime,f.physics_types.character_component));
    committed(f.world.attach_components(entity.id(),std::array{w::Attachment{character.id()}}));
    auto frame=take(w::create_entity(f.runtime));take(f.world.set_transform(frame.id(),{{3,0,0},{0,0,0,1},{1,1,1}}));
    committed(f.world.attach(frame.id()));
    auto alternate=take(w::create_entity(f.runtime));take(f.world.set_transform(alternate.id(),{{-4,0,0},{0,0,0,1},{1,1,1}}));
    committed(f.world.attach(alternate.id()));
    f.start();take(f.scene->update(0));
    const auto first=take(f.scene->character_handle(entity.id()));
    take(f.scene->update(0));require(take(f.scene->character_handle(entity.id()))==first,"Idempotent component reconcile rebuilt a character.");
    committed(f.world.reparent(entity.id(),frame.id(),w::ReparentRule::keep_world));
    take(f.scene->update(0));require(take(f.scene->character_handle(entity.id()))==first,"Keep-world reparent rebuilt a Character controller.");
    const auto character_pose=take(f.scene->read_character_state(first)).position;
    committed(f.world.set_space_reference(entity.id(),alternate.id(),w::ReparentRule::keep_world));take(f.scene->update(0));
    require(take(f.scene->character_handle(entity.id()))==first && take(f.scene->read_character_state(first)).position==character_pose,
        "Running keep-world spatial reference change rebuilt or moved a Character controller.");
    committed(f.world.set_enabled(character.id(),false));require(!f.scene->read_character_state(first),"Disabled character component retained its backend.");
    take(f.scene->update(0));require(!f.scene->character_handle(entity.id()),"Disabled character recreated itself.");
    committed(f.world.set_enabled(character.id(),true));take(f.scene->update(0));
    const auto second=take(f.scene->character_handle(entity.id()));require(second!=first,"Character re-enable reused its old generation.");
    committed(f.world.set_enabled(entity.entity.id(),false));require(!f.scene->read_character_state(second),"Root disable retained component physics.");
    committed(f.world.set_enabled(entity.entity.id(),true));take(f.scene->update(0));
    auto removal=take(f.world.remove_component(character.id()));
    require(!f.scene->character_handle(entity.id()),"Removing a controller retained component physics.");
    committed(f.world.restore(removal));take(f.scene->update(0));
    const auto restored=take(f.scene->character_handle(entity.id()));
    committed(f.world.stop());require(!f.scene->read_character_state(restored),"World stop retained a character.");
    committed(f.world.start());take(f.scene->update(0));require(take(f.scene->character_handle(entity.id()))!=restored,"Restart retained the special handle.");
    committed(f.world.stop());
    // A component may not silently become a second transform writer.
    auto physical=take(p::read_settings(f.runtime,entity.body.id()));physical.enabled=true;take(p::configure(f.runtime,entity.body.id(),physical));
    const auto conflict=take(f.world.start());
    require(conflict.notification_error && !f.world.running(),"Body and Character components were accepted together.");committed(f.world.stop());
    committed(f.world.set_enabled(character.id(),false));
    auto vehicle=take(w::create_component(f.runtime,f.physics_types.vehicle_component));
    auto settings=take(p::read_settings(f.runtime,entity.body.id()));settings.motion=p::MotionKind::dynamic;
    take(p::configure(f.runtime,entity.body.id(),settings));committed(f.world.attach_components(entity.id(),std::array{w::Attachment{vehicle.id()}}));
    f.start();take(f.scene->update(0));const auto car=take(f.scene->vehicle_handle(entity.id()));
    committed(f.world.set_enabled(entity.body.id(),false));require(!f.scene->read_vehicle_state(car),"Chassis disable retained vehicle controller.");
    take(f.scene->update(0));committed(f.world.set_enabled(entity.body.id(),true));take(f.scene->update(0));
    require(take(f.scene->vehicle_handle(entity.id()))!=car,"Chassis re-enable reused a vehicle handle.");
    auto detached=take(f.world.detach(entity.id()));require(!f.scene->vehicle_handle(entity.id()),"Detached Entity retained vehicle controller.");
    take(f.scene->update(0));committed(f.world.restore(detached));take(f.scene->update(0));
    require(bool(f.scene->vehicle_handle(entity.id())),"Restored Entity did not reconstruct its vehicle.");
    committed(f.world.stop());

    auto doll=f.make({12,4,0},{},false);
    auto controller=take(w::create_component(f.runtime,f.physics_types.ragdoll_component));
    auto first_part=take(w::create_component(f.runtime,f.physics_types.ragdoll_part_component));
    auto second_part=take(w::create_component(f.runtime,f.physics_types.ragdoll_part_component));
    take(f.runtime.with_native<p::RagdollPartComponent>(first_part.id(),[](auto& value)->atelier::Result<void>{value.rest_position={.2f,0,0};return {};}));
    take(f.runtime.with_native<p::RagdollPartComponent>(second_part.id(),[](auto& value)->atelier::Result<void>{
        value.parent_index=0;value.rest_position={0,.8f,0};return {};
    }));
    auto first_entity=take(w::create_entity(f.runtime)),second_entity=take(w::create_entity(f.runtime));
    take(f.world.set_transform(first_entity.id(),{}));take(f.world.set_transform(second_entity.id(),{}));
    committed(f.world.attach_components(first_entity.id(),std::array{w::Attachment{first_part.id()}}));
    committed(f.world.attach_components(second_entity.id(),std::array{w::Attachment{second_part.id()}}));
    committed(f.world.attach(first_entity.id(),w::append,doll.id()));
    committed(f.world.attach(second_entity.id(),w::append,doll.id()));
    committed(f.world.attach_components(doll.id(),std::array{w::Attachment{controller.id()}}));
    take(p::set_ragdoll_parts(f.runtime,controller.id(),std::array{first_entity.id(),second_entity.id()}));
    f.start();take(f.scene->update(0));const auto ragdoll=take(f.scene->ragdoll_handle(doll.id()));
    reject_bound_native_controllers(f,first_entity.id());
    require(take(f.scene->ragdoll_handle(doll.id()))==ragdoll && bool(f.scene->read_ragdoll_pose(ragdoll)),
        "Rejected native part controller replaced the original ragdoll.");
    take(f.scene->update(0));require(take(f.scene->ragdoll_handle(doll.id()))==ragdoll,"Ragdoll reconcile duplicated its backend.");
    committed(f.world.reparent(doll.id(),frame.id(),w::ReparentRule::keep_world));take(f.scene->update(0));
    require(take(f.scene->ragdoll_handle(doll.id()))==ragdoll,"Keep-world reparent rebuilt an unsolved ragdoll with a nonzero first rest pose.");
    committed(f.world.set_enabled(second_part.id(),false));require(!f.scene->read_ragdoll_pose(ragdoll),"Disabling a ragdoll part retained its backend.");
    take(f.scene->update(0));require(!f.scene->ragdoll_handle(doll.id()),"Incomplete ragdoll recreated itself.");
    committed(f.world.set_enabled(second_part.id(),true));take(f.scene->update(0));
    const auto rebuilt=take(f.scene->ragdoll_handle(doll.id()));require(rebuilt!=ragdoll,"Restoring a ragdoll part reused its generation.");
    committed(f.world.set_enabled(controller.id(),false));require(!f.scene->read_ragdoll_pose(rebuilt),"Disabling the ragdoll controller retained its bodies.");
    committed(f.world.stop());

    auto anchor=f.make({-12,6,0});auto bob=f.make({-12,4,0},dynamic_body());
    auto joint=take(w::create_component(f.runtime,f.physics_types.distance_constraint_component));
    take(f.runtime.with_native<p::DistanceConstraintComponent>(joint.id(),[](auto& value)->atelier::Result<void>{value.minimum=2;value.maximum=2;return {};}));
    committed(f.world.attach_components(bob.id(),std::array{w::Attachment{joint.id()}}));
    take(p::set_constraint_target(f.runtime,joint.id(),anchor.id()));
    f.start();f.advance(120);require(near(f.state(bob).position.y,4,.05f),"Component constraint did not support its body.");
    require(!p::set_constraint_target(f.runtime,joint.id(),ground.id()),"Active constraint target was rebound.");
    committed(f.world.set_enabled(anchor.entity.id(),false));f.advance(30);
    require(f.state(bob).position.y<3.5f,"Disabling a constraint endpoint did not release and wake its body.");
    committed(f.world.stop());
    w::World foreign{f.runtime,f.world_types};auto foreign_entity=take(w::create_entity(f.runtime));
    committed(foreign.attach(foreign_entity.id()));
    take(p::set_constraint_target(f.runtime,joint.id(),foreign_entity.id()));
    committed(foreign.start());f.start();
    const auto cross_world=f.scene->update(0);
    require(!cross_world && cross_world.error().message.find("another World")!=std::string::npos,
        "Constraint component accepted an active target from a different World.");
    committed(f.world.stop());committed(foreign.stop());
}

void entity_physics_bindings() {
    PhysicsFixture f;auto ground=f.ground();
    auto base=take(w::create_entity(f.runtime));
    take(f.world.set_transform(base.id(),{{10,0,0},{0,.70710678f,0,.70710678f},{1,1,1}}));
    committed(f.world.attach(base.id()));
    auto character=f.make({0,3,2},{},false);
    committed(f.world.reparent(character.id(),base.id(),w::ReparentRule::keep_local));
    auto driver=take(w::create_component(f.runtime,f.physics_types.character_component));
    committed(f.world.attach_components(character.id(),std::array{w::Attachment{driver.id()}}));
    auto chassis=f.make({20,2,0},dynamic_body());
    auto vehicle=take(w::create_component(f.runtime,f.physics_types.vehicle_component));
    committed(f.world.attach_components(chassis.id(),std::array{w::Attachment{vehicle.id()}}));
    auto wheel=take(w::create_entity(f.runtime));
    take(f.world.set_transform(wheel.id(),{}));
    auto metadata=take(w::create_component(f.runtime,f.physics_types.vehicle_wheel_component));
    auto mesh=take(w::create_component(f.runtime,f.world_types.static_mesh_component));
    take(w::set_geometry(f.runtime,mesh.id(),atelier::geometry::cube_mesh()));
    committed(f.world.attach_components(wheel.id(),std::array{w::Attachment{metadata.id()},w::Attachment{mesh.id()}}));
    committed(f.world.attach(wheel.id(),w::append,chassis.id()));
    require(!take(f.runtime.is_a(take(f.runtime.type_of(metadata.id())),f.world_types.static_mesh_component)),
        "Wheel metadata still carries presentation or spatial inheritance.");
    require(!p::set_vehicle_wheels(f.runtime,vehicle.id(),std::array{wheel.id(),wheel.id()}),
        "Duplicate Entity bindings were accepted.");
    take(p::set_vehicle_wheels(f.runtime,vehicle.id(),std::array{wheel.id()}));
    require(take(p::vehicle_wheels(f.runtime,vehicle.id()))==std::vector{wheel.id()},"Explicit wheel binding was not preserved.");
    const auto field=take(f.runtime.find_field(f.physics_types.vehicle_component,"wheels"));
    const auto description=take(f.runtime.describe(f.physics_types.vehicle_component));
    const auto exposed=std::ranges::find(description->fields,field,&atelier::FieldInfo::id);
    require(exposed!=description->fields.end() && exposed->value.form==atelier::ValueForm::strong_sequence,"Entity binding is hidden from the shared type definition.");
    f.start();take(f.scene->update(0));
    const auto vehicle_handle=take(f.scene->vehicle_handle(chassis.id()));
    reject_bound_native_controllers(f,wheel.id());
    require(take(f.scene->vehicle_handle(chassis.id()))==vehicle_handle && bool(f.scene->read_vehicle_state(vehicle_handle)),
        "Rejected native wheel controller replaced the original vehicle.");
    committed(f.world.stop());
    take(f.world.set_transform(wheel.id(),{{0,0,0},{0,0,0,1},{.35f,.35f,.15f}}));
    f.start();take(f.scene->update(0));
    const auto character_handle=take(f.scene->character_handle(character.id()));
    const auto initial=take(f.scene->read_character_state(character_handle));
    require(near(initial.position.x,12) && near(initial.position.z,0),"Nested Character used local pose as world pose.");
    require(!p::set_vehicle_wheels(f.runtime,vehicle.id(),{}),"Active vehicle binding was mutable.");
    f.advance(2);
    const auto vehicle_state=take(f.scene->read_vehicle_state(take(f.scene->vehicle_handle(chassis.id()))));
    const auto world=take(w::world_matrix(f.runtime,wheel.id()));
    require(near(world[12],vehicle_state.wheel_positions[0].x) && near(world[13],vehicle_state.wheel_positions[0].y) &&
        near(world[14],vehicle_state.wheel_positions[0].z),"Wheel pose was not published through its bound Entity.");
    require(take(w::read_transform(f.runtime,wheel.id())).scale==Vector3{.35f,.35f,.15f},"Physics pose overwrote visual Entity scale.");
    const auto state=take(f.scene->read_character_state(character_handle));
    const auto character_world=take(w::world_matrix(f.runtime,character.id()));
    require(near(character_world[12],state.position.x) && near(character_world[13],state.position.y),
        "Nested Character pose did not use the batch spatial publication.");
    committed(f.world.set_enabled(wheel.id(),false));
    require(!f.scene->vehicle_handle(chassis.id()),"Deactivating a bound Entity retained its controller.");
    take(f.scene->update(0));require(!f.scene->vehicle_handle(chassis.id()),"Incomplete binding recreated its controller.");
    committed(f.world.set_enabled(wheel.id(),true));take(f.scene->update(0));
    require(bool(f.scene->vehicle_handle(chassis.id())),"Reactivating a bound Entity did not recreate its controller.");
    committed(f.world.stop());
    // The wheel target cannot also be driven by its own ordinary rigid body.
    take(p::configure(f.runtime,mesh.id(),dynamic_body()));f.start();
    require(!f.scene->update(0),"Two distinct producers were allowed to write one Entity pose.");
    committed(f.world.stop());require(f.scene->body_count()==0,"Rejected pose bindings leaked ordinary bodies.");
    auto extra=take(w::create_component(f.runtime,f.world_types.collision_component));
    committed(f.world.attach_components(chassis.id(),std::array{w::Attachment{extra.id()}}));
    const auto duplicate=take(f.world.start());
    require(duplicate.notification_error && !f.world.running() && f.scene->body_count()==0 &&
        !f.scene->body_handle(chassis.id()),"Two Primitive writers on one Entity were accepted or failed to clean their indexes.");
}
