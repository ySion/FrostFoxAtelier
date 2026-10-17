# World 与游戏对象

正式入口为 [world.hpp](../../../src/world/world.hpp)，数学值为 [math.hpp](../../../src/world/math.hpp)。`atelier_world` 使用同一 Runtime 类型、对象身份、引用和 GC；网格使用 [mesh.hpp](../../../src/geometry/mesh.hpp) 的不可变 CPU 资源协议。无窗口宿主不链接 renderer/RHI，语言行为由宿主绑定正式 Source 入口。

## 类型与所有权

```text
core.Object
├─ Entity
│  └─ StaticMeshEntity
└─ Component
   ├─ PrimitiveComponent
   │  ├─ StaticMeshComponent
   │  └─ CollisionComponent
   └─ LightComponent
```

Entity 是唯一的组成与可选空间节点。它内联保存 position、rotation、scale；启用空间能力时这些字段形成一个 TRS，不需要额外 Transform 对象。Component 只承担能力和业务字段，使用 owner Entity 的空间。多个需要独立位置的网格、灯或碰撞体使用多个子 Entity，业务派生 Entity/Component 保留自己的真实类型和默认字段。

Entity 的隐藏强引用序列分别拥有有序 children 和 components；组合 parent 和显式空间 origin 为弱引用，Component 强持 owner。同类型组件允许多实例，物理位姿写入者的排他规则由物理模块校验。普通根可以保住组成图；无外部根的引用环沿 Runtime GC 回收。

World 为每个成员 Entity 持有 ObjectRoot，成员身份以 Entity 的 World 归属为准；执行和查询目录是派生索引。`members()` 按组合前序列出所有 Entity，每个身份一次，每棵子树占连续区间；attach、detach、reparent、启用与组件增删只更新受影响的子树区间及其祖先计数，不重建整个目录。`contains(entity)` 以常数时间判断成员资格。类型查询不改变执行顺序。`components / children` 返回普通 ObjectRoot 快照，保留身份与当前数据。对象可以活过旧 World；Runtime 必须晚于 World、对象操作和任务结束。

## 完整创建与组合

`create_entity / create_component` 只建立真实类型与默认存储。`attach_components` 准备组件组成，`reparent` 准备子 Entity 关系，`World::attach` 注册完整子树。`attach_batch` 接收整个待发布森林及其空间参考，按最终归属统一校验，再一起提交关系和成员身份；通知和 Begin 只能看到完整图。Detached 图组装不登记 World、呈现、查询或运行回调。

Detached 的 keep_local 组装立即校验组合身份与环，空间可等完整关系准备后再校验；矩阵查询仍会报告当前图的无效状态。keep_world 要即时保留世界位姿，因此仍要求操作前后空间均可求值。World 预览与发布统一校验完整空间图。

`create_static_mesh_entity(World&, StaticMeshEntityDesc)` 创建尚未注册的完整 StaticMeshEntity：Entity 自身具有 TRS，仅包含一个 StaticMeshComponent。描述提供共享 mesh、material、transform 与 mesh_source；空网格仍保留组成身份但不产生绘制。工厂失败清理本次对象，成功后由同一 attach 入口发布。

EntityRemoval 保留子树、原组合父与位置；ComponentRemoval 保留 owner 与组成位置。撤销恢复同一 Runtime 身份，不重建另一套对象。编辑器把整组创建记录成一个历史项。`Commit.notification_error` 表示关系已经提交，消费者记录变更后再报告通知错误。

离场对象保留向外的弱空间参考；Detached 状态下若目标仍在原 World，世界矩阵查询明确失败，重新发布时按最终成员归属校验。留在 World 的对象若仍显式参考离场目标，整批离场被拒绝。由此复制对象的撤销与恢复可以保留同一份作者关系，无需额外的空间补偿记录。

`reparent` 的 index 是移除目标后的同级位置。批量 `EntityAttachment` 的 index 则指向提交前的目录：有 parent 时是其 children 位置，无 parent 时是 World 成员目录中的根边界或末尾。根不能插进已有子树中间；省略 index 表示追加。

## 空间树与网格

`has_transform / read_transform / set_transform / remove_transform` 操作 Entity 的可选内联空间。默认参考最近具有空间能力的组合祖先；`space_reference` 的 nullopt 表示继承，空 ObjectId 表示显式世界空间，非空身份表示显式弱空间参考。无空间 Entity 可以组织子树；组件不再保存 TRS、parent 或 root。

`component_requires_space` 按实际组件判断空间需求，天空光不要求 Transform。移除 Transform 前须移除依赖该空间的能力，并解除直接指向它的显式空间参考。默认继承的子节点可继续寻找更上层空间，保持原局部 TRS；最终图仍须有效。

`reparent` 改变组合归属，`set_space_reference` 改变坐标参考；两者均提供 keep_local / keep_world。keep_world 在验证完整候选后保留原世界位姿，不能表示的剪切、反射、奇异矩阵或溢出返回错误。空间环、跨 World 关系和无效身份在发布前拒绝。`attachment_world_matrices` 用同一套候选图规则预览待发布 Entity 的世界矩阵，供 Cook 等派生资源准备；最终 attach 仍重新校验。

运行中的 World 可在生命周期回调之外切换空间参考。只改空间不会改变组合资格，不重发 Begin/End 或启用通知，也不取消任务。物理在下一步前协调空间依赖；keep_world 保留有效后端身份和连续状态，实际世界姿态或尺度变化按物理规则处理。

TRS 使用有限 position、非零且规范化的 xyzw 四元数、严格正的每轴 scale。Matrix4 为行主序、行向量，local = S * R * T，平移位于 12–14，world = local * reference_world。层级矩阵可含剪切，不保存第二份可编辑空间状态，也不以伪造单位矩阵掩盖无效关系。

StaticMeshComponent 保存网格与材质，LightComponent 保存灯参数，PrimitiveComponent 保存 body 作者配置；绘制、灯光与物理都查询 owner Entity 的空间。需要独立空间时创建子 Entity，将能力组件放到该子实体。

## 生命周期与执行顺序

`bind_behavior(TypeId, Behavior{begin,tick,end})` 给真实 Entity 或 Component 配置可选执行入口，派生类型沿 Runtime 基类链选择最近完整登记；显式空登记关闭继承行为。原生与 Source 回调在 native borrow 外执行，接收者受普通根和 ExecutionScope 保护。

Begin/Tick 沿组合前序：Entity 自身、其组件组成顺序、子 Entity 子树。End 反转当前组成顺序，只配对已成功完成 Begin 的对象。没有 Tick 的数据组件不产生空调用；静态呈现不依赖 Tick。运行中插入只开始新对象，禁用不重发 Begin/End；祖先禁用阻止其子树 Tick 并取消相应任务。

任一 Begin 或 Tick 首错停止后续执行，并逆序清理已开始对象。End 或通知错误仍继续清理，保留首错，不回滚已产生业务副作用。stop、异常停机与 close 共用任务取消、逆序 End 和物理参与撤销入口。

attached/detached 表示 World 成员注册与注销，activated/deactivated 表示启用资格变化，不代替 Begin/End。回调中同步结构修改拒绝，可通过 `request/flush` 延后到宿主边界；请求在执行前复核 World 与归属，不跟随对象迁入另一个 World。

## 编辑、呈现与资源

已入场成员的作者字段、Transform 增删、材质与灯参数编辑要求 World 停止；组合归属和空间参考按结构操作的准入规则处理。同 Runtime 的 Detached 图可以在 World 运行时于宿主边界准备，完整校验后经同一入口原子发布。回调内重入及其他 World 的成员仍拒绝。Entity 和 Component 属性走同一 Runtime FieldPath 与 scalar 编辑事务；TRS 历史保存完整前后值，业务字段保留精确 Scalar。空 Entity 仍可选择编辑；成员资格、对象存活、运行激活和呈现登记分别判断。

`destroy_entity / destroy_component` 是游戏对象终结入口：先退出 World，再清理组成并调用 Runtime；可信宿主不得绕过它直接销毁活跃成员。detach 保留对象图；历史裁剪只释放根，不隐式 destroy。close 退出所有子树、释放成员根后不可重启，重复 close 无副作用。

`set_mesh_binding` 同时设置持久来源键与不可变 MeshRef，清理不匹配材质槽；`set_geometry` 清除来源键。`read_static_mesh` 支持 Detached 与空网格，供存档读取。`material.Material / material.Texture` 是同一 Runtime 的资源对象，槽绑定强持资源；Renderer 只消费拥有型不可变 Surface 快照。具体合同见 [Lit 数据](lighting.md)和[项目场景](project-scenes.md)。

`render_items()` 按成员前序与组件顺序提取网格，结果容量等于实际网格数；零网格或纯灯光场景不分配结果存储。成员的世界矩阵来自 World 持有的一份空间缓存，`render_items`、`light_items`、`world_matrix` 与 `spatial_parent` 共用：组合关系、空间参考与 Transform 增删经 World 结构操作改变并使缓存失效；每次提取重新读取所访问 Entity 的当前 TRS，按位比较后只重组变化的链，因此 Runtime 直接写入与生成代码的写入在下一次查询即可见。Detached 对象和候选图仍使用临时求值。

## 运行位姿与验收入口

Component 的 `translate_owner(dx,dy,dz)` 记录所属 Entity 的平移命令，在行为阶段结束后由宿主边界复核并批量应用；不保存字段地址。物理控制的 Entity 拒绝普通平移命令。`apply_simulation_transforms` 接受 Entity 世界位置/旋转，整批先验证并按空间依赖顺序转换，然后发布，保留局部显示 scale；不进入编辑历史。

端到端入口为 `world_acceptance`、`project_scene_acceptance` 和 `editor_document_acceptance`，开发与发布配置均需执行。官方内容使用 Scene v6；旧 v1–v5 仅通过严格导入进入同一模型。实际结果与仍未完成的图形验收统一记录在[编辑器验收状态](../editor/README.md#entity-迁移验收状态)。
