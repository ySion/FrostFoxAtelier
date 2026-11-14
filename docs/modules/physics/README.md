# 物理模块

`atelier_physics` 使用仓库锁定的 Jolt 源码，公开接口只暴露项目的 Runtime、World、数学值和资源身份。本文是当前物理模型、接入不变量与接口边界的唯一入口。

## 组件与空间关系

先在 Runtime 创建对象前依次调用 `world::register_types(runtime)` 和 `physics::register_types(runtime)`。一个运行中的 World 对应一个由宿主持有的 `PhysicsScene`，场景先于 World/Runtime 销毁；同进程多个场景共享 Jolt 全局注册。`PrimitiveComponent : Component` 统一持有反射的 `BodySettings body`；`StaticMeshComponent` 默认关闭 body，`CollisionComponent` 默认开启且不参与网格呈现。二者使用同一物理注册、查询和生命周期入口。空间由 owner Entity 的可选内联 TRS 提供；每个 Entity 的位姿写入者单独验证，多个独立物理部件使用子 Entity，组件不形成第二棵空间树。

共享碰撞几何、作者配置、运行中 body 与业务身份分别拥有自己的寿命：Jolt body/shape 不再包装成 Runtime 对象，后端 handle 不写入场景。空间挂接、复合形状与关节也不是同一种关系；当前子 Entity 多刚体不能替代尚未实现的 compound 或焊接。这与 UE 的 [BodySetup](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UBodySetup) / [BodyInstance](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FBodyInstance) 分开定义和实例的责任相通，不要求复制其类型层次。

Box、Sphere、Capsule 支持静态、运动学和动态体。世界缩放先按 World 规则分解为真旋转与带符号 scale；Box 烘入各轴缩放的绝对值，Sphere/Capsule 要求各轴绝对值相同。这三种形状没有局部偏移且关于各局部轴对称，镜像只改变符号，不影响碰撞；以后加入形状偏移或非对称形状时须重新处理反射。静态三角网格把带符号的世界缩放烘入顶点，并按作者法线的逆转置决定正面，镜像后正反面不变；Cook 数据以带符号缩放为键。剪切变换拒绝；运动体的空间祖先要求各轴缩放绝对值相同，以保证模拟旋转可无损转换回局部 TRS，镜像祖先的回写保持作者局部 TRS。Character 与 Ragdoll 要求单位且未镜像的世界缩放。零缩放的空间不建刚体，只在最上层零缩放节点报告一条 `degenerate_value` 警告，恢复非零缩放后重新协调即重建刚体。

父子挂接不产生物理焊接。动态子体独立模拟，纯视觉后代照常跟随。当前不支持 compound，也不让静态/运动学 body（包含 Sensor）隐含跟随移动的物理祖先；此组合明确报错，应使用独立空间分支。Character/Ragdoll 控制器 Entity 下新增普通物理子体也明确拒绝，避免多套姿态驱动交错。

## 固定步与位姿写入

### 显式顺序

普通 body 和组件控制器各自在协调时按当时的 World 成员顺序加入，同一 Entity 内按组件顺序；直接调用原生创建 API 的专用后端按调用顺序加入。每次成功创建或重建分配新的加入序号。加入后在实体树里调整顺序，不改变已有 body、专用后端或组件控制器的序号，直到该项被重建；Play 从文档重新加载，初次协调使用存盘成员顺序。ObjectId 和哈希函数都不是执行顺序的来源。

现有阶段与批次边界不变：普通 body 协调先于组件协调，输入先于求解，位姿先于后回调；各批次内的运动学驱动、角色更新、组件输入/查询、控制器位姿、连带撤销与协调末尾未确认条目的清理按加入序号遍历，由这些遍历产生的诊断也沿用该顺序。生命周期事件仍按 World 的通知顺序处理，单条显式命令仍按提交顺序处理。求解线程收集的普通 body 位姿继续按 Jolt BodyID 去重和定序（BodyID 的分配/复用取决于上述显式创建/撤销顺序），之后依次追加角色、布娃娃、绑定部件与需要保持世界位姿的子 body；World 再按空间依赖换算局部 TRS。该规则消除哈希迭代的隐式影响，不承诺跨平台位级确定性。

替换旧 Entry 时仍随当前成员协调顺序立即释放，不延迟到末尾清理。例如 A、B 加入后将树重排为 B、A，再同时重建两项，旧 Entry 可以按 B、A 释放；这不受旧加入序号约束。

### 推进与回写

PhysicsScene 创建时向 World 登记为模拟系统，析构时注销；`World::update(dt)` 在 pre_physics 阶段之后调用它的 step。创建时还把 Primitive、Character、Ragdoll 组件登记为 World 能力（见 [World](../world/README.md)），World 随组合、成员和启用的每次变化维护这些桶，同一 World 上重建的 PhysicsScene 复用同一登记。step 只协调可能参与物理的 Entity：桶中启用且 body 已启用的 Primitive 的 owner、桶中启用的 Character/Ragdoll 的 owner，以及已有刚体或专用后端的 Entity。哪些要重新协调由 Runtime 的[写代号](../../runtime-model.md#写代号与变化游标)决定：场景在每次 step 的回写之后、分发接触通知之前取游标，下一次 step 只协调此后被写过的对象及本次前/后回调写过的对象，即被写的 Entity、被写组件的 owner，以及经刚体祖先链反向索引找到的、被写 Entity（或被写组件的 owner）之下的全部刚体；body 设置和空间祖先链（成员、启用、缩放、祖先上移动的物理体、父节点）对这些刚体重新读取，同一次协调中每个祖先只读一次、由全部子孙刚体共用，协调结束即丢弃。`World::revision()` 前进（组合、成员、启用、结构变化）或 World 重新启动后协调全部候选；协调失败或因零缩放而没有刚体的 Entity 每一步重试，所以持续的失败每步都报告。控制器组件（Character、Ragdoll、约束、力、运动学、查询）在有任何写入、有刚体或控制器被创建或移除、或上一次协调隔离过失败时重新协调。没有写入的步不读任何成员。随后按 60 Hz 最多推进 8 个固定子步，超出上限的时间丢弃。`last_step()` 返回最近一次 step 的 `StepResult`：完成步号、子步数、剩余时间、运行时 Cook 次数和求解接触缓冲溢出的子步数。登记的 `writes_pose` 声明物理写入位姿的 Entity：已有 body、角色、布娃娃或绑定目标的 Entity，以及带有启用的 Character / Ragdoll 组件的 Entity（控制器在下一次 step 创建之前即已声明）；`translate_owner` 对它们逐条拒绝。启用但被隔离拒绝的控制器（例如镜像 Entity 上的 Character、同一 Entity 上的双控制器）同样声明位姿：作者配置了控制器，位姿就归物理，该 Entity 的平移命令持续被拒绝，控制器修好后也不会出现位姿跳变。`set_paused`、`step_once` 控制子步；`on_before_step` 在消费命令前调用，`on_after_step` 在求解、位姿回写与接触收集后调用。运动学目标保留到被下一目标或传送替换，每个子步重新计算速度，到达后停止。位姿以物理体世界状态为准。每个固定子步只回写本步移动过的刚体：求解后仍活跃的、本步内转入睡眠的、被传送的，以及空间祖先在本步被回写的刚体（保持其世界位姿）；Character、Ragdoll 与专用后端照常回写。静态和保持睡眠的刚体不回写。任何途径写入刚体所在 Entity 或其非物理祖先的 TRS（经 World、`translate_owner`、Runtime 直接写入、Source）都与 `set_transform` 等效、传送刚体并保留速度：经 World 的写入立即经下文的 `relocated` 通知物理；绕过 World 的写入在下一步协调时发现：场景记录每个刚体创建、传送或回写时 owner 的局部 TRS 和世界位姿，协调时局部 TRS 逐位不同，或世界位姿超出 TRS 往返的浮点误差（祖先移动），即按 `relocated` 的路径传送。`on_before_step`、`on_after_step` 中的写入在回调返回时即这样处理：前回调的传送在本子步求解前生效，不被本步回写覆盖；后回调的传送在下一子步生效。这些传送排在该回调自己发出的物理命令（例如 `teleport`）之后；需要与命令定序时用 `set_transform`，它立即排入命令。回调中改了结构（reparent、空间参考）时，同一步内对新祖先的写入沿当前空间链找到其下刚体。场景先收集这些求解结果，再按 Entity 空间依赖顺序整批回写 Entity，保留局部缩放。移动使用力、冲量、运动学目标或显式传送。宿主在运行中直接编辑物理控制 Entity 的变换时（例如 Play 收到编辑器的修改），World 经 `relocated` 通知物理：刚体在下一步按新的世界位姿传送并保留速度，Character 立即设置位置与朝向；Ragdoll 及其部件的位姿跟随控制器，这类编辑以 `unsupported_operation` 拒绝并恢复原 TRS；编辑它们的祖先同样使整次变换编辑失败（Play 的增量应用记为 unsupported 跳过）。缩放变化仍由逐步协调重建刚体。运行中 keep_local 的结构编辑（reparent、空间参考）不通知物理，只依赖下一次逐步协调；当前宿主路径都按 keep_world 改结构，再经 `set_transform` 写最终局部变换。

step 的 CPU 墙钟记为[帧阶段计时](../../foundation.md#帧阶段计时)的 `physics.*` 阶段（标记见 [physics/timing.hpp](../../../modules/engine/physics/include/physics/timing.hpp)），挂在 World 的 `world.simulation` 下：`physics.step` 包含逐帧协调 `physics.reconcile`（成员 body 协调与组件协调）、每个固定子步一次的 `physics.substep`，以及子步全部完成后的接触通知分发 `physics.events`。子步内 `physics.solve` 是 Jolt 更新的墙钟，`physics.write_back` 覆盖位姿读出与 Entity 回写。

暂停下重复 `step_once` 合并为一次请求，由完成的求解消耗。step 返回失败（前/后回调失败，或 halt 下的协调失败）时 World 停止；isolate 下按对象隔离的协调失败和 Jolt 的缓冲满（见下文 `Capacity`）都不是 step 的失败。已完成的求解保留步号，World 停止丢弃待处理的单步请求与命令。当前使用单宿主同步推进，求解期间不开放外部 World/Runtime 访问或并发查询；Jolt 接触回调只收集值数据，业务通知在求解结束后交付。World 的 Tick 阶段每显示帧各执行一次（pre_physics 在全部子步之前，post_physics 与 late 在之后），每个固定子步的业务工作通过场景前/后回调接入。`on_before_step`、`on_after_step`、`on_contact` 是宿主 C++ 回调，失败按 World 级停机；将来向 Source 开放接触事件时，必须经 World 的逐对象隔离分发，不能直接走这几个回调。

物理失败遵循 World 的[失败策略](../world/README.md#失败策略与诊断)，诊断以 physics 阶段写入同一 World 日志。isolate 下，普通 body 协调失败的 Entity 不保留普通 body，其余 Entity 照常模拟；位姿写入者冲突时拒绝普通 body，保留控制器组件；同一 Entity 上有多个位姿控制器（Character、Ragdoll 任意组合或同类重复）时，该 Entity 上写入位姿或依赖 body 的控制器组件（Character、Ragdoll、Force、KinematicMotion、DistanceConstraint）全部拒绝，与创建先后无关，只读 owner 空间的 Scene Query 照常工作；控制器组件逐项创建、采样与回写，失败项不创建或跳过该步，下一步重新采样；被拒绝的位姿不回写。halt 保持原行为：首个失败使该次 `World::update` 或生命周期调用失败。

前步、后步或接触宿主回调成功调用 `World::stop/close` 后，本次 update 成功结束，不继续求解、回写或派发剩余接触。`last_step` 保留本次已完成的子步与累计步号，剩余时间归零；停止期间产生的待交付 ended 也被丢弃，重启不补旧步或旧事件。World 仍运行时普通对象停用、移除产生的 ended 不受此规则影响。

组件注册、绑定、协调、输入和查询的可恢复失败均显式返回 `Result`，不借 C++ 异常传错；前步、后步及接触宿主回调也必须返回 `Result<void>`，场景不再捕获任意异常并翻译为业务错误。原结果的 code、message、source 沿已有隔离或停机路径传递；注册仍逐项发布，失败不回滚已发布类型。组件移除时查询祖先的既有特殊映射保留：`parent_entity` 的 busy 中止该链查找，其他失败返回 invalid_member 和原 message，不携带 source。

协调先验证所有位姿占用，再创建后端；成员拒绝撤销本成员的计划和占用，重复控制器只保留查询计划。旧 Entry 仍在原替换位置释放，未确认的旧项在末尾按加入序清理。jump、初始冲量和运动学计时只在对应操作成功后消费或前进；查询先清旧结果，失败保留待处理请求，成功才发布并消费。业务容量失败仍不可隔离；宿主 CPU 分配耗尽遵循[工程错误合同](../../engineering.md#错误处理)，不再映射为可恢复的 capacity_exceeded，也不承诺 fatal 时完成补偿清理。

### 渲染插值

模拟按 60 Hz 固定步推进，显示帧率更高时 World 的位姿每隔几帧才变一次。`presented_poses()` 给出本帧的呈现位姿，交给 `presentation::Scene::update`（见[呈现位姿](../presentation/README.md#呈现位姿)）：最近一个发布了位姿的固定子步里，每个被 World 接受的 Entity 一条，位置在上一位姿和本步位姿之间线性插值，旋转沿短弧 nlerp，比例为 `remaining / step`（剩余时间除以步长，∈ [0, 1)），所以画面最多比模拟晚一步，不外推。只有呈现结果插值：World 的 TRS、玩法、查询、TRS 逐位比较都看本步的权威位姿。

- 上一位姿：场景为每个模拟 Entity 记下上一次发布、创建或传送时的位姿；没有记录的从本步位姿开始，不拖影。刚体创建或重建、Character 与 Ragdoll 的创建、`relocated`（`set_transform`、绕过 World 的写入、随祖先传送的子孙）以及 Character 的位置设置，都把该 Entity 立即画在新的 World 位姿，下一步从这里开始；`teleport` 命令在执行的那一步从新位姿开始。
- 嵌套：空间祖先在本步被回写的刚体本来就随本步发布（保持世界位姿），它们的两个位姿相同，作为边界列在表里，运动父刚体下的睡眠子刚体及其视觉子孙就不跟着父体晃。两步之间经 keep_world 改挂到运动刚体下的刚体还没有被发布，协调时就以相同位姿列入表中。
- 睡眠：全部刚体睡眠后不再发布位姿，表在下一步清空，Scene 回到静止。暂停时表为空，画面停在 World 位姿，`step_once` 的那一步完整显示；World 停止时清空记录。

## 身份、碰撞与查询

`body_handle` 接受 Primitive 身份；传 Entity 时只解析其自身启用的 Primitive，不从子 Entity 猜选刚体。`BodyHandle` 包含 Entity、组件、场景与重建代际；查询返回同一 handle，状态读取和命令验证其有效性。`BodySettings` 提供 16 类对象的双向 Ignore/Overlap/Block 响应、Sensor、摩擦和恢复系数。两个不相交的 16 位类别掩码 `ignore_categories` / `overlap_categories` 表示 Ignore/Overlap，其余为 Block；C++ 的 `response` 从同一值读取响应。双方 Block 才阻挡，Ignore 优先，Overlap/Sensor 不求解阻挡。接触按刚体对归并为 began/persisted/ended，卸载的 Exit 标记 `body_removed`。`ContactEvent::first/second` 保存历史 BodyHandle；刚体移除后的结束事件仍可交付，旧 handle 不能操作同组件重建的新 body。事件在本帧子步结束后交付，其新命令从后续固定步生效。

Jolt 的 [BodyID](../../../third_party/jolt/Jolt/Physics/Body/BodyID.h) 只有有限索引与 sequence，没有场景身份；引擎命令使用自己的 BodyHandle 场景/重建代际检查，不能仅靠后端索引判断存活。body 创建、加入、移除与销毁是不同状态，退出时先 Remove 再 Destroy；单次力/冲量命令保留到下一固定步消费，目标已重建时丢弃旧命令。持续 ForceComponent 每步重新施力，不把单次命令重复执行。

[ContactListener](../../../third_party/jolt/Jolt/Physics/Collision/ContactListener.h) 可以从多个求解线程并发调用，记录器在已有 mutex 下只保存值；Removed 不访问 body 内存。每个固定子步的 Update 返回后，宿主才发布位姿、归并接触和交付业务回调。此时同一场景没有并发读写，位姿批次使用 NoLock；需要同时锁两个 body 的约束创建使用 MultiWrite，不能任意顺序逐体上锁。访问前提以 [Jolt 并发合同](../../../third_party/jolt/Docs/Architecture.md#multithreaded-access-multi-threaded-access) 为准，不以调小线程数代替同步保证。

通知队列的 16384 条容量与 Jolt 求解接触容量不是同一个预算。队列实际丢弃时，Update 返回后的宿主线程每步至多报告一条 `capacity_exceeded` 物理警告，字段路径 `contact_notifications`，`value` 为 `dropped=<本步数量>; capacity=16384`。重复合并、已取走后不重发沿用 World 诊断通路；不停止模拟，也不将该计数暴露为 StepResult 字段。

每场景预留 4 MiB 物理临时内存，超出时使用 Jolt 官方 `TempAllocatorImplWithMallocFallback` 从通用分配器申请额外内存，并在求解释放时归还。4 MiB 是预留容量，不是上限；这不保证系统内存耗尽可恢复。每步临时用量主要随下文的接触约束容量增长，实测约每个 0.5 KB：500 个箱子的场景按默认容量（8192 个接触约束）每步峰值 4,163,120 字节，占预留的 99.3%，默认容量再往上调一点就会每步回退；2000 个箱子（16384 个）每步回退一次，含回退在内 step 的中位数为 0.13 ms，所以预留不随容量放大。Jolt 里一个刚体都没有之后（普通刚体与布娃娃部件都算，Character 不建刚体；创建时，以及每次停止移除全部刚体后）第一个有刚体的 step 在求解前整体构建一次宽相树（Jolt `OptimizeBroadPhase`），每次运行开始都是这种情况：同时加入的大批刚体否则要在前两步求解中逐步重排，2000 个箱子时前两步的 solve 各约 4 ms，构建后 0.5–0.7 ms，构建本身约 0.3 ms；已有刚体时新增的刚体不触发它。刚体、刚体对与接触约束的容量（`Capacity`）在创建 PhysicsScene 时固定，Jolt 之后不能扩容：宿主可以显式指定，填 0 的项按创建时 World 里的 Primitive 数 N 推导：刚体数为 max(4096, 2N) 向上取到 2 的幂，刚体对为它的 8 倍，接触约束为它的 2 倍（Jolt Samples 的比例）；刚体对与接触约束至少为 4，与 Jolt 一致，超过 Jolt 上限的显式值拒绝创建。刚体对或接触缓冲满时 Jolt 只丢弃多出的接触（对应物体可能互相穿过），模拟照常继续：该子步计入 `StepResult::contact_overflows`，并记一条 `capacity_exceeded` 物理警告（字段 `body_pairs` 或 `contact_constraints`，持续时合并计数），任何失败策略下都不停机。缓冲满时 Jolt 会把仍保留的静止接触报告为移除，之后只报告持续，因此刚体对的接触计数归零时先向 Jolt 核实，仍在接触就继续发 persisted，缓冲恢复后接触事件照常；开发构建中 Jolt 对这一结果的断言也不中断进程，其余 Jolt 断言照常中断。求解使用进程内共享的 Jolt `JobSystemThreadPool`：第一个 PhysicsScene 创建时建立、最后一个析构时销毁，所有 World 与运行时上下文中的场景共用；工作线程数为逻辑核数减一（至少 1、至多 3：在 500 个活跃箱子上，3 个工作线程比 1、7、15 个都快），发起 step 的线程同时执行任务，不同线程上并发的 step 各占一个屏障（上限 8）。

每个场景拥有独立临时分配器，活到该场景的 Update 完成；[Update](../../../third_party/jolt/Jolt/Physics/PhysicsSystem.h) 返回前等待本次作业完成。Factory/类型注册与共享工作池由进程内引用计数保活，Cook/校验也取得同一注册租约；最后一个使用者释放前不能撤销默认 material 和类型注册。注册/卸载顺序依据 [RegisterTypes](../../../third_party/jolt/Jolt/RegisterTypes.cpp)，固定预留不足的处理依据 [TempAllocator](../../../third_party/jolt/Jolt/Core/TempAllocator.h)，不是吞掉内存耗尽错误。

未发生容量溢出时，同一二进制、同一 API 调用顺序下结果与线程调度无关。
容量耗尽时，保留的接触集合可能受线程调度影响，数值结果不保证逐位一致。
1、3、7、15 个工作线程的未溢出场景位姿曾实测逐位一致；这是既有测量，不是常驻验收或跨平台保证。

同步 `raycast`、`sweep_sphere`、`overlap_sphere` 返回带完成步号的 Primitive 命中，支持类别、忽略 Entity/组件、any/closest/all 和容量截断。虚拟角色与布娃娃的专用刚体尚未进入普通查询身份映射。

`PhysicsScene::trace` 提供参考 UE 的通道/对象类型选择和 test/single/multi 模式，`SceneQueryComponent` 把射线、球扫掠、球重叠配置接入场景保存、固定步采样和脚本原生调用。同 Entity 可挂多个查询，默认忽略自身；停用或离场清空活动结果，保存只保留配置。完整语义、C++/脚本示例及限制见[场景查询与射线组件](scene-queries.md)，官方 `Raycasts.scene` 提供六种配置。

## 资源、编辑与场景保存

`cook_triangle_mesh` 将静态网格生成带格式版本、Jolt 版本、源身份、缩放和摘要的二进制形状；读取时验证原始网格与 Cook 数据一致。这段头部就是 `BodySettings.cooked_shape` 的输入键：物理在建三角网格刚体时校验，形状不是 Triangle Mesh 时忽略该缓存，因此编辑 body 字段不清除它。输入键与当前源网格或世界缩放不一致时（例如运行中缩放网格碰撞体），物理在内存中按当前缩放重新生成形状，不写缓存，并计入 `StepResult::runtime_cooks`；格式、Jolt 版本或摘要不符仍拒绝。`Project::cook_collision` 将工程 OBJ 的形状写入 `Cache/Physics`；`Project::collision` 校验并读取，缓存缺失或不一致（源网格、缩放或上游 Jolt 版本变化）时从源网格重新 Cook 并写回。编辑器场景文件持久化组件配置及网格来源，加载不要求缓存预先存在。内置网格可直接从 CPU 网格创建三角形状，不生成工程缓存。场景加载先恢复网格、局部 TRS 与空间关系，再按 Primitive owner Entity 世界缩放配置物理。

编辑器沿 Entity Structure 的统一入口添加 `CollisionComponent`；网格已有物理能力，可直接编辑 Parameters 中的 `body.enabled`、形状、运动模式、尺寸、碰撞类别/掩码及材料。`BodySettings` 和 `PhysicsMaterial` 由头文件反射生成，Source/JIT 直接访问同一字段。单字段规则是反射硬边界（尺寸、摩擦非负，恢复系数 [0,1]，类别 [0,15]，掩码不超过 16 位），写入越界时夹到边界；跨字段规则是 BodySettings 的类型校验器：形状与运动类型必须是已声明值，尺寸为正，Ignore 与 Overlap 掩码不相交，Triangle Mesh 只能 Stationary。`configure`、逐步协调与场景加载经 Runtime 检查同一规则。Scene 沿通用 `properties.body` 保存配置，没有第二份物理配置记录；`Session::configure_body` 提供完整配置事务。碰撞可视化及 Cook 管理页尚未实现。

[Physic](../../../examples/Content/Physic/README.md) 提供 9 个可保存、重开的组件场景。通过 `atelier_editor --project ./examples --scene Content/Physic/Scenes/Overview.scene` 打开；编辑场景保持静止，Project → Run 从快照创建独立运行会话，使用场景主相机，经 Session 统一推进 World 和 Jolt。

## 生命周期与专用控制器

Character、Ragdoll/RagdollPart、DistanceConstraint、Force、KinematicMotion 的组件协调器沿 PhysicsScene 管理专用后端；重复 reconcile 保留有效句柄。普通 Primitive 的停用或卸载只撤销自身及依赖它的约束；组合祖先停用会撤销组合子树的物理参与。空间参考只决定坐标，参考目标停用不会使独立组合的 owner 失去启用资格。Force/KinematicMotion 等控制器选择 owner Entity 的刚体。Primitive、Character、Ragdoll 不能同时驱动同一 Entity 位姿；`translate_owner` 不得覆盖物理控制的 Entity。专用控制器的形状/关节在激活时采样，实时输入每个固定步读取。

同 World 内重挂后，若启用资格、配置与后端绑定仍有效，只更新空间依赖；不重复施加初始冲量、不重置运动学计时，也不重新创建专用句柄。真正改变配置、绑定或参与资格时仍由同一协调器重建。

布娃娃通过公开强 Entity 引用序列 `RagdollComponent.parts` 显式绑定目标。每个目标必须是控制器 owner 的直接组合子 Entity，空间参考指向同一 owner；`parts` 顺序决定后端部件索引。目标保存普通 RagdollPartComponent 元数据和独立 StaticMeshComponent；metadata 不继承网格、不拥有空间。目标显示缩放保留，控制器 owner 要求单位世界缩放，部件物理尺寸由配置决定。

姿态整批写回目标 Entity，保留显示 scale；目标 mesh 的 body 关闭，避免重复姿态写入者。绑定变化、停用、删除、离场或元数据失效会撤销专用后端；不会从组件目录隐式猜测部件。距离约束 target 是 Runtime 弱 Entity 引用，停止后用 `set_constraint_target` 绑定；Scene 统一保存 `properties.target` 文档引用。目标选择器和专用资产编辑面板尚未提供。

专用场景 API 包含 `CharacterVirtual` 胶囊角色的移动、跳跃、坡道/台阶与移动平台，两个 Body 之间的距离约束，以及基于 Jolt Ragdoll 的多部件布娃娃。角色由场景每固定步调用 `ExtendedUpdate`，不以自由动态刚体代替角色控制。角色和布娃娃要求已启用、单位世界缩放的 owner Entity；布娃娃局部部件位置按 owner 旋转转换。owner Primitive 启用普通 body 时会撤销其角色或布娃娃，使同一 Entity 只有一个物理位姿写入者；旧专用句柄立即失效。布娃娃的专用句柄也带场景与代际，销毁、停用和 World 停止后失效。当前只提供这些玩法系统的基础配置与状态读取；骨骼动画映射、关节限位编辑器、专用资产持久格式与网络协议尚未建立。

显式删除距离约束或移除任一端点都会唤醒幸存端点，解除支撑后继续受重力影响；不同 Entity 的 Primitive 可作为两个端点。角色、布娃娃创建同时检查 Entity 活跃状态和所属 World；共享同一 Runtime 不代表可以跨 World 使用 Entity，detach/转移后须在新场景重新创建专用句柄。

## 验收与边界

无窗口真实 World/Jolt 验收使用：

```powershell
cmake --build --preset dev --target physics_acceptance project_scene_acceptance
cmake --build --preset release --target physics_acceptance project_scene_acceptance
```

`physics_acceptance` 按独立场景逐项输出 RUN/PASS，只保留一个刚体落地、当前 Cook 往返和 Tick 失败后的清理序列。当前保留范围与单场景重跑方法见[物理验收](acceptance.md)。`project_scene_acceptance` 保留失败 Tick 中嵌套 stop 的物理停止顺序代表；Entity Structure 添加/删除 CollisionComponent 及 Primitive 配置历史目前没有自动验收。

编辑器接入的覆盖范围见[场景模型验收](../editor/README.md#场景模型验收)，逐次日志由根 README 的验收门禁记录。

验收宿主的断言和异常入口尚未迁移，不代表物理组件或宿主回调另有异常业务通道。Jolt 可空分配与线程失败的致命接线仍待独立核验。

尚未提供 Convex/Compound/Heightfield、焊接、独立查询/模拟参与开关、具名碰撞 Profile、异步查询或专用服务器可执行宿主。刚体逐个加入（`CreateAndAddBody`，约 0.5 µs 一个）；已有刚体的运行中一次生成大批刚体（例如关卡流式加载、一次实例化很多预制体）时也会出现前两步的建树尖峰，Jolt 为此提供批量加入（`AddBodiesPrepare` / `AddBodiesFinalize`），出现运行中大批生成的需求时再测量、再接入。验收代表不等于全部公开行为的组合矩阵，没有测量大规模吞吐，也不承诺跨平台位级确定性；`physics-cpu` 作为独立测量入口保留，不进入验收或 Full 清单。
