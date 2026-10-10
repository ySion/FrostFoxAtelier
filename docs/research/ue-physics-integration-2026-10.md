# UE5 物理接入与当前 Jolt 对接评估

日期：2026-10-10。状态：外部研究保留，项目评估已同步本轮 PrimitiveComponent 实现。Class 在本轮讨论中指核心 Runtime 可引用的类描述，不以资产化为前提。

第 1–8 节依据 Epic 官方 UE 5.x 文档和 API；无版本参数页面在调研时显示 UE 5.8。没有访问完整引擎实现源码，所以文中区分文档明确的契约、接口能证明的结构，以及据此提出的架构判断。带历史措辞或 TODO 的 API 注释不能当作完整线程行为的证明。第 9 节补充其他官方引擎资料；第 10–12 节从研究起始基线 `5d24e4a6ab10b954c3bda03ffdfea674e939b12d` 更新到已完成的组件改造。当前接口和边界以[物理模块](../modules/physics/README.md)为准。

## 1. 组件、body 与 shape 不是一对一同义词

| 层次 | 明确的职责 | 依据 |
| --- | --- | --- |
| Actor | 世界中的业务对象，包含组件；Actor 变换来自其 RootComponent。Actor 身份不等于物理刚体身份。 | [Actors][S1] |
| SceneComponent | 有空间变换、支持空间层级；不要求具备碰撞或模拟刚体。 | [Actors][S1] |
| PrimitiveComponent | SceneComponent 的派生类型，承接碰撞、模拟、物理状态和事件等场景功能；具体派生组件可以管理单 body 或多 body。 | [PrimitiveComponent][S2]、[SetSimulatePhysics][S3] |
| ShapeComponent | 以球、胶囊、盒等简单几何表示的 PrimitiveComponent。它是用户可放置的场景组件，并非所有底层 collision shape 的共同对象入口。 | [ShapeComponent][S4] |
| StaticMeshComponent | 通过 MeshComponent 继承 PrimitiveComponent，创建 UStaticMesh 的场景实例，并可使用网格的 body setup。 | [StaticMeshComponent][S5] |
| FBodyInstance | 每个物理实例的表示容器，关联内部物理 handle、组件及模拟参数；也有非 PrimitiveComponent 的 owner 接口。 | [FBodyInstance][S6] |
| UBodySetup | 资产关联的碰撞定义，可被多个 BodyInstance 共享，避免复制几何数据。 | [BodySetup][S7] |
| shape | 碰撞几何元素；BodySetup 的 AggGeom 可包含多个球、盒、胶囊、凸体等。BodyInstance API 还按 shape index 构建过滤信息。 | [AggregateGeom][S8]、[FBodyInstance][S6] |
| PhysicsAsset | 为 Skeletal Mesh 定义一组刚体及约束，用于骨骼物理与碰撞。 | [Physics Asset Editor][S9] |

由这些契约能确认三种不同的基数：

- **一个 Actor 可包含多个 PrimitiveComponent，因此可有多个独立 body。** Actor 的组件集合没有“仅一个物理组件”的限制；官方物理组件指南也直接以两个 StaticMeshComponent 连接一个 PhysicsConstraintComponent 举例。[Actors][S1]、[Physics Components][S10]
- **一个刚体可包含多个碰撞 shape。** AggGeom 的几何元素是数组，BodyInstance 初始化以 BodySetup 为输入；不能把每个 shape 都误当成一个独立模拟刚体。[AggregateGeom][S8]、[FBodyInstance][S6]
- **一个组件可包含多个刚体。** SetSimulatePhysics 明确区分简单单 body 组件和包含多个 body 的复杂组件，如使用 PhysicsAsset 的 Skeletal Mesh。[SetSimulatePhysics][S3]

**架构判断：** 用户对象的数量、空间节点数量、物理刚体数量和 shape 数量需要各自清楚。工业化的接入不能以“Primitive 就是 body”或“每 Actor 只有一个根刚体”替代这些关系。是否用继承、组合或数据表承载它们，是自身设计选择。

## 2. 共享定义与实例状态的边界

UBodySetup 保存可共享的碰撞定义；FBodyInstance 承担实例表示。共享 BodySetup 可以减少几何重复，不意味着共享每个实例的模拟状态。FBodyInstance 的 `InitBody` 接收 BodySetup、初始变换、组件及物理场景；`TermBody` 终结 body 并释放资源。[BodySetup][S7]、[FBodyInstance][S6]

这条边界适用于独立 mesh 刚体，也适用于具有多个 body 的组合对象。PhysicsAsset 再提供一组 body 与 constraint 的定义；它与单一碰撞 shape、单一 SceneComponent 都不同。[Physics Asset Editor][S9]

**架构判断：** 可共享几何定义、可编辑实例参数、已注册的后端物理状态，应有明确边界。保存、复制或重新进入 World 时，重建实例状态应沿同一责任链完成，不能将后端 handle 当作持久业务身份。

## 3. physics state 与 Chaos scene/proxy

PrimitiveComponent 提供 `ShouldCreatePhysicsState`、`OnCreatePhysicsState` 和 `OnDestroyPhysicsState`；`OnRegister` 的说明明确其发生在 Scene 设置后、物理状态创建前。组件的业务存在与其后端物理状态存在不是同一时刻、同一条件。[PrimitiveComponent][S2]

Chaos 场景接口 `FPhysScene_Chaos` 提供组件与 physics proxy 的关联，支持查找组件的 solver objects，以及从 proxy 或 proxy/shape 找回 BodyInstance；也提供 AddObject、RemoveObject、延迟创建清理等接口。部分 AddObject 注释明确用于 physics state 创建时把游戏线程对象交给物理线程。[PhysScene_Chaos][S11]

**能据接口确认的接入轮廓：**

```text
组件的定义、实例参数和空间状态
    → 创建 physics state
    → 实例 body / 后端 proxy 加入物理场景
    → 模拟、查询、状态回传和事件归属
    → 销毁 physics state，移除关联并释放实例资源
```

这是责任轮廓，不声称每种组件都执行同一条同步调用栈。PhysScene 和 BodyInstance 都暴露异步/延迟状态相关接口；RemoveObject 的部分历史注释还包含实现 TODO。本文不能据此保证所有物理对象的删除在调用点立即完成，也不假设组件、BodyInstance、proxy 永远严格一对一。[PhysScene_Chaos][S11]、[FBodyInstance][S6]

**架构判断：** scene/proxy 接入承载的是真实的线程交接、注册归属和资源生命周期责任，不能笼统称为“多层包装”。自身采用其他物理后端时，也需要这些责任，但不必复制 UE 的类名和完整层次。

## 4. 变换数据的方向

| 模式或操作 | 官方可确认事实 | 数据方向判断 |
| --- | --- | --- |
| 静态对象 | 官方碰撞指南将 WorldStatic 用于不移动对象；body 创建接收初始变换。 | 初始场景姿态进入物理表示。不能据此推论静态 body 永远禁止显式重定位。 |
| kinematic | 官方指南以动画/代码驱动的门、电梯为例；PhysScene 提供 `SetKinematicTarget_AssumesLocked`。 | 游戏/动画姿态或目标输入物理系统。 |
| simulated | 官方指南将 PhysicsBody 用于模拟驱动对象；`SyncComponentToRBPhysics` 明确将组件移动到刚体姿态。 | 模拟结果回写相应组件或骨骼状态。 |
| 显式重定位 | `SetBodyTransform` 将物理 body 移到新姿态，并有 Teleport 参数。 | 重定位与正常模拟回传必须区别；不是所有变换写入都应解释为 kinematic target。 |

依据：[Collision Response Reference][S12]、[FBodyInstance][S6]、[PhysScene_Chaos][S11]、[PrimitiveComponent][S2]。

**架构判断：** 上表是由接口契约归纳的数据方向，并非所有异步物理、动画混合和网络纠正路径的完整算法。关键是确定每个受驱动节点在本阶段的权威写入者；多 body 组件的结果也不能一概回写 Actor root。特定 teleport、缩放、父变换传播及动画/物理混合的细节，需要在实际实现时另行核实。

## 5. 帧阶段是契约的一部分

官方 Tick 文档给出：[Actor Ticking][S13]

| 阶段 | 对物理结果的保证 | 典型责任 |
| --- | --- | --- |
| TG_PrePhysics | 所读模拟数据是上一帧；本阶段的运动可以纳入随后模拟。 | 提交运动、物理交互所需输入。 |
| TG_DuringPhysics | 模拟正在进行，可能随时完成；读取结果可能来自上一帧或当前帧。 | 不依赖确定物理结果的逻辑。 |
| TG_PostPhysics | 本帧模拟已完成，物理数据为本帧结果。 | 依赖最终位置的 trace、表现或业务读取。 |

Actor 与 Component 可设置 TickGroup，具体 Tick 之间还可建立 prerequisite。**架构判断：** 工业化调度要表达输入阶段、模拟完成点和结果消费顺序；“每帧遍历组件后调用后端 step”不足以自动保证全部调用者获得正确时刻的数据。[Actor Ticking][S13]

## 6. 查询、模拟响应与事件通知分别表达

Collision Enabled 的常用模式包括 No Collision、Query Only、Physics Only 及查询和模拟均启用：Query Only 用于 raycast、sweep、overlap 等空间查询；Physics Only 用于刚体/约束模拟，不参加这些空间查询。这里列出职责区别，不将教程表格视为当前枚举全部取值。[Collision Response Reference][S12]

对象类型与通道响应决定相互作用；Block、Overlap、Ignore 与事件生成开关不同。模拟中的阻挡不会自动保证 Hit 通知，官方要求启用 Simulation Generates Hit Events；Overlap 通知也需要 Generate Overlap Events。组件接口提供 BeginOverlap、EndOverlap 与 Hit 事件入口。[Collision Overview][S14]、[PrimitiveComponent][S2]

**架构判断：** 一个后端接触点，不等于完整的业务 Hit/Overlap 契约。接入需要明确查询结果、过滤规则、接触/重叠状态、通知开关和业务对象归属；否则物理后端能力再完整，上层仍会缺触发器、拾取、交互和正确事件链路。

## 7. attachment、welding 与 constraint 分别解决问题

SceneComponent attachment 表达空间层级。启用简单单 body 组件的模拟时，UE 会将其从现有挂接关系脱离；停止模拟不会自动重新挂接。复杂多 body 组件如 Skeletal Mesh 则不会因开启模拟而自动脱离，因为其中 body 可以相对组件独立运动。[Actors][S1]、[SetSimulatePhysics][S3]

FBodyInstance 的 `Weld` 契约是将两个 body instance 合为一个模拟刚体；PhysicsConstraintComponent 则连接两个刚体，并允许通过约束参数控制它们的相对运动。这两者都不同于仅继承父空间变换。[FBodyInstance][S6]、[Physics Components][S10]

**架构判断：** 挂接树、复合刚体内部 shape、多个刚体的约束关系不能混为一张树。独立模拟、刚性焊接和带自由度的关节，需要能分别表达。父子编辑便利入口可以复用这些机制，但不能隐式决定全部物理语义。

## 8. 能支持的成熟度判断

本轮资料支持的判断是：物理工业架构需要完整接通**业务/空间节点 → 几何定义与实例参数 → 物理注册和资源生命周期 → 模式对应的变换同步 → 查询与事件归属 → 帧阶段**。它同时需要容纳 Actor 多 body、组件多 body、body 多 shape 这些真实组合。

本轮资料不能证明某种继承结构是唯一方案，不能证明 UE 每一层都有最低开销，也不能证明简短的 Body/Collider 组合必然欠缺能力。应检查责任和组合是否完整、数据方向是否明确，以及后端机制是否接通；性能和资源成本必须在本项目真实工作负载中测量。

[S1]: https://dev.epicgames.com/documentation/en-us/unreal-engine/actors-in-unreal-engine
[S2]: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UPrimitiveComponent
[S3]: https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UPrimitiveComponent/SetSimulatePhysics
[S4]: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UShapeComponent
[S5]: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UStaticMeshComponent
[S6]: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FBodyInstance
[S7]: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UBodySetup
[S8]: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FKAggregateGeom
[S9]: https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-asset-editor-in-unreal-engine
[S10]: https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-components-in-unreal-engine
[S11]: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FPhysScene_Chaos
[S12]: https://dev.epicgames.com/documentation/en-us/unreal-engine/collision-response-reference-in-unreal-engine
[S13]: https://dev.epicgames.com/documentation/unreal-engine/actor-ticking-in-unreal-engine
[S14]: https://dev.epicgames.com/documentation/en-us/unreal-engine/collision-in-unreal-engine---overview

## 9. 其他工业引擎的共同边界

Unity 6 的 compound collider 由一个 Rigidbody 和同一层级内多个 Collider 组成；子 Collider 可以具有各自局部变换。Godot 的 PhysicsBody3D 可以拥有多个 CollisionShape3D，每个碰撞节点引用 Shape3D 资源。二者的作者模型与 UE 不同，但都没有把游戏对象、刚体和碰撞形状强制做成一一对应。[Unity compound colliders](https://docs.unity3d.com/6000.0/Documentation/Manual/compound-colliders-introduction.html)、[Godot collision shapes](https://docs.godotengine.org/en/stable/tutorials/physics/collision_shapes_3d.html)

Godot 也提供 Jolt 后端，其文档明确列出后端能力与上层关节属性之间的对应限制。采用 Jolt 可以支撑完整引擎；上层对象模型与后端能力是否完整衔接，仍由接入层负责。[Godot Jolt integration](https://docs.godotengine.org/en/stable/tutorials/physics/using_jolt_physics.html)

Jolt 的版本化契约核查见 [Jolt 接入笔记](jolt-integration-contracts-2026-10.md)，尤其是 compound 的子形状身份、延迟 Removed 回调和临时内存耗尽行为。

## 10. 当前接入已完成的基础

实现入口为 [World 组件](../../src/world/world.hpp)、[物理接口](../../src/physics/physics.hpp)、[PhysicsScene](../../src/physics/physics.cpp)与[专用组件协调器](../../src/physics/components.cpp)。

- **共享组件模型。** PrimitiveComponent 继承 SceneComponent；StaticMeshComponent 与 CollisionComponent 共用 BodySettings、物理注册及退出链。网格默认只呈现，碰撞组件默认启用 body。旧 BodyComponent 仅存在于文件读取迁移与专用兼容夹具，运行时不注册。
- **每个物理组件有独立身份。** PhysicsScene 按组件登记普通刚体，同 Actor 可有多个。BodyHandle 带 Actor、组件、场景及重建代际，命令、查询和状态读取使用同一解析；ContactEvent 保存发生接触时的历史 handle。移除后的结束事件可交付，旧 handle 无法操作重建后的刚体。
- **空间与物理对应。** 以 Primitive 世界位姿创建刚体，先缓存全部求解位姿，再父先子后换算局部 TRS；保留局部缩放。Box 烘入正世界缩放，Sphere/Capsule 要求均匀世界缩放；静态三角网格读取同一 StaticMeshComponent 的资源。运动学目标每子步重新应用，到达后停止。
- **统一作者配置。** BodySettings、PhysicsMaterial 及关联枚举由头文件反射生成。C++、Source/JIT、Parameters 与 Scene 的通用 `properties.body` 使用同一字段，Jolt 句柄和 Cook 字节不写入场景。
- **运行与退出。** 每帧一次 World update，随后按 1/60 秒执行至多 8 个固定子步。Jolt 回调不调用游戏逻辑；完成求解和回写后交付事件。普通体卸载只清理该组件及依赖它的约束，World 正常/异常停止共用 End/deactivated 清理。
- **后端寿命与临时内存。** 宿主持有 PhysicsScene，场景寿命位于 World/Runtime 之内；多场景共享进程级注册。4 MiB 临时预留改用官方 `TempAllocatorImplWithMallocFallback`，容量不足可申请额外内存；系统内存耗尽仍不保证可恢复。

开发与发布配置的 20 项物理、9 项 World、Project Scene/JIT、完整编辑器文档，以及 10 个官方场景的模拟与 GPU 验收均通过。证据与覆盖范围统一见[物理验收](../modules/physics/acceptance.md)。

## 11. 仍存在的边界

### 11.1 多个 Primitive 与复合刚体分别表达

一个 Actor 可有多个独立刚体，但空间挂接不自动焊接。动态子体独立模拟；静态/运动学 body（含 Sensor）不能挂在启用的移动物理祖先下，当前明确拒绝。移动体的祖先要求均匀缩放，无法无损转换为局部 TRS 的姿态拒绝。Compound、局部 shape 集合与焊接尚未实现，后续应由真实物体需求驱动。

### 11.2 专用系统尚未完全接入普通查询身份

布娃娃使用专用 body 映射，普通查询和接触事件尚未完整返回其部件身份与统一过滤。CharacterVirtual 无 inner body 时不会成为普通刚体查询命中，也需要明确角色代理选择。Character/Ragdoll 根下的普通物理子体目前拒绝；本轮保留既有专用系统，没有扩大布娃娃能力。

### 11.3 固定步已有，World 通用阶段仍待定义

普通 Actor/Component Tick 每显示帧执行一次；PhysicsScene 的 before/after-step 为固定子步提供入口。World 尚无通用 PrePhysics/PostPhysics 或 TickGroup。每显示帧阶段与每个固定子步回调是不同契约，新增阶段需沿唯一宿主驱动链定义。

### 11.4 规模与查询成本尚未测量

当前仍使用同步单线程求解、4096 个 body 的场景容量及固定接触预算。临时分配器回退消除了原 4 MiB 超限直接终止的问题，但没有证明高负载下的资源预算合理。

普通与专用组件每帧分别扫描世界进行 reconcile；查询仍先收集后过滤和截断，返回容量不约束全部中间成本。增量注册、过滤下推和提前终止可作为实际负载出现后的优化方向；这只是静态成本判断，没有吞吐测量。开放并行前还需明确 World 访问、求解屏障、事件交接及关闭等待。

## 12. 后续演进原则

物理继续消费核心 Runtime 的 Class、ObjectId 和生命周期，JIT 访问相同组件；Jolt body、shape 和求解句柄由 PhysicsScene 管理，无需逐个包装成业务对象。

Primitive 已解决普通网格与碰撞的共享配置、空间定位及生命周期。接下来的候选是实际游戏需要的复合形状/跟随规则、查询身份补齐，以及与 World 协调的运行阶段；不把布娃娃扩展或多后端框架设为前置任务。序列化继续保存作者配置和对象关系，运行态在加载后重建。全对象图归档、核心 Class 引用与实例化仍需和 Runtime 共同推进。
