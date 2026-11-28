#include "detail/components.hpp"
#include "foundation/set.hpp"
#include "runtime/native.hpp"
#include <algorithm>
#include <cmath>

namespace atelier::physics {
namespace {
template <class T, class F>
auto read(Runtime& runtime, ObjectId id, F callback) {
    using V = std::invoke_result_t<F, const T&>;
    return runtime.with_native<const T>(id, [&](const T& value) -> Result<V> { return callback(value); });
}
}

Result<Types> register_types(Runtime& runtime) {
    Types types;
    auto character_settings =
        NativeRegistration<CharacterSettings>{
            runtime, "physics.CharacterSettings", TypeKind::record,
            "胶囊角色的形状、坡度、跳跃和台阶设置。配置在激活时建立，修改后需停用并重新启用组件。"}
            .field<&CharacterSettings::radius>("radius",
                                               "角色胶囊的半径，单位 m，必须大于 0。角色 Entity 要求单位世界缩放。")
            .field<&CharacterSettings::capsule_half_height>("capsule_half_height",
                                                            "胶囊中段沿 Y 轴的一半长度，不含两端半球，单位 m。")
            .field<&CharacterSettings::max_slope_radians>(
                "max_slope_radians", "可站立坡面的最大坡度角，单位 rad，范围为 0 到 π/2（不含 π/2）。")
            .field<&CharacterSettings::jump_speed>("jump_speed",
                                                   "在地面上消费 Jump 请求时施加的向上速度，单位 m/s，必须非负。")
            .field<&CharacterSettings::step_height>("step_height",
                                                    "向上跨越台阶和向下贴地时使用的垂直距离，单位 m，必须非负。")
            .publish();
    ATELIER_TRY(character_settings);
    auto character =
        NativeRegistration<CharacterComponent>{
            runtime, "physics.CharacterComponent", TypeKind::object,
            "用胶囊 Character 控制 Entity 的运动、台阶和跳跃。每个固定步读取输入，不与同一 Entity 的普通物理 body 同时写入姿态。"}
            .base<world::Component>()
            .field<&CharacterComponent::settings>("settings",
                                                  "角色的形状和移动限制；激活后修改这些设置需停用并重新启用。")
            .field<&CharacterComponent::velocity>(
                "velocity", "世界空间的水平目标速度，单位 m/s。Y 必须为 0；重力、跳跃和平台垂直速度由角色控制器处理。")
            .field<&CharacterComponent::jump>(
                "jump", "一次跳跃请求，在下一个固定步被消费并清除。只有角色当前站在地面上时才起跳。")
            .publish();
    ATELIER_TRY(character);
    types.character_component = *character;
    auto ragdoll =
        NativeRegistration<RagdollComponent>{
            runtime, "physics.RagdollComponent", TypeKind::object,
            "由显式绑定的胶囊部件组成 Ragdoll，模拟后将姿态写回部件 Entity。所属 Entity 要求单位世界缩放。"}
            .base<world::Component>()
            .field<&RagdollComponent::parts>(
                "parts", {TypeReference::native<world::Entity>(), ValueForm::strong_sequence}, {}, {}, {},
                "按顺序绑定 Ragdoll 部件 Entity，顺序就是 Parent Index 使用的部件编号。修改绑定前必须停用控制组件。")
            .publish();
    ATELIER_TRY(ragdoll);
    types.ragdoll_component = *ragdoll;
    auto ragdoll_part =
        NativeRegistration<RagdollPartComponent>{runtime, "physics.RagdollPartComponent", TypeKind::object,
                                                 "Ragdoll 一个胶囊部件的作者信息，附着在要接收模拟姿态的 Entity 上。"}
            .base<world::Component>()
            .field<&RagdollPartComponent::parent_index>(
                "parent_index", "在 Ragdoll 绑定列表中的父部件编号。-1 表示根部件，其他父编号必须位于当前部件之前。")
            .field<&RagdollPartComponent::rest_position>(
                "rest_position",
                "相对于 Ragdoll 控制 Entity 的初始局部位置，单位 m；不是相对于父部件，也不是当前模拟位置。")
            .field<&RagdollPartComponent::radius>("radius", "此部件胶囊的半径，单位 m，必须大于 0。")
            .field<&RagdollPartComponent::capsule_half_height>(
                "capsule_half_height", "此部件胶囊中段沿 Y 轴的一半长度，不含两端半球，单位 m。")
            .publish();
    ATELIER_TRY(ragdoll_part);
    types.ragdoll_part_component = *ragdoll_part;
    auto distance =
        NativeRegistration<DistanceConstraintComponent>{
            runtime, "physics.DistanceConstraintComponent", TypeKind::object,
            "限制所属 Entity 与目标 Entity 的物理 body 锚点距离。修改目标或距离范围后需重新建立约束。"}
            .base<world::Component>()
            .field<&DistanceConstraintComponent::target>(
                "target", {TypeReference::native<world::Entity>(), ValueForm::weak_reference}, {}, {}, {},
                "约束另一端的 Entity，必须具有可用的物理 body。此弱引用不让目标保活，修改绑定前须停用约束。")
            .field<&DistanceConstraintComponent::minimum>("minimum",
                                                          "两端约束锚点的最小距离，单位 m，必须非负且不大于 Maximum。")
            .field<&DistanceConstraintComponent::maximum>(
                "maximum", "两端约束锚点的最大距离，单位 m。与 Minimum 相等时保持固定距离。")
            .publish();
    ATELIER_TRY(distance);
    types.distance_constraint_component = *distance;
    auto force =
        NativeRegistration<ForceComponent>{runtime, "physics.ForceComponent", TypeKind::object,
                                           "向所属 Entity 的 Dynamic body 施加持续 Force 和一次 Initial Impulse。"}
            .base<world::Component>()
            .field<&ForceComponent::force>("force",
                                           "每个固定步施加的世界空间力，单位 N。零向量不增加作用力，不会清除已有速度。")
            .field<&ForceComponent::initial_impulse>(
                "initial_impulse", "世界空间冲量，单位 N·s。每次建立物理绑定后的首次固定步只施加一次。")
            .publish();
    ATELIER_TRY(force);
    types.force_component = *force;
    auto kinematic =
        NativeRegistration<KinematicMotionComponent>{
            runtime, "physics.KinematicMotionComponent", TypeKind::object,
            "在激活位置上叠加匀速位移和正弦位移，逐固定步驱动 Kinematic body。"}
            .base<world::Component>()
            .field<&KinematicMotionComponent::velocity>(
                "velocity", "沿世界 X、Y、Z 的匀速移动速度，单位 m/s。位移从激活时的位置开始累计。")
            .field<&KinematicMotionComponent::amplitude>(
                "amplitude", "沿世界 X、Y、Z 的正弦往返幅度，单位 m，叠加在 Velocity 的位移上。")
            .field<&KinematicMotionComponent::period>("period", "正弦往返运动的周期，单位 s，必须大于 0。")
            .publish();
    ATELIER_TRY(kinematic);
    types.kinematic_motion_component = *kinematic;
    auto trace_shape =
        NativeRegistration<TraceShape>{
            runtime, "physics.TraceShape", TypeKind::enumeration,
            "Line 查询线段；Sphere Sweep 沿线段移动一个球；Sphere Overlap 只检查起点处的球。"}
            .enumerator("line", TraceShape::line)
            .enumerator("sphere_sweep", TraceShape::sphere_sweep)
            .enumerator("sphere_overlap", TraceShape::sphere_overlap)
            .publish();
    ATELIER_TRY(trace_shape);
    auto trace_selection =
        NativeRegistration<TraceSelection>{
            runtime, "physics.TraceSelection", TypeKind::enumeration,
            "Channel 按目标 body 的响应表筛选；Object Type 按类别位掩码筛选，不读取响应表。"}
            .enumerator("channel", TraceSelection::channel)
            .enumerator("object_type", TraceSelection::object_type)
            .publish();
    ATELIER_TRY(trace_selection);
    auto trace_mode =
        NativeRegistration<TraceMode>{
            runtime, "physics.TraceMode", TypeKind::enumeration,
            "Test 只保留命中摘要；Single 保留一个命中；Multi 保留命中列表，数量受 Capacity 限制。"}
            .enumerator("test", TraceMode::test)
            .enumerator("single", TraceMode::single)
            .enumerator("multi", TraceMode::multi)
            .publish();
    ATELIER_TRY(trace_mode);
    auto trace_settings =
        NativeRegistration<TraceSettings>{
            runtime, "physics.TraceSettings", TypeKind::record,
            "物理场景查询的形状、筛选和结果模式，每次采样读取。查询使用已登记的物理体，不查询全部显示网格。"}
            .field<&TraceSettings::shape>(
                "shape", "Line 查询 Start 到 End 的线段；Sphere Sweep 扫过一个球；Sphere Overlap 只使用 Start。")
            .field<&TraceSettings::selection>(
                "selection",
                "Channel 使用目标的响应表；Object Type 使用 Object Types 位掩码。两者不是同时叠加的筛选条件。")
            .field<&TraceSettings::mode>(
                "mode",
                "Test 只保留摘要，Single 保留一个符合规则的命中，Multi 保留命中列表。球 Overlap 没有沿线段的最近距离顺序。")
            .field<&TraceSettings::channel>(
                "channel",
                "Channel 查询使用的响应槽编号，范围 0–15。目标的 Ignore、Overlap、Block 决定能否命中或继续查询。")
            .field<&TraceSettings::object_types>(
                "object_types", "Object Type 查询选择的 16 位类别掩码，bit 0 对应类别 0。值为 0 时不选择任何类别。")
            .field<&TraceSettings::capacity>(
                "capacity", "最多保留的命中条目数，范围 1–4096。这是输出上限，不是查询耗时或后端内存预算。")
            .field<&TraceSettings::radius>(
                "radius",
                "Sphere Sweep 和 Sphere Overlap 的世界空间半径，单位 m，必须大于 0。不随 Entity 缩放变化；Line 不使用它。")
            .field<&TraceSettings::include_sensors>(
                "include_sensors",
                "是否允许 Sensor 进入查询结果。Sensor 只产生非阻挡命中，因此 Channel 的 Test/Single 仍不会选中它。")
            .publish();
    ATELIER_TRY(trace_settings);
    auto query =
        NativeRegistration<SceneQueryComponent>{
            runtime, "physics.SceneQueryComponent", TypeKind::object,
            "通过物理场景执行线段、球扫掠或球重叠查询。配置随 Scene 保存，请求与结果只属于当前运行状态。"}
            .base<world::Component>()
            .field<&SceneQueryComponent::ignored_entities>(
                "ignored_entities", {TypeReference::native<world::Entity>(), ValueForm::strong_sequence}, {},
                {}, {},
                "显式排除这些 Entity 的物理体，并与 Ignore Self 和调用时的排除集合一起生效。运行时修改会在下次采样使用。")
            .field<&SceneQueryComponent::settings>("settings", "查询形状、筛选方式和结果上限，每次采样读取当前值。")
            .field<&SceneQueryComponent::start>(
                "start", "所属 Entity 局部空间中的查询起点，单位 m。Sphere Overlap 使用它作为球心。")
            .field<&SceneQueryComponent::end>(
                "end", "所属 Entity 局部空间中的查询终点，单位 m。Line 和 Sphere Sweep 使用它；Sphere Overlap 忽略它。")
            .field<&SceneQueryComponent::automatic>(
                "automatic",
                "每个实际完成的物理固定步后自动查询。关闭后由 Request Query 请求下一次采样；暂停时不会自动刷新。")
            .field<&SceneQueryComponent::ignore_self>(
                "ignore_self", "忽略所属 Entity 的物理体，不自动忽略组合父实体、子实体或兄弟实体。")
            .method<&SceneQueryComponent::request_query>(
                "request_query", true, "请求下一个实际固定步完成后查询。重复请求合并为一次，不立即执行查询。")
            .method<&SceneQueryComponent::query_ready>("query_ready", true,
                                                       "是否已有一次成功完成的查询。成功但没有命中时也为 true。")
            .method<&SceneQueryComponent::has_hit>(
                "has_hit", true, "上次查询是否存在命中，按 Capacity 截断之前计算；Test 模式也提供此摘要。")
            .method<&SceneQueryComponent::has_blocking_hit>("has_blocking_hit", true,
                                                            "上次查询是否存在 Block 命中，按 Capacity 截断之前计算。")
            .method<&SceneQueryComponent::result_overflow>("result_overflow", true,
                                                           "上次查询是否因 Capacity 限制丢弃了部分命中条目。")
            .method<&SceneQueryComponent::hit_count>("hit_count", true,
                                                     "上次查询保留的命中条目数。Test 模式不保留列表，因此为 0。")
            .method<&SceneQueryComponent::hit_distance>(
                "hit_distance", true,
                "首个保留命中的世界距离，单位 m。空列表或 Test 模式返回 0；Overlap 不表达最近物体距离。")
            .method<&SceneQueryComponent::hit_point>(
                "hit_point", true, "首个保留命中在目标表面的世界接触点。球 Sweep 时它不等于球心；空列表返回零向量。")
            .method<&SceneQueryComponent::hit_normal>(
                "hit_normal", true,
                "首个保留命中的世界表面法线或分离方向。空列表返回零向量，射线从内部开始时法线也可能为零。")
            .publish();
    ATELIER_TRY(query);
    types.scene_query_component = *query;
    return types;
}

Result<void> set_constraint_target(Runtime& runtime, ObjectId component, ObjectId entity) {
    ATELIER_TRY_BIND(status, world::inspect_component(runtime, component));
    if (status->active)
        return std::unexpected(
            Error{ErrorCode::invalid_member, "Stop the constraint component before rebinding its target."});
    ATELIER_TRY_BIND(type, runtime.native_type<world::Entity>());
    ATELIER_TRY_BIND(reference, runtime.null_reference(*type, ValueForm::weak_reference));
    if (entity != ObjectId{}) {
        ATELIER_TRY(world::inspect_entity(runtime, entity));
        ATELIER_TRY_BIND(held, runtime.hold(entity));
        reference = runtime.observe(held->reference());
        ATELIER_TRY(reference);
    }
    return runtime.with_native<DistanceConstraintComponent>(component, [&](auto& value) -> Result<void> {
        ATELIER_TRY_BIND(field, runtime.reference_field(component, value.target));
        const std::array<ReferenceChange, 1> changes{ReferenceWrite{*field, *reference}};
        return runtime.apply_reference_changes(changes);
    });
}

Result<ObjectId> constraint_target(Runtime& runtime, ObjectId component) {
    return runtime.with_native<const DistanceConstraintComponent>(
        component, [](const auto& value) -> Result<ObjectId> { return value.target.id(); });
}

namespace {
template <class T, ReferenceSequence T::* Member>
Result<Array<ObjectId>> bindings(Runtime& runtime, ObjectId component, std::size_t additional_capacity = 0) {
    return runtime.with_native<const T>(component, [&](const T& value) -> Result<Array<ObjectId>> {
        ATELIER_TRY_BIND(field, runtime.reference_field(component, value.*Member));
        ATELIER_TRY_BIND(count, runtime.sequence_size(*field));
        Array<ObjectId> result;
        result.reserve(*count + additional_capacity);
        for (std::size_t i = 0; i < *count; ++i) {
            ATELIER_TRY_BIND(reference, runtime.sequence_read(*field, i));
            result.add(reference->id());
        }
        return result;
    });
}

template <class T, ReferenceSequence T::* Member>
Result<void> set_bindings(Runtime& runtime, ObjectId component, Span<const ObjectId> entities,
                          bool requires_inactive = true) {
    ATELIER_TRY_BIND(status, world::inspect_component(runtime, component));
    if (requires_inactive && status->active)
        return std::unexpected(Error{ErrorCode::invalid_member, "Stop the controller before rebinding its Entities."});
    Set<ObjectId> seen;
    seen.reserve(entities.size());
    Array<ObjectRoot> roots;
    Array<ObjectReference> references;
    roots.reserve(entities.size());
    references.reserve(entities.size());
    for (auto entity : entities) {
        if (entity == ObjectId{} || !seen.add(entity))
            return std::unexpected(
                Error{ErrorCode::invalid_member, "Physics bindings require distinct non-null Entities."});
        ATELIER_TRY(world::inspect_entity(runtime, entity));
        ATELIER_TRY_BIND(held, runtime.hold(entity));
        roots.add(std::move(*held));
        references.add(roots.back().reference());
    }
    return runtime.with_native<T>(component, [&](T& value) -> Result<void> {
        ATELIER_TRY_BIND(field, runtime.reference_field(component, value.*Member));
        ATELIER_TRY_BIND(count, runtime.sequence_size(*field));
        const std::array<ReferenceChange, 1> changes{SequenceSplice{*field, 0, *count, references}};
        return runtime.apply_reference_changes(changes);
    });
}
}

Result<void> set_ragdoll_parts(Runtime& runtime, ObjectId component, Span<const ObjectId> entities) {
    return set_bindings<RagdollComponent, &RagdollComponent::parts>(runtime, component, entities);
}

Result<Array<ObjectId>> ragdoll_parts(Runtime& runtime, ObjectId component) {
    return bindings<RagdollComponent, &RagdollComponent::parts>(runtime, component);
}

Result<void> set_query_ignored_entities(Runtime& runtime, ObjectId component, Span<const ObjectId> entities) {
    return set_bindings<SceneQueryComponent, &SceneQueryComponent::ignored_entities>(runtime, component, entities,
                                                                                     false);
}

Result<Array<ObjectId>> query_ignored_entities(Runtime& runtime, ObjectId component) {
    return bindings<SceneQueryComponent, &SceneQueryComponent::ignored_entities>(runtime, component);
}

}

namespace atelier::physics::detail {
ComponentSimulation::~ComponentSimulation() {
    clear();
}

void ComponentSimulation::release(const Entry& entry) noexcept {
    switch (entry.kind) {
    case Kind::character:
        (void)scene_.destroy_character(entry.special);
        break;
    case Kind::ragdoll:
        (void)scene_.destroy_ragdoll(entry.special);
        break;
    case Kind::distance:
        (void)scene_.destroy_constraint(entry.constraint);
        break;
    case Kind::query:
        clear_query(entry.component, true);
        break;
    default:
        break;
    }
    // Ragdoll parts follow the released controller no longer.
    if (entry.kind == Kind::ragdoll)
        for (const auto target : entry.targets)
            scene_.forget_pose(target);
}

void ComponentSimulation::clear() noexcept {
    for (const auto [id, entry] : entries_) {
        (void)id;
        release(entry);
    }
    entries_.clear();
    order_.invalidate();
}

ObjectId ComponentSimulation::bound_pose_writer(ObjectId object) const {
    // This query also runs through PhysicsScene's noexcept pose-writer lookup.
    // Select the earliest live claim without allocating a traversal snapshot.
    ObjectId writer;
    std::uint64_t revision = UINT64_MAX;
    for (const auto [component, entry] : entries_) {
        if (entry.kind != Kind::ragdoll)
            continue;
        if (std::ranges::find(entry.targets, object) == entry.targets.end())
            continue;
        const auto handle = scene_.ragdoll_handle(entry.entity);
        if (handle && *handle == entry.special && entry.revision < revision) {
            writer = component;
            revision = entry.revision;
        }
    }
    return writer;
}

Result<void> ComponentSimulation::removed(ObjectId object, bool detached) {
    clear_query(object, true);
    for (const auto id : order_.get(entries_)) {
        const auto& e = entries_.at(id);
        if (e.entity == object || e.component == object || e.target == object || e.body.component == object ||
            e.other_body.component == object || std::ranges::find(e.targets, object) != e.targets.end() ||
            std::ranges::find(e.metadata, object) != e.metadata.end() ||
            (detached && std::ranges::find(e.ancestors, object) != e.ancestors.end())) {
            release(e);
            entries_.remove(id);
            order_.invalidate();
        }
    }
    auto& runtime = world_.runtime();
    ATELIER_TRY_BIND(type, runtime.native_type<SceneQueryComponent>());
    auto queries = world_.query(*type);
    if (!queries && queries.error().code == ErrorCode::busy)
        return {};
    ATELIER_TRY(queries);
    for (auto query : *queries) {
        if (entries_.contains(query))
            continue;
        ATELIER_TRY_BIND(status, world::inspect_component(runtime, query));
        if (query == object || status->owner == object) {
            clear_query(query, true);
            continue;
        }
        for (auto node = status->owner; node != ObjectId{};) {
            if (node == object) {
                clear_query(query, true);
                break;
            }
            auto parent = world::parent_entity(runtime, node);
            if (!parent) {
                if (parent.error().code == ErrorCode::busy)
                    break;
                return std::unexpected(Error{ErrorCode::invalid_member, parent.error().message});
            }
            node = *parent;
        }
    }
    return {};
}

Result<void> ComponentSimulation::reconcile() {
    contained_ = false;
    auto& runtime = world_.runtime();
    Set<ObjectId> seen;
    Set<ObjectId> owners;
    Map<ObjectId, ObjectId> writers;
    Array<std::pair<Entry, bool>> desired_entries;
    const auto is = [&]<class T>(ObjectId id) -> Result<bool> {
        ATELIER_TRY_BIND(type, runtime.type_of(id));
        ATELIER_TRY_BIND(expected, runtime.native_type<T>());
        return runtime.is_a(*type, *expected);
    };
    const auto membership = [&](ObjectId id) { return world_.contains(id); };
    // Only owners of controller components can produce entries; visit them in member order.
    const auto add_owners = [&]<class T>() -> Result<void> {
        ATELIER_TRY_BIND(type, runtime.native_type<T>());
        ATELIER_TRY_BIND(components, world_.query(*type));
        for (auto component : *components) {
            ATELIER_TRY_BIND(status, world::inspect_component(runtime, component));
            owners.add(status->owner);
        }
        return {};
    };
    ATELIER_TRY(add_owners.template operator()<CharacterComponent>());
    ATELIER_TRY(add_owners.template operator()<RagdollComponent>());
    ATELIER_TRY(add_owners.template operator()<DistanceConstraintComponent>());
    ATELIER_TRY(add_owners.template operator()<ForceComponent>());
    ATELIER_TRY(add_owners.template operator()<KinematicMotionComponent>());
    ATELIER_TRY(add_owners.template operator()<SceneQueryComponent>());
    // A member's existing body or special controller claims its pose before any desired claim.
    const auto claim = [&](ObjectId entity, ObjectId writer) -> Result<void> {
        auto [at, added] = writers.try_emplace(entity, writer);
        if (added && membership(entity))
            if (const auto current = scene_.pose_writer(entity); current != ObjectId{}) {
                *at = current;
                added = false;
            }
        if (!added && *at != writer)
            return std::unexpected(
                Error{ErrorCode::invalid_member, "An Entity may have only one active physics pose writer."});
        return {};
    };
    ATELIER_TRY_BIND(members, owners.empty() ? Result<Span<const world::Member>>{} : world_.members());
    for (const auto& member : *members) {
        if (!owners.contains(member.id()))
            continue;
        // An isolated member contributes no entries or pose claims to this pass,
        // except that a controller conflict keeps its scene queries.
        const auto claimed = writers;
        const auto planned = desired_entries.size();
        bool keep_queries = false;
        auto planned_member = [&]() -> Result<void> {
            Optional<Error> duplicate;
            ATELIER_TRY_BIND(status, world::inspect_entity(runtime, member.id()));
            if (status->state != world::EntityState::active)
                return {};
            ATELIER_TRY_BIND(spatial, world::has_transform(runtime, member.id()));
            if (!*spatial)
                return {};
            ATELIER_TRY_BIND(group, world::components(runtime, member.id()));
            Set<Kind> unique;
            for (const auto& item : *group) {
                ATELIER_TRY_BIND(component, world::inspect_component(runtime, item.id()));
                if (!component->active)
                    continue;
                auto classification = [&]() -> Result<Optional<Kind>> {
                    ATELIER_TRY_BIND(character, is.template operator()<CharacterComponent>(item.id()));
                    if (*character)
                        return Kind::character;
                    ATELIER_TRY_BIND(ragdoll, is.template operator()<RagdollComponent>(item.id()));
                    if (*ragdoll)
                        return Kind::ragdoll;
                    ATELIER_TRY_BIND(distance, is.template operator()<DistanceConstraintComponent>(item.id()));
                    if (*distance)
                        return Kind::distance;
                    ATELIER_TRY_BIND(force, is.template operator()<ForceComponent>(item.id()));
                    if (*force)
                        return Kind::force;
                    ATELIER_TRY_BIND(kinematic, is.template operator()<KinematicMotionComponent>(item.id()));
                    if (*kinematic)
                        return Kind::kinematic;
                    ATELIER_TRY_BIND(query, is.template operator()<SceneQueryComponent>(item.id()));
                    if (*query)
                        return Kind::query;
                    return {};
                }();
                ATELIER_TRY(classification);
                if (!*classification)
                    continue;
                const auto kind = **classification;
                // Character and Ragdoll share one pose-writer slot.
                const auto slot = kind == Kind::ragdoll ? Kind::character : kind;
                if (kind != Kind::distance && kind != Kind::force && kind != Kind::query && !unique.add(slot)) {
                    duplicate = Error{ErrorCode::invalid_member, "Duplicate physics controller on one Entity."};
                    continue;
                }
                Entry desired{kind, member.id(), item.id()};
                bool available = true;
                for (auto node = member.id(); node != ObjectId{};) {
                    if (!membership(node))
                        return std::unexpected(
                            Error{ErrorCode::invalid_member,
                                  "Physics spatial origin belongs to another World or is detached."});
                    desired.ancestors.add(node);
                    ATELIER_TRY_BIND(parent, world::spatial_parent(runtime, node));
                    node = *parent;
                }
                if (kind == Kind::ragdoll) {
                    ATELIER_TRY_BIND(parts, ragdoll_parts(runtime, item.id()));
                    desired.targets = std::move(*parts);
                    if (desired.targets.empty())
                        return std::unexpected(
                            Error{ErrorCode::invalid_member, "Ragdoll requires explicit part Entity bindings."});
                    Set<ObjectId> targets;
                    targets.reserve(desired.targets.size());
                    for (auto target : desired.targets) {
                        if (target == ObjectId{} || !targets.add(target))
                            return std::unexpected(Error{ErrorCode::invalid_member,
                                                         "Physics bindings require distinct non-null Entities."});
                        ATELIER_TRY_BIND(target_status, world::inspect_entity(runtime, target));
                        if (!membership(target)) {
                            if (target_status->state == world::EntityState::active)
                                return std::unexpected(Error{ErrorCode::invalid_member,
                                                             "Physics binding Entity belongs to another World."});
                            available = false;
                            continue;
                        }
                        ATELIER_TRY_BIND(parent, world::parent_entity(runtime, target));
                        bool direct = *parent == member.id();
                        if (direct) {
                            ATELIER_TRY_BIND(origin, world::spatial_parent(runtime, target));
                            direct = *origin == member.id();
                        }
                        if (!direct)
                            return std::unexpected(Error{ErrorCode::invalid_member,
                                                         "Physics parts must bind direct spatial Entity children."});
                        ATELIER_TRY_BIND(has_transform, world::has_transform(runtime, target));
                        if (!*has_transform)
                            return std::unexpected(
                                Error{ErrorCode::invalid_member, "Physics binding Entity must have a transform."});
                        ATELIER_TRY_BIND(active, world::inspect_entity(runtime, target));
                        available &= active->state == world::EntityState::active;
                        ObjectId metadata;
                        ATELIER_TRY_BIND(children, world::components(runtime, target));
                        for (const auto& child : *children) {
                            ATELIER_TRY_BIND(part, is.template operator()<RagdollPartComponent>(child.id()));
                            if (!*part)
                                continue;
                            if (metadata != ObjectId{})
                                return std::unexpected(Error{ErrorCode::invalid_member,
                                                             "Physics binding Entity has duplicate part metadata."});
                            metadata = child.id();
                            ATELIER_TRY_BIND(part_status, world::inspect_component(runtime, metadata));
                            available &= part_status->active;
                        }
                        if (metadata == ObjectId{})
                            return std::unexpected(
                                Error{ErrorCode::invalid_member, "Physics binding Entity is missing part metadata."});
                        desired.metadata.add(metadata);
                    }
                }
                if (kind == Kind::distance || kind == Kind::force || kind == Kind::kinematic) {
                    auto handle = scene_.body_handle(member.id());
                    if (handle)
                        desired.body = *handle;
                    else
                        available = false;
                }
                if (kind == Kind::distance) {
                    ATELIER_TRY_BIND(target, constraint_target(runtime, item.id()));
                    desired.target = *target;
                    if (desired.target != ObjectId{} && !membership(desired.target)) {
                        auto status = world::inspect_entity(runtime, desired.target);
                        if (status && status->state == world::EntityState::active)
                            return std::unexpected(
                                Error{ErrorCode::invalid_member, "Constraint target belongs to another World."});
                    }
                    auto other = scene_.body_handle(desired.target);
                    if (other && membership(desired.target))
                        desired.other_body = *other;
                    else
                        available = false;
                }
                if (available) {
                    if (kind == Kind::character || kind == Kind::ragdoll)
                        ATELIER_TRY(claim(member.id(), member.id()));
                    if (kind == Kind::ragdoll)
                        for (auto target : desired.targets)
                            ATELIER_TRY(claim(target, item.id()));
                }
                desired_entries.emplace(std::move(desired), available);
            }
            if (duplicate) {
                keep_queries = true;
                return std::unexpected(std::move(*duplicate));
            }
            return {};
        }();
        if (!planned_member) {
            if (!contain(world_, planned_member.error(), member.id(), member.id()))
                return std::unexpected(std::move(planned_member).error());
            contained_ = true;
            writers = claimed;
            const auto kept = std::remove_if(
                desired_entries.begin() + std::ptrdiff_t(planned), desired_entries.end(),
                [&](const auto& entry) { return !keep_queries || entry.first.kind != Kind::query; });
            desired_entries.truncate(static_cast<std::size_t>(kept - desired_entries.begin()));
        }
    }
    // Validate every pose claim before constructing or replacing any controller.
    for (auto& [desired, available] : desired_entries) {
        auto reconciled = [&]() -> Result<void> {
            const auto kind = desired.kind;
            if (auto* current = entries_.find(desired.component)) {
                auto& e = *current;
                bool valid = available && e.entity == desired.entity && e.target == desired.target &&
                             e.body == desired.body && e.other_body == desired.other_body &&
                             e.targets == desired.targets && e.metadata == desired.metadata;
                if (kind == Kind::character)
                    valid &= scene_.character_handle(e.entity) == e.special;
                if (kind == Kind::ragdoll)
                    valid &= scene_.ragdoll_handle(e.entity) == e.special;
                if (valid && (kind == Kind::character || kind == Kind::ragdoll)) {
                    ATELIER_TRY_BIND(matches, scene_.controller_transform_matches(e.entity, e.origin, e.rotation));
                    valid = *matches;
                }
                if (valid) {
                    e.ancestors = std::move(desired.ancestors);
                    seen.add(desired.component);
                    return {};
                }
                release(e);
                entries_.remove(desired.component);
                order_.invalidate();
            }
            if (!available)
                return {};
            if (kind != Kind::query) {
                ATELIER_TRY_BIND(matrix, world::world_matrix(runtime, desired.entity));
                ATELIER_TRY_BIND(origin, world::relative_transform(*matrix, world::transform_matrix({})));
                desired.origin = origin->position;
                desired.rotation = origin->rotation;
            }
            switch (kind) {
            case Kind::character: {
                auto settings =
                    read<CharacterComponent>(runtime, desired.component, [](const auto& v) { return v.settings; });
                ATELIER_TRY(settings);
                ATELIER_TRY_BIND(created, scene_.create_character(desired.entity, *settings));
                desired.special = *created;
                break;
            }
            case Kind::ragdoll: {
                Array<RagdollPart> parts;
                for (auto metadata : desired.metadata) {
                    auto part = read<RagdollPartComponent>(runtime, metadata, [&](const auto& v) {
                        return RagdollPart{String::from_number(parts.size()), v.parent_index, v.rest_position, v.radius,
                                           v.capsule_half_height};
                    });
                    ATELIER_TRY(part);
                    parts.add(std::move(*part));
                }
                ATELIER_TRY_BIND(created, scene_.create_ragdoll(desired.entity, parts));
                desired.special = *created;
                break;
            }
            case Kind::distance: {
                auto limits = read<DistanceConstraintComponent>(
                    runtime, desired.component, [](const auto& v) { return std::pair{v.minimum, v.maximum}; });
                ATELIER_TRY(limits);
                ATELIER_TRY_BIND(created, scene_.create_distance_constraint(desired.body, desired.other_body,
                                                                            limits->first, limits->second));
                desired.constraint = *created;
                break;
            }
            case Kind::force:
            case Kind::kinematic:
            case Kind::query:
                break;
            }
            desired.revision = next_revision_++;
            entries_.try_emplace(desired.component, desired);
            order_.invalidate();
            seen.add(desired.component);
            return {};
        }();
        if (!reconciled) {
            // An isolated controller is not created; an unconfirmed old one is released below.
            if (!contain(world_, reconciled.error(), desired.component, desired.entity))
                return std::unexpected(std::move(reconciled).error());
            contained_ = true;
        }
    }
    for (const auto id : order_.get(entries_)) {
        if (!seen.contains(id)) {
            const auto removed = entries_.take(id);
            release(*removed);
            order_.invalidate();
        }
    }
    return {};
}

Result<void> ComponentSimulation::before_step(float fixed) {
    auto& runtime = world_.runtime();
    // An isolated controller skips this step and is sampled again next step.
    for (const auto id : order_.get(entries_)) {
        auto& e = entries_.at(id);
        auto applied = [&]() -> Result<void> {
            switch (e.kind) {
            case Kind::character: {
                const auto input =
                    read<CharacterComponent>(runtime, id, [](const auto& v) { return std::pair{v.velocity, v.jump}; });
                ATELIER_TRY(input);
                ATELIER_TRY(scene_.set_character_input(e.special, input->first, input->second));
                // A jump request is consumed; an idle one is left unwritten.
                if (input->second)
                    return runtime.with_native<CharacterComponent>(id, [](auto& v) -> Result<void> {
                        v.jump = false;
                        return {};
                    });
                break;
            }
            case Kind::force: {
                const auto input = read<ForceComponent>(
                    runtime, id, [](const auto& v) { return std::pair{v.force, v.initial_impulse}; });
                ATELIER_TRY(input);
                if (!math::is_finite(input->first) || !math::is_finite(input->second))
                    return std::unexpected(Error{ErrorCode::invalid_member, "Force component inputs must be finite."});
                if (e.impulse_pending) {
                    ATELIER_TRY(scene_.add_impulse(e.body, input->second));
                    e.impulse_pending = false;
                }
                ATELIER_TRY(scene_.add_force(e.body, input->first));
                break;
            }
            case Kind::kinematic: {
                const auto input = read<KinematicMotionComponent>(
                    runtime, id, [](const auto& v) { return std::tuple{v.velocity, v.amplitude, v.period}; });
                ATELIER_TRY(input);
                const auto& [velocity, amplitude, period] = *input;
                if (!math::is_finite(velocity) || !math::is_finite(amplitude) || !std::isfinite(period) || period <= 0)
                    return std::unexpected(
                        Error{ErrorCode::invalid_member,
                              "Kinematic motion requires finite velocity/amplitude and a positive period."});
                const auto next = e.elapsed + fixed;
                const auto where =
                    e.origin + velocity * float(next) + amplitude * float(std::sin(next * 6.283185307179586 / period));
                ATELIER_TRY(scene_.set_kinematic_target(e.body, where, e.rotation));
                e.elapsed = next;
                break;
            }
            default:
                break;
            }
            return {};
        }();
        if (!applied && !contain(world_, applied.error(), id, e.entity))
            return applied;
    }
    return {};
}

Result<void> ComponentSimulation::append_poses(Array<world::SimulationPose>& poses) {
    for (const auto id : order_.get(entries_)) {
        const auto& e = entries_.at(id);
        const auto appended = poses.size();
        auto appended_pose = [&]() -> Result<void> {
            if (e.kind == Kind::ragdoll) {
                ATELIER_TRY_BIND(pose, scene_.read_ragdoll_pose(e.special));
                if (pose->positions.size() != e.targets.size() || pose->rotations.size() != e.targets.size())
                    return std::unexpected(
                        Error{ErrorCode::invalid_member, "Ragdoll pose does not match its Entity bindings."});
                for (std::size_t i = 0; i < e.targets.size(); ++i)
                    poses.add({e.targets[i], pose->positions[i], pose->rotations[i]});
            }
            return {};
        }();
        if (!appended_pose) {
            if (!contain(world_, appended_pose.error(), id, e.entity))
                return appended_pose;
            poses.truncate(appended);
        }
    }
    return {};
}

Result<void> ComponentSimulation::after_step() {
    for (const auto id : order_.get(entries_)) {
        auto& e = entries_.at(id);
        auto sampled = [&]() -> Result<void> {
            if (e.kind == Kind::character || e.kind == Kind::ragdoll) {
                // Remember the published owner frame, which need not equal a ragdoll's
                // first rest-body frame before its first completed simulation step.
                ATELIER_TRY_BIND(matrix, world::world_matrix(world_.runtime(), e.entity));
                ATELIER_TRY_BIND(pose, world::relative_transform(*matrix, world::transform_matrix({})));
                e.origin = pose->position;
                e.rotation = pose->rotation;
            } else if (e.kind == Kind::query) {
                auto requested = read<SceneQueryComponent>(world_.runtime(), id,
                                                           [](const auto& v) { return v.automatic || v.pending_; });
                ATELIER_TRY(requested);
                if (*requested)
                    return sample_query(id);
            }
            return {};
        }();
        if (!sampled && !contain(world_, sampled.error(), id, e.entity))
            return sampled;
    }
    return {};
}

void ComponentSimulation::clear_query(ObjectId id, bool clear_request) noexcept {
    (void)world_.runtime().with_native<SceneQueryComponent>(id, [&](auto& q) -> Result<void> {
        if (clear_request)
            q.pending_ = false;
        q.ready_ = q.hit_ = q.blocking_ = q.overflow_ = false;
        q.count_ = 0;
        q.distance_ = 0;
        q.point_ = {};
        q.normal_ = {};
        return {};
    });
}

Result<void> ComponentSimulation::sample_query(ObjectId id, QueryIgnore ignore) {
    auto* found = entries_.find(id);
    if (!found || found->kind != Kind::query)
        return std::unexpected(
            Error{ErrorCode::invalid_member, "Query component is inactive, foreign or has not been reconciled."});
    auto& entry = *found;
    entry.query.reset();
    clear_query(id, false);
    auto& runtime = world_.runtime();
    const auto input = read<SceneQueryComponent>(
        runtime, id, [](const auto& q) { return std::tuple{q.settings, q.start, q.end, q.ignore_self}; });
    ATELIER_TRY(input);
    const auto& [settings, start, end, ignore_self] = *input;
    ATELIER_TRY_BIND(matrix, world::world_matrix(runtime, entry.entity));
    auto ignored = bindings<SceneQueryComponent, &SceneQueryComponent::ignored_entities>(
        runtime, id, ignore.entities.size() + std::size_t(ignore_self));
    ATELIER_TRY(ignored);
    ignored->append(ignore.entities);
    if (ignore_self)
        ignored->add(entry.entity);
    ATELIER_TRY_BIND(result, scene_.trace(math::transform_point(start, *matrix), math::transform_point(end, *matrix),
                                          settings, {*ignored, ignore.components}));
    auto published = runtime.with_native<SceneQueryComponent>(id, [&](auto& q) -> Result<void> {
        q.pending_ = false;
        q.ready_ = true;
        q.hit_ = result->has_hit;
        q.blocking_ = result->has_block;
        q.overflow_ = result->overflow;
        q.count_ = static_cast<std::uint32_t>(result->hits.size());
        if (!result->hits.empty()) {
            q.distance_ = result->hits.front().distance;
            q.point_ = result->hits.front().point;
            q.normal_ = result->hits.front().normal;
        }
        return {};
    });
    ATELIER_TRY(published);
    entry.query = std::move(*result);
    return {};
}

Result<QueryResult> ComponentSimulation::execute_query(ObjectId id, QueryIgnore ignore) {
    ATELIER_TRY(sample_query(id, ignore));
    return read_query_result(id);
}

Result<QueryResult> ComponentSimulation::read_query_result(ObjectId id) const {
    const auto* found = entries_.find(id);
    if (!found || !found->query)
        return std::unexpected(
            Error{ErrorCode::invalid_member, "Query component has no completed result in this scene."});
    return *found->query;
}
}
