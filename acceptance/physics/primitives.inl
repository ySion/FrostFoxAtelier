void primitive_composition() {
    PhysicsFixture f;
    auto entity=take(w::create_entity(f.runtime));
    auto parent=take(w::create_component(f.runtime,f.presentation_types.static_mesh_component));
    auto child_entity=take(w::create_entity(f.runtime)),visual_entity=take(w::create_entity(f.runtime));
    auto alternate_parent=take(w::create_entity(f.runtime));
    auto child=take(w::create_component(f.runtime,f.world_types.collision_component));
    auto visual=take(w::create_component(f.runtime,f.presentation_types.static_mesh_component));
    require(!take(p::read_settings(f.runtime,parent.id())).enabled,"A visual mesh unexpectedly creates a body.");
    take(pr::set_geometry(f.runtime,parent.id(),atelier::geometry::cube_mesh()));
    take(pr::set_geometry(f.runtime,visual.id(),atelier::geometry::cube_mesh()));
    take(f.world.set_transform(entity.id(),{{10,2,0},{0,.70710677f,0,.70710677f},{1,1,1}}));
    take(f.world.set_transform(child_entity.id(),{{4,2,0},{0,0,0,1},{1,1,1}}));
    take(f.world.set_transform(visual_entity.id(),{{0,1,0},{0,0,0,1},{.25f,.25f,.25f}}));
    take(f.world.set_transform(alternate_parent.id(),{{-3,1,5},{0,0,.70710677f,.70710677f},{1,1,1}}));
    take(p::configure(f.runtime,parent.id(),dynamic_body()));
    take(p::configure(f.runtime,child.id(),dynamic_body()));
    // Build a visual descendant before its physical ancestors; backend map order cannot determine pose correctness.
    committed(f.world.attach_components(visual_entity.id(),std::array{w::Attachment{visual.id()}}));
    committed(f.world.attach_components(child_entity.id(),std::array{w::Attachment{child.id()}}));
    committed(f.world.attach_components(entity.id(),std::array{w::Attachment{parent.id()}}));
    committed(f.world.reparent(visual_entity.id(),child_entity.id(),w::ReparentRule::keep_local));
    committed(f.world.reparent(child_entity.id(),entity.id(),w::ReparentRule::keep_local));
    committed(f.world.attach(entity.id()));
    committed(f.world.attach(alternate_parent.id()));
    f.start();take(f.scene->set_gravity({0,0,0}));
    require(f.scene->body_count()==2,"Explicit child Entities did not create independent bodies.");
    const auto a=take(f.scene->body_handle(parent.id())),b=take(f.scene->body_handle(child.id()));
    require(a.entity==entity.id() && b.entity==child_entity.id() && a.component!=b.component && a==take(f.scene->body_handle(entity.id())),
        "Primitive handles or Entity body selection are ambiguous.");
    const auto before=take(f.scene->read_body_state(b));
    require(near(before.position.x,10) && near(before.position.y,4) && near(before.position.z,-4),
        "Child body was created from local rather than world pose.");
    take(f.scene->add_impulse(a,{300,0,0}));take(f.scene->add_impulse(b,{0,0,600}));
    f.advance(3);
    const auto first=take(f.scene->read_body_state(a)),second=take(f.scene->read_body_state(b));
    require(first.linear_velocity.x>0 && near(second.linear_velocity.x,0) && second.linear_velocity.z>0,
        "A command to one Primitive affected the Entity's other body.");
    for (const auto& state:{first,second}) {
        const auto matrix=take(w::world_matrix(f.runtime,state.handle.entity));
        require(near(matrix[12],state.position.x) && near(matrix[13],state.position.y) && near(matrix[14],state.position.z),
            "Parent-first world pose writeback disagrees with the independent body.");
        auto hit=take(f.scene->overlap_sphere(state.position,.1f));
        require(hit.has_hit && hit.hits.front().body==state.handle,"Query lost a Primitive's identity or current pose.");
    }
    const auto rendered=take(f.shown.render_items());
    require(rendered.size()==2,"Physical meshes and ordinary visual children lost presentation.");
    const auto child_visual=take(w::world_matrix(f.runtime,visual_entity.id()));
    require(near(child_visual[13],second.position.y+1),"Visual child did not follow its simulated Primitive.");
    const auto retained_constraint=take(f.scene->create_distance_constraint(a,b,0,100));
    for(const auto destination:{alternate_parent.id(),entity.id()}) {
        committed(f.world.reparent(child_entity.id(),destination,w::ReparentRule::keep_world));
        step(f.world, *f.scene, 0);
        require(take(f.scene->body_handle(child_entity.id()))==b && f.scene->body_count()==2,
            "A keep-world reparent unnecessarily replaced a body handle.");
        const auto retained=take(f.scene->read_body_state(b));
        require(near(speed(retained.position-second.position),0) &&
            near(speed(retained.linear_velocity-second.linear_velocity),0) &&
            near(speed(retained.angular_velocity-second.angular_velocity),0),
            "A keep-world reparent reset the body's pose or velocity.");
    }
    take(f.scene->destroy_constraint(retained_constraint));
    const auto constraint=take(f.scene->create_distance_constraint(a,b,0,100));
    take(f.scene->add_impulse(b,{100,0,0}));
    committed(f.world.set_enabled(child.id(),false));
    require(f.scene->body_count()==1 && bool(f.scene->read_body_state(a)) && !f.scene->read_body_state(b),
        "Disabling one Primitive removed its sibling or retained its body.");
    require(!f.scene->destroy_constraint(constraint),"An Entity-bound constraint survived removal of its Primitive endpoint.");
    committed(f.world.set_enabled(child.id(),true));
    const auto fresh=take(f.scene->body_handle(child.id()));
    require(fresh!=b && !f.scene->add_impulse(b,{100,0,0}),"Old body handle can act on a rebuilt Primitive.");
    require(step(f.world, *f.scene, fixed_delta).rejected_commands==1,"Queued old-generation command reached a replacement body.");
    committed(f.world.stop());committed(f.world.stop());
    require(f.scene->body_count()==0,"Primitive bodies survived World stop.");
}

void primitive_authoring() {
    PhysicsFixture f;
    auto mesh_entity=take(pr::create_static_mesh_entity(f.world,{.mesh=atelier::geometry::cube_mesh()}));
    const auto mesh=mesh_component(f.runtime,mesh_entity.id());
    const auto body_field=take(f.runtime.find_field(f.world_types.primitive_component,"body"));
    const auto settings_type=take(f.runtime.native_type<p::BodySettings>());
    const auto enabled_field=take(f.runtime.find_field(settings_type,"enabled"));
    const auto radius_field=take(f.runtime.find_field(settings_type,"radius"));
    take(f.world.write(mesh,std::array{body_field,enabled_field},true));
    require(take(p::read_settings(f.runtime,mesh)).enabled,"Reflected body property did not configure the real Primitive.");
    require(!f.world.write(mesh,std::array{body_field,radius_field},-1.f) &&
        take(p::read_settings(f.runtime,mesh)).radius==.5f,"Invalid reflected configuration changed the authoring state.");
    take(f.world.set_transform(mesh_entity.id(),{{0,0,0},{0,0,0,1},{4,2,1}}));
    auto trigger=take(w::create_component(f.runtime,f.world_types.collision_component));
    auto trigger_entity=take(w::create_entity(f.runtime));
    auto owner=take(w::create_entity(f.runtime));
    auto settings=p::BodySettings{};settings.motion=p::MotionKind::kinematic;settings.sensor=true;settings.shape=p::ShapeKind::sphere;
    take(p::configure(f.runtime,trigger.id(),settings));
    committed(f.world.attach_components(trigger_entity.id(),std::array{w::Attachment{trigger.id()}}));
    take(f.world.set_transform(owner.id(),{{10,0,0},{0,0,0,1},{1,1,1}}));
    take(f.world.set_transform(trigger_entity.id(),{}));
    committed(f.world.reparent(trigger_entity.id(),owner.id(),w::ReparentRule::keep_local));
    committed(f.world.attach(owner.id()));committed(f.world.attach(mesh_entity.id()));f.start();
    const auto scaled=take(f.scene->raycast({3,0,0},{0,0,0}));
    require(scaled.has_hit && near(scaled.hits.front().point.x,2),"Box collision ignored Primitive world scale.");
    require(!f.scene->body_handle(owner.id()),"Entity convenience lookup silently selected a descendant body.");
    const auto handle=take(f.scene->body_handle(trigger.id()));
    take(f.scene->set_kinematic_target(handle,{12,0,0},{0,0,0,1}));
    step(f.world, *f.scene, 3*fixed_delta);
    const auto state=take(f.scene->read_body_state(handle));
    require(near(state.position.x,12) && near(speed(state.linear_velocity),0),"Kinematic body drifted past a one-shot target.");
    require(near(take(w::read_transform(f.runtime,trigger_entity.id())).position.x,2),"Kinematic child pose was not converted back to local space.");
    committed(f.world.stop());
    settings.motion=p::MotionKind::dynamic;settings.sensor=false;
    take(p::configure(f.runtime,trigger.id(),settings));
    take(f.world.set_transform(owner.id(),{{10,0,0},{0,0,0,1},{2,1,1}}));
    // Isolate keeps the World and unrelated bodies running without the unrepresentable body.
    committed(f.world.start());step(f.world, *f.scene, 0);
    const auto isolated=f.world.take_diagnostics();
    require(f.world.running() && !f.scene->body_handle(trigger.id()) && bool(f.scene->body_handle(mesh)) && !isolated.empty() &&
        std::ranges::all_of(isolated,[&](const auto& d){return d.entity==trigger_entity.id();}) &&
        std::ranges::any_of(isolated,[](const auto& d){return d.phase==w::DiagnosticPhase::physics;}),
        "Isolated physics kept an unrepresentable body, affected another Entity or did not report it.");
    committed(f.world.stop());take(f.world.set_failure_policy(w::FailurePolicy::halt));
    const auto invalid=take(f.world.start());
    require(invalid.notification_error && !f.world.running() && f.scene->body_count()==0,
        "Unrepresentable moving hierarchy did not fail and clean up its bodies.");
    take(f.world.set_transform(owner.id(),{{10,0,0},{0,0,0,1},{1,1,1}}));
    auto sensor=take(w::create_component(f.runtime,f.world_types.collision_component));
    auto sensor_entity=take(w::create_entity(f.runtime));
    take(f.world.set_transform(sensor_entity.id(),{}));
    settings.shape=p::ShapeKind::box;settings.motion=p::MotionKind::kinematic;settings.sensor=true;
    take(p::configure(f.runtime,sensor.id(),settings));
    take(f.world.set_transform(mesh_entity.id(),{{0,0,0},{0,0,0,1},{1,1,1}}));
    take(p::configure(f.runtime,mesh,dynamic_body()));
    committed(f.world.attach_components(sensor_entity.id(),std::array{w::Attachment{sensor.id()}}));
    committed(f.world.attach(sensor_entity.id(),w::append,mesh_entity.id()));
    const auto attached_sensor=take(f.world.start());
    require(attached_sensor.notification_error && attached_sensor.notification_error->code==atelier::ErrorCode::unsupported_operation &&
        !f.world.running() && f.scene->body_count()==0,
        "A kinematic sensor silently followed a dynamic ancestor with delayed collision state.");
}

void mirrored_scale() {
    PhysicsFixture f;
    // Halt makes any unexpected object failure fail this scenario; degenerate space is a warning, not a failure.
    take(f.world.set_failure_policy(w::FailurePolicy::halt));
    const float n=1/std::sqrt(5.f);
    const auto vertex=[&](float x,float y,float z){
        atelier::geometry::Vertex v{};v.position[0]=x;v.position[1]=y;v.position[2]=z;v.normal[0]=2*n;v.normal[1]=n;return v;
    };
    // An asymmetric ramp over x in [1,2] whose front faces +X and up: mirroring X must move it and turn its front to -X.
    const auto ramp=std::make_shared<const atelier::geometry::Mesh>(std::vector{
        vertex(1,2,-1),vertex(2,0,-1),vertex(2,0,1),vertex(1,2,-1),vertex(2,0,1),vertex(1,2,1)});
    const Vector3 mirror{-1,1,1};
    const auto cooked=std::make_shared<const std::string>(take(p::cook_triangle_mesh(ramp,mirror,"acceptance/ramp")));
    std::vector<atelier::ObjectRoot> roots;
    for(const auto& [z,shape]:{std::pair{0.f,std::shared_ptr<const std::string>{}},std::pair{10.f,cooked}}) {
        auto entity=take(pr::create_static_mesh_entity(f.world,{.mesh=ramp,.transform={{0,0,z},{0,0,0,1},mirror},.mesh_source="acceptance/ramp"}));
        p::BodySettings settings;settings.shape=p::ShapeKind::triangle_mesh;settings.cooked_shape=shape;
        take(p::configure(f.runtime,mesh_component(f.runtime,entity.id()),settings));
        committed(f.world.attach(entity.id()));roots.push_back(std::move(entity));
    }
    auto box=f.make({0,0,20});
    take(f.world.set_transform(box.id(),{{0,0,20},{0,0,0,1},{-2,1,1}}));
    auto walker=take(w::create_entity(f.runtime));
    take(f.world.set_transform(walker.id(),{{0,0,30},{0,0,0,1},mirror}));
    committed(f.world.attach(walker.id()));
    // A uniform reflection (-1,-1,-1) decomposes as (-1,1,1) and a half turn; writeback must restore the authored TRS.
    const atelier::math::Quaternion turn{0,.38268343f,0,.92387953f};
    const w::Trs inverted_pose{{0,0,40},turn,{-1,-1,-1}},child_pose{{2,0,0},turn,{1,1,1}};
    auto inverted=f.make({0,0,40},dynamic_body());
    take(f.world.set_transform(inverted.id(),inverted_pose));
    auto inverted_parent=take(w::create_entity(f.runtime));
    take(f.world.set_transform(inverted_parent.id(),{{0,0,60},{0,0,0,1},{-1,-1,-1}}));
    committed(f.world.attach(inverted_parent.id()));
    auto inverted_child=f.make({},dynamic_body());
    take(f.world.set_transform(inverted_child.id(),child_pose));
    committed(f.world.reparent(inverted_child.id(),inverted_parent.id(),w::ReparentRule::keep_local));
    // Zero scale on a nested chain: one warning on the topmost degenerate node, no body below it.
    auto flat=take(w::create_entity(f.runtime));
    take(f.world.set_transform(flat.id(),{{0,0,80},{0,0,0,1},{1,0,1}}));
    committed(f.world.attach(flat.id()));
    auto flat_child=f.make({},dynamic_body()),flat_grandchild=f.make({},dynamic_body()),flat_sibling=f.make({});
    take(f.world.set_transform(flat_child.id(),{{0,1,0},{0,0,0,1},{1,0,1}}));
    committed(f.world.reparent(flat_child.id(),flat.id(),w::ReparentRule::keep_local));
    committed(f.world.reparent(flat_grandchild.id(),flat_child.id(),w::ReparentRule::keep_local));
    committed(f.world.reparent(flat_sibling.id(),flat.id(),w::ReparentRule::keep_local));
    f.start();take(f.scene->set_gravity({0,0,0}));step(f.world, *f.scene, 0);

    for(const float z:{0.f,10.f}) {
        const auto hit=take(f.scene->raycast({-1.5f,10,z},{-1.5f,-10,z}));
        require(hit.has_hit && near(hit.hits.front().point.y,1) && hit.hits.front().normal.x<0,
            "A mirrored triangle mesh lost its front face or its reflected position.");
        require(!take(f.scene->raycast({1.5f,10,z},{1.5f,-10,z})).has_hit,"A mirrored triangle mesh kept its unmirrored position.");
    }
    const auto side=take(f.scene->raycast({5,0,20},{0,0,20}));
    require(side.has_hit && near(side.hits.front().point.x,1),"A mirrored box did not use the scale magnitude.");
    require(!f.scene->create_character(walker.id()),"A Character accepted a mirrored world scale.");
    for(const auto& body:{flat_child.id(),flat_grandchild.id(),flat_sibling.id()})
        require(!f.scene->body_handle(body),"A Primitive in a degenerate space kept a physical body.");
    auto diagnostics=f.world.take_diagnostics();
    require(diagnostics.size()==1 && diagnostics.front().kind==w::DiagnosticKind::degenerate_value &&
        diagnostics.front().severity==w::Severity::warning && diagnostics.front().phase==w::DiagnosticPhase::physics &&
        diagnostics.front().entity==flat.id(),"Degenerate space was not reported once on its topmost zero-scale node.");

    take(f.scene->add_impulse(f.handle(inverted),{300,0,0}));take(f.scene->add_impulse(f.handle(inverted_child),{300,0,0}));
    f.advance(3);
    const auto same_rotation=[](atelier::math::Quaternion a,atelier::math::Quaternion b) {
        return std::abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w)>1-1e-5f;
    };
    const auto root_local=take(w::read_transform(f.runtime,inverted.id())),child_local=take(w::read_transform(f.runtime,inverted_child.id()));
    require(root_local.scale==inverted_pose.scale && same_rotation(root_local.rotation,inverted_pose.rotation) &&
        root_local.position.x>.01f && near(root_local.position.z,40),"A (-1,-1,-1) body did not round-trip its authored local TRS.");
    // The parent mirrors X, so moving along world +X moves the child along local -X.
    require(child_local.scale==child_pose.scale && same_rotation(child_local.rotation,child_pose.rotation) &&
        child_local.position.x<1.99f && near(child_local.position.z,0),"A body under a (-1,-1,-1) parent did not round-trip its local TRS.");
    for(const auto& body:{inverted.id(),inverted_child.id()}) {
        const auto state=take(f.scene->read_body_state(take(f.scene->body_handle(body))));
        const auto matrix=take(w::world_matrix(f.runtime,body));
        require(near(matrix[12],state.position.x) && near(matrix[13],state.position.y) && near(matrix[14],state.position.z),
            "Mirrored writeback disagrees with the simulated body.");
    }
    require(f.world.take_diagnostics().empty(),"A persisting degenerate space was reissued or mirrored writeback was rejected.");

    committed(f.world.stop());
    take(f.world.set_transform(flat.id(),{{0,0,80},{0,0,0,1},{1,1,1}}));
    take(f.world.set_transform(flat_child.id(),{{0,1,0},{0,0,0,1},{1,1,1}}));
    f.start();step(f.world, *f.scene, 0);
    for(const auto& body:{flat_child.id(),flat_grandchild.id(),flat_sibling.id()})
        require(bool(f.scene->body_handle(body)),"A Primitive did not regain its body once its space was no longer degenerate.");
    committed(f.world.stop());
}

// Any TRS write moves its body as set_transform does: a write past World is read at the
// next step and teleports the body. A step publishes only the poses it moved (active
// bodies, bodies that fell asleep in it, teleported bodies and bodies below a published
// Entity), so it never reverts a write it did not see. Ancestors are read on every step.
void pose_publication() {
    PhysicsFixture f;take(f.world.set_failure_policy(w::FailurePolicy::isolate));
    auto floor=f.ground();
    auto resting=f.make({0,.5f,0},dynamic_body());
    auto falling=f.make({4,60,0},dynamic_body());
    auto carried=f.make({8,.5f,0},dynamic_body());
    committed(f.world.reparent(carried.id(),falling.id(),w::ReparentRule::keep_world));
    auto holder=take(w::create_entity(f.runtime));take(f.world.set_transform(holder.id(),{{20,0,0},{0,0,0,1},{1,1,1}}));
    committed(f.world.attach(holder.id()));
    auto held=f.make({20,.5f,0},dynamic_body());
    committed(f.world.reparent(held.id(),holder.id(),w::ReparentRule::keep_world));
    f.start();
    for(unsigned frame=0;frame<240 && !(f.state(resting).sleeping && f.state(carried).sleeping);++frame) f.advance(1);
    require(f.state(resting).sleeping && f.state(carried).sleeping && !f.state(falling).sleeping,
        "The fixture did not settle its resting bodies while another still falls.");
    require(take(w::read_transform(f.runtime,resting.id())).position==f.state(resting).position,
        "A body that fell asleep kept a pose from before its last step.");
    const auto& x=f.world_types.position_fields[0];
    const auto& z=f.world_types.position_fields[2];
    const auto local=[&](const auto& entity){return take(w::read_transform(f.runtime,entity.id()));};
    const auto rest_x=f.state(resting).position.x+.25f;
    take(f.runtime.write(floor.id(),x,atelier::Scalar{.25f}));
    take(f.runtime.write(resting.id(),x,atelier::Scalar{rest_x}));
    f.advance(1);
    require(near(f.state(floor).position.x,.25f,1e-6f) && local(floor).position==f.state(floor).position,
        "A TRS written past World did not move its static body.");
    require(near(f.state(resting).position.x,rest_x,1e-3f) && local(resting).position==f.state(resting).position,
        "A TRS written past World did not move its sleeping body.");
    require(local(falling).position==f.state(falling).position,"A step did not publish a falling body.");
    const auto carried_state=f.state(carried);const auto carried_matrix=take(w::world_matrix(f.runtime,carried.id()));
    require(near(carried_matrix[12],carried_state.position.x,1e-4f) && near(carried_matrix[13],carried_state.position.y,1e-4f),
        "A sleeping body below a moving one did not keep its world pose.");
    f.scene->on_before_step([&](std::uint64_t,float)->atelier::Result<void> {return f.runtime.write(floor.id(),z,atelier::Scalar{1.5f});});
    f.advance(1);
    f.scene->on_before_step({});
    require(local(floor).position.z==1.5f && f.state(floor).position.z==0,"A step reverted a TRS written while it ran.");
    f.advance(1);
    require(f.state(floor).position.z==1.5f,"A TRS written while a step ran did not move its body at the next step.");
    take(f.scene->teleport(f.handle(floor),{0,-.5f,2},{0,0,0,1}));f.advance(1);
    require(local(floor).position.z==2,"A teleported static body did not publish its pose.");
    require(f.world.take_diagnostics().empty() && bool(f.scene->body_handle(held.id())),"The fixture reported an unexpected physics failure.");
    take(f.runtime.write(holder.id(),f.world_types.scale_fields[1],atelier::Scalar{2.f}));f.advance(1);
    const auto reports=f.world.take_diagnostics();
    require(!f.scene->body_handle(held.id()) && reports.size()==1 && reports[0].entity==held.id() && reports[0].phase==w::DiagnosticPhase::physics,
        "An ancestor scale made nonuniform past World was not checked again on the next step.");
    committed(f.world.stop());
}

// The solver pool runs on several threads; the same input still gives the same poses bit for bit.
void solver_determinism() {
    const auto run=[] {
        PhysicsFixture f;f.ground();
        std::vector<PhysicsFixture::Entity> boxes;
        for(unsigned i=0;i<160;++i) {
            const auto layer=i/40,cell=i%40;
            const float offset=layer%2?.35f:0.f;
            boxes.push_back(f.make({float(cell%8)*1.15f-4+offset,1.5f+float(layer)*1.6f,float(cell/8)*1.15f-2+offset},dynamic_body()));
        }
        f.start();
        std::vector<float> poses;
        for(unsigned frame=0;frame<120;++frame) {
            f.advance(1);
            for(const auto& box:boxes) {
                const auto state=f.state(box);const auto local=take(w::read_transform(f.runtime,box.id()));
                for(const auto* value:{&state.position,&local.position}) poses.insert(poses.end(),{value->x,value->y,value->z});
                for(const auto* value:{&state.rotation,&local.rotation}) poses.insert(poses.end(),{value->x,value->y,value->z,value->w});
            }
        }
        committed(f.world.stop());
        return poses;
    };
    const auto first=run(),second=run();
    require(first.size()==second.size() && std::memcmp(first.data(),second.data(),first.size()*sizeof(float))==0,
        "The same physics input produced different poses on the solver pool.");
}

// A full body pair or contact buffer only drops contacts: the simulation runs on, even under
// the halt policy, and every overflowing step reports one coalesced physics warning.
void contact_capacity() {
    {
        PhysicsFixture f;f.scene.reset();
        require(!p::PhysicsScene::create(f.world,f.physics_types,{.bodies=~0u}),"A capacity above Jolt's limits was accepted.");
        f.scene=take(p::PhysicsScene::create(f.world,f.physics_types));
        const auto derived=f.scene->capacity();
        require(derived.bodies==4096 && derived.body_pairs==8*4096 && derived.contact_constraints==2*4096,
            "A small World did not derive the default physics capacity.");
        f.scene.reset();
        f.scene=take(p::PhysicsScene::create(f.world,f.physics_types,{.body_pairs=1,.contact_constraints=3}));
        require(f.scene->capacity().body_pairs==4 && f.scene->capacity().contact_constraints==4,
            "capacity() did not report Jolt's minimum buffers.");
    }
    // Jolt's body pairs share one buffer with the contact manifolds, so a full contact buffer is the reliable case.
    {
        PhysicsFixture f;f.scene.reset();
        f.scene=take(p::PhysicsScene::create(f.world,f.physics_types,{.contact_constraints=8}));
        require(f.scene->capacity().contact_constraints==8 && f.scene->capacity().body_pairs==8*4096,"An explicit physics capacity was not kept.");
        take(f.world.set_failure_policy(w::FailurePolicy::halt));
        f.ground();
        for(unsigned i=0;i<32;++i) f.make({float(i%8)*1.2f-4,.5f,float(i/8)*1.2f-2},dynamic_body());
        f.start();
        std::uint32_t overflows=0;
        for(unsigned frame=0;frame<30;++frame) overflows+=step(f.world,*f.scene,fixed_delta).contact_overflows;
        const auto reports=f.world.take_diagnostics();
        require(f.world.running() && f.scene->last_step().completed_step==30 && overflows==30,
            "A full physics buffer stopped the simulation or was not counted on every step.");
        require(reports.size()==1 && reports[0].kind==w::DiagnosticKind::capacity_exceeded && reports[0].severity==w::Severity::warning &&
            reports[0].phase==w::DiagnosticPhase::physics && reports[0].field==std::vector<std::string>{"contact_constraints"} && reports[0].repeats==30,
            "A full physics buffer was not reported as one coalesced warning on its buffer.");
        committed(f.world.stop());
    }
    // A resting contact whose cached solve was dropped while the buffer was full is reported again once
    // there is room. The falling boxes are created first, so Jolt's active list puts them before the
    // resting boxes and the resting contacts are the ones left without a constraint.
    {
        PhysicsFixture f;f.scene.reset();
        f.scene=take(p::PhysicsScene::create(f.world,f.physics_types,{.contact_constraints=4}));
        auto floor=f.ground();
        std::vector<PhysicsFixture::Entity> falling,resting;
        for(unsigned i=0;i<28;++i) falling.push_back(f.make({10+float(i%7)*1.5f,.59f,float(i/7)*1.5f},dynamic_body()));
        for(unsigned i=0;i<4;++i) resting.push_back(f.make({-10+float(i)*1.5f,.5f,0},dynamic_body()));
        std::vector<atelier::ObjectId> touching;
        f.scene->on_contact([&](const p::ContactEvent& event)->atelier::Result<void>{
            if(event.phase!=p::ContactPhase::ended && (event.first.entity==floor.id() || event.second.entity==floor.id()))
                touching.push_back(event.first.entity==floor.id()?event.second.entity:event.first.entity);
            return {};
        });
        f.start();
        unsigned quiet=0;
        while(quiet<15 && !step(f.world,*f.scene,fixed_delta).contact_overflows) ++quiet;
        require(quiet>=3 && quiet<15,"The resting contacts did not settle before the falling boxes filled the contact buffer.");
        f.advance(2);
        for(const auto& box:falling) committed(f.world.set_enabled(box.id(),false));
        f.advance(4);
        touching.clear();
        require(!step(f.world,*f.scene,fixed_delta).contact_overflows,"The contact buffer stayed full after the falling boxes left.");
        for(const auto& box:resting)
            require(std::ranges::find(touching,box.id())!=touching.end() && near(f.state(box).position.y,.5f,.05f),
                "A resting contact dropped while the buffer was full was not reported again once there was room.");
        committed(f.world.stop());
    }
}

// A fixed 60 Hz step drawn at 120 frames per second: the World keeps the simulated pose while
// presentation draws each simulated Entity between its previous and current step.
void render_interpolation() {
    PhysicsFixture f;
    f.ground();
    auto falling=f.make({4,60,0},dynamic_body());
    auto carried=f.make({8,.5f,0},dynamic_body());
    committed(f.world.reparent(carried.id(),falling.id(),w::ReparentRule::keep_world));
    const auto visual=[&](atelier::ObjectId parent) {
        auto entity=take(pr::create_static_mesh_entity(f.world,{atelier::geometry::cube_mesh(),{{0,2,0},{0,0,0,1},{1,1,1}}}));
        committed(f.world.attach(entity.id()));
        committed(f.world.reparent(entity.id(),parent,w::ReparentRule::keep_local));
        return entity;
    };
    auto above=visual(falling.id()),below=visual(carried.id());
    auto resting=f.make({-8,.5f,0},dynamic_body());
    auto rested=visual(resting.id());
    f.start();
    for(unsigned frame=0;frame<240 && !(f.state(carried).sleeping && f.state(resting).sleeping);++frame) f.advance(1);
    require(f.state(carried).sleeping && f.state(resting).sleeping && !f.state(falling).sleeping,
        "The resting bodies did not sleep while the falling body falls.");
    const auto slot=[&](const atelier::ObjectRoot& entity) -> const pr::MeshSlot& {
        for(const auto& value:f.shown.mesh_slots()) if(value.item && value.item->entity==entity.id()) return value;
        throw std::runtime_error("A mesh lost its presentation.");
    };
    const auto drawn=[&](const atelier::ObjectRoot& entity) { const auto& value=slot(entity);return value.presented?*value.presented:value.item->world; };
    const auto frame=[&](float elapsed) {
        const auto result=step(f.world,*f.scene,elapsed);
        take(f.shown.update(f.scene->presented_poses()));
        return result.steps;
    };
    constexpr float half=1.f/120;
    frame(half);
    float older=slot(above).item->world[13];
    frame(half);
    float newer=slot(above).item->world[13],last=drawn(above)[13];
    unsigned stepped=0;
    for(unsigned i=0;i<8;++i) {
        const auto steps=frame(half);
        stepped+=steps;
        const auto& simulated=slot(above).item->world;
        const auto shown=drawn(above)[13];
        require(simulated==take(w::world_matrix(f.runtime,above.id())),"Presentation changed the World pose of a descendant.");
        require(steps<=1 && (steps!=0)==(simulated[13]!=newer),"The World pose of a falling body did not change with its steps only.");
        if(steps) {older=newer;newer=simulated[13];}
        require(shown<last && shown>=newer-1e-4f && shown<=older+1e-4f,
            "A falling body's descendant was not drawn between its previous and current step on every frame.");
        require(!slot(below).presented,"A sleeping body below a moving body followed it on screen.");
        last=shown;
    }
    require(stepped==4,"Eight 120 Hz frames did not run four 60 Hz steps.");
    // No step has published a sleeping body reparented between steps, nor a ragdoll created
    // between them; their World poses must bound the search.
    committed(f.world.reparent(resting.id(),falling.id(),w::ReparentRule::keep_world));
    require(frame(0)==0 && slot(above).presented && !slot(rested).presented,
        "A body reparented below a moving body between steps followed it on screen.");
    committed(f.world.reparent(resting.id(),{},w::ReparentRule::keep_world));
    auto doll=take(w::create_entity(f.runtime));
    take(f.world.set_transform(doll.id(),{{0,4,0},{0,0,0,1},{1,1,1}}));
    committed(f.world.attach(doll.id(),w::append,falling.id()));
    auto posed=visual(doll.id());
    const auto ragdoll=take(f.scene->create_ragdoll(doll.id(),std::array{p::RagdollPart{"root"}}));
    require(frame(0)==0 && slot(above).presented && !slot(posed).presented,
        "A ragdoll created below a moving body followed it on screen before its first step.");
    take(f.scene->destroy_ragdoll(ragdoll));
    const auto dropped=take(f.world.detach(doll.id()));
    frame(0);
    const auto authoritative=take(f.shown.render_items());
    require(std::ranges::all_of(authoritative,[&](const pr::RenderItem& item){return item.world==take(w::world_matrix(f.runtime,item.entity));}),
        "render_items() drew a presented pose.");
    // A horizontal move keeps the carried body on the floor.
    const auto height=take(w::read_transform(f.runtime,falling.id())).position.y;
    take(f.world.set_transform(falling.id(),{{-20,height,0},{0,0,0,1},{1,1,1}}));
    take(f.shown.update(f.scene->presented_poses()));
    require(!slot(above).presented && !slot(below).presented,"A relocated body was not drawn at its new pose at once.");
    for(unsigned i=0;i<3;++i) {
        frame(half);
        require(std::abs(drawn(above)[12]+20)<1e-3f && std::abs(drawn(above)[13]-slot(above).item->world[13])<.5f,
            "A relocated body was drawn trailing from its old pose.");
    }
    take(f.runtime.write(falling.id(),f.world_types.position_fields[0],atelier::Scalar{-40.f}));
    frame(fixed_delta);
    require(std::abs(drawn(above)[12]+40)<1e-3f,"A body moved past World was drawn trailing from its old pose.");
    for(unsigned i=0;i<2400 && !(f.state(falling).sleeping && f.state(carried).sleeping);++i) frame(half);
    require(f.state(falling).sleeping && f.state(carried).sleeping,"The bodies did not settle.");
    frame(half);frame(half);
    const auto version=take(f.shown.update(f.scene->presented_poses()));
    frame(half);frame(half);
    require(f.scene->presented_poses().empty() && take(f.shown.update(f.scene->presented_poses()))==version &&
        std::ranges::none_of(f.shown.mesh_slots(),[](const pr::MeshSlot& value){return value.presented.has_value();}),
        "A sleeping scene kept presented poses or a changing version.");
    committed(f.world.stop());
    // A teleported chassis carries its wheel along without a trail from the old place.
    PhysicsFixture v;
    v.ground();
    auto chassis=v.make({0,2,0},dynamic_body());
    auto vehicle=take(w::create_component(v.runtime,v.physics_types.vehicle_component));
    committed(v.world.attach_components(chassis.id(),std::array{w::Attachment{vehicle.id()}}));
    auto wheel=take(w::create_entity(v.runtime));
    take(v.world.set_transform(wheel.id(),{}));
    auto metadata=take(w::create_component(v.runtime,v.physics_types.vehicle_wheel_component));
    committed(v.world.attach_components(wheel.id(),std::array{w::Attachment{metadata.id()}}));
    committed(v.world.attach(wheel.id(),w::append,chassis.id()));
    take(p::set_vehicle_wheels(v.runtime,vehicle.id(),std::array{wheel.id()}));
    v.start();
    v.advance(2);
    // World::set_transform refuses a chassis whose wheels follow it; a Runtime write teleports it alone.
    take(v.runtime.write(chassis.id(),v.world_types.position_fields[0],atelier::Scalar{100.f}));
    v.advance(1);
    const auto poses=v.scene->presented_poses();
    const auto shown=std::ranges::find(poses,wheel.id(),&pr::PresentedPose::entity);
    require(shown!=poses.end() && shown->world_position.x>99 && std::abs(shown->position.x-shown->world_position.x)<1,
        "A teleported chassis drew its wheel trailing from its old place.");
    committed(v.world.stop());
}