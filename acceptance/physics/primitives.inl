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
