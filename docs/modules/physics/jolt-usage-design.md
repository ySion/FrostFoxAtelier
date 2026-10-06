# Jolt 使用设计方案

状态：P0-P4 的基础场景接口已在 `physic-engine` 分支实现，本文仍保留目标语义和未落地的扩展要求；实际可调用范围以[物理模块](README.md)和公开头文件为准。参考资料是仓库外的 `C:/Users/qq137/Documents/ChatGPT/UE/docs/12-ue-native-physics-design.md`，只借鉴职责、使用范式和验收方法，不移植 UE 类型、单位或默认行为。Jolt API 以当前子模块 `v5.6.0` 为核对基线，升级以主仓库 gitlink 为准。

## 1. 目标与当前边界

物理能力服务 Windows x64 游戏客户端和无窗口专用服务器。游戏、编辑器及未来自有语言使用同一 Runtime 的 Actor、Component、`ObjectId` 和生命周期；Jolt 只负责几何查询、刚体和约束求解，不建立另一套业务对象身份。当前 `atelier_physics` 已隔离 Jolt 头文件的公开边界，不为假设中的第二后端增加虚接口层。

| 能力 | 已实现 | 本方案的下一步 |
| --- | --- | --- |
| 场景与身份 | 每 World 的 `PhysicsScene`、进程级 Jolt 注册、Body/专用句柄的场景与重建代际 | 多线程关闭屏障与并发查询尚未定义 |
| 形状与运动 | Box/Sphere/Capsule、静态三角网格、运动学目标与传送 | Convex、Compound、形状偏移尚未实现 |
| 模拟 | 60 Hz、最多 8 子步、丢时统计、暂停/单步、力和冲量、场景固定步前/后回调 | World 业务系统尚未自动注册到固定步阶段 |
| 接触 | 对象对归并、Sensor、双向过滤、销毁 Exit、通知溢出 | 接触冲量与物理材质组合策略尚未公开 |
| 查询与资产 | 同步 Ray/Sphere Sweep/Sphere Overlap、工程网格离线 Cook 与版本/摘要校验 | 通用形状查询、资产导入自动 Cook、异步查询尚未实现 |
| 工具与专用系统 | 编辑器基础 Body 参数；角色、距离约束、布娃娃、四轮车基础 API | 碰撞可视化、完整专用资产流程、专用服务器可执行宿主尚未实现 |

当前调用顺序是 `PhysicsScene::update(elapsed)` 先执行一次 `World::update`，再对物理做固定子步；结构和网格变化会在生命周期通知或下一次 reconcile 时映射到 Jolt。`World::set_transform` 禁止运行时编辑，`World::apply_simulation_transform` 是受控模拟写入口。`StepResult` 已报告完成步、丢时及失效命令；接触按对象对归并。以下章节保留最初设计的推导和更完整的目标要求，其中“建议”“下一阶段”等语句不是当前 API 声明。

## 2. 使用模型与职责

| 使用者 | 持有什么 | 写入什么 | 从哪里读取 |
| --- | --- | --- | --- |
| World/宿主 | `PhysicsScene` 与其所属 Runtime；必须先销毁场景再销毁 World/Runtime | 启停、帧推进、场景重力 | 完成步结果、诊断 |
| Actor 与 `BodyComponent` | 原生注册的真实 Component；Actor 根提供世界位姿 | 停止状态配置形状、运动类别、材质及碰撞策略 | 组件状态和最近完成的体状态 |
| Gameplay/语言 | Actor/组件的 `ObjectId`，必要时使用带重建代际的物理体标识 | 受控力、冲量、运动学目标、传送命令 | 场景查询和物理事件中的已验证身份 |
| 编辑器/导入器 | 碰撞配置与资源来源 | 资产、Profile、可视化和停止状态编辑 | 验证错误、形状/过滤调试快照 |
| `atelier_physics` | 每场景的 Jolt `PhysicsSystem`、BodyID 映射、监听器及临时分配器 | 安全阶段内的后端操作 | Jolt 求解结果 |

公开头文件继续只使用引擎数学、Runtime、World 和资源类型。Jolt `BodyID`、`ShapeRefC`、监听器及锁留在 `.cpp` 或将来确有独立生命周期时拆出的私有实现文件；游戏存档、脚本、网络包均不引用 Jolt 对象。`BodyComponent` 是非空间组件，形状在 Actor 根位姿上实例化。若以后一个 Actor 需要多个可独立运动的 Body（布娃娃、车辆），由专用组件或资产明确定义骨/部件身份，不让现有单根体配置暗中变成多体数组。

项目侧操作按生命周期组织：先在未运行的 World 中组装 Actor、根组件与 `BodyComponent`，用 `physics::configure` 校验配置，再 `attach/start`；静态网格根选择三角碰撞，普通动态 Actor 选择 Box/Sphere/Capsule。运行后由宿主推进 `PhysicsScene::update`，Gameplay 对活动动态体提交力/冲量命令，事件通过 `on_contact` 在更新后消费；停止/离场自动撤体。运动学目标、传送、体状态、Ray/Sphere Sweep/Sphere Overlap 和 Sensor 已有场景入口。具名 Profile、通用 Shape Sweep/Overlap 与更多形状仍属后续扩展。

## 3. 所有权、状态与写入权

`World` 拥有 Actor 成员资格；Runtime 拥有对象身份语义；宿主拥有 `PhysicsScene`；场景拥有 Jolt Body，组件只描述配置而不持有 Jolt 指针。`ObjectId` 已含 Runtime 上下文与对象代际，但同一组件因网格/配置变化重建 Body 时 `ObjectId` 不变。因此，若公开可缓存的体句柄，必须另含场景身份和 **Body 重建代际**；当前只按 Actor 调用的 API 可继续用 `ObjectId`，调用时复验该 Actor 当前是否属于该场景并有活动体。Jolt `BodyID` 永不当持久、网络或业务身份。

| 运动类别 | 位姿主写入者 | 运行时规则 |
| --- | --- | --- |
| 静态 | 停止状态的场景编辑/加载 | 运行中位姿或形状改变通过明确的重建命令；没有逐帧回写 |
| 运动学 | Gameplay/动画提交目标 | 每个固定步按目标移动；传送是另一个命令，不能以直接写根 TRS 冒充连续运动 |
| 动态 | Jolt 求解器 | 受力、冲量、速度和显式传送经物理命令；完成步回写根，Gameplay 不覆盖同一位姿 |

建议体登记状态为 `unregistered -> pending_create -> active -> pending_destroy -> unregistered`，创建失败进入可诊断的 `invalid`。这是命令队列落地时的内部状态，不要求立刻将状态机作为第二套 Runtime 对象公开。`pending_destroy` 起拒绝新的力与查询命中；已排队的事件在派发前复验场景、Actor/组件代际及 Body 重建代际。替换网格或运动类别时明确视为旧体退出、新体创建；旧体速度/接触是否迁移不得隐式猜测，默认不迁移。一次创建失败应带 Actor、组件和资源来源返回错误，不能留下半登记体。

场景关闭先停止接收命令/查询，等待在途步进，解绑 World 生命周期观察者，清理活动接触对，再按约束、Body、形状引用、Jolt 场景的顺序释放。进程级注册仅在最后一个场景释放后注销；资源管理器持有的共享碰撞资产可长于单个场景。World 的离场、停用与对象终结不同，离场/停用只撤物理参与，重新激活按当前组件配置新建体。

## 4. 帧调度与可见时间

近期保留单宿主同步推进：一帧调用一次 `World::update(elapsed)`，然后运行零个或多个固定物理子步。物理命令由宿主/Gameplay 在安全阶段提交，并指定最早生效的物理步；序号保证同一步的创建、属性变更、施力、销毁顺序。查询只在 `Jolt::Update` 之外的安全阶段调用，读取**最后一个已完成步的后端状态**，这不是可跨线程长期持有的不可变快照。事件在本帧全部子步结束、状态回写后派发；事件回调产生的命令最早于下一次帧推进的安全点生效。

```text
帧输入 / World::update 一次
-> reconcile 生命周期和资源变化，应用结构命令
-> 对每个固定子步：取目标与力/冲量 -> Jolt::Update
   -> 回写动态根位姿 -> 记录步号、状态和原始接触
-> 发布最后完成步，归并并派发事件
-> Gameplay/渲染读取完成态；下一帧再消费事件回调的命令
```

`step_id` 独立于渲染帧号；查询结果、体状态和事件都标明来源步号。没有子步时继续读上次完成态；场景初建的静态/运动学体可视为初始步 `0`。固定步目前是 `1/60 s`、最多 8 步且积压会被截断；`StepResult` 已提供剩余/丢弃时长和子步计数，决定是否变更上限前仍需真实负载测量。暂停时不推进 Jolt，单步只推进一个固定步；前回调失败保留单步请求，后回调失败不回滚或重复已完成步。`fixed_step` 场景已验证这些边界及 30/120 Hz 宿主得到相同固定步结果；编辑器尚未提供运行时模拟控制。

现有 `World::update` 只按显示帧执行一次，不能把多次物理子步中的角色控制、动画根运动或持续施力伪称为每步 Gameplay Tick。`PhysicsScene::on_before_step` 和 `on_after_step` 为宿主提供每子步的业务阶段；前者提交的命令在当前步执行，后者可读取该步完成态并向后续步提交命令。World 业务系统尚未自动绑定这些阶段，持续力需由宿主在前回调逐步提交。若未来改为异步物理，先定义输入快照、发布屏障、查询锁/快照与关闭等待，再开放工作线程调用。

## 5. 碰撞配置、形状与场景查询

UE 的 `CollisionEnabled`、`ObjectType`、`TraceChannel`、Profile 和 `Ignore/Overlap/Block` 可借鉴为**使用语义**，但 Jolt Object Layer/BroadPhase Layer 不是 UE 通道的同构类型。当前公开对象类别、双向响应、Sensor 和查询过滤；具名 Profile、独立查询/模拟参与开关仍未实现。双方均 Block 且非 Sensor 才有求解阻挡，任一 Ignore 不相遇，至少一方 Overlap 且无 Ignore 只通知重叠，已通过 18 组双向响应与 Sensor 组合验证。过滤规则在模拟和查询适配中执行，后端 `stationary/moving` 两层仅用于粗分。Sensor 可被查询；查询专用代理、仅模拟体仍需另行定义，不把 `IsSensor` 等同于 UE 的 QueryOnly。

| 用途 | 推荐形状/运动 | 调用方应配置或检查 |
| --- | --- | --- |
| 地面、建筑 | 静态简单碰撞或静态三角网格 | 资产缩放、面朝向、材质、是否参与角色查询 |
| 箱子、道具、投射物 | 动态 Box/Sphere，后续 Convex/Compound | 质量/密度、阻尼、CCD、阻挡/事件、速度上限 |
| 门、升降台 | 运动学简单形状 | 每步目标、平台速度、接触与角色继承策略 |
| 伤害区、交互区 | 独立 Sensor/查询体 | Overlap 配对、进入/离开、销毁时退出；不依赖阻挡冲量 |
| 角色 | 专用胶囊和运动控制器 | Sweep、地面、坡度、台阶、跳跃；不用自由动态刚体代替角色规则 |

查询从 `PhysicsScene` 暴露 Ray、Shape Sweep、Overlap 三类值接口，返回引擎身份而非 Jolt 指针。请求明确场景、起终点/形状、过滤配置、忽略 Actor/组件、`any/closest/all` 模式和容量；结果包含 `step_id`、命中列表、`has_block`、`overflow`。`all` 结果按距离比例、体代际和子形状 ID 稳定排序；截断必须显式报告。Hit 区分阻挡与重叠、初始穿透、位置/法线、形状子 ID、可选物理材质。未命中和初始穿透不得伪造普通表面接触点。首次实现先提供同步安全阶段查询，之后才考虑异步查询和持久快照。

形状使用顺序：当前 Box/Sphere/Capsule 参数化，静态三角网格读取不可变 CPU MeshRef；Convex、局部形状偏移、Compound/Heightfield 按后续玩法需求扩展。动态三角网格仍拒绝。根 TRS 位置、xyzw 旋转、严格正缩放遵循 World 规则；当前原始 Box/Sphere/Capsule 根须单位缩放，三角网格在创建时烘入根缩放。将来支持非均匀缩放时，逐形状定义可烘焙组合，不能偷偷改变质量或惯性。

资源导入链建立后再增加离线 `ShapeAsset` Cook：源网格及碰撞配置 -> 版本化碰撞资产 -> 场景加载。缓存键至少包括源内容、导入/Cook 版本、Jolt 版本、平台与缩放；文件验证格式版本、长度和内容摘要。编辑器和运行时共享资产描述，但不在场景加载关键路径上临时对大型网格做复杂 Cook。`MeshRef` 目前不是持久资产 ID，存档需保存可解析资源来源而不是进程内指针。

建议公开单位为米、千克、秒、弧度，保持当前 `+Y` 向上和 `-9.81` 默认重力的数值含义；这是需在资源导入、渲染和物理之间正式锁定的**提案**，不能直接照抄 UE 的厘米、`+Z` 示例。当前 `Vector3`/World 位置为 float，Jolt 也按单精度构建；大世界原点迁移和精度策略留待实际地图规模验证。物理材质与渲染材质分离，摩擦、恢复系数及密度的组合规则在引擎层声明，并以 Jolt 接触结果验收。

## 6. 事件、Jolt 映射与错误边界

| 引擎职责 | Jolt 5.6.0 对应设施 | 适配层必须补的语义 |
| --- | --- | --- |
| 场景/固定步 | `PhysicsSystem::Init/Update`、`TempAllocator`、`JobSystem` | World 生命周期、步号、补步与错误传播 |
| Body 与形状 | `BodyCreationSettings`、`BodyInterface`、Shape Settings/ShapeRef | Actor/组件及重建代际映射、配置校验、资源引用 |
| 碰撞过滤 | Object Layer、BroadPhase Layer、Pair/Object filters | Profile、双向响应、查询与模拟分别过滤 |
| 空间查询 | NarrowPhaseQuery 的 Ray/Shape Cast、Shape Collide | 忽略集合、排序、容量、Hit 语义和身份复验 |
| 接触/触发 | `ContactListener` 的 Added/Persisted/Removed | 配对归并、退出原因、完成步派发和事件预算 |
| 运动控制 | `MoveKinematic`、力/冲量及位姿接口 | 固定步目标、传送速度策略、动态体回写 |

Jolt 回调只收集原始 BodyID/子形状及必要的值数据，不访问 World/Runtime、脚本、音频或编辑器，也不在回调中删 Body。完成 `Update` 后多个接触点归并成对象对，公开 began/persisted/ended 与 trigger 标志；停用和卸载 Exit 带 `body_removed` 原因。`contacts` 与 `collision_matrix` 已覆盖配对通知、触发响应和停用退出；更多接触拓扑与容量压力仍需专门场景，不能从基础验收推断已全部覆盖。求解后的冲量不在当前公共事件中承诺数值；通知溢出通过 `StepResult` 计数并标记不完整。

无效尺寸、NaN、动态三角网格、跨 World 身份、无活动动态体施力、缺失/损坏形状资产等都返回可定位错误。查询容量截断不是“无命中”。后端升级必须重跑开发与发布构建中的真实物理验收，以及过滤、查询、事件、Cook 兼容性和代表性性能场景；不假定旧 Cook 二进制可读或跨机器浮点逐位一致。

## 7. 落地顺序与端到端验收

| 阶段 | 交付结果 | 必须通过的场景 |
| --- | --- | --- |
| P0 已有基线 | Jolt 源码/构建、World 体、网格地面、重力/力/冲量、开始/结束接触 | 现有 `physics_acceptance` 在 dev/release 通过 |
| P1 时序与身份 | 步号、可观测积压、命令安全点、运动学目标/传送、重建代际 | 不同显示帧率下相同步输入；平台移动；重建后旧命令失效；World 停止清理 |
| P2 碰撞与查询 | Profile/过滤、Sensor、Ray/Sweep/Overlap、稳定结果 | 射击/视线、体积预检、触发区；逐对过滤矩阵；初始穿透、容量溢出和卸载 |
| P3 资产与工具 | 基本物理材质、离线 Cook、编辑器配置/可视化 | 资产版本升级/损坏报错；场景反复加载；编辑器与无窗口宿主同结果 |
| P4 玩法专用 | 角色运动；随后按游戏需求增加约束/布娃娃、车辆等 | 坡度/台阶/跳跃/移动平台；约束限位与生命周期；对应专用场景 |

验收使用真实 Jolt、World 和资源路径，不建立 mock 物理世界。当前 15 个可独立运行的场景及 dev/release 证据见[物理验收](acceptance.md)，覆盖已公开的基础 API；上表包含尚未实现的目标，不代表 Profile、可视化或全部关节限位已交付。专用服务器宿主出现后，使用相同场景输入比较可观察状态与事件顺序；结果允许声明过的浮点容差，不承诺跨平台位级确定。后续性能工作再补充每步活动/休眠体、接触对、查询量和耗时统计，性能预算由目标机器测量后确定。

## 8. 实施前需要锁定的决策

1. 将 `+Y`、米/千克/秒/弧度、坐标手性及导入变换确认为项目统一约定；明确现有资产的迁移范围。
2. P1 固定步前/后 Gameplay 阶段与当前每显示帧一次 `World::update` 的关系，尤其持续力、动画目标和同帧多子步的可见性。
3. P2 Profile 的持久格式、类别数量、双向响应与查询通道规则；Sensor 的查询/模拟参与方式需以 Jolt 5.6.0 实测定案。
4. Body 重建时的速度、睡眠、约束、接触 Exit 和外部句柄失效规则；跨 World 迁入后的新体身份。
5. 静态网格 Cook 所用持久资源标识、格式版本与错误定位路径；在资产系统具备稳定来源前不冻结二进制格式。

这些决策分别在对应阶段实现前形成可审查的接口和真实场景验收，不要求为了本设计稿先建立未被游戏使用的通用后端框架。

Jolt 官方资料：[架构与角色控制器](https://jrouwe.github.io/JoltPhysics/)、[CharacterVirtual API](https://jrouwe.github.io/JoltPhysics/class_character_virtual.html)、[VehicleConstraint API](https://jrouwe.github.io/JoltPhysics/class_vehicle_constraint.html)、[Ragdoll API](https://jrouwe.github.io/JoltPhysics/class_ragdoll.html)。本分支仍以仓库内锁定的 `v5.6.0` 源码和 dev/release 实际构建为最终 API 基线。
