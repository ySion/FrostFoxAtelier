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

World 为每个成员 Entity 持有 ObjectRoot，成员身份以 Entity 的 World 归属为准；执行和查询目录是派生索引。`members()` 按组合前序列出所有 Entity，每个身份一次，每棵子树占连续区间。attach 只从 Runtime 推导进入的子树，Begin/End 与生命周期通知只访问进出的子树，并调整受影响祖先的子树计数；插入点或移除点之后的成员只移动并更新下标，reparent 只移动源位置与目标位置之间的区间，均不重新从 Runtime 推导。启用与组件增删只更新对应成员。Tick 计划与类型目录在下次使用时由内存中的成员状态重建。`contains(entity)` 以常数时间判断成员资格。类型查询不改变执行顺序。`components / children` 返回普通 ObjectRoot 快照，保留身份与当前数据。对象可以活过旧 World；Runtime 必须晚于 World、对象操作和任务结束。

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

`reparent` 改变组合归属，`set_space_reference` 改变坐标参考；两者均提供 keep_local / keep_world。keep_world 在验证完整候选后保留原世界位姿，不能表示的剪切、奇异矩阵或溢出返回错误；反射由局部 scale 的符号承载，优先沿用对象原有的负轴。空间环、跨 World 关系和无效身份在发布前拒绝。`attachment_world_matrices` 用同一套候选图规则预览待发布 Entity 的世界矩阵，供 Cook 等派生资源准备；最终 attach 仍重新校验。

运行中的 World 可在生命周期回调之外切换空间参考。只改空间不会改变组合资格，不重发 Begin/End 或启用通知，也不取消任务。物理在下一步前协调空间依赖；keep_world 保留有效后端身份和连续状态，实际世界姿态或尺度变化按物理规则处理。

TRS 使用有限 position、非零且规范化的 xyzw 四元数、有限的每轴 scale。Matrix4 为行主序、行向量，local = S * R * T，平移位于 12–14，world = local * reference_world。层级矩阵可含剪切，不保存第二份可编辑空间状态，也不以伪造单位矩阵掩盖无效关系。

负 scale 表示镜像，rotation 始终是真旋转。从矩阵分解 TRS（keep_world、物理回写）时，奇数个负轴优先取对象原局部 scale 的符号，符号奇偶不符时只取 X 为负，因此 (-1,-1,-1) 的回写保持原 TRS，而无偏好的世界分解为 (-1,1,1) 加半周旋转。零 scale 可以存储，但该节点及其整棵空间子树的世界矩阵奇异：网格不绘制、Primitive 不建刚体，各报告一条 `degenerate_value` 警告，身份指向链上最上层的零缩放节点，持续期间合并计数；这不是对象失败，任何策略下都不停用行为或停机。涉及奇异矩阵的 keep_world 仍拒绝。灯光朝向忽略 scale 幅值、保留符号，随镜像一起镜像；零缩放不使灯光退化，见[灯光](lighting.md)。

StaticMeshComponent 保存网格与材质，LightComponent 保存灯参数，PrimitiveComponent 保存 body 作者配置；绘制、灯光与物理都查询 owner Entity 的空间。需要独立空间时创建子 Entity，将能力组件放到该子实体。

PrimitiveComponent、CollisionComponent、BodySettings 与 PhysicsMaterial 留在 world 库，是呈现与物理共用的基元层：类型定义放在这里供两侧引用，规则不归 World 核心。字段规则迁入 Runtime 类型校验器之后，World 核心不再对这些类型做任何判断；具体能力由登记的呈现、准入与模拟系统负责。

## 生命周期与执行顺序

`bind_behavior(TypeId, Behavior{begin,tick,end,post_physics_tick,late_tick})` 给真实 Entity 或 Component 配置可选执行入口，派生类型沿 Runtime 基类链选择最近完整登记；显式空登记关闭继承行为。原生与 Source 回调在 native borrow 外执行，接收者受普通根和 ExecutionScope 保护。Source 的生命周期方法名为 `begin / tick / end / post_physics_tick / late_tick`。运行中也可以在宿主边界重新绑定（行为分发内返回 busy）：已开始的对象从下一个执行的阶段起使用新的 Tick，不补发 Begin；End 使用退出时的当前绑定；`clear_faults()` 仍按原规则重新 Begin。

`World::update(dt)` 是唯一的帧入口，顺序由结构保证，不公开单独推进某个阶段的入口：

```text
pre_physics Tick → 任务恢复 → 平移命令 → flush 请求
→ 各模拟系统 step(dt)（按登记顺序）
→ post_physics Tick → 平移命令 → flush 请求
→ late Tick → 平移命令 → flush 请求
```

三个阶段都收到整帧 dt，post_physics 不随物理子步数变化；late 位于 post_physics 之后、宿主提取呈现之前。阶段内顺序不作承诺，只由显式前置约束排序：`add_tick_prerequisite(object, prerequisite)` 让 prerequisite 在两者都有 Tick 的每个阶段先执行；约束按全图的传递闭包生效，A→B→C 中 B 在某阶段没有 Tick 时，A 仍先于 C。形成环的约束在添加时拒绝；约束跨 stop/start 保留，任一端离开 World 时移除。目前只提供宿主/原生入口，Source 在引用字段落地后接入。

Begin 沿组合前序：Entity 自身、其组件组成顺序、子 Entity 子树。End 反转当前组成顺序，只配对已成功完成 Begin 的对象。没有 Tick 的数据组件不产生空调用；静态呈现不依赖 Tick。运行中插入立即 Begin，从下一个执行的阶段开始 Tick；禁用不重发 Begin/End；祖先禁用阻止其子树 Tick 并取消相应任务。

每次 update 记为[帧阶段计时](../../foundation.md#帧阶段计时)的 `world.*` 阶段（标记见 [world/timing.hpp](../../../src/world/timing.hpp)）：`world.update` 下依次是 `world.pre_physics`、`world.simulation`、`world.post_physics` 和 `world.late`；每个 Tick 阶段各有自己的 `.tick`、`.commands`、`.flush` 子阶段，pre_physics 另有任务恢复子阶段 `world.pre_physics.tasks`。结构变化后的调度重建记为 `world.update` 下的 `world.schedule`，只在重建发生的阶段之前进入。停止的 World 不记录任何阶段；某阶段使 World 停止时，之后的阶段不再出现。模拟系统自己的阶段挂在 `world.simulation` 下。

模拟系统用 `add_simulation(Simulation{step, writes_pose})` 登记；PhysicsScene 创建时登记，析构时注销。step 在行为分发之外的宿主边界执行，可以调用 World API；step 失败按 World 级处理：停止 World 并使该次 update 失败。World 停止时不调用 step，子系统经生命周期通知清理。`writes_pose(entity)` 声明该系统写入此 Entity 的位姿。可选的 `relocated(entities)` 在运行中的 World 经 `set_transform`（含 Transform 标量写入）改变了位姿时，于宿主边界同步调用一次，参数是被编辑 Entity 及其空间后代中被该系统声明位姿的那些；系统据此让模拟跟上作者位姿。回调可以读取 World，但不能编辑它（返回 busy）；回调返回错误时 World 恢复原 TRS，该次编辑以这个错误失败。模拟系统自己的 `apply_simulation_transforms` 不触发它，World 停止时也不调用。

失败按 World 的 `FailurePolicy` 处理，见下一节。stop、异常停机与 close 共用任务取消、逆序 End 和物理参与撤销入口。

attached/detached 表示 World 成员注册与注销，activated/deactivated 表示启用资格变化，不代替 Begin/End。回调中同步结构修改拒绝，改用 `request` 入队：每个 update 阶段结束时自动 flush，宿主也可以在帧外调用 `flush()` 取得回执。Entity 与 Component 的原生方法 `destroy()`、`set_enabled(bool)`（Source 可调用）只入队，走同一队列。请求在执行前复核 World 与归属，不跟随对象迁入另一个 World；阶段末 flush 中失败的请求记为目标上的 `command_rejected` 警告，并按失败策略处理（halt 下停机）。运行期生成对象尚未提供。

## 失败策略与诊断

`FailurePolicy` 默认为 isolate，Play 使用默认值；验收与编辑器测试用 `set_failure_policy(halt)` 选择首错即停，只能在 World 停止时切换。两种策略都把对象失败写入同一诊断日志。

只有能归属到对象的错误才隔离：Begin/Tick/End 行为回调、任务恢复、生命周期观察者、呈现/灯光/物理的逐项校验与协调、单条平移命令与模拟位姿。World 与 Runtime 自身的错误在任何策略下都按 World 级处理：停止 World 或使整个调用失败，不写对象诊断。`isolatable(Error)` 是这条分界：busy（重入、遍历中）、rebuild_required（代码发布检查）、execution_aborted（执行中止）、capacity_exceeded（含分配失败）不可隔离；World 发现自身成员或组件索引不一致后，此后的任何失败都不再隔离。结构编辑、attach/detach、start/stop 等宿主调用的失败照常返回给调用者。

isolate 下：

- Begin 失败的对象记为 Begin 故障，不 Tick、不启动任务、不收到 End；它的组件与子 Entity 照常 Begin。
- Tick 或任务失败的对象停止 Tick，取消其任务并丢弃它待应用的平移；它已完成 Begin，退出时仍收到一次 End。End 失败只记诊断，清理继续。
- 观察者失败记为 `observer_failed`，不写 `Commit.notification_error`。
- `render_items / light_items` 逐项跳过：空间不可求值时诊断记在失败的 Entity 上，材质、灯参数记在组件上。重复天光按组合前序保留第一个，之后的以 `conflict` 跳过。
- `translate_owner` 命令逐条复核，被拒绝的命令（例如模拟系统写入位姿的 Entity）记 `command_rejected` 警告；`apply_simulation_transforms` 逐个拒绝无效位姿并以剩余位姿重新换算，被拒绝的 Entity 保持局部 TRS。物理按 Entity 或控制器排除失败项，见[物理](../physics/README.md)。

halt 保持首错即停：任一 Begin、Tick 或任务首错停止后续执行，并逆序清理已开始对象；End 或通知错误继续清理并保留首错，不回滚已产生的业务副作用；提取与位姿批次整次失败，不部分发布。

故障状态属于一次运行，按对象存于 World 执行状态；对象退出参与或 World 停止时清除。`clear_faults()` 由宿主在兼容代码发布成功后于宿主边界调用：Tick 故障的对象恢复 Tick，不重发 Begin；Begin 故障的对象经运行中插入的 Begin 路径重新 Begin。

诊断记录定义在 [diagnostic.hpp](../../../src/world/diagnostic.hpp)：severity、kind、phase、object、entity、type（限定名）、field（字段名路径）、source（生成代码的失败位置，有值时转发）、message、serial、repeats。场景加载诊断使用同一记录与 kind 词表。`take_diagnostics()` 取走日志；日志最多 1024 条，溢出计入 `dropped_diagnostics()`。呈现、灯光、物理与命令阶段中，同一对象、kind 与字段的失败若在下一次提取或更新中重复，只增加 repeats，某次未再出现即视为解决；已取走的条目在失败持续期间不再重发。行为失败每次发生各记一条。物理等子系统经 `World::contain(Error, Diagnostic)` 按同一策略报告与判断。

## 编辑、呈现与资源

已入场成员的作者字段、Transform 增删、材质、材质槽与灯参数，以及 World 光照设置，都可以在宿主边界编辑，World 运行中也一样：新值从下一个执行的阶段起生效，不重发 Begin/End，不取消任务；夹值、类型规则、准入与失败恢复与停止时相同。行为分发内的重入、结构转换期间的编辑以及其他 World 的成员仍拒绝；`inspect_entity` 的 `can_edit` 表示当前不在行为分发内。失败策略属于 World 配置，仍只在停止时可改。组合归属和空间参考按结构操作的准入规则处理。同 Runtime 的 Detached 图可以在 World 运行时于宿主边界准备，完整校验后经同一入口原子发布。Entity 和 Component 属性走同一 Runtime FieldPath 与 scalar 编辑事务；TRS 历史保存完整前后值，业务字段保留精确 Scalar。空 Entity 仍可选择编辑；成员资格、对象存活、运行激活和呈现登记分别判断。

`World::write` 对 Transform 字段走 `set_transform`；其余字段不按类型分支：先经 `Runtime::write` 写入（硬边界夹值，NaN/Inf 与错误 Scalar 类型拒绝），再用 `Runtime::validate` 检查对象的硬边界与类型校验器，最后运行准入。写入前有效的对象若因这次编辑变为无效，或准入失败，就恢复原值并返回该错误；写入前已经无效的对象（例如脚本或可信路径写坏）照常可编辑，便于逐字段修复，提取时仍按对象诊断。字段规则属于类型本身（反射硬边界与 `TypeDefinition::validate`），World 核心不判断具体类型；`validation_diagnostic` 把 Runtime 的校验失败转为 `invalid_value` 诊断，field 为记录前缀加失败路径，value 为越界值预览。

`destroy_entity / destroy_component` 是游戏对象终结入口：先退出 World，再清理组成并调用 Runtime；可信宿主不得绕过它直接销毁活跃成员。detach 保留对象图；历史裁剪只释放根，不隐式 destroy。close 退出所有子树、释放成员根后不可重启，重复 close 无副作用。

`set_mesh_binding` 同时设置持久来源键与不可变 MeshRef，清理不匹配材质槽；`set_geometry` 清除来源键。`read_static_mesh` 支持 Detached 与空网格，供存档读取。`material.Material / material.Texture` 是同一 Runtime 的资源对象，槽绑定强持资源；Renderer 只消费拥有型不可变 Surface 快照。具体合同见 [Lit 数据](lighting.md)和[项目场景](project-scenes.md)。

能力按组件类型登记：`register_capability(component_type, predicate)` 返回不透明的 `CapabilityId`，World 只维护该登记下成员组件的桶（组合前序，包含未启用项并以 active 标出，active 即自身启用且 owner 有效启用，不含是否已 Begin），随组合、成员与启用的每次变化更新，由 `members(id)` 查询；组件数据改变后，负责该数据的写入方调用 `reevaluate_capabilities(component)` 重新求值谓词。不带谓词再次登记同一类型时返回已有的无谓词登记，所以在同一 World 上重建的领域宿主（例如每次 Play 新建的 PhysicsScene）不会累积登记。每个 World 最多 32 个登记。目前 world 库自己登记网格（需要有几何）与灯光，`render_items / light_items` 只遍历这两个桶，`can_present` 与 `Activity::presentation_registrations` 也只按这两个登记计算；物理登记 Primitive、Character、Ragdoll 作为逐帧协调的候选来源。

跨对象的成员规则同样按类型登记：`add_admission(component_type, Admission)`，Admission 收到该类型（含派生）的候选组件。attach、attach_batch、restore 与向已入场 Entity 的 attach_components 在发布前以全部新组件为候选调用它，失败则整批拒绝；Detached 组合不调用。作者编辑（`write`、`set_material`、`set_light`）改动成员组件时采用“先写、再检查、失败恢复”：先写入，候选组件以写入后的值出现在 World 中，Admission 失败时恢复原值并返回该错误。`write` 用写入前读出的 Scalar 经 Runtime::write 恢复，原值若由可信路径写成越过硬边界，恢复时按硬边界夹值；这样的原值本身已违反字段不变量，`Runtime::validate` 会报告它。`set_material / set_light` 按整值精确恢复。不经 World 的 Runtime 写入与生成代码写入不调用准入，由消费端的确定规则兜底。world 库目前登记两条：每个 World 至多一个 Sky Light；非 Sky 灯光的成员 owner 必须有 Transform（attach 时由空间需求检查覆盖，这条用于编辑改变灯光种类）。

`spatial_pass(phase)` 开始一次提取遍历，同时开始该阶段的诊断遍历（重复失败按遍历合并 repeats），直到下一次遍历开始。`matrix(entity)` 返回成员 Entity 的世界矩阵，空间不可求值时在失败的 Entity 上记 `invalid_value` 并返回空（halt 策略或 World 级失败时返回错误）；`degenerate(entity, matrix, message)` 对奇异矩阵在链上最上层的零缩放节点记一条 `degenerate_value` 警告并返回 true；`orientation(entity)` 返回忽略 scale 幅值、保留符号的方向（`mirrored` 时帧为 −rotation，见[灯光](lighting.md)）。`render_items / light_items` 只经这三个入口读取空间。

`render_items()` 按成员前序与组件顺序提取网格，结果容量等于实际网格数；零网格或纯灯光场景不分配结果存储。成员的世界矩阵来自 World 持有的一份空间缓存，`render_items`、`light_items`、`world_matrix` 与 `spatial_parent` 共用：组合关系、空间参考与 Transform 增删经 World 结构操作改变并使缓存失效；每次提取重新读取所访问 Entity 的当前 TRS，按位比较后只重组变化的链，因此 Runtime 直接写入与生成代码的写入在下一次查询即可见。Detached 对象和候选图仍使用临时求值。

## 运行位姿与验收入口

Component 的 `translate_owner(dx,dy,dz)` 记录所属 Entity 的平移命令，在行为阶段结束后由宿主边界逐条复核并批量应用；不保存字段地址。已登记模拟系统以 `writes_pose` 声明位姿的 Entity（例如物理 body 或控制器所在的 Entity）拒绝普通平移命令。`apply_simulation_transforms` 接受 Entity 世界位置/旋转，先验证并按空间依赖顺序转换，然后发布，保留局部显示 scale；不进入编辑历史。被拒绝的命令与位姿按失败策略处理。

端到端入口为 `world_acceptance`、`project_scene_acceptance` 和 `editor_document_acceptance`，开发与发布配置均需执行。官方内容使用 Scene v6；旧 v1–v5 仅通过严格导入进入同一模型。实际结果与仍未完成的图形验收统一记录在[编辑器验收状态](../editor/README.md#entity-迁移验收状态)。
