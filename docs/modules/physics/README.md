# 物理模块

`atelier_physics` 使用仓库锁定的 Jolt 源码，公开接口只暴露项目的 Runtime、World、数学值和资源身份。本文是当前物理模型与接口边界的入口；外部设计依据见 [UE 物理对照](../../research/ue-physics-integration-2026-10.md)与 [Jolt 版本契约](../../research/jolt-integration-contracts-2026-10.md)。

## 组件与空间关系

先在 Runtime 创建对象前依次调用 `world::register_types(runtime)` 和 `physics::register_types(runtime)`。一个运行中的 World 对应一个由宿主持有的 `PhysicsScene`，场景先于 World/Runtime 销毁；同进程多个场景共享 Jolt 全局注册。`PrimitiveComponent : Component` 统一持有反射的 `BodySettings body`；`StaticMeshComponent` 默认关闭 body，`CollisionComponent` 默认开启且不参与网格呈现。二者使用同一物理注册、查询和生命周期入口，运行时不再注册 `BodyComponent`。空间由 owner Entity 的可选内联 TRS 提供；每个 Entity 的位姿写入者单独验证，多个独立物理部件使用子 Entity，组件不再形成第二棵空间树。

Box、Sphere、Capsule 支持静态、运动学和动态体。世界缩放先按 World 规则分解为真旋转与带符号 scale；Box 烘入各轴缩放的绝对值，Sphere/Capsule 要求各轴绝对值相同。这三种形状没有局部偏移且关于各局部轴对称，镜像只改变符号，不影响碰撞；以后加入形状偏移或非对称形状时须重新处理反射。静态三角网格把带符号的世界缩放烘入顶点，并按作者法线的逆转置决定正面，镜像后正反面不变；Cook 数据以带符号缩放为键。剪切变换拒绝；运动体的空间祖先要求各轴缩放绝对值相同，以保证模拟旋转可无损转换回局部 TRS，镜像祖先的回写保持作者局部 TRS。Character 与 Ragdoll 要求单位且未镜像的世界缩放；车轮显示 Entity 可以镜像。零缩放的空间不建刚体，只在最上层零缩放节点报告一条 `degenerate_value` 警告，恢复非零缩放后重新协调即重建刚体。

父子挂接不产生物理焊接。动态子体独立模拟，纯视觉后代照常跟随。当前不支持 compound，也不让静态/运动学 body（包含 Sensor）隐含跟随移动的物理祖先；此组合明确报错，应使用独立空间分支。Character/Ragdoll 控制器 Entity 下新增普通物理子体也明确拒绝，避免多套姿态驱动交错。

## 固定步与位姿写入

PhysicsScene 创建时向 World 登记为模拟系统，析构时注销；`World::update(dt)` 在 pre_physics 阶段之后调用它的 step。创建时还把 Primitive、Character、Ragdoll 组件登记为 World 能力（见 [World](../world/README.md)），World 随组合、成员和启用的每次变化维护这些桶，同一 World 上重建的 PhysicsScene 复用同一登记。step 只协调可能参与物理的 Entity：桶中启用且 body 已启用的 Primitive 的 owner、桶中启用的 Character/Ragdoll 的 owner，以及已有刚体或专用后端的 Entity；body 设置每帧采样，因为 Runtime 直接写字段不发通知；Entity 的空间祖先链（成员、启用、缩放、祖先上移动的物理体、父节点）同样每步重读：同一次协调中每个祖先只读一次、由全部子孙刚体共用，协调结束即丢弃，不跨步缓存；其余成员不进入逐帧检查。随后按 60 Hz 最多推进 8 个固定子步，超出上限的时间丢弃。`last_step()` 返回最近一次 step 的 `StepResult`：完成步号、子步数、剩余/丢弃时间、失效命令和接触通知溢出。登记的 `writes_pose` 声明物理写入位姿的 Entity：已有 body、角色、布娃娃或绑定目标的 Entity，以及带有启用的 Character / Ragdoll 组件的 Entity（控制器在下一次 step 创建之前即已声明）；`translate_owner` 对它们逐条拒绝。启用但被隔离拒绝的控制器（例如镜像 Entity 上的 Character、同一 Entity 上的双控制器）同样声明位姿：作者配置了控制器，位姿就归物理，该 Entity 的平移命令持续被拒绝，控制器修好后也不会出现位姿跳变。`set_paused`、`step_once` 控制子步；`on_before_step` 在消费命令前调用，`on_after_step` 在求解、位姿回写与接触收集后调用。运动学目标保留到被下一目标或传送替换，每个子步重新计算速度，到达后停止。位姿以物理体世界状态为准。每个固定子步只回写本步移动过的刚体：求解后仍活跃的、本步内转入睡眠的、被传送的，以及空间祖先在本步被回写的刚体（保持其世界位姿）；Character、Ragdoll 与专用后端照常回写。静态和保持睡眠的刚体不回写。任何途径写入刚体所在 Entity 的 TRS（经 World、Runtime 直接写入、Source）都与 `set_transform` 等效、传送刚体并保留速度：经 World 的写入立即经下文的 `relocated` 通知物理；绕过 World 的写入在下一步协调时发现：场景记录每个刚体创建、传送或回写时 owner 的局部 TRS，协调时逐位比较，不一致即按 `relocated` 的路径传送。宿主在 `on_before_step` 中写入本步要回写的刚体，其值被本步回写覆盖。已知限制：非物理祖先只有经 `set_transform` 移动时才传送子孙刚体；经 `translate_owner` 或绕过 World 的写入移动祖先时，子孙刚体不随之传送，待 Runtime 写代号与空间子孙反向索引落地后补齐。场景先收集这些求解结果，再按 Entity 空间依赖顺序整批回写 Entity，保留局部缩放。移动使用力、冲量、运动学目标或显式传送。宿主在运行中直接编辑物理控制 Entity 的变换时（例如 Play 收到编辑器的修改），World 经 `relocated` 通知物理：刚体（含 Vehicle 底盘）在下一步按新的世界位姿传送并保留速度，Character 立即设置位置与朝向；Ragdoll 及其部件、Vehicle 车轮的位姿跟随控制器，这类编辑以 `unsupported_operation` 拒绝并恢复原 TRS；编辑它们的祖先同样使整次变换编辑失败（Play 的增量应用记为 unsupported 跳过）。缩放变化仍由逐步协调重建刚体。运行中 keep_local 的结构编辑（reparent、空间参考）不通知物理，只依赖下一次逐步协调；当前宿主路径都按 keep_world 改结构，再经 `set_transform` 写最终局部变换。

step 的 CPU 墙钟记为[帧阶段计时](../../foundation.md#帧阶段计时)的 `physics.*` 阶段（标记见 [physics/timing.hpp](../../../src/physics/timing.hpp)），挂在 World 的 `world.simulation` 下：`physics.step` 包含逐帧协调 `physics.reconcile`（成员 body 协调与组件协调）、每个固定子步一次的 `physics.substep`，以及子步全部完成后的接触通知分发 `physics.events`。子步内 `physics.solve` 是 Jolt 更新的墙钟，`physics.write_back` 覆盖位姿读出与 Entity 回写。

暂停下重复 `step_once` 合并为一次请求，由完成的求解消耗。step 内任何失败（包括前/后回调）都停止 World（Jolt 的缓冲满不是失败，见下文 `Capacity`），已完成的求解保留步号，World 停止丢弃待处理的单步请求与命令。当前使用单宿主同步推进，求解期间不开放外部 World/Runtime 访问或并发查询；Jolt 接触回调只收集值数据，业务通知在求解结束后交付。World 的 Tick 阶段每显示帧各执行一次（pre_physics 在全部子步之前，post_physics 与 late 在之后），每个固定子步的业务工作通过场景前/后回调接入。`on_before_step`、`on_after_step`、`on_contact` 是宿主 C++ 回调，失败按 World 级停机；将来向 Source 开放接触事件时，必须经 World 的逐对象隔离分发，不能直接走这几个回调。

物理失败遵循 World 的[失败策略](../world/README.md#失败策略与诊断)，诊断以 physics 阶段写入同一 World 日志。isolate 下，普通 body 协调失败的 Entity 不保留普通 body，其余 Entity 照常模拟；位姿写入者冲突时拒绝普通 body，保留控制器组件；同一 Entity 上有多个位姿控制器（Character、Ragdoll 任意组合或同类重复）时，该 Entity 上写入位姿或依赖 body 的控制器组件（Character、Ragdoll、Vehicle、Force、KinematicMotion、DistanceConstraint）全部拒绝，与创建先后无关，只读 owner 空间的 Scene Query 照常工作；控制器组件逐项创建、采样与回写，失败项不创建或跳过该步，下一步重新采样；被拒绝的位姿不回写。halt 保持原行为：首个失败使该次 `World::update` 或生命周期调用失败。

## 身份、碰撞与查询

`body_handle` 接受 Primitive 身份；传 Entity 时只解析其自身启用的 Primitive，不从子 Entity 猜选刚体。`BodyHandle` 包含 Entity、组件、场景与重建代际；查询返回同一 handle，状态读取和命令验证其有效性。`BodySettings` 提供 16 类对象的双向 Ignore/Overlap/Block 响应、Sensor、摩擦和恢复系数。两个不相交的 16 位类别掩码表示 Ignore/Overlap，其余为 Block；C++ 可使用 `response`/`set_response`，底层使用同一值。双方 Block 才阻挡，Ignore 优先，Overlap/Sensor 不求解阻挡。接触按刚体对归并为 began/persisted/ended，卸载的 Exit 标记 `body_removed`。`ContactEvent::first/second` 保存历史 BodyHandle；刚体移除后的结束事件仍可交付，旧 handle 不能操作同组件重建的新 body。事件在本帧子步结束后交付，其新命令从后续固定步生效。

每场景预留 4 MiB 物理临时内存，超出时使用 Jolt 官方 `TempAllocatorImplWithMallocFallback` 从通用分配器申请额外内存，并在求解释放时归还。4 MiB 是预留容量，不再是触发进程终止的固定上限；这不保证系统内存耗尽可恢复。刚体、刚体对与接触约束的容量（`Capacity`）在创建 PhysicsScene 时固定，Jolt 之后不能扩容：宿主可以显式指定，填 0 的项按创建时 World 里的 Primitive 数 N 推导：刚体数为 max(4096, 2N) 向上取到 2 的幂，刚体对为它的 8 倍，接触约束为它的 2 倍（Jolt Samples 的比例），`capacity()` 返回实际值（刚体对与接触约束至少为 4，与 Jolt 一致）；超过 Jolt 上限的显式值拒绝创建。刚体对或接触缓冲满时 Jolt 只丢弃多出的接触（对应物体可能互相穿过），模拟照常继续：该子步计入 `StepResult::contact_overflows`，并记一条 `capacity_exceeded` 物理警告（字段 `body_pairs` 或 `contact_constraints`，持续时合并计数），任何失败策略下都不停机。缓冲满时 Jolt 会把仍保留的静止接触报告为移除，之后只报告持续，因此刚体对的接触计数归零时先向 Jolt 核实，仍在接触就继续发 persisted，缓冲恢复后接触事件照常；开发构建中 Jolt 对这一结果的断言也不中断进程，其余 Jolt 断言照常中断。求解使用进程内共享的 Jolt `JobSystemThreadPool`：第一个 PhysicsScene 创建时建立、最后一个析构时销毁，所有 World 与运行时上下文中的场景共用；工作线程数为逻辑核数减一（至少 1、至多 3：在 500 个活跃箱子上，3 个工作线程比 1、7、15 个都快），发起 step 的线程同时执行任务，不同线程上并发的 step 各占一个屏障（上限 8）。同一二进制、同一 API 调用顺序下结果与线程调度无关，`solver_determinism` 逐位检查同一线程数下的两次运行；1、3、7、15 个工作线程的位姿也实测逐位一致，但不在常驻验收中。

同步 `raycast`、`sweep_sphere`、`overlap_sphere` 返回带完成步号的 Primitive 命中，支持类别、忽略 Entity/组件、any/closest/all 和容量截断。虚拟角色与布娃娃的专用刚体尚未进入普通查询身份映射。

`PhysicsScene::trace` 提供参考 UE 的通道/对象类型选择和 test/single/multi 模式，`SceneQueryComponent` 把射线、球扫掠、球重叠配置接入 Scene v6、固定步采样和脚本原生调用。同 Entity 可挂多个查询，默认忽略自身；停用或离场清空活动结果，保存只保留配置。完整语义、C++/脚本示例及限制见[场景查询与射线组件](scene-queries.md)，官方 `Raycasts.scene` 提供六种配置。

## 资源、编辑与场景保存

`cook_triangle_mesh` 将静态网格生成带格式版本、Jolt 版本、源身份、缩放和摘要的二进制形状；读取时验证原始网格与 Cook 数据一致。这段头部就是 `BodySettings.cooked_shape` 的输入键：物理在建三角网格刚体时校验，形状不是 Triangle Mesh 时忽略该缓存，因此编辑 body 字段不清除它。输入键与当前源网格或世界缩放不一致时（例如运行中缩放网格碰撞体），物理在内存中按当前缩放重新生成形状，不写缓存，并计入 `StepResult::runtime_cooks`；格式、Jolt 版本或摘要不符仍拒绝。`Project::cook_collision` 将工程 OBJ 的形状写入 `Cache/Physics`；`Project::collision` 校验并读取，缓存缺失或不一致（源网格、缩放或上游 Jolt 版本变化）时从源网格重新 Cook 并写回。编辑器场景文件持久化组件配置及网格来源，加载不要求缓存预先存在。内置网格可直接从 CPU 网格创建三角形状，不生成工程缓存。场景加载先恢复网格、局部 TRS 与空间关系，再按 Primitive owner Entity 世界缩放配置物理。

编辑器沿 Entity Structure 的统一入口添加 `CollisionComponent`；网格已有物理能力，可直接编辑 Parameters 中的 `body.enabled`、形状、运动模式、尺寸、碰撞类别/掩码及材料。`BodySettings` 和 `PhysicsMaterial` 由头文件反射生成，Source/JIT 直接访问同一字段。单字段规则是反射硬边界（尺寸、摩擦非负，恢复系数 [0,1]，类别 [0,15]，掩码不超过 16 位），写入越界时夹到边界；跨字段规则是 BodySettings 的类型校验器：形状与运动类型必须是已声明值，尺寸为正，Ignore 与 Overlap 掩码不相交，Triangle Mesh 只能 Stationary。`configure`、逐步协调与场景加载经 Runtime 检查同一规则。Scene v6 沿通用 `properties.body` 保存配置，没有第二份物理配置记录；`Session::configure_body` 仍提供完整配置事务。旧 v1–v5 只在严格导入时恢复到 Entity + 能力组件图，旧独立 Body 配置合并到对应 Primitive，保存即采用 Scene v6；不保留旧运行组件。不能无损迁移的自定义非 Primitive 根或多 Body 旧文档明确报错。碰撞可视化及 Cook 管理页尚未实现。

官方 [Examples/Physic](../../../assets/examples/Examples/Physic/README.md) 提供 10 个可保存、重开的组件场景。通过 Project → Examples → Physic 或 `atelier_editor --physic` 打开；编辑场景保持静止，Project → Run 从快照创建独立运行会话，经 Session 统一推进 World 和 Jolt。原 [物理验收夹具](../../../acceptance/editor/fixtures/physics-project/README.md)继续验证既有 Body 场景兼容性。

## 生命周期与专用控制器

Character、Ragdoll/RagdollPart、Vehicle/VehicleWheel、DistanceConstraint、Force、KinematicMotion 的组件协调器沿 PhysicsScene 管理专用后端；重复 reconcile 保留有效句柄。普通 Primitive 的停用或卸载只撤销自身及依赖它的约束；组合祖先停用会撤销组合子树的物理参与。空间参考只决定坐标，参考目标停用不会使独立组合的 owner 失去启用资格。Force/KinematicMotion 等控制器选择 owner Entity 的刚体。Primitive、Character、Ragdoll 不能同时驱动同一 Entity 位姿；`translate_owner` 不得覆盖物理控制的 Entity。专用控制器的形状/关节/悬挂在激活时采样，实时输入每个固定步读取。

同 World 内重挂后，若启用资格、配置与后端绑定仍有效，只更新空间依赖；不重复施加初始冲量、不重置运动学计时，也不重新创建专用句柄。真正改变配置、绑定或参与资格时仍由同一协调器重建。

布娃娃和车辆通过公开强 Entity 引用序列 `RagdollComponent.parts / VehicleComponent.wheels` 显式绑定目标。每个目标必须是控制器 owner 的直接组合子 Entity，空间参考指向同一 owner；`parts` 顺序决定后端部件索引，`wheels` 每个目标的 wheel_index 必须位于 0–3 且不重复；显示目标可为四轮中的子集。目标保存普通 RagdollPartComponent / VehicleWheelComponent 元数据和独立 StaticMeshComponent；metadata 不继承网格、不拥有空间。目标显示缩放保留，控制器 owner 要求单位世界缩放，部件物理尺寸由配置决定。

姿态整批写回目标 Entity，保留显示 scale；目标 mesh 的 body 关闭，避免重复姿态写入者。绑定变化、停用、删除、离场或元数据失效会撤销专用后端；不会从组件目录隐式猜测部件。距离约束 target 是 Runtime 弱 Entity 引用，停止后用 `set_constraint_target` 绑定；Scene v6 统一保存 `properties.target` 文档引用，旧场景序号只在导入边界处理。目标选择器和专用资产编辑面板尚未提供。

专用场景 API 包含 `CharacterVirtual` 胶囊角色的移动、跳跃、坡道/台阶与移动平台，两个 Body 之间的距离约束，基于 Jolt Ragdoll 的多部件布娃娃，以及四轮车辆的油门、转向和刹车。角色由场景每固定步调用 `ExtendedUpdate`，不以自由动态刚体代替角色控制。角色和布娃娃要求已启用、单位世界缩放的 owner Entity；布娃娃局部部件位置按 owner 旋转转换。owner Primitive 启用普通 body 时会撤销其角色或布娃娃，使同一 Entity 只有一个物理位姿写入者；旧专用句柄立即失效。布娃娃和车辆的专用句柄也带场景与代际，销毁、停用和 World 停止后失效。当前只提供这些玩法系统的基础配置与状态读取；骨骼动画映射、车辆资产参数、关节限位编辑器、专用资产持久格式与网络协议尚未建立。

显式删除距离约束或移除任一端点都会唤醒幸存端点，解除支撑后继续受重力影响；不同 Entity 的 Primitive 可作为两个端点。角色、布娃娃创建同时检查 Entity 活跃状态和所属 World；共享同一 Runtime 不代表可以跨 World 使用 Entity，detach/转移后须在新场景重新创建专用句柄。

## 验收与边界

无窗口真实 World/Jolt 验收使用：

```powershell
cmake --build --preset dev --target physics_acceptance editor_document_acceptance
cmake --build --preset release --target physics_acceptance editor_document_acceptance
```

`physics_acceptance` 按独立场景逐项输出 RUN/PASS/FAIL。`physics_examples_acceptance` 验证 10 个官方场景的真实磁盘往返、Jolt 模拟和编辑器 GPU 画面。完整覆盖矩阵、单场景重跑方法和结果见[物理验收](acceptance.md)。`editor_document_acceptance` 覆盖工程缓存、Primitive 配置历史和场景文档；`editor_acceptance` 的真实窗口用例包含 Entity Structure 添加/删除 CollisionComponent 及撤销，运行时需要可授予前台焦点的桌面会话。

实际日志与验收限制见[当前验收状态](../editor/README.md#entity-迁移验收状态)。

尚未提供 Convex/Compound/Heightfield、焊接、独立查询/模拟参与开关、具名碰撞 Profile、异步查询或专用服务器可执行宿主。当前验收覆盖已公开行为，没有测量大规模吞吐，也不承诺跨平台位级确定性。
