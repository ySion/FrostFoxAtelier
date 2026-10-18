# 场景查询与射线组件

`PhysicsScene::trace` 和 `SceneQueryComponent` 使用现有 Jolt `PhysicsSystem` 的 NarrowPhaseQuery，不创建第二套碰撞世界。接口采用 UE 的通道/对象类型、单次/多次查询语义，返回引擎的对象身份和数学值。基础 `raycast`、`sweep_sphere`、`overlap_sphere` 仍可直接使用。

## 查询语义

`TraceSettings` 定义 `shape`、`selection`、`mode`、`channel`、`object_types`、`capacity`、`radius` 和 `include_sensors`。

| 设置 | 当前行为 |
| --- | --- |
| `line` | 从 start 到 end 的线段，不是无限射线 |
| `sphere_sweep` | 半径固定的球从 start 移动到 end |
| `sphere_overlap` | 查询 start 处的球；忽略 end，不进行移动 |
| `channel` + `test/single` | 根据目标 Body 的 `responses[channel]`，忽略 Ignore 和 Overlap，仅检测最近 Block |
| `channel` + `multi` | 返回沿途 Overlap，直到并包含第一个 Block；Overlap 球查询没有行进顺序，返回所有匹配项 |
| `object_type` | 用 `object_types` 位掩码选择 Body category，不读取响应表；multi 返回所有匹配项 |
| `test` | 只保留命中摘要，`hits` 为空；不是后端提前终止的性能保证 |
| `single` | 返回最近一个符合上述规则的命中 |
| `multi` | 稳定排序后返回命中列表，受 capacity 限制 |

当前复用 Body 原有的 16 槽响应表，channel 范围为 0–15；object_types 为 16 位掩码，0 表示不选择任何类别。没有独立的 UE TraceChannel/Profile 配置系统。Sensor 可由 `include_sensors` 排除；包含时只产生非阻挡命中，因此通道 test/single 不会选中 Sensor，对象类型查询可以选中它。

`QueryIgnore` 支持同时忽略多个 Entity 和 Component，借用的 span 只在同步调用期间有效。忽略的 Component 指启用了物理状态的 Primitive；StaticMeshComponent 可以直接成为命中组件。同一 Entity 下只忽略指定组件时，其余 Primitive 仍可命中。基础 `QueryFilter` 默认 `interaction` 策略继续使用双方响应；`trace` 显式选择 channel/object_type。基础 closest 的 `has_block` 仍描述最近命中。

输入必须有限，线段差值与长度不能溢出；line/sweep 拒绝零长度，静止体积应使用 overlap。球半径必须有限且大于 0；容量为 1–4096，非法枚举或超出 16 位的掩码返回错误。半径不用于 line。容量只是输出上限，目前 Jolt 全命中收集器仍可能收集更多候选，不是内存或耗时预算。

## 返回值

`QueryResult` 保存完成物理步 `step_id`、世界空间 `start/end`、`has_hit`、`has_block`、`overflow` 和拥有型 `hits`。未命中是成功的空结果。multi 摘要在容量截断前计算：即使容量只保留前方 Overlap，`has_block` 仍能表示后面的首个 Block；此时 `overflow` 为 true。

每个 `QueryHit` 包含带场景与重建代际的 `BodyHandle`（Entity、PrimitiveComponent）、`sub_shape`、物理材质、Sensor/Block 标记，以及：

- `fraction`：线段上的比例；`distance`：沿线段的世界距离。
- `location`：射线命中位置或扫掠球心；`point`：目标表面的接触点，球扫掠时两者不同。
- `normal`：目标表面法线或分离方向；`initial_overlap`：起点接触/穿透，或球重叠命中。
- `penetration_depth`：球扫掠/重叠的非负穿透深度；射线没有体积，深度为 0。射线从内部开始时法线可能为零，不伪造外表面法线。

球扫掠启用 Jolt 最深穿透点，并允许凸形状起始穿透在向外移动时被报告；三角网格仍沿用默认背面过滤。命中按 fraction、Body 重建代际、子形状 ID 排序。同距离用身份打破平局；overlap 全部 fraction 为 0，不能把第一项解释成最近物体。同一网格的不同子形状可以产生多条命中，接口不保证每 Entity 只有一项。

结果不持有 Entity，也不会让目标保活。缓存表示采样时刻；目标稍后停用时，旧结果仍是历史快照。使用保存的句柄施加动作前，应通过现有 `read_body_state` 等接口验证其仍有效。

## C++ 接口

已有运行中的 World/PhysicsScene 经 `World::update(0)` 完成登记后，可以同步调用：

```cpp
physics::TraceSettings settings;
settings.selection = physics::TraceSelection::channel;
settings.channel = 7;
settings.mode = physics::TraceMode::multi;
std::array ignored{owner_entity}; // ObjectId
auto result = scene.trace({0, 1, -4}, {0, 1, 12}, settings, {ignored, {}});
if (!result) return std::unexpected(result.error());
for (const auto& hit : result->hits) {
    // hit.body.entity / hit.body.component / hit.point / hit.distance
}
```

改为 `selection=object_type` 并设置 `object_types=1u<<2` 可选择类别 2；改为 `shape=sphere_sweep` 并设置 radius 可使用球扫掠。所有调用在拥有场景的宿主线程、Jolt 求解之外执行，没有异步或跨线程读保证。

## 可保存的 SceneQueryComponent

这是 Runtime 注册的真实 `physics.SceneQueryComponent : world.Component`，可通过 Entity Structure → Add Component 添加；同一个 Entity 可有多个查询。它只描述和执行查询，不拥有刚体，也不占用 Entity 位姿写入权。

公开配置随 Scene v6 保存：settings、局部 start/end、automatic、ignore_self，以及公开持有 Entity 引用序列 `ignored_entities`。枚举按名称保存；请求和结果为私有运行状态，不参与序列化、复制文档或存档。查询使用 owner Entity 的完整世界矩阵变换端点，支持空间链组合产生的有限剪切，不要求分解为 TRS；需要独立摆放时将查询组件放在子 Entity。球半径使用世界单位，不随 Entity 缩放变化。配置每次采样读取，配置错误在执行时返回，不沿用旧成功结果。

- 默认 automatic=true：每个已完成物理步、所有模拟位姿回写后采样，在外部 `on_after_step` 之前发布。
- automatic=false：调用 `request_query()`，在下一个实际固定步采样；重复请求合并，成功消费一次。没有子步或处于暂停时不自动更新。
- 宿主可用 `execute_query(component, ignore)` 立即采样，即使物理暂停；必须先通过 `World::update(0)` 登记活动组件。
- `read_query_result(component)` 返回拥有型快照副本。未采样、采样失败或组件失效返回错误；已完成但未命中返回成功空结果。
- ignore_self 默认只追加忽略实际 owner Entity，不自动扩展到组合父实体或兄弟实体。`ignored_entities` 显式排除所列 Entity，与 ignore_self 及调用时的 ignore 集合共同生效；显式绑定目标必须有效且不重复，空身份或重复目标更新失败并保留旧集合。`set_query_ignored_entities / query_ignored_entities` 使用同一字段，允许活动查询更新，并在下次采样生效。
- 组件、所属 Entity 或其组合祖先停用/删除/离场会清除其活动结果和请求；恢复后重新登记。独立空间参考目标的停用不清除查询资格。尚未首次 reconcile 的活动组件在停用时也会取消请求。

脚本直接调用继承的原生方法：`request_query`、`query_ready`、`has_hit`、`has_blocking_hit`、`result_overflow`、`hit_count`、`hit_distance`、`hit_point`、`hit_normal`。后三项读取保留列表的首个命中，空列表或 test 模式返回零值。脚本 Tick 在物理子步之前运行，因此读取上次完成的结果，再请求本帧后续物理步查询。例如绑定在查询派生组件上的 Tick：

```cpp
void tick(float dt) {
    if (self.query_ready() && self.has_hit()) {
        float distance = self.hit_distance();
        // 使用 distance；test 模式应只读取布尔摘要。
    }
    self.request_query();
}
```

Scene v6 在 `properties.ignored_entities` 中保存 `{"$ref":"Entity文档ID"}` 的引用数组，持有语义由 Runtime schema 决定；通用引用编码允许 null，但本查询绑定合同拒绝空目标。旧 Actor 中 ignore_self=true 的查询经严格导入时，将该 Actor 内所有 Primitive 的实际新 owner Entity 写入显式排除集合，保留原自组忽略范围；query owner 与旧 Actor 组不是同一范围。

完整命中列表与对象身份目前由 C++ 场景接口消费；没有为脚本新增对象引用返回 ABI 或一套对象包装。

## 场景与边界

[Raycasts.scene](../../../assets/examples/Examples/Physic/Scenes/Raycasts.scene) 包含绿色 Overlap 目标、红色首个 Block、蓝色遮挡后目标、黄色通道 Ignore 对象、青色 Sensor，以及包含六个查询子 Entity 的发射器组合。六个查询都显式排除发射器碰撞 Entity 和视觉网格 Entity，保留旧 Actor 分组的自忽略行为。通道使用 7，对象掩码选择类别 1/2；球重叠单独选择类别 3 Sensor。官方场景验收检查身份、命中数、步号、保存重开和模拟后的真实 GPU 画面。视口尚无射线绘制和命中面板，查询结果通过 API/脚本及自动验收读取。

已有用户副本的 Physic 目录不会被模板更新覆盖。缺少新场景时，可把此新文件复制到用户工程的 `Examples/Physic/Scenes/`，再刷新并打开；不要替换已编辑的场景。

当前查询返回已登记的 Primitive 物理体（含车辆底盘和静态网格）；虚拟角色、布娃娃独立部件未建立可返回的 BodyHandle 映射。尚无通用 Box/Capsule Sweep、异步查询、骨骼/UV 命中、TraceComplex 切换、碰撞 Profile 或调试绘制。

设计参考：[UE Trace 概览](https://dev.epicgames.com/documentation/unreal-engine/traces-in-unreal-engine---overview)、[Raycast 使用](https://dev.epicgames.com/documentation/unreal-engine/traces-with-raycasts-in-unreal-engine)、[FCollisionQueryParams](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FCollisionQueryParams)。后端细节以仓库固定版本的 [Jolt ShapeCast](../../../third_party/jolt/Jolt/Physics/Collision/ShapeCast.h) 为准。实际覆盖与证据见[物理验收](acceptance.md)。
