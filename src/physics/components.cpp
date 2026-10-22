#include "physics/components.hpp"
#include "runtime/native.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_set>
#include <stdexcept>

namespace atelier::physics {
namespace {
// Keeps the original error, so World-level codes stay distinguishable from object failures.
struct Failure : std::runtime_error {
    Error error;
    explicit Failure(Error value) : std::runtime_error(value.message), error(std::move(value)) {}
};
template<class T> T take(Result<T> result) {
    if(!result) throw Failure(result.error());
    if constexpr(!std::is_void_v<T>) return std::move(*result);
}
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
Error failure_of(const std::exception& e) {
    if(const auto* carried=dynamic_cast<const Failure*>(&e)) return carried->error;
    if(dynamic_cast<const std::bad_alloc*>(&e)) return Error{ErrorCode::capacity_exceeded,e.what()};
    return Error{ErrorCode::invalid_member,e.what()};
}
auto failed(const std::exception& e) {return std::unexpected(failure_of(e));}
template<class T,class F> auto read(Runtime& runtime,ObjectId id,F callback) {
    using V=std::invoke_result_t<F,const T&>;
    return take(runtime.with_native<const T>(id,[&](const T& value)->Result<V>{return callback(value);}));
}
}
Result<Types> register_types(Runtime& runtime) try {
    Types types;
    take(NativeRegistration<CharacterSettings>{runtime,"physics.CharacterSettings",TypeKind::record,
        "胶囊角色的形状、坡度、跳跃和台阶设置。配置在激活时建立，修改后需停用并重新启用组件。"}
        .field<&CharacterSettings::radius>("radius","角色胶囊的半径，单位 m，必须大于 0。角色 Entity 要求单位世界缩放。")
        .field<&CharacterSettings::capsule_half_height>("capsule_half_height","胶囊中段沿 Y 轴的一半长度，不含两端半球，单位 m。")
        .field<&CharacterSettings::max_slope_radians>("max_slope_radians","可站立坡面的最大坡度角，单位 rad，范围为 0 到 π/2（不含 π/2）。")
        .field<&CharacterSettings::jump_speed>("jump_speed","在地面上消费 Jump 请求时施加的向上速度，单位 m/s，必须非负。")
        .field<&CharacterSettings::step_height>("step_height","向上跨越台阶和向下贴地时使用的垂直距离，单位 m，必须非负。")
        .publish());
    types.character_component=take(NativeRegistration<CharacterComponent>{runtime,"physics.CharacterComponent",TypeKind::object,
        "用胶囊 Character 控制 Entity 的运动、台阶和跳跃。每个固定步读取输入，不与同一 Entity 的普通物理 body 同时写入姿态。"}
        .base<world::Component>().field<&CharacterComponent::settings>("settings","角色的形状和移动限制；激活后修改这些设置需停用并重新启用。")
        .field<&CharacterComponent::velocity>("velocity","世界空间的水平目标速度，单位 m/s。Y 必须为 0；重力、跳跃和平台垂直速度由角色控制器处理。")
        .field<&CharacterComponent::jump>("jump","一次跳跃请求，在下一个固定步被消费并清除。只有角色当前站在地面上时才起跳。")
        .publish());
    types.ragdoll_component=take(NativeRegistration<RagdollComponent>{runtime,"physics.RagdollComponent",TypeKind::object,
        "由显式绑定的胶囊部件组成 Ragdoll，模拟后将姿态写回部件 Entity。所属 Entity 要求单位世界缩放。"}
        .base<world::Component>().field<&RagdollComponent::parts>("parts",{TypeReference::native<world::Entity>(),ValueForm::strong_sequence},
            std::nullopt,{},{},"按顺序绑定 Ragdoll 部件 Entity，顺序就是 Parent Index 使用的部件编号。修改绑定前必须停用控制组件。")
        .publish());
    types.ragdoll_part_component=take(NativeRegistration<RagdollPartComponent>{runtime,"physics.RagdollPartComponent",TypeKind::object,
        "Ragdoll 一个胶囊部件的作者信息，附着在要接收模拟姿态的 Entity 上。"}
        .base<world::Component>().field<&RagdollPartComponent::parent_index>("parent_index","在 Ragdoll 绑定列表中的父部件编号。-1 表示根部件，其他父编号必须位于当前部件之前。")
        .field<&RagdollPartComponent::rest_position>("rest_position","相对于 Ragdoll 控制 Entity 的初始局部位置，单位 m；不是相对于父部件，也不是当前模拟位置。")
        .field<&RagdollPartComponent::radius>("radius","此部件胶囊的半径，单位 m，必须大于 0。")
        .field<&RagdollPartComponent::capsule_half_height>("capsule_half_height","此部件胶囊中段沿 Y 轴的一半长度，不含两端半球，单位 m。")
        .publish());
    take(NativeRegistration<VehicleSettings>{runtime,"physics.VehicleSettings",TypeKind::record,
        "四轮车辆的轮位、轮径和悬挂尺寸。激活时建立车辆约束，修改后需停用并重新启用组件。"}
        .field<&VehicleSettings::half_width>("half_width","左右轮心到车身中心线的横向距离，单位 m；不会改变底盘碰撞尺寸。")
        .field<&VehicleSettings::half_length>("half_length","前后轮轴到车身中心的纵向距离，单位 m，必须大于 Wheel Radius。")
        .field<&VehicleSettings::wheel_radius>("wheel_radius","四个车轮的半径，单位 m，必须大于 0。")
        .field<&VehicleSettings::suspension_length>("suspension_length","车轮悬挂允许伸展的最大长度，单位 m，必须大于 0。")
        .publish());
    types.vehicle_component=take(NativeRegistration<VehicleComponent>{runtime,"physics.VehicleComponent",TypeKind::object,
        "给 Dynamic 底盘添加四轮车辆约束。每个固定步读取油门、转向和刹车，并更新显式绑定的车轮 Entity。"}
        .base<world::Component>().field<&VehicleComponent::wheels>("wheels",{TypeReference::native<world::Entity>(),ValueForm::strong_sequence},
            std::nullopt,{},{},"接收轮位和轮转姿态的 Entity。各 Entity 上的 VehicleWheelComponent 指定 Wheel Index；修改绑定前须停用车辆。")
        .field<&VehicleComponent::settings>("settings","车轮位置、半径和悬挂尺寸；激活后修改这些设置需停用并重新启用。")
        .field<&VehicleComponent::throttle>("throttle","油门输入，范围 -1–1。0 不加油门，正负值指定相反的驱动方向。")
        .field<&VehicleComponent::steering>("steering","转向输入，范围 -1–1。0 回到正向，正负值指定相反的转向方向。")
        .field<&VehicleComponent::brake>("brake","刹车输入，范围 0–1。0 不刹车，1 使用完整刹车输入。")
        .publish());
    types.vehicle_wheel_component=take(NativeRegistration<VehicleWheelComponent>{runtime,"physics.VehicleWheelComponent",TypeKind::object,
        "将一个车轮 Entity 对应到车辆控制器的轮位，用于接收模拟后的姿态。"}
        .base<world::Component>().field<&VehicleWheelComponent::wheel_index>("wheel_index","车辆轮位编号，范围 0–3。0/1 位于前轴，2/3 位于后轴；偶数位于局部 +X，奇数位于 -X。")
        .publish());
    types.distance_constraint_component=take(NativeRegistration<DistanceConstraintComponent>{runtime,"physics.DistanceConstraintComponent",TypeKind::object,
        "限制所属 Entity 与目标 Entity 的物理 body 锚点距离。修改目标或距离范围后需重新建立约束。"}
        .base<world::Component>().field<&DistanceConstraintComponent::target>("target",{TypeReference::native<world::Entity>(),ValueForm::weak_reference},
            std::nullopt,{},{},"约束另一端的 Entity，必须具有可用的物理 body。此弱引用不让目标保活，修改绑定前须停用约束。")
        .field<&DistanceConstraintComponent::minimum>("minimum","两端约束锚点的最小距离，单位 m，必须非负且不大于 Maximum。")
        .field<&DistanceConstraintComponent::maximum>("maximum","两端约束锚点的最大距离，单位 m。与 Minimum 相等时保持固定距离。")
        .publish());
    types.force_component=take(NativeRegistration<ForceComponent>{runtime,"physics.ForceComponent",TypeKind::object,
        "向所属 Entity 的 Dynamic body 施加持续 Force 和一次 Initial Impulse。"}
        .base<world::Component>().field<&ForceComponent::force>("force","每个固定步施加的世界空间力，单位 N。零向量不增加作用力，不会清除已有速度。")
        .field<&ForceComponent::initial_impulse>("initial_impulse","世界空间冲量，单位 N·s。每次建立物理绑定后的首次固定步只施加一次。")
        .publish());
    types.kinematic_motion_component=take(NativeRegistration<KinematicMotionComponent>{runtime,"physics.KinematicMotionComponent",TypeKind::object,
        "在激活位置上叠加匀速位移和正弦位移，逐固定步驱动 Kinematic body。"}
        .base<world::Component>().field<&KinematicMotionComponent::velocity>("velocity","沿世界 X、Y、Z 的匀速移动速度，单位 m/s。位移从激活时的位置开始累计。")
        .field<&KinematicMotionComponent::amplitude>("amplitude","沿世界 X、Y、Z 的正弦往返幅度，单位 m，叠加在 Velocity 的位移上。")
        .field<&KinematicMotionComponent::period>("period","正弦往返运动的周期，单位 s，必须大于 0。")
        .publish());
    take(NativeRegistration<TraceShape>{runtime,"physics.TraceShape",TypeKind::enumeration,
        "Line 查询线段；Sphere Sweep 沿线段移动一个球；Sphere Overlap 只检查起点处的球。"}
        .enumerator("line",TraceShape::line).enumerator("sphere_sweep",TraceShape::sphere_sweep)
        .enumerator("sphere_overlap",TraceShape::sphere_overlap).publish());
    take(NativeRegistration<TraceSelection>{runtime,"physics.TraceSelection",TypeKind::enumeration,
        "Channel 按目标 body 的响应表筛选；Object Type 按类别位掩码筛选，不读取响应表。"}
        .enumerator("channel",TraceSelection::channel).enumerator("object_type",TraceSelection::object_type).publish());
    take(NativeRegistration<TraceMode>{runtime,"physics.TraceMode",TypeKind::enumeration,
        "Test 只保留命中摘要；Single 保留一个命中；Multi 保留命中列表，数量受 Capacity 限制。"}
        .enumerator("test",TraceMode::test).enumerator("single",TraceMode::single).enumerator("multi",TraceMode::multi).publish());
    take(NativeRegistration<TraceSettings>{runtime,"physics.TraceSettings",TypeKind::record,
        "物理场景查询的形状、筛选和结果模式，每次采样读取。查询使用已登记的物理体，不查询全部显示网格。"}
        .field<&TraceSettings::shape>("shape","Line 查询 Start 到 End 的线段；Sphere Sweep 扫过一个球；Sphere Overlap 只使用 Start。")
        .field<&TraceSettings::selection>("selection","Channel 使用目标的响应表；Object Type 使用 Object Types 位掩码。两者不是同时叠加的筛选条件。")
        .field<&TraceSettings::mode>("mode","Test 只保留摘要，Single 保留一个符合规则的命中，Multi 保留命中列表。球 Overlap 没有沿线段的最近距离顺序。")
        .field<&TraceSettings::channel>("channel","Channel 查询使用的响应槽编号，范围 0–15。目标的 Ignore、Overlap、Block 决定能否命中或继续查询。")
        .field<&TraceSettings::object_types>("object_types","Object Type 查询选择的 16 位类别掩码，bit 0 对应类别 0。值为 0 时不选择任何类别。")
        .field<&TraceSettings::capacity>("capacity","最多保留的命中条目数，范围 1–4096。这是输出上限，不是查询耗时或后端内存预算。")
        .field<&TraceSettings::radius>("radius","Sphere Sweep 和 Sphere Overlap 的世界空间半径，单位 m，必须大于 0。不随 Entity 缩放变化；Line 不使用它。")
        .field<&TraceSettings::include_sensors>("include_sensors","是否允许 Sensor 进入查询结果。Sensor 只产生非阻挡命中，因此 Channel 的 Test/Single 仍不会选中它。")
        .publish());
    types.scene_query_component=take(NativeRegistration<SceneQueryComponent>{runtime,"physics.SceneQueryComponent",TypeKind::object,
        "通过物理场景执行线段、球扫掠或球重叠查询。配置随 Scene 保存，请求与结果只属于当前运行状态。"}
        .base<world::Component>().field<&SceneQueryComponent::ignored_entities>("ignored_entities",{TypeReference::native<world::Entity>(),ValueForm::strong_sequence},
            std::nullopt,{},{},"显式排除这些 Entity 的物理体，并与 Ignore Self 和调用时的排除集合一起生效。运行时修改会在下次采样使用。")
        .field<&SceneQueryComponent::settings>("settings","查询形状、筛选方式和结果上限，每次采样读取当前值。")
        .field<&SceneQueryComponent::start>("start","所属 Entity 局部空间中的查询起点，单位 m。Sphere Overlap 使用它作为球心。")
        .field<&SceneQueryComponent::end>("end","所属 Entity 局部空间中的查询终点，单位 m。Line 和 Sphere Sweep 使用它；Sphere Overlap 忽略它。")
        .field<&SceneQueryComponent::automatic>("automatic","每个实际完成的物理固定步后自动查询。关闭后由 Request Query 请求下一次采样；暂停时不会自动刷新。")
        .field<&SceneQueryComponent::ignore_self>("ignore_self","忽略所属 Entity 的物理体，不自动忽略组合父实体、子实体或兄弟实体。")
        .method<&SceneQueryComponent::request_query>("request_query",true,"请求下一个实际固定步完成后查询。重复请求合并为一次，不立即执行查询。")
        .method<&SceneQueryComponent::query_ready>("query_ready",true,"是否已有一次成功完成的查询。成功但没有命中时也为 true。")
        .method<&SceneQueryComponent::has_hit>("has_hit",true,"上次查询是否存在命中，按 Capacity 截断之前计算；Test 模式也提供此摘要。")
        .method<&SceneQueryComponent::has_blocking_hit>("has_blocking_hit",true,"上次查询是否存在 Block 命中，按 Capacity 截断之前计算。")
        .method<&SceneQueryComponent::result_overflow>("result_overflow",true,"上次查询是否因 Capacity 限制丢弃了部分命中条目。")
        .method<&SceneQueryComponent::hit_count>("hit_count",true,"上次查询保留的命中条目数。Test 模式不保留列表，因此为 0。")
        .method<&SceneQueryComponent::hit_distance>("hit_distance",true,"首个保留命中的世界距离，单位 m。空列表或 Test 模式返回 0；Overlap 不表达最近物体距离。")
        .method<&SceneQueryComponent::hit_point>("hit_point",true,"首个保留命中在目标表面的世界接触点。球 Sweep 时它不等于球心；空列表返回零向量。")
        .method<&SceneQueryComponent::hit_normal>("hit_normal",true,"首个保留命中的世界表面法线或分离方向。空列表返回零向量，射线从内部开始时法线也可能为零。")
        .publish());
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
    // Ragdoll parts and wheels follow the released controller no longer.
    if(entry.kind==Kind::ragdoll || entry.kind==Kind::vehicle) for(const auto target:entry.targets) scene_.forget_pose(target);
}
void ComponentSimulation::forget_wheel_poses(ObjectId chassis) noexcept {
    for(const auto& [id,entry]:entries_) {
        (void)id;
        if(entry.kind==Kind::vehicle && entry.entity==chassis) for(const auto target:entry.targets) scene_.forget_pose(target);
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
    std::unordered_set<ObjectId> seen,owners;std::unordered_map<ObjectId,ObjectId> writers;
    std::vector<std::pair<Entry,bool>> desired_entries;
    const auto is=[&]<class T>(ObjectId id) {return take(runtime.is_a(take(runtime.type_of(id)),take(runtime.native_type<T>())));};
    const auto membership=[&](ObjectId id) {return world_.contains(id);};
    // Only owners of controller components can produce entries; visit them in member order.
    const auto add_owners=[&]<class T>() {
        for(auto component:take(world_.query(take(runtime.native_type<T>())))) owners.insert(take(world::inspect_component(runtime,component)).owner);
    };
    add_owners.template operator()<CharacterComponent>();add_owners.template operator()<RagdollComponent>();
    add_owners.template operator()<VehicleComponent>();add_owners.template operator()<DistanceConstraintComponent>();
    add_owners.template operator()<ForceComponent>();add_owners.template operator()<KinematicMotionComponent>();
    add_owners.template operator()<SceneQueryComponent>();
    // A member's existing body or special controller claims its pose before any desired claim.
    const auto claim=[&](ObjectId entity,ObjectId writer) {
        if(!writers.contains(entity) && membership(entity))
            if(const auto current=scene_.pose_writer(entity);current!=ObjectId{}) writers.emplace(entity,current);
        auto [at,added]=writers.try_emplace(entity,writer);
        require(added || at->second==writer,"An Entity may have only one active physics pose writer.");
    };
    const auto members=owners.empty()?std::span<const world::Member>{}:take(world_.members());
    for(const auto& member:members) {
        if(!owners.contains(member.id())) continue;
        // An isolated member contributes no entries or pose claims to this pass,
        // except that a controller conflict keeps its scene queries.
        const auto claimed=writers;const auto planned=desired_entries.size();
        bool keep_queries=false;
        try {
        std::optional<Error> duplicate;
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
            // Character and Ragdoll share one pose-writer slot.
            const auto slot=kind==Kind::ragdoll?Kind::character:kind;
            if(kind!=Kind::distance && kind!=Kind::force && kind!=Kind::query && !unique.insert(slot).second) {
                duplicate=Error{ErrorCode::invalid_member,"Duplicate physics controller on one Entity."};continue;
            }
            Entry desired{kind,member.id(),item.id()};bool available=true;
            for(auto node=member.id();node!=ObjectId{};) {
                require(membership(node),"Physics spatial origin belongs to another World or is detached.");
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
                    if(!membership(target)) {
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
                if(desired.target!=ObjectId{} && !membership(desired.target)) {
                    auto status=world::inspect_entity(runtime,desired.target);
                    require(!status || status->state!=world::EntityState::active,"Constraint target belongs to another World.");
                }
                auto other=scene_.body_handle(desired.target);
                if(other && membership(desired.target)) desired.other_body=*other;else available=false;
            }
            if(available) {
                if(kind==Kind::character || kind==Kind::ragdoll) claim(member.id(),member.id());
                if(kind==Kind::ragdoll || kind==Kind::vehicle) for(auto target:desired.targets) claim(target,item.id());
            }
            desired_entries.emplace_back(std::move(desired),available);
        }
        if(duplicate) {keep_queries=true;throw Failure(std::move(*duplicate));}
        } catch(const std::exception& e) {
            if(!contain(world_,failure_of(e),member.id(),member.id())) throw;
            writers=claimed;
            desired_entries.erase(std::remove_if(desired_entries.begin()+std::ptrdiff_t(planned),desired_entries.end(),
                [&](const auto& entry){return !keep_queries || entry.first.kind!=Kind::query;}),desired_entries.end());
        }
    }
    // Validate every pose claim before constructing or replacing any controller.
    for(auto& [desired,available]:desired_entries) try {
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
    } catch(const std::exception& e) {
        // An isolated controller is not created; an unconfirmed old one is released below.
        if(!contain(world_,failure_of(e),desired.component,desired.entity)) throw;
    }
    for(auto it=entries_.begin();it!=entries_.end();) {
        if(seen.contains(it->first)) ++it;else {release(it->second);it=entries_.erase(it);}
    }
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<void> ComponentSimulation::before_step(float fixed) try {
    auto& runtime=world_.runtime();
    // An isolated controller skips this step and is sampled again next step.
    for(auto& [id,e]:entries_) try {
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
    } catch(const std::exception& failure) {
        if(!contain(world_,failure_of(failure),id,e.entity)) throw;
    }
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<void> ComponentSimulation::append_poses(std::vector<world::SimulationPose>& poses) try {
    auto& runtime=world_.runtime();
    for(const auto& [id,e]:entries_) {
        const auto appended=poses.size();
        try {
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
        } catch(const std::exception& failure) {
            if(!contain(world_,failure_of(failure),id,e.entity)) throw;
            poses.resize(appended);
        }
    }
    return {};
} catch(const std::exception& e) {return failed(e);}
Result<void> ComponentSimulation::after_step() try {
    for(auto& [id,e]:entries_) try {
        if(e.kind==Kind::character || e.kind==Kind::ragdoll) {
            // Remember the published owner frame, which need not equal a ragdoll's
            // first rest-body frame before its first completed simulation step.
            const auto pose=take(world::relative_transform(take(world::world_matrix(world_.runtime(),e.entity)),world::transform_matrix({})));
            e.origin=pose.position;e.rotation=pose.rotation;
        } else if(e.kind==Kind::query &&
            read<SceneQueryComponent>(world_.runtime(),id,[](const auto& v){return v.automatic || v.pending_;})) take(sample_query(id));
    } catch(const std::exception& failure) {
        if(!contain(world_,failure_of(failure),id,e.entity)) throw;
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
