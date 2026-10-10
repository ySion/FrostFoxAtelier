void component_lifecycle() {
    PhysicsFixture f;auto ground=f.ground();
    auto actor=f.make({0,3,0},{},false);
    auto character=take(w::create_component(f.runtime,f.physics_types.character_component));
    committed(f.world.attach_components(actor.id(),std::array{w::Attachment{character.id()}}));
    f.start();take(f.scene->update(0));
    const auto first=take(f.scene->character_handle(actor.id()));
    take(f.scene->update(0));require(take(f.scene->character_handle(actor.id()))==first,"Idempotent component reconcile rebuilt a character.");
    committed(f.world.set_enabled(character.id(),false));require(!f.scene->read_character_state(first),"Disabled character component retained its backend.");
    take(f.scene->update(0));require(!f.scene->character_handle(actor.id()),"Disabled character recreated itself.");
    committed(f.world.set_enabled(character.id(),true));take(f.scene->update(0));
    const auto second=take(f.scene->character_handle(actor.id()));require(second!=first,"Character re-enable reused its old generation.");
    committed(f.world.set_enabled(actor.root.id(),false));require(!f.scene->read_character_state(second),"Root disable retained component physics.");
    committed(f.world.set_enabled(actor.root.id(),true));take(f.scene->update(0));
    auto removal=take(f.world.remove_component(character.id()));
    require(!f.scene->character_handle(actor.id()),"Removing a controller retained component physics.");
    committed(f.world.restore(removal));take(f.scene->update(0));
    const auto restored=take(f.scene->character_handle(actor.id()));
    committed(f.world.stop());require(!f.scene->read_character_state(restored),"World stop retained a character.");
    committed(f.world.start());take(f.scene->update(0));require(take(f.scene->character_handle(actor.id()))!=restored,"Restart retained the special handle.");
    committed(f.world.stop());
    // A component may not silently become a second transform writer.
    auto physical=take(p::read_settings(f.runtime,actor.body.id()));physical.enabled=true;take(p::configure(f.runtime,actor.body.id(),physical));f.start();
    require(!f.scene->update(0),"Body and Character components were accepted together.");committed(f.world.stop());
    committed(f.world.set_enabled(character.id(),false));
    auto vehicle=take(w::create_component(f.runtime,f.physics_types.vehicle_component));
    auto settings=take(p::read_settings(f.runtime,actor.body.id()));settings.motion=p::MotionKind::dynamic;
    take(p::configure(f.runtime,actor.body.id(),settings));committed(f.world.attach_components(actor.id(),std::array{w::Attachment{vehicle.id()}}));
    f.start();take(f.scene->update(0));const auto car=take(f.scene->vehicle_handle(actor.id()));
    committed(f.world.set_enabled(actor.body.id(),false));require(!f.scene->read_vehicle_state(car),"Chassis disable retained vehicle controller.");
    take(f.scene->update(0));committed(f.world.set_enabled(actor.body.id(),true));take(f.scene->update(0));
    require(take(f.scene->vehicle_handle(actor.id()))!=car,"Chassis re-enable reused a vehicle handle.");
    auto detached=take(f.world.detach(actor.id()));require(!f.scene->vehicle_handle(actor.id()),"Detached Actor retained vehicle controller.");
    take(f.scene->update(0));committed(f.world.restore(detached));take(f.scene->update(0));
    require(bool(f.scene->vehicle_handle(actor.id())),"Restored Actor did not reconstruct its vehicle.");
    committed(f.world.stop());

    auto doll=f.make({12,4,0},{},false);
    auto controller=take(w::create_component(f.runtime,f.physics_types.ragdoll_component));
    auto first_part=take(w::create_component(f.runtime,f.physics_types.ragdoll_part_component));
    auto second_part=take(w::create_component(f.runtime,f.physics_types.ragdoll_part_component));
    take(f.runtime.with_native<p::RagdollPartComponent>(second_part.id(),[](auto& value)->atelier::Result<void>{
        value.parent_index=0;value.rest_position={0,.8f,0};return {};
    }));
    committed(f.world.attach_components(doll.id(),std::array{w::Attachment{controller.id()},
        w::Attachment{first_part.id(),doll.root.id()},w::Attachment{second_part.id(),doll.root.id()}}));
    f.start();take(f.scene->update(0));const auto ragdoll=take(f.scene->ragdoll_handle(doll.id()));
    take(f.scene->update(0));require(take(f.scene->ragdoll_handle(doll.id()))==ragdoll,"Ragdoll reconcile duplicated its backend.");
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
    committed(f.world.set_enabled(anchor.root.id(),false));f.advance(30);
    require(f.state(bob).position.y<3.5f,"Disabling a constraint endpoint did not release and wake its body.");
    committed(f.world.stop());
    w::World foreign{f.runtime,f.world_types};auto foreign_actor=take(w::create_actor(f.runtime));
    committed(foreign.attach(foreign_actor.id()));
    take(p::set_constraint_target(f.runtime,joint.id(),foreign_actor.id()));
    committed(foreign.start());f.start();
    const auto cross_world=f.scene->update(0);
    require(!cross_world && cross_world.error().message.find("another World")!=std::string::npos,
        "Constraint component accepted an active target from a different World.");
    committed(f.world.stop());committed(foreign.stop());
}
