# 物理模块

`atelier_physics` 使用仓库锁定的 Jolt 源码，公开接口只暴露项目的 Runtime、World、数学值和资源身份。本文是当前物理模型与接口边界的入口；外部设计依据见 [UE 物理对照](../../research/ue-physics-integration-2026-10.md)与 [Jolt 版本契约](../../research/jolt-integration-contracts-2026-10.md)。

## 组件与空间关系

先在 Runtime 创建对象前依次调用 `world::register_types(runtime)` 和 `physics::register_types(runtime)`。一个运行中的 World 对应一个由宿主持有的 `PhysicsScene`，场景先于 World/Runtime 销毁；同进程多个场景共享 Jolt 全局注册。`PrimitiveComponent : SceneComponent` 统一持有反射的 `BodySettings body`；`StaticMeshComponent` 默认关闭 body，`CollisionComponent` 默认开启且不参与网格呈现。二者使用同一物理注册、查询和生命周期入口，运行时不再注册 `BodyComponent`。一个 Actor 可以拥有多个独立物理 Primitive，Actor 只是拥有者。

Box、Sphere、Capsule 支持静态、运动学和动态体。Box 烘入正世界缩放；Sphere/Capsule 要求均匀世界缩放；静态三角网格使用该 StaticMeshComponent 自己的网格及正世界缩放。剪切变换拒绝；运动体的空间祖先要求均匀缩放，以保证模拟旋转可无损转换回局部 TRS。

父子挂接不产生物理焊接。动态子体独立模拟，纯视觉后代照常跟随。当前不支持 compound，也不让静态/运动学 body（包含 Sensor）隐含跟随移动的物理祖先；此组合明确报错，应使用独立空间分支。Character/Ragdoll 根下新增普通物理子体也明确拒绝，避免多套姿态驱动交错。

## 固定步与位姿写入

`PhysicsScene::update(elapsed)` 每帧先执行一次 `World::update`，再按 60 Hz 最多推进 8 个固定子步。`StepResult` 报告完成步号、子步数、剩余/丢弃时间、失效命令和接触通知溢出。`set_paused`、`step_once` 控制子步；`on_before_step` 在消费命令前调用，`on_after_step` 在求解、位姿回写与接触收集后调用。运动学目标保留到被下一目标或传送替换，每个子步重新计算速度，到达后停止。位姿以物理体世界状态为准；场景先缓存全部刚体求解结果，再按空间深度父先子后回写 Primitive，保留局部缩放。移动使用力、冲量、运动学目标或显式传送；直接改写作者变换不替代这些物理命令。

暂停下重复 `step_once` 合并为一次请求；前回调失败时请求保留，修复回调后可重试；求解完成后的回调失败不重复执行已完成步。当前使用单宿主同步推进，求解期间不开放外部 World/Runtime 访问或并发查询；Jolt 接触回调只收集值数据，业务通知在求解结束后交付。World 的普通 Tick 每显示帧执行一次，每个固定子步的业务工作通过场景前/后回调接入；尚无通用 World TickGroup。

## 身份、碰撞与查询

`body_handle` 接受 Primitive 身份；传 Actor 时只选择它的显式根，不任意选取一个子刚体。`BodyHandle` 包含 Actor、组件、场景与重建代际；查询返回同一 handle，状态读取和命令验证其有效性。`BodySettings` 提供 16 类对象的双向 Ignore/Overlap/Block 响应、Sensor、摩擦和恢复系数。两个不相交的 16 位类别掩码表示 Ignore/Overlap，其余为 Block；C++ 可使用 `response`/`set_response`，底层使用同一值。双方 Block 才阻挡，Ignore 优先，Overlap/Sensor 不求解阻挡。接触按刚体对归并为 began/persisted/ended，卸载的 Exit 标记 `body_removed`。`ContactEvent::first/second` 保存历史 BodyHandle；刚体移除后的结束事件仍可交付，旧 handle 不能操作同组件重建的新 body。事件在本帧子步结束后交付，其新命令从后续固定步生效。

每场景预留 4 MiB 物理临时内存，超出时使用 Jolt 官方 `TempAllocatorImplWithMallocFallback` 从通用分配器申请额外内存，并在求解释放时归还。4 MiB 是预留容量，不再是触发进程终止的固定上限；这不保证系统内存耗尽可恢复。刚体与接触预算仍保持现有容量，当前仍采用同步单线程求解。

同步 `raycast`、`sweep_sphere`、`overlap_sphere` 返回带完成步号的 Primitive 命中，支持类别、忽略 Actor/组件、any/closest/all 和容量截断。虚拟角色与布娃娃的专用刚体尚未进入普通查询身份映射。

`PhysicsScene::trace` 提供参考 UE 的通道/对象类型选择和 test/single/multi 模式，`SceneQueryComponent` 把射线、球扫掠、球重叠配置接入 Scene v2、固定步采样和脚本原生调用。同 Actor 可挂多个查询，默认忽略自身；停用或离场清空活动结果，保存只保留配置。完整语义、C++/脚本示例及限制见[场景查询与射线组件](scene-queries.md)，官方 `Raycasts.scene` 提供六种配置。

## 资源、编辑与场景保存

`cook_triangle_mesh` 将静态网格生成带格式版本、Jolt 版本、源身份、缩放和摘要的二进制形状；读取时验证原始网格与 Cook 数据一致。`Project::cook_collision` 将工程 OBJ 的形状写入 `Cache/Physics`，`Project::collision` 校验并读取。编辑器场景文件持久化组件配置及网格来源，工程网格加载需要对应的有效 Cook 缓存，缺失、损坏或上游 Jolt 版本变化须重新 Cook。内置网格可直接从 CPU 网格创建三角形状，不生成工程缓存。场景加载先恢复网格、局部 TRS 与空间关系，再按 Primitive 世界缩放配置物理。

编辑器沿 Actor Structure 的统一入口添加 `CollisionComponent`；网格已有物理能力，可直接编辑 Parameters 中的 `body.enabled`、形状、运动模式、尺寸、碰撞类别/掩码及材料。`BodySettings` 和 `PhysicsMaterial` 由头文件反射生成，Source/JIT 直接访问同一字段。Scene v2 沿通用 `properties.body` 保存配置，没有第二份物理配置记录；`Session::configure_body` 仍提供完整配置事务。旧 v1/v2 的独立 Body 文档只在读取时迁移到根 Primitive，保存即采用新模型；不保留旧运行组件。不能无损迁移的自定义非 Primitive 根或多 Body 旧文档明确报错。碰撞可视化及 Cook 管理页尚未实现。

官方 [Examples/Physic](../../../assets/examples/Examples/Physic/README.md) 提供 10 个可保存、重开的组件场景。通过 Project → Examples → Physic 或 `atelier_editor --physic` 打开；编辑场景保持静止，Project → Run 从快照创建独立运行会话，经 Session 统一推进 World 和 Jolt。原 [物理验收夹具](../../../acceptance/editor/fixtures/physics-project/README.md)继续验证既有 Body 场景兼容性。

## 生命周期与专用控制器

Character、Ragdoll/RagdollPart、Vehicle/VehicleWheel、DistanceConstraint、Force、KinematicMotion 的组件协调器沿 PhysicsScene 管理专用后端；重复 reconcile 保留有效句柄。普通 Primitive 的停用或卸载只撤销自身及依赖它的约束；祖先停用同时撤销其空间后代物理体。Force/KinematicMotion 等 Actor 控制器选择显式根刚体。根 Primitive、Character、Ragdoll 不能同时驱动根位姿；`translate_owner` 不得覆盖物理控制的 Actor。专用控制器的形状/关节/悬挂在激活时采样，实时输入每个固定步读取。

布娃娃部件与车轮继承 StaticMeshComponent，作为单位缩放根的直接子组件，通过 World 的受控局部模拟入口更新姿态，保留网格缩放。约束目标为 Runtime 跟踪的弱 Actor 引用，停止后用 `set_constraint_target` 绑定；Scene v2 的 `constraint_target` 保存从 1 开始的场景 Actor 顺序（0 为空），加载全部 Actor 后恢复引用，拒绝越界身份或跨 World 活跃目标。句柄不写入文档，目标选择器和专用资产编辑面板尚未提供。

专用场景 API 包含 `CharacterVirtual` 胶囊角色的移动、跳跃、坡道/台阶与移动平台，两个 Body 之间的距离约束，基于 Jolt Ragdoll 的多部件布娃娃，以及四轮车辆的油门、转向和刹车。角色由场景每固定步调用 `ExtendedUpdate`，不以自由动态刚体代替角色控制。角色和布娃娃要求已启用、单位缩放的根组件；布娃娃局部部件位置按根旋转转换。根 Primitive 启用普通 body 时会撤销其角色或布娃娃，使同一根只有一个物理位姿写入者；旧专用句柄立即失效。布娃娃和车辆的专用句柄也带场景与代际，销毁、停用和 World 停止后失效。当前只提供这些玩法系统的基础配置与状态读取；骨骼动画映射、车辆资产参数、关节限位编辑器和存档/网络格式尚未建立。

显式删除距离约束或移除任一端点都会唤醒幸存端点，解除支撑后继续受重力影响；同 Actor 的不同 Primitive 可作为两个端点。角色、布娃娃创建同时检查 Actor 活跃状态和所属 World；共享同一 Runtime 不代表可以跨 World 使用 Actor，detach/转移后须在新场景重新创建专用句柄。

## 验收与边界

无窗口真实 World/Jolt 验收使用：

```powershell
cmake --build --preset dev --target physics_acceptance editor_document_acceptance
cmake --build --preset release --target physics_acceptance editor_document_acceptance
```

`physics_acceptance` 提供 20 个独立场景，逐项输出 RUN/PASS/FAIL。`physics_examples_acceptance` 验证 10 个官方场景的真实磁盘往返、Jolt 模拟和编辑器 GPU 画面。完整覆盖矩阵、单场景重跑方法和结果见[物理验收](acceptance.md)。`editor_document_acceptance` 覆盖工程缓存、Primitive 配置历史和场景文档；`editor_acceptance` 的真实窗口用例包含 Actor Structure 添加/删除 CollisionComponent 及撤销，运行时需要可授予前台焦点的桌面会话；该原生交互入口未计入本次 GPU 验收结果。

尚未提供 Convex/Compound/Heightfield、焊接、独立查询/模拟参与开关、具名碰撞 Profile、异步查询、多线程求解或专用服务器可执行宿主。当前验收覆盖已公开行为，没有测量大规模吞吐，也不承诺跨平台位级确定性。
