#include "physics/components.hpp"
#include "runtime/native.hpp"
#include <cmath>
#include <set>
#include <unordered_set>
#include <stdexcept>

namespace atelier::physics {
namespace {
template<class T> T take(Result<T> result) {
    if(!result) throw std::runtime_error(result.error().message);
    if constexpr(!std::is_void_v<T>) return std::move(*result);
}
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
auto failed(const std::exception& e) {return std::unexpected(Error{ErrorCode::invalid_member,e.what()});}
template<class T,class F> auto read(Runtime& runtime,ObjectId id,F callback) {
    using V=std::invoke_result_t<F,const T&>;
    return take(runtime.with_native<const T>(id,[&](const T& value)->Result<V>{return callback(value);}));
}
}
Result<Types> register_types(Runtime& runtime) try {
    Types types;
    take(NativeRegistration<CharacterSettings>{runtime,"physics.CharacterSettings",TypeKind::record}
        .field<&CharacterSettings::radius>("radius").field<&CharacterSettings::capsule_half_height>("capsule_half_height")
        .field<&CharacterSettings::max_slope_radians>("max_slope_radians").field<&CharacterSettings::jump_speed>("jump_speed")
        .field<&CharacterSettings::step_height>("step_height").publish());
    types.character_component=take(NativeRegistration<CharacterComponent>{runtime,"physics.CharacterComponent",TypeKind::object}
        .base<world::Component>().field<&CharacterComponent::settings>("settings")
        .field<&CharacterComponent::velocity>("velocity").field<&CharacterComponent::jump>("jump").publish());
    types.ragdoll_component=take(NativeRegistration<RagdollComponent>{runtime,"physics.RagdollComponent",TypeKind::object}
        .base<world::Component>().field<&RagdollComponent::parts>("parts",{TypeReference::native<world::Entity>(),ValueForm::strong_sequence}).publish());
    types.ragdoll_part_component=take(NativeRegistration<RagdollPartComponent>{runtime,"physics.RagdollPartComponent",TypeKind::object}
        .base<world::Component>().field<&RagdollPartComponent::parent_index>("parent_index")
        .field<&RagdollPartComponent::rest_position>("rest_position").field<&RagdollPartComponent::radius>("radius")
        .field<&RagdollPartComponent::capsule_half_height>("capsule_half_height").publish());
    take(NativeRegistration<VehicleSettings>{runtime,"physics.VehicleSettings",TypeKind::record}
        .field<&VehicleSettings::half_width>("half_width").field<&VehicleSettings::half_length>("half_length")
        .field<&VehicleSettings::wheel_radius>("wheel_radius").field<&VehicleSettings::suspension_length>("suspension_length").publish());
    types.vehicle_component=take(NativeRegistration<VehicleComponent>{runtime,"physics.VehicleComponent",TypeKind::object}
        .base<world::Component>().field<&VehicleComponent::wheels>("wheels",{TypeReference::native<world::Entity>(),ValueForm::strong_sequence}).field<&VehicleComponent::settings>("settings").field<&VehicleComponent::throttle>("throttle")
        .field<&VehicleComponent::steering>("steering").field<&VehicleComponent::brake>("brake").publish());
    types.vehicle_wheel_component=take(NativeRegistration<VehicleWheelComponent>{runtime,"physics.VehicleWheelComponent",TypeKind::object}
        .base<world::Component>().field<&VehicleWheelComponent::wheel_index>("wheel_index").publish());
    types.distance_constraint_component=take(NativeRegistration<DistanceConstraintComponent>{runtime,"physics.DistanceConstraintComponent",TypeKind::object}
        .base<world::Component>().field<&DistanceConstraintComponent::target>("target",{TypeReference::native<world::Entity>(),ValueForm::weak_reference})
        .field<&DistanceConstraintComponent::minimum>("minimum").field<&DistanceConstraintComponent::maximum>("maximum").publish());
    types.force_component=take(NativeRegistration<ForceComponent>{runtime,"physics.ForceComponent",TypeKind::object}
        .base<world::Component>().field<&ForceComponent::force>("force").field<&ForceComponent::initial_impulse>("initial_impulse").publish());
    types.kinematic_motion_component=take(NativeRegistration<KinematicMotionComponent>{runtime,"physics.KinematicMotionComponent",TypeKind::object}
        .base<world::Component>().field<&KinematicMotionComponent::velocity>("velocity")
        .field<&KinematicMotionComponent::amplitude>("amplitude").field<&KinematicMotionComponent::period>("period").publish());
    take(NativeRegistration<TraceShape>{runtime,"physics.TraceShape",TypeKind::enumeration}
        .enumerator("line",TraceShape::line).enumerator("sphere_sweep",TraceShape::sphere_sweep)
        .enumerator("sphere_overlap",TraceShape::sphere_overlap).publish());
    take(NativeRegistration<TraceSelection>{runtime,"physics.TraceSelection",TypeKind::enumeration}
        .enumerator("channel",TraceSelection::channel).enumerator("object_type",TraceSelection::object_type).publish());
    take(NativeRegistration<TraceMode>{runtime,"physics.TraceMode",TypeKind::enumeration}
        .enumerator("test",TraceMode::test).enumerator("single",TraceMode::single).enumerator("multi",TraceMode::multi).publish());
    take(NativeRegistration<TraceSettings>{runtime,"physics.TraceSettings",TypeKind::record}
        .field<&TraceSettings::shape>("shape").field<&TraceSettings::selection>("selection").field<&TraceSettings::mode>("mode")
        .field<&TraceSettings::channel>("channel").field<&TraceSettings::object_types>("object_types")
        .field<&TraceSettings::capacity>("capacity").field<&TraceSettings::radius>("radius")
        .field<&TraceSettings::include_sensors>("include_sensors").publish());
    types.scene_query_component=take(NativeRegistration<SceneQueryComponent>{runtime,"physics.SceneQueryComponent",TypeKind::object}
        .base<world::Component>().field<&SceneQueryComponent::ignored_entities>("ignored_entities",{TypeReference::native<world::Entity>(),ValueForm::strong_sequence})
        .field<&SceneQueryComponent::settings>("settings")
        .field<&SceneQueryComponent::start>("start").field<&SceneQueryComponent::end>("end")
        .field<&SceneQueryComponent::automatic>("automatic").field<&SceneQueryComponent::ignore_self>("ignore_self")
        .method<&SceneQueryComponent::request_query>("request_query",true).method<&SceneQueryComponent::query_ready>("query_ready",true)
        .method<&SceneQueryComponent::has_hit>("has_hit",true).method<&SceneQueryComponent::has_blocking_hit>("has_blocking_hit",true)
        .method<&SceneQueryComponent::result_overflow>("result_overflow",true).method<&SceneQueryComponent::hit_count>("hit_count",true)
        .method<&SceneQueryComponent::hit_distance>("hit_distance",true).method<&SceneQueryComponent::hit_point>("hit_point",true)
        .method<&SceneQueryComponent::hit_normal>("hit_normal",true).publish());
    return types;
} catch(const std::exception& e) {return failed(e);}
Result<void> set_constraint_target(Runtime& runtime,ObjectId component,ObjectId entity) try {
    const auto status=take(world::inspect_component(runtime,component));
    require(!status.active,"Stop the constraint component before rebinding its target.");
    auto reference=take(runtime.null_reference(take(runtime.native_type<world::Entity>()),ValueForm::weak_reference));
    if(entity!=ObjectId{}) {
        take(world::inspect_entity(runtime,entity));
        auto held=take(runtime.hold(entity));reference=take(runtime.observe(held.reference()));
    }
    take(runtime.with_native<DistanceConstraintComponent>(component,[&](auto& value)->Result<void>{
        const auto field=take(runtime.reference_field(component,value.target));
        const std::array<ReferenceChange,1> changes{ReferenceWrite{field,reference}};
        return runtime.apply_reference_changes(changes);
    }));
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<ObjectId> constraint_target(Runtime& runtime,ObjectId component) {
    return runtime.with_native<const DistanceConstraintComponent>(component,[](const auto& value)->Result<ObjectId>{return value.target.id();});
}
namespace {
template<class T, ReferenceSequence T::* Member>
Result<std::vector<ObjectId>> bindings(Runtime& runtime,ObjectId component,std::size_t additional_capacity=0) {
    return runtime.with_native<T>(component,[&](T& value)->Result<std::vector<ObjectId>> {
        auto field=runtime.reference_field(component,value.*Member);if(!field) return std::unexpected(field.error());
        auto count=runtime.sequence_size(*field);if(!count) return std::unexpected(count.error());
        std::vector<ObjectId> result;result.reserve(*count+additional_capacity);
        for(std::size_t i=0;i<*count;++i) {
            auto reference=runtime.sequence_read(*field,i);if(!reference) return std::unexpected(reference.error());
            result.push_back(reference->id());
        }
        return result;
    });
}
template<class T, ReferenceSequence T::* Member>
Result<void> set_bindings(Runtime& runtime,ObjectId component,std::span<const ObjectId> entities,bool requires_inactive=true) try {
    const auto status=take(world::inspect_component(runtime,component));
    require(!requires_inactive || !status.active,"Stop the controller before rebinding its Entities.");
    std::unordered_set<ObjectId> seen;std::vector<ObjectRoot> roots;std::vector<ObjectReference> references;
    roots.reserve(entities.size());references.reserve(entities.size());
    for(auto entity:entities) {
        require(entity!=ObjectId{} && seen.insert(entity).second,"Physics bindings require distinct non-null Entities.");
        take(world::inspect_entity(runtime,entity));roots.push_back(take(runtime.hold(entity)));
        references.push_back(roots.back().reference());
    }
    return runtime.with_native<T>(component,[&](T& value)->Result<void> {
        auto field=runtime.reference_field(component,value.*Member);if(!field) return std::unexpected(field.error());
        auto count=runtime.sequence_size(*field);if(!count) return std::unexpected(count.error());
        const std::array<ReferenceChange,1> changes{SequenceSplice{*field,0,*count,references}};
        return runtime.apply_reference_changes(changes);
    });
} catch(const std::exception& e) {return failed(e);}
}
Result<void> set_ragdoll_parts(Runtime& runtime,ObjectId component,std::span<const ObjectId> entities) {
    return set_bindings<RagdollComponent,&RagdollComponent::parts>(runtime,component,entities);
}
Result<std::vector<ObjectId>> ragdoll_parts(Runtime& runtime,ObjectId component) {
    return bindings<RagdollComponent,&RagdollComponent::parts>(runtime,component);
}
Result<void> set_vehicle_wheels(Runtime& runtime,ObjectId component,std::span<const ObjectId> entities) {
    return set_bindings<VehicleComponent,&VehicleComponent::wheels>(runtime,component,entities);
}
Result<std::vector<ObjectId>> vehicle_wheels(Runtime& runtime,ObjectId component) {
    return bindings<VehicleComponent,&VehicleComponent::wheels>(runtime,component);
}
Result<void> set_query_ignored_entities(Runtime& runtime,ObjectId component,std::span<const ObjectId> entities) {
    return set_bindings<SceneQueryComponent,&SceneQueryComponent::ignored_entities>(runtime,component,entities,false);
}
Result<std::vector<ObjectId>> query_ignored_entities(Runtime& runtime,ObjectId component) {
    return bindings<SceneQueryComponent,&SceneQueryComponent::ignored_entities>(runtime,component);
}

}

namespace atelier::physics::detail {
ComponentSimulation::~ComponentSimulation() {clear();}
void ComponentSimulation::release(const Entry& entry) noexcept {
    switch(entry.kind) {
    case Kind::character: (void)scene_.destroy_character(entry.special);break;
    case Kind::ragdoll: (void)scene_.destroy_ragdoll(entry.special);break;
    case Kind::vehicle: (void)scene_.destroy_vehicle(entry.special);break;
    case Kind::distance: (void)scene_.destroy_constraint(entry.constraint);break;
    case Kind::query: clear_query(entry.component,true);break;
    default:break;
    }
}
void ComponentSimulation::clear() noexcept {for(const auto& [id,entry]:entries_) {(void)id;release(entry);}entries_.clear();}
ObjectId ComponentSimulation::bound_pose_writer(ObjectId object) const {
    for(const auto& [component,entry]:entries_) {
        if(entry.kind!=Kind::ragdoll && entry.kind!=Kind::vehicle) continue;
        if(std::ranges::find(entry.targets,object)==entry.targets.end()) continue;
        const auto handle=entry.kind==Kind::ragdoll?scene_.ragdoll_handle(entry.entity):scene_.vehicle_handle(entry.entity);
        if(handle && *handle==entry.special) return component;
    }
    return {};
}
Result<void> ComponentSimulation::removed(ObjectId object,bool detached) try {
    clear_query(object,true);
    for(auto it=entries_.begin();it!=entries_.end();) {
        const auto& e=it->second;
        if(e.entity==object || e.component==object || e.target==object ||
            e.body.component==object || e.other_body.component==object ||
            std::ranges::find(e.targets,object)!=e.targets.end() ||
            std::ranges::find(e.metadata,object)!=e.metadata.end() ||
            (detached && std::ranges::find(e.ancestors,object)!=e.ancestors.end())) {
            release(e);it=entries_.erase(it);
        } else ++it;
    }
    auto& runtime=world_.runtime();
    auto queries=world_.query(take(runtime.native_type<SceneQueryComponent>()));
    if(!queries && queries.error().code==ErrorCode::busy) return {};
    for(auto query:take(std::move(queries))) {
        if(entries_.contains(query)) continue;
        const auto status=take(world::inspect_component(runtime,query));
        if(query==object || status.owner==object) {clear_query(query,true);continue;}
        for(auto node=status.owner;node!=ObjectId{};) {
            if(node==object) {clear_query(query,true);break;}
            auto parent=world::parent_entity(runtime,node);
            if(!parent) {if(parent.error().code==ErrorCode::busy) break;throw std::runtime_error(parent.error().message);}
            node=*parent;
        }
    }
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<void> ComponentSimulation::reconcile() try {
    auto& runtime=world_.runtime();
    std::unordered_set<ObjectId> seen,membership;std::unordered_map<ObjectId,ObjectId> writers;
    std::vector<std::pair<Entry,bool>> desired_entries;
    const auto is=[&]<class T>(ObjectId id) {return take(runtime.is_a(take(runtime.type_of(id)),take(runtime.native_type<T>())));};
    const auto members=take(world_.members());
    for(const auto& member:members) membership.insert(member.id());
    const auto claim=[&](ObjectId entity,ObjectId writer) {
        auto [at,added]=writers.try_emplace(entity,writer);
        require(added || at->second==writer,"An Entity may have only one active physics pose writer.");
    };
    for(const auto& member:members) {
        if(const auto writer=scene_.pose_writer(member.id());writer!=ObjectId{}) claim(member.id(),writer);
    }
    for(const auto& member:members) {
        if(take(world::inspect_entity(runtime,member.id())).state!=world::EntityState::active) continue;
        if(!take(world::has_transform(runtime,member.id()))) continue;
        const auto group=take(world::components(runtime,member.id()));std::set<Kind> unique;
        for(const auto& item:group) {
            if(!take(world::inspect_component(runtime,item.id())).active) continue;
            Kind kind;
            if(is.template operator()<CharacterComponent>(item.id())) kind=Kind::character;
            else if(is.template operator()<RagdollComponent>(item.id())) kind=Kind::ragdoll;
            else if(is.template operator()<VehicleComponent>(item.id())) kind=Kind::vehicle;
            else if(is.template operator()<DistanceConstraintComponent>(item.id())) kind=Kind::distance;
            else if(is.template operator()<ForceComponent>(item.id())) kind=Kind::force;
            else if(is.template operator()<KinematicMotionComponent>(item.id())) kind=Kind::kinematic;
            else if(is.template operator()<SceneQueryComponent>(item.id())) kind=Kind::query;
            else continue;
            if(kind!=Kind::distance && kind!=Kind::force && kind!=Kind::query) require(unique.insert(kind).second,"Duplicate physics controller on one Entity.");
            Entry desired{kind,member.id(),item.id()};bool available=true;
            for(auto node=member.id();node!=ObjectId{};) {
                require(membership.contains(node),"Physics spatial origin belongs to another World or is detached.");
                desired.ancestors.push_back(node);
                node=take(world::spatial_parent(runtime,node));
            }
            if(kind==Kind::ragdoll || kind==Kind::vehicle) {
                desired.targets=kind==Kind::ragdoll?take(ragdoll_parts(runtime,item.id())):take(vehicle_wheels(runtime,item.id()));
                if(kind==Kind::ragdoll) require(!desired.targets.empty(),"Ragdoll requires explicit part Entity bindings.");
                std::unordered_set<ObjectId> targets;std::set<unsigned> wheels;
                for(auto target:desired.targets) {
                    require(target!=ObjectId{} && targets.insert(target).second,"Physics bindings require distinct non-null Entities.");
                    const auto target_status=take(world::inspect_entity(runtime,target));
                    if(!membership.contains(target)) {
                        require(target_status.state!=world::EntityState::active,"Physics binding Entity belongs to another World.");
                        available=false;continue;
                    }
                    require(take(world::parent_entity(runtime,target))==member.id() && take(world::spatial_parent(runtime,target))==member.id(),"Physics parts must bind direct spatial Entity children.");
                    require(take(world::has_transform(runtime,target)),"Physics binding Entity must have a transform.");
                    available&=take(world::inspect_entity(runtime,target)).state==world::EntityState::active;
                    ObjectId metadata;
                    for(const auto& child:take(world::components(runtime,target))) {
                        const bool matches=kind==Kind::ragdoll?is.template operator()<RagdollPartComponent>(child.id()):is.template operator()<VehicleWheelComponent>(child.id());
                        if(!matches) continue;
                        require(metadata==ObjectId{},"Physics binding Entity has duplicate part metadata.");metadata=child.id();
                        available&=take(world::inspect_component(runtime,metadata)).active;
                    }
                    require(metadata!=ObjectId{},"Physics binding Entity is missing part metadata.");
                    if(kind==Kind::vehicle) {
                        const auto index=read<VehicleWheelComponent>(runtime,metadata,[](const auto& v){return v.wheel_index;});
                        require(index<4 && wheels.insert(index).second,"Vehicle wheel indices must be unique and within [0,3].");
                    }
                    desired.metadata.push_back(metadata);
                }
            }
            if(kind==Kind::vehicle || kind==Kind::distance || kind==Kind::force || kind==Kind::kinematic) {
                auto handle=scene_.body_handle(member.id());if(handle) desired.body=*handle;else available=false;
            }
            if(kind==Kind::distance) {
                desired.target=take(constraint_target(runtime,item.id()));
                if(desired.target!=ObjectId{} && !membership.contains(desired.target)) {
                    auto status=world::inspect_entity(runtime,desired.target);
                    require(!status || status->state!=world::EntityState::active,"Constraint target belongs to another World.");
                }
                auto other=scene_.body_handle(desired.target);
                if(other && membership.contains(desired.target)) desired.other_body=*other;else available=false;
            }
            if(available) {
                if(kind==Kind::character || kind==Kind::ragdoll) claim(member.id(),member.id());
                if(kind==Kind::ragdoll || kind==Kind::vehicle) for(auto target:desired.targets) claim(target,item.id());
            }
            desired_entries.emplace_back(std::move(desired),available);
        }
    }
    // Validate every pose claim before constructing or replacing any controller.
    for(auto& [desired,available]:desired_entries) {
        const auto kind=desired.kind;
        auto current=entries_.find(desired.component);
        if(current!=entries_.end()) {
            auto& e=current->second;
            bool valid=available && e.entity==desired.entity && e.target==desired.target && e.body==desired.body &&
                e.other_body==desired.other_body && e.targets==desired.targets && e.metadata==desired.metadata;
            if(kind==Kind::character) valid&=scene_.character_handle(e.entity)==e.special;
            if(kind==Kind::ragdoll) valid&=scene_.ragdoll_handle(e.entity)==e.special;
            if(kind==Kind::vehicle) valid&=scene_.vehicle_handle(e.entity)==e.special;
            if(valid && (kind==Kind::character || kind==Kind::ragdoll)) valid=take(scene_.controller_transform_matches(e.entity,e.origin,e.rotation));
            if(valid) {e.ancestors=std::move(desired.ancestors);seen.insert(desired.component);continue;}
            release(e);entries_.erase(current);
        }
        if(!available) continue;
        if(kind!=Kind::query) {
            const auto origin=take(world::relative_transform(take(world::world_matrix(runtime,desired.entity)),world::transform_matrix({})));
            desired.origin=origin.position;desired.rotation=origin.rotation;
        }
        switch(kind) {
        case Kind::character:
            desired.special=take(scene_.create_character(desired.entity,read<CharacterComponent>(runtime,desired.component,[](const auto& v){return v.settings;})));break;
        case Kind::ragdoll: {
            std::vector<RagdollPart> parts;
            for(auto metadata:desired.metadata) parts.push_back(read<RagdollPartComponent>(runtime,metadata,[&](const auto& v){
                return RagdollPart{std::to_string(parts.size()),v.parent_index,v.rest_position,v.radius,v.capsule_half_height};}));
            desired.special=take(scene_.create_ragdoll(desired.entity,parts));break;
        }
        case Kind::vehicle:
            desired.special=take(scene_.create_vehicle(desired.body,read<VehicleComponent>(runtime,desired.component,[](const auto& v){return v.settings;})));break;
        case Kind::distance: {
            auto limits=read<DistanceConstraintComponent>(runtime,desired.component,[](const auto& v){return std::pair{v.minimum,v.maximum};});
            desired.constraint=take(scene_.create_distance_constraint(desired.body,desired.other_body,limits.first,limits.second));break;
        }
        case Kind::force:case Kind::kinematic:case Kind::query:break;
        }
        try {entries_.emplace(desired.component,desired);}catch(...) {release(desired);throw;}
        seen.insert(desired.component);
    }
    for(auto it=entries_.begin();it!=entries_.end();) {
        if(seen.contains(it->first)) ++it;else {release(it->second);it=entries_.erase(it);}
    }
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<void> ComponentSimulation::before_step(float fixed) try {
    auto& runtime=world_.runtime();
    for(auto& [id,e]:entries_) {
        switch(e.kind) {
        case Kind::character: {
            const auto input=read<CharacterComponent>(runtime,id,[](const auto& v){return std::pair{v.velocity,v.jump};});
            take(scene_.set_character_input(e.special,input.first,input.second));
            take(runtime.with_native<CharacterComponent>(id,[](auto& v)->Result<void>{v.jump=false;return {};}));break;
        }
        case Kind::vehicle: {
            const auto input=read<VehicleComponent>(runtime,id,[](const auto& v){return std::array{v.throttle,v.steering,v.brake};});
            take(scene_.set_vehicle_input(e.special,input[0],input[1],input[2]));break;
        }
        case Kind::force: {
            const auto input=read<ForceComponent>(runtime,id,[](const auto& v){return std::pair{v.force,v.initial_impulse};});
            require(math::is_finite(input.first)&&math::is_finite(input.second),"Force component inputs must be finite.");
            if(e.impulse_pending) {take(scene_.add_impulse(e.body,input.second));e.impulse_pending=false;}
            take(scene_.add_force(e.body,input.first));break;
        }
        case Kind::kinematic: {
            const auto input=read<KinematicMotionComponent>(runtime,id,[](const auto& v){return std::tuple{v.velocity,v.amplitude,v.period};});
            const auto& [velocity,amplitude,period]=input;
            require(math::is_finite(velocity)&&math::is_finite(amplitude)&&std::isfinite(period)&&period>0,"Kinematic motion requires finite velocity/amplitude and a positive period.");
            const auto next=e.elapsed+fixed;const auto where=e.origin+velocity*float(next)+amplitude*float(std::sin(next*6.283185307179586/period));
            take(scene_.set_kinematic_target(e.body,where,e.rotation));e.elapsed=next;break;
        }
        default:break;
        }
    }
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<void> ComponentSimulation::append_poses(std::vector<world::SimulationPose>& poses) try {
    auto& runtime=world_.runtime();
    for(const auto& [id,e]:entries_) {
        (void)id;
        if(e.kind==Kind::ragdoll) {
            auto pose=take(scene_.read_ragdoll_pose(e.special));
            require(pose.positions.size()==e.targets.size() && pose.rotations.size()==e.targets.size(),"Ragdoll pose does not match its Entity bindings.");
            for(std::size_t i=0;i<e.targets.size();++i) poses.push_back({e.targets[i],pose.positions[i],pose.rotations[i]});
        } else if(e.kind==Kind::vehicle) {
            const auto state=take(scene_.read_vehicle_state(e.special));
            for(std::size_t i=0;i<e.targets.size();++i) {
                const auto index=read<VehicleWheelComponent>(runtime,e.metadata[i],[](const auto& v){return v.wheel_index;});
                require(index<4,"Vehicle wheel index is out of range.");
                poses.push_back({e.targets[i],state.wheel_positions[index],state.wheel_rotations[index]});
            }
        }
    }
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<void> ComponentSimulation::after_step() try {
    for(auto& [id,e]:entries_) {
        if(e.kind==Kind::character || e.kind==Kind::ragdoll) {
            // Remember the published owner frame, which need not equal a ragdoll's
            // first rest-body frame before its first completed simulation step.
            const auto pose=take(world::relative_transform(take(world::world_matrix(world_.runtime(),e.entity)),world::transform_matrix({})));
            e.origin=pose.position;e.rotation=pose.rotation;
        } else if(e.kind==Kind::query &&
            read<SceneQueryComponent>(world_.runtime(),id,[](const auto& v){return v.automatic || v.pending_;})) take(sample_query(id));
    }
    return {};
} catch(const std::exception& e) {return failed(e);}
void ComponentSimulation::clear_query(ObjectId id,bool clear_request) noexcept {
    (void)world_.runtime().with_native<SceneQueryComponent>(id,[&](auto& q)->Result<void>{
        if(clear_request) q.pending_=false;
        q.ready_=q.hit_=q.blocking_=q.overflow_=false;q.count_=0;q.distance_=0;q.point_={};q.normal_={};return {};
    });
}
Result<void> ComponentSimulation::sample_query(ObjectId id,QueryIgnore ignore) try {
    auto found=entries_.find(id);
    require(found!=entries_.end() && found->second.kind==Kind::query,"Query component is inactive, foreign or has not been reconciled.");
    auto& entry=found->second;entry.query.reset();clear_query(id,false);
    auto& runtime=world_.runtime();
    const auto [settings,start,end,ignore_self]=read<SceneQueryComponent>(runtime,id,[](const auto& q){return std::tuple{q.settings,q.start,q.end,q.ignore_self};});
    const auto matrix=take(world::world_matrix(runtime,entry.entity));
    auto ignored=take(bindings<SceneQueryComponent,&SceneQueryComponent::ignored_entities>(runtime,id,ignore.entities.size()+std::size_t(ignore_self)));
    ignored.insert(ignored.end(),ignore.entities.begin(),ignore.entities.end());
    if(ignore_self) ignored.push_back(entry.entity);
    auto result=take(scene_.trace(math::transform_point(start,matrix),math::transform_point(end,matrix),settings,{ignored,ignore.components}));
    take(runtime.with_native<SceneQueryComponent>(id,[&](auto& q)->Result<void>{
        q.pending_=false;q.ready_=true;q.hit_=result.has_hit;q.blocking_=result.has_block;q.overflow_=result.overflow;
        q.count_=static_cast<std::uint32_t>(result.hits.size());
        if(!result.hits.empty()) {q.distance_=result.hits.front().distance;q.point_=result.hits.front().point;q.normal_=result.hits.front().normal;}
        return {};
    }));
    entry.query=std::move(result);
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<QueryResult> ComponentSimulation::execute_query(ObjectId id,QueryIgnore ignore) {
    if(auto sampled=sample_query(id,ignore); !sampled) return std::unexpected(sampled.error());
    return read_query_result(id);
}
Result<QueryResult> ComponentSimulation::read_query_result(ObjectId id) const {
    const auto found=entries_.find(id);
    if(found==entries_.end() || !found->second.query)
        return std::unexpected(Error{ErrorCode::invalid_member,"Query component has no completed result in this scene."});
    return *found->second.query;
}
}
