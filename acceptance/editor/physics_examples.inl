void verify_physics_examples() {
    const auto committed=[](atelier::Result<atelier::world::Commit> result) {const auto value=take(std::move(result));if(value.notification_error) throw std::runtime_error(value.notification_error->message);};
    namespace p=atelier::physics;namespace fs=std::filesystem;
    const auto evidence=fs::absolute("physics-examples-document")/std::to_string(GetTickCount64());
    auto project=take(editor::Project::create(evidence));
    const auto folder=editor::asset_directory()/"examples/Examples/Physic/Scenes";
    const auto entity_named=[](editor::Session& doc,std::wstring_view name) {
        for(const auto& entity:take(doc.world().members())) if(doc.name(entity.id())==name) return entity.id();
        throw std::runtime_error("Physics example Entity missing.");
    };
    for(const auto* name:{"RigidBodies","Forces","Triggers","Queries","Character","Constraints","Ragdoll","Vehicle","Overview","Raycasts"}) {
        const auto text=take(editor::read_file(folder/(std::string(name)+".scene")));
        auto doc=take(editor::Session::deserialize(text,project));
        const auto saved=take(doc->serialize());const auto path=evidence/"Content/Scenes"/(std::string(name)+".scene");
        take(editor::write_file(path,saved));doc=take(editor::Session::deserialize(take(editor::read_file(path)),project));
        require(take(doc->serialize())==saved,"Physics component scene roundtrip changed settings or target identities.");
        if(std::string_view(name)=="Constraints") {
            auto malformed=nlohmann::json::parse(saved);
            bool changed_target{};
            for(auto& entity:malformed["entities"]) for(auto& component:entity["components"]) if(component["type"]=="physics.DistanceConstraintComponent") {
                component["properties"]["target"]={{"$ref","18446744073709551614"}};changed_target=true;
            }
            require(changed_target,"Constraint malformed-input acceptance did not edit a real reference.");
            require(!editor::Session::deserialize(malformed.dump(),project),"Out-of-scene constraint identity was accepted.");
        }
        committed(doc->start());auto* scene=doc->physics_scene();require(scene!=nullptr,"Project Run did not create physics.");
        take(doc->update(0));const auto bodies=scene->body_count();committed(doc->start());take(doc->update(0));
        require(scene==doc->physics_scene() && bodies==scene->body_count(),"Repeated start/update duplicated physical ownership.");
        unsigned enters{};scene->on_contact([&](const p::ContactEvent& event)->atelier::Result<void>{enters+=event.trigger && event.phase==p::ContactPhase::began;return {};});
        p::SpecialHandle old{};
        const auto named=[&](std::wstring_view label){return entity_named(*doc,label);};
        const std::string_view which=name;
        if(which=="Ragdoll") old=take(scene->ragdoll_handle(named(L"Six part ragdoll")));
        if(which=="Vehicle") old=take(scene->vehicle_handle(named(L"Four wheel vehicle")));
        if(which=="Character") old=take(scene->character_handle(named(L"Capsule character")));
        for(int step=0;step<180;++step) take(doc->update(1.f/60));
        const auto body=[&](std::wstring_view label){return take(scene->read_body_state(take(scene->body_handle(named(label)))));};
        if(which=="RigidBodies") {
            require(std::abs(body(L"Falling box").position.y-.5f)<.1f && std::abs(body(L"Falling sphere").position.y-.5f)<.1f &&
                std::abs(body(L"Falling capsule").position.y-1.f)<.1f,"Rigid shapes did not settle on the triangle mesh floor.");
        } else if(which=="Forces") {
            require(body(L"Continuous force").position.x>-3 && body(L"Single impulse").position.x>2,"Force or impulse component did not drive its body.");
            require(body(L"Moving platform").position.x < -2.8f,"Kinematic component did not follow fixed-step motion.");
        } else if(which=="Triggers") {
            require(enters>0 && body(L"Ignored falling body").position.y<-10 && body(L"Blocking falling body").position.y>.3f,
                "Trigger/filter/blocking example did not simulate its configured response matrix.");
        } else if(which=="Queries") {
            const auto ray=take(scene->raycast({0,1,-4},{0,1,8},{.mode=p::QueryMode::all}));
            require(ray.hits.size()==2 && ray.hits.front().body.entity==named(L"Near query target"),"Ray scene did not return ordered targets.");
            require(!take(scene->sweep_sphere({0,1,-4},{0,1,8},.2f)).hits.empty() &&
                !take(scene->overlap_sphere({4,1,0},.8f)).hits.empty(),"Query example lost sphere sweep/overlap.");
        } else if(which=="Character") {
            const auto state=take(scene->read_character_state(old));require(state.grounded && state.position.x>-2,"Character component did not walk and land.");
        } else if(which=="Constraints") {
            const auto anchor=body(L"Constraint anchor").position;const auto bob=body(L"Constrained pendulum").position;
            require(std::abs(std::hypot(bob.x-anchor.x,bob.y-anchor.y,bob.z-anchor.z)-2)<.1f,"Saved constraint target was not restored.");
        } else if(which=="Ragdoll") {
            const auto pose=take(scene->read_ragdoll_pose(old));require(pose.positions.size()==6 && pose.positions.front().y<2,"Ragdoll did not fall as six connected bodies.");
            const auto parts=take(p::ragdoll_parts(doc->world().runtime(),component(*doc,old.entity,doc->physics_types().ragdoll_component)));
            require(parts.size()==pose.positions.size(),"Ragdoll simulation and authored Entity part sequence disagree.");
            std::unordered_set<ObjectId> presented;
            for(const auto& item:take(doc->presentation().render_items())) {
                const auto found=std::ranges::find(parts,item.entity);if(found==parts.end()) continue;
                const auto index=static_cast<std::size_t>(found-parts.begin());
                require(std::abs(item.world.elements[13]-pose.positions[index].y)<.01f,"Ragdoll visual part did not follow its simulated pose.");
                presented.insert(item.entity);
            }
            require(presented.size()==6,"Ragdoll example lost visual Entity parts.");
        } else if(which=="Vehicle") {
            const auto state=take(scene->read_vehicle_state(old));require(std::abs(state.position.z)>1,"Vehicle component did not accelerate.");
            require(std::ranges::count(state.wheel_contact,true)>=2,"Vehicle wheels did not contact the floor.");
        } else if(which=="Raycasts") {
            unsigned checked{};auto& runtime=doc->world().runtime();
            const auto emitter=named(L"Scene query emitter");
            for(const auto& member:take(doc->world().members())) {
                auto ancestor=member.id();
                while(ancestor!=ObjectId{} && ancestor!=emitter) ancestor=take(world::parent_entity(runtime,ancestor));
                if(ancestor!=emitter) continue;
                for(const auto& item:take(world::components(runtime,member.id()))) {
                auto settings=runtime.with_native<const p::SceneQueryComponent>(item.id(),[](const auto& q)->Result<p::TraceSettings>{return q.settings;});
                if(!settings) continue;
                const auto result=take(scene->read_query_result(item.id()));
                const auto expected=settings->mode==p::TraceMode::test?0u:settings->shape==p::TraceShape::sphere_overlap?1u:
                    settings->selection==p::TraceSelection::object_type?4u:settings->mode==p::TraceMode::single?1u:2u;
                require(result.has_hit && result.hits.size()==expected && result.step_id==scene->completed_step(),"Authored query component has incorrect hits or stamp.");
                if(settings->mode==p::TraceMode::single) require(result.hits.front().body.entity==named(L"First blocking target"),"Authored single trace stopped on a touch.");
                ++checked;
                }
            }
            require(checked==6,"Raycast scene lost a query component.");
        }
        const auto completed=scene->completed_step();scene->set_paused(true);take(doc->update(1.f/30));
        require(scene->completed_step()==completed,"Paused example advanced physics.");scene->step_once();scene->step_once();take(doc->update(0));
        require(scene->completed_step()==completed+1,"Example single-step requests did not coalesce.");
        committed(doc->stop());require(!doc->physics_scene(),"Stopping the document retained its physics scene.");
        committed(doc->start());take(doc->update(0));
        if(which=="Ragdoll") require(!doc->physics_scene()->read_ragdoll_pose(old),"Ragdoll handle survived restart.");
        if(which=="Vehicle") require(!doc->physics_scene()->read_vehicle_state(old),"Vehicle handle survived restart.");
        if(which=="Character") require(!doc->physics_scene()->read_character_state(old),"Character handle survived restart.");
        committed(doc->stop());committed(doc->close());
        std::printf("Physics example: %s PASS\n",name);
    }
    std::puts("Physics examples acceptance: PASS (10 scenes, disk roundtrip, real simulation, queries, visual poses, pause/restart)");
}
