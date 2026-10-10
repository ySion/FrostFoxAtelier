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
        .base<world::Component>().publish());
    types.ragdoll_part_component=take(NativeRegistration<RagdollPartComponent>{runtime,"physics.RagdollPartComponent",TypeKind::object}
        .base<world::StaticMeshComponent>().field<&RagdollPartComponent::parent_index>("parent_index")
        .field<&RagdollPartComponent::rest_position>("rest_position").field<&RagdollPartComponent::radius>("radius")
        .field<&RagdollPartComponent::capsule_half_height>("capsule_half_height").publish());
    take(NativeRegistration<VehicleSettings>{runtime,"physics.VehicleSettings",TypeKind::record}
        .field<&VehicleSettings::half_width>("half_width").field<&VehicleSettings::half_length>("half_length")
        .field<&VehicleSettings::wheel_radius>("wheel_radius").field<&VehicleSettings::suspension_length>("suspension_length").publish());
    types.vehicle_component=take(NativeRegistration<VehicleComponent>{runtime,"physics.VehicleComponent",TypeKind::object}
        .base<world::Component>().field<&VehicleComponent::settings>("settings").field<&VehicleComponent::throttle>("throttle")
        .field<&VehicleComponent::steering>("steering").field<&VehicleComponent::brake>("brake").publish());
    types.vehicle_wheel_component=take(NativeRegistration<VehicleWheelComponent>{runtime,"physics.VehicleWheelComponent",TypeKind::object}
        .base<world::StaticMeshComponent>().field<&VehicleWheelComponent::wheel_index>("wheel_index").publish());
    types.distance_constraint_component=take(NativeRegistration<DistanceConstraintComponent>{runtime,"physics.DistanceConstraintComponent",TypeKind::object}
        .base<world::Component>().hidden_field<&DistanceConstraintComponent::target>("target",{TypeReference::native<world::Actor>(),ValueForm::weak_reference})
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
        .base<world::SceneComponent>().field<&SceneQueryComponent::settings>("settings")
        .field<&SceneQueryComponent::start>("start").field<&SceneQueryComponent::end>("end")
        .field<&SceneQueryComponent::automatic>("automatic").field<&SceneQueryComponent::ignore_self>("ignore_self")
        .method<&SceneQueryComponent::request_query>("request_query",true).method<&SceneQueryComponent::query_ready>("query_ready",true)
        .method<&SceneQueryComponent::has_hit>("has_hit",true).method<&SceneQueryComponent::has_blocking_hit>("has_blocking_hit",true)
        .method<&SceneQueryComponent::result_overflow>("result_overflow",true).method<&SceneQueryComponent::hit_count>("hit_count",true)
        .method<&SceneQueryComponent::hit_distance>("hit_distance",true).method<&SceneQueryComponent::hit_point>("hit_point",true)
        .method<&SceneQueryComponent::hit_normal>("hit_normal",true).publish());
    return types;
} catch(const std::exception& e) {return failed(e);}
Result<void> set_constraint_target(Runtime& runtime,ObjectId component,ObjectId actor) try {
    const auto status=take(world::inspect_component(runtime,component));
    require(!status.active,"Stop the constraint component before rebinding its target.");
    auto reference=take(runtime.null_reference(take(runtime.native_type<world::Actor>()),ValueForm::weak_reference));
    if(actor!=ObjectId{}) {
        take(world::inspect_actor(runtime,actor));
        auto held=take(runtime.hold(actor));reference=take(runtime.observe(held.reference()));
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
Result<void> ComponentSimulation::removed(ObjectId object) try {
    // A component can receive a request after activation but before the first
    // reconcile. Deactivation must cancel it even when no Entry exists yet.
    clear_query(object,true);
    for(auto it=entries_.begin();it!=entries_.end();) {
        const auto& e=it->second;
        if(e.actor==object || e.component==object || e.root==object || e.target==object ||
            e.body.component==object || e.other_body.component==object || std::ranges::find(e.visuals,object)!=e.visuals.end()) {
            release(e);it=entries_.erase(it);
        } else ++it;
    }
    // World stop/deactivation notifies the Actor, not every child. Inspect its
    // composition as well, including queries that have never been reconciled.
    auto& runtime=world_.runtime();
    const auto component=world::inspect_component(runtime,object);
    // During detach the World deliberately hides the transitioning composition;
    // its following detached notifications visit the children individually.
    if(!component && component.error().code==ErrorCode::busy) return {};
    const auto owner=component?component->owner:object;
    if(owner==ObjectId{}) return {};
    auto group=world::components(runtime,owner);
    if(!group && group.error().code==ErrorCode::busy) return {};
    for(const auto& child:take(std::move(group))) {
        if(entries_.contains(child.id())) continue;
        const auto is_query=take(runtime.is_a(take(runtime.type_of(child.id())),take(runtime.native_type<SceneQueryComponent>())));
        if(!is_query) continue;
        if(owner==object) {clear_query(child.id(),true);continue;}
        auto ancestor=child.id();
        while(ancestor!=ObjectId{}) {
            if(ancestor==object) {clear_query(child.id(),true);break;}
            ancestor=take(world::scene_parent(runtime,ancestor));
        }
    }
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<void> ComponentSimulation::reconcile() try {
    auto& runtime=world_.runtime();
    std::unordered_set<ObjectId> seen;
    const auto is=[&]<class T>(ObjectId id) {return take(runtime.is_a(take(runtime.type_of(id)),take(runtime.native_type<T>())));};
    const auto members=take(world_.members());
    for(const auto& member:members) {
        if(take(world::inspect_actor(runtime,member.id())).state!=world::ActorState::active) continue;
        const auto root=take(world::root_component(runtime,member.id()));
        if(!root || !take(world::inspect_component(runtime,*root)).active) continue;
        const auto group=take(world::components(runtime,member.id()));
        unsigned writers=0;std::set<Kind> unique;
        for(const auto& item:group) if(take(world::inspect_component(runtime,item.id())).active)
            writers+=is.template operator()<CharacterComponent>(item.id()) || is.template operator()<RagdollComponent>(item.id());
        writers+=bool(scene_.body_handle(*root));
        require(writers<=1,"The Actor root may have only one physics pose driver.");
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
            if(kind!=Kind::distance && kind!=Kind::force && kind!=Kind::query) require(unique.insert(kind).second,"Duplicate physics controller on one Actor.");
            Entry desired{kind,member.id(),item.id(),*root};
            bool available=true;
            if(kind==Kind::query) {
                auto ancestor=item.id();
                while(ancestor!=ObjectId{}) {
                    desired.visuals.push_back(ancestor);
                    available&=take(world::inspect_component(runtime,ancestor)).active;
                    ancestor=take(world::scene_parent(runtime,ancestor));
                }
                require(desired.visuals.back()==*root,"Scene query must belong to the Actor root's spatial tree.");
            }
            if(kind==Kind::ragdoll || kind==Kind::vehicle) {
                std::set<unsigned> wheels;
                for(const auto& child:group) {
                    const bool visual=kind==Kind::ragdoll?is.template operator()<RagdollPartComponent>(child.id()):is.template operator()<VehicleWheelComponent>(child.id());
                    if(!visual) continue;
                    available&=take(world::inspect_component(runtime,child.id())).active;
                    require(take(world::scene_parent(runtime,child.id()))==*root,"Physics visual parts must be direct children of the Actor root.");
                    if(kind==Kind::vehicle) {
                        const auto index=read<VehicleWheelComponent>(runtime,child.id(),[](const auto& v){return v.wheel_index;});
                        require(index<4 && wheels.insert(index).second,"Vehicle wheel indices must be unique and within [0,3].");
                    }
                    desired.visuals.push_back(child.id());
                }
                if(kind==Kind::ragdoll) require(!desired.visuals.empty(),"Ragdoll requires RagdollPart components.");
            }
            if(kind==Kind::vehicle || kind==Kind::distance || kind==Kind::force || kind==Kind::kinematic) {
                auto handle=scene_.body_handle(member.id());
                if(handle) desired.body=*handle;else available=false;
            }
            if(kind==Kind::distance) {
                desired.target=take(constraint_target(runtime,item.id()));
                const bool member_target=std::ranges::any_of(members,[&](const auto& a){return a.id()==desired.target;});
                if(desired.target!=ObjectId{} && !member_target) {
                    auto status=world::inspect_actor(runtime,desired.target);
                    require(!status || status->state!=world::ActorState::active,"Constraint target belongs to another World.");
                }
                auto other=scene_.body_handle(desired.target);
                if(other && member_target) desired.other_body=*other;else available=false;
            }
            auto current=entries_.find(item.id());
            if(current!=entries_.end()) {
                auto& e=current->second;
                bool valid=available && e.actor==desired.actor && e.root==desired.root && e.target==desired.target &&
                    e.body==desired.body && e.other_body==desired.other_body && e.visuals==desired.visuals;
                if(kind==Kind::character) valid&=scene_.character_handle(e.actor)==e.special;
                if(kind==Kind::ragdoll) valid&=scene_.ragdoll_handle(e.actor)==e.special;
                if(kind==Kind::vehicle) valid&=scene_.vehicle_handle(e.actor)==e.special;
                if(valid) {seen.insert(item.id());continue;}
                release(e);entries_.erase(current);
            }
            if(!available) continue; // Dependencies may be temporarily disabled or detached.
            const auto origin=take(world::read_transform(runtime,*root));desired.origin=origin.position;desired.rotation=origin.rotation;
            switch(kind) {
            case Kind::character:
                desired.special=take(scene_.create_character(member.id(),read<CharacterComponent>(runtime,item.id(),[](const auto& v){return v.settings;})));break;
            case Kind::ragdoll: {
                std::vector<RagdollPart> parts;
                for(auto visual:desired.visuals) parts.push_back(read<RagdollPartComponent>(runtime,visual,[&](const auto& v){
                    return RagdollPart{std::to_string(parts.size()),v.parent_index,v.rest_position,v.radius,v.capsule_half_height};}));
                desired.special=take(scene_.create_ragdoll(member.id(),parts));break;
            }
            case Kind::vehicle:
                desired.special=take(scene_.create_vehicle(desired.body,read<VehicleComponent>(runtime,item.id(),[](const auto& v){return v.settings;})));break;
            case Kind::distance: {
                auto limits=read<DistanceConstraintComponent>(runtime,item.id(),[](const auto& v){return std::pair{v.minimum,v.maximum};});
                desired.constraint=take(scene_.create_distance_constraint(desired.body,desired.other_body,limits.first,limits.second));break;
            }
            case Kind::force:case Kind::kinematic:case Kind::query:break;
            }
            try {entries_.emplace(item.id(),desired);}catch(...) {release(desired);throw;}
            seen.insert(item.id());
        }
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
Result<void> ComponentSimulation::after_step() try {
    auto& runtime=world_.runtime();
    for(const auto& [id,e]:entries_) {
        (void)id;
        if(e.kind!=Kind::ragdoll && e.kind!=Kind::vehicle) continue;
        std::vector<math::Vector3> positions;std::vector<math::Quaternion> rotations;
        if(e.kind==Kind::ragdoll) {
            auto pose=take(scene_.read_ragdoll_pose(e.special));positions=std::move(pose.positions);rotations=std::move(pose.rotations);
            take(world_.apply_simulation_transform(e.root,positions.front(),rotations.front()));
        } else {
            const auto state=take(scene_.read_vehicle_state(e.special));
            for(auto visual:e.visuals) {
                const auto index=read<VehicleWheelComponent>(runtime,visual,[](const auto& v){return v.wheel_index;});
                require(index<4,"Vehicle wheel index is out of range.");positions.push_back(state.wheel_positions[index]);rotations.push_back(state.wheel_rotations[index]);
            }
        }
        const auto root=take(world::read_transform(runtime,e.root));const auto inverse=math::conjugate(root.rotation);
        for(std::size_t i=0;i<e.visuals.size();++i)
            take(world_.apply_simulation_local_transform(e.visuals[i],math::rotate(inverse,positions[i]-root.position),inverse*rotations[i]));
    }
    // All simulated transforms, including visual children, are published before
    // a query samples its spatial chain. No result depends on map iteration order.
    for(const auto& [id,e]:entries_) if(e.kind==Kind::query &&
        read<SceneQueryComponent>(runtime,id,[](const auto& v){return v.automatic || v.pending_;}))
        take(sample_query(id));
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
    const auto matrix=take(world::world_matrix(runtime,id));
    std::vector<ObjectId> ignored(ignore.actors.begin(),ignore.actors.end());
    if(ignore_self) ignored.push_back(entry.actor);
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
