# Jolt 接入 Actor / Component 的最小契约

核查日期：2026-10-10。范围：官方接入语义与仓库锁定版本，不评价项目物理实现是否已经满足所有契约，不运行测试。

本仓库 `third_party/jolt` 为 **v5.6.0**，提交 `e77f175595e64cb44218cc9d9d56fc365ad0e36a`。以下源码链接固定到该提交；本地头文件和官方文档共同核对。`cmake/jolt.cmake` 使用静态库、关闭 double precision 和 object stream；这些配置没有改变下述对象身份、锁和生命周期语义。

Jolt 提供物理 body、shape 和模拟契约；Actor、Component、引擎对象身份以及世界调度属于引擎。**严谨接入的判断标准是两边契约能完整衔接，而不是 Actor 的外观是否与 Jolt Body 一一对应。**下面“最低要求”是根据官方语义推导的引擎接入要求，不是 Jolt 指定的引擎架构。

## 1. BodyID 是物理系统内的有限代际标识

`BodyID` 包含 23 位索引和 8 位 sequence；完整值才用于比较和哈希，sequence 用于识别索引复用。它没有世界标识，也不是永久不复用的身份。`BodyLockRead/Write` 获取锁后通过 `TryGetBody` 判断完整 ID 是否仍有效，调用者必须检查 `Succeeded()`；`SucceededAndIsInBroadPhase()` 进一步区分 body 是否已加入模拟。[BodyID.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/BodyID.h#L16-L74)、[BodyLock.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/BodyLock.h#L16-L72)

最低要求：跨世界消息携带所属场景；延迟命令和事件保留完整 BodyID，并校验引擎对象自身身份与存活状态。BodyID 索引、裸 Body 指针或 Component 地址不能单独承担引擎对象身份。8 位 sequence 有循环边界，不能宣称能防止任意时间跨度的旧引用复活。

## 2. UserData 可以连接现有对象体系，但不管理其生命周期

`Body::GetUserData/SetUserData` 存取应用提供的 `uint64`；官方 ContactListener 注释明确允许通过 UserData 找到 game object。Jolt 没有为这个数字附加对象所有权或失效校验。[Body.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/Body.h#L332-L334)、[ContactListener.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/ContactListener.h#L68-L86)

最低要求：映射回同一套引擎对象身份；若放裸指针，必须实际保证回调期间地址和对象均有效；若放 handle，解析时检查代际与所属世界。物理组件可以负责 body 的创建、同步和销毁，无须再建立另一套业务对象世界。

## 3. Body + Shape 描述刚体，不规定 Actor 的组件结构

一个 body 持有一个根 shape；compound 根 shape 可以包含多个带局部位置、旋转的子 shape。Shape 使用引用计数，可在多个 body 间共享。compound 的每个子实例有独立 `uint32 mUserData`；这与叶子 Shape 的 `uint64 UserData` 不同，`GetSubShapeUserData()` 返回叶子数据，不能当作 compound 子实例数据。SubShapeID 表示形状路径，修改 compound 后它可能改变或复用。[Body.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/Body.h#L451-L453)、[CompoundShape.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/Shape/CompoundShape.h#L22-L43)、[MutableCompoundShape.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/Shape/MutableCompoundShape.h#L23-L31)

最低要求：组件建模取决于游戏的实际刚体和碰撞需求，不能从“有多个 Component”推出“必须有多个 Body”。若要把接触精确映射到碰撞子组件，需区分 shape 资源身份与 compound 子实例身份，且处理结构变化。并发查询时修改 mutable shape 应采用官方 Clone + SetShape 路径，或证明查询与修改互斥；原地修改还需维护质心并调用 NotifyShapeChanged。不要把 body 原点变换误当成质心变换。[MutableCompoundShape.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/Shape/MutableCompoundShape.h#L101-L104)、[Body.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/Body.h#L272-L285)

## 4. NoLock 的使用需要执行阶段或已持锁的证明

普通 BodyInterface 使用锁；NoLock 变体省略 body 锁，不提供额外线程安全。普通读写锁只能保证其作用域内的 body 访问，访问多个 body 应使用 MultiRead/MultiWrite，不能以任意顺序逐个上锁。Update 中所有 body 锁被占用；step listener 是单独的扩展点，允许在自身无竞争的前提下读写 body/constraint，但不允许添加或移除它们。[官方并发访问说明](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Docs/Architecture.md#multithreaded-access-multi-threaded-access)、[PhysicsSystem.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/PhysicsSystem.h#L114-L125)

最低要求：公开物理入口有明确的线程和阶段契约。使用 NoLock 时要能说明其他访问为何不会并发发生；单线程业务代码也不能在 Update 运行时直接操作 body。仅把 worker 数设为零不能替代访问契约。引擎锁与 Jolt 锁的获取顺序还需一致，避免引擎线程持业务锁等待 Update，而回调反向等待业务锁。

## 5. 接触回调并发、只读，Removed 回调不能访问 body

ContactListener 可以从多个线程同时回调；同一 body pair 的回调串行。Added/Persisted/Validate 可以读取提供的 body，但不能改变物理状态，不能调用带锁接口；必要读取使用 NoLock。Removed 时连读取 body 都禁止，因为它可能正被其他线程修改，或已移除并销毁；销毁后的 Removed 会在下一次 Update 发出。官方建议提前缓存数据或记录事件并在 Update 后处理。[ContactListener.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/ContactListener.h#L73-L86)、[Removed 契约](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/ContactListener.h#L152-L175)

最低要求：回调中的记录器线程安全，缓存内容不依赖 body 内存；业务事件在明确阶段分发，解析仍存活的引擎对象时验证其身份。事件须明确携带历史接触快照还是指向当前 body 的操作资格：旧 body 已移除后的结束事件仍可以合法交付；要操作当前 body 则须重新验证映射，不能把旧事件绑定到重建后的 body。没有业务并发保证时，不能从物理回调直接执行任意 Component 逻辑。Removed 表示模拟接触缓存结束，休眠也会触发，不能天然等同于游戏对象离开区域；compound 结构变化还可能复用子形状 ID。[Added/Persisted 语义](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/ContactListener.h#L117-L150)

## 6. Create、Add、Remove、Destroy 是四个不同状态

Create 分配并初始化 body；Add 才参与模拟；Remove 后状态保留，可以再次 Add；Destroy 释放 body，必须先 Remove。CreateBody 可能返回 null，CreateAndAddBody 可能返回 invalid ID。批量加入优先使用 AddBodiesPrepare/Finalize，避免大量逐个插入带来的 broadphase 开销。[BodyInterface.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/BodyInterface.h#L41-L109)

最低要求：组件注册失败不会留下“已生效”的假状态；组件卸载和世界清理恰好释放一次；临时移除与永久销毁有清楚区别；销毁同时使延迟命令失效，并兼顾下一次 Update 的 Removed 回调。必要监听器和过滤器在 Jolt 可能访问它们时仍然存活。避免以每帧 OptimizeBroadPhase 掩盖批量导入问题。[PhysicsSystem 过滤器与优化契约](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/PhysicsSystem.h#L84-L89)

## 7. Static、Kinematic、Dynamic 对应不同控制权

Static 不参与运动积分；Kinematic 通过速度驱动；Dynamic 受力和求解器驱动。MoveKinematic 根据目标位置、旋转和时间计算速度。需要让最初 static 的 body 后续转为 dynamic/kinematic，必须在创建时保留相应 MotionProperties 能力。[MotionType.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/MotionType.h)、[BodyInterface.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/BodyInterface.h#L199-L216)、[BodyCreationSettings.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/BodyCreationSettings.h#L95-L102)

最低要求：dynamic 的常态变换来自物理回写；游戏传送是显式操作；kinematic 目标在固定步中转为运动输入。引擎不应在每帧读回 dynamic 后又用旧 Actor 变换覆盖它。控制权是引擎根据这些语义需要制定的规则，不是 Jolt 自动维护的 Actor 规则。

## 8. CharacterVirtual 有独立更新和生命周期语义

CharacterVirtual 使用碰撞查询实现运动，由引擎调用 Update/ExtendedUpdate；它不被 PhysicsSystem 作为普通 rigid body 管理，PhysicsSystem::SaveState 也不保存其状态。可选 inner body 是跟随它的代理；角色接触主要使用 CharacterContactListener，不能仅依赖普通 ContactListener。没有 inner body 时，普通世界射线查询和传感器看不到它。[CharacterVirtual.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Character/CharacterVirtual.h#L261-L267)、[官方角色控制器说明](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Docs/Architecture.md#character-controllers-character-controllers)

最低要求：角色仍属于现有 Actor/Component 和同一个物理场景入口，但明确安排独立更新顺序、保存恢复、查询和接触转发。可选 inner body 不能被误认为角色本身的完整模拟状态。Jolt 没有替引擎选择角色更新在 rigid body 更新之前还是之后；这里不假定唯一正确顺序。

## 9. 固定步、作业与临时内存需要明确资源边界

Update 将 inDeltaTime 分成 inCollisionSteps，内部派发作业并在返回前等待完成；临时分配在返回前释放。官方 HelloWorld 使用 1/60 秒，但帧时间累积器、追赶上限和溢出时间策略由引擎决定。TempAllocatorImpl 是依赖作业顺序的栈分配器，容量不足会 Trace 后 **std::abort()**，不是可恢复的 EPhysicsUpdateError。官方提供 TempAllocatorImplWithMallocFallback 可在固定块不足时分配额外内存。[PhysicsSystem.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/PhysicsSystem.h#L156-L162)、[HelloWorld](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/HelloWorld/HelloWorld.cpp#L324-L348)、[TempAllocator.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Core/TempAllocator.h#L11-L14)、[固定容量失败与回退实现](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Core/TempAllocator.h#L59-L95)

最低要求：固定步调度有单一责任入口，并保留不足一个步长的时间余数；作业系统和 allocator 活到 Update 完成；并发 scene 更新不能无保护地共用同一个栈 allocator。容量应有工作负载依据或可接受的官方回退策略；检查 Update 的容量错误，因为某些错误会让接触被忽略。单线程 JobSystem 是合法后端，是否需要并行取决于实际负载。[EPhysicsUpdateError.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/EPhysicsUpdateError.h#L9-L16)

形状构造也必须核对锁定版本的实际约束：v5.6.0 的 BoxShape 直接构造器和 settings 路径都会把凸半径钳制到最小 half extent；默认 0.05 不要求每个 half extent 至少为 0.05。因此“正的小 box 必然违反默认凸半径约束”不成立。仍应在引擎输入边界保证有限、合理的数据。[BoxShape.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/Shape/BoxShape.h#L42-L44)、[BoxShape.cpp](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/Shape/BoxShape.cpp#L55-L75)

## 10. 全局注册与延迟力的寿命不能绑定错对象

Factory::sInstance、类型注册和默认 material 是库的全局设施。官方 HelloWorld 先注册 allocator，再建 Factory、RegisterTypes；退出时 UnregisterTypes 清理类型和默认 material，再删除 Factory。UnregisterTypes 实现直接 Clear，全局没有 scene 引用计数。[HelloWorld 初始化](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/HelloWorld/HelloWorld.cpp#L214-L241)、[RegisterTypes.cpp](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/RegisterTypes.cpp#L195-L202)

最低要求：多个 scene 共存时由共享运行时寿命维护全局设施；销毁一个 scene 不能撤销其他 scene 仍需的注册，也不能在仍会使用默认 material 的资源存活时卸载全局设施。官方示例不是多 scene 生命周期规范；这里根据实际全局变量和清理实现推导要求。

AddForce/AddTorque 累积到下次 Update，随后重置；body 必须先加入模拟，力与冲量的物理意义不同。[Body.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/Body.h#L181-L203)、[BodyInterface.h](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Body/BodyInterface.h#L218-L230)

最低要求：引擎若延迟投递力，要定义命令对应的物理步、目标代际和 scene；目标销毁或被替换后丢弃旧命令。一帧没有物理步时，不能因为清空业务队列而丢输入；一帧追赶多个 Update 时，持续力是否每步重施与一次命令是否只消费一次要明确区分。Jolt 只定义自身 AddForce 的累积范围，不定义引擎队列策略；本次没有确认项目队列当前行为，也没有测量固定内存容量是否足够。
