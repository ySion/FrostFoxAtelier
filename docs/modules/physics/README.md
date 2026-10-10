# 物理模块

`atelier_physics` 使用仓库锁定的 Jolt 源码，公开接口只暴露项目的 Runtime、World、数学值和资源身份。[Jolt 使用设计方案](jolt-usage-design.md)记录目标语义与阶段边界；本文描述当前源码可用的功能。

先在 Runtime 创建对象前依次调用 `world::register_types(runtime)` 和 `physics::register_types(runtime)`。一个运行中的 World 对应一个由宿主持有的 `PhysicsScene`，场景须先于 World/Runtime 销毁；同进程多个场景共享 Jolt 全局注册。Actor 最多启用一个非空间 `BodyComponent`，形状使用显式根 `SceneComponent` 的世界位姿。Box、Sphere、Capsule 支持静态、运动学或动态体，根缩放须为 `(1,1,1)`；静态三角网格读取根 `StaticMeshComponent` 的不可变 CPU 网格，并在创建时烘入正缩放，不支持动态三角网格或复合碰撞。

`PhysicsScene::update(elapsed)` 每帧先执行一次 `World::update`，然后按 60 Hz 最多推进 8 个固定子步。`StepResult` 报告完成步号、子步数、剩余/丢弃时间、失效命令及接触通知溢出。`set_paused` 和 `step_once` 控制物理子步。`on_before_step` 每个子步在消费命令前调用，适合持续施力或角色/平台输入；`on_after_step` 在求解、位姿回写和接触收集后调用，可读取当前完成步，提交的新命令由后续子步消费。两者收到步号及固定步长，回调失败会中止当前帧推进。动态体和运动学体完成步后回写根位姿；业务可用 `BodyHandle` 提交一次性力、冲量、运动学目标或传送命令，并用 `read_body_state` 读取最近完成态。句柄含场景与重建代际，重建、停用或离场后失效；事件回调在本帧子步结束后派发，其命令从下一帧的固定步生效。

`BodySettings` 提供 16 类对象的双向 Ignore/Overlap/Block 响应、Sensor 和摩擦/恢复系数；双方 Block 才阻挡，Ignore 优先，Overlap/Sensor 不求解阻挡。接触按对象对归并为 began/persisted/ended，卸载或停用的 Exit 标记 `body_removed`。事件在本帧所有子步及位姿回写后派发，回调身份来自同一 Runtime。同步 `raycast`、`sweep_sphere`、`overlap_sphere` 返回带完成步号的命中列表，并支持类别、忽略对象、any/closest/all 与容量截断。查询只返回已登记 BodyComponent 的物理体；虚拟角色和布娃娃部件未映射到普通查询命中身份。

`PhysicsScene::trace` 提供参考 UE 的通道/对象类型选择和 test/single/multi 模式，`SceneQueryComponent` 把射线、球扫掠、球重叠配置接入 Scene v2、固定步采样和脚本原生调用。同 Actor 可挂多个查询，默认忽略自身；停用或离场清空活动结果，保存只保留配置。完整语义、C++/脚本示例及限制见[场景查询与射线组件](scene-queries.md)，官方 `Raycasts.scene` 提供六种配置。

`cook_triangle_mesh` 将静态网格生成带格式版本、Jolt 版本、源身份、缩放和摘要的二进制形状；读取时验证原始网格与 Cook 数据一致。`Project::cook_collision` 将工程 OBJ 的形状写入 `Cache/Physics`，`Project::collision` 校验并读取。编辑器场景文件持久化组件配置及网格来源，工程网格加载需要对应的有效 Cook 缓存，缺失、损坏或上游 Jolt 版本变化须重新 Cook。内置网格可直接从 CPU 网格创建三角形状，不生成工程缓存。场景加载先恢复网格与 Transform，再配置依赖它们的 Body。

编辑器通过 Actor Structure 的统一 Add Component 菜单添加 Body，选择组件后 Delete 删除，操作参与场景撤销。`Session::configure_body` 提供完整配置事务，Scene v2 保存全部 Body 设置，读取兼容已有 v1 物理工程。Parameters 连续展示已反射属性；BodySettings 尚未反射，形状、Sensor、材料、类别/响应矩阵等仍通过 C++ 配置及 Scene 文件保存，没有专有 Components 页或重复按钮入口。工程三角网格的 Cook 目前使用 Project API；碰撞几何可视化、Cook 管理页尚未实现。

官方 [Examples/Physic](../../../assets/examples/Examples/Physic/README.md) 提供 10 个可保存、重开的组件场景。通过 Project → Examples → Physic 或 `atelier_editor --physic` 打开；编辑场景保持静止，Project → Run 从快照创建独立运行会话，经 Session 统一推进 World 和 Jolt。原 [物理验收夹具](../../../acceptance/editor/fixtures/physics-project/README.md)继续验证既有 Body 场景兼容性。

在已有 Body 基础上，Character、Ragdoll/RagdollPart、Vehicle/VehicleWheel、DistanceConstraint、Force、KinematicMotion 都是 Runtime 注册的真实组件，可沿统一 Add Component 入口添加并保存公开数值属性。场景内部的组件协调器通过原有 PhysicsScene API 管理后端，没有第二套 World/对象表。重复 reconcile 保留有效句柄；组件、根、Body 或可视部件停用/离场立即撤销依赖，恢复时重建代际。输入每固定步读取，形状/关节/悬挂设置在激活时采样，修改配置后需要停用再启用。Body、Character、Ragdoll 只能有一个活跃根位姿写入者，`translate_owner` 不得移动含有这些组件的 Actor。

布娃娃部件与车轮继承 StaticMeshComponent，作为单位缩放根的直接子组件，通过 World 的受控局部模拟入口更新姿态，保留网格缩放。约束目标为 Runtime 跟踪的弱 Actor 引用，停止后用 `set_constraint_target` 绑定；Scene v2 的 `constraint_target` 保存从 1 开始的场景 Actor 顺序（0 为空），加载全部 Actor 后恢复引用，拒绝越界身份或跨 World 活跃目标。句柄不写入文档，目标选择器和专用资产编辑面板尚未提供。

P4 的场景 API 包含 `CharacterVirtual` 胶囊角色的移动、跳跃、坡道/台阶与移动平台，两个 Body 之间的距离约束，基于 Jolt Ragdoll 的多部件布娃娃，以及四轮车辆的油门、转向和刹车。角色由场景每固定步调用 `ExtendedUpdate`，不以自由动态刚体代替角色控制。角色和布娃娃要求已启用、单位缩放的根组件；布娃娃局部部件位置按根旋转转换。Actor 启用普通 Body 时会撤销其角色或布娃娃，使同一根只有一个物理位姿写入者；旧专用句柄立即失效。布娃娃和车辆的专用句柄也带场景与代际，销毁、停用和 World 停止后失效。当前只提供这些玩法系统的基础配置与状态读取；骨骼动画映射、车辆资产参数、关节限位编辑器和存档/网络格式尚未建立。

无窗口真实 World/Jolt 验收使用：

```powershell
cmake --build --preset dev --target physics_acceptance editor_document_acceptance
cmake --build --preset release --target physics_acceptance editor_document_acceptance
```

`physics_acceptance` 提供 18 个独立场景，逐项输出 RUN/PASS/FAIL。`physics_examples_acceptance` 验证 10 个官方场景的真实磁盘往返、Jolt 模拟和编辑器 GPU 画面。完整覆盖矩阵、单场景重跑方法和结果见[物理验收](acceptance.md)。`editor_document_acceptance` 覆盖工程缓存、Body 配置历史和场景文档；`editor_acceptance` 的真实窗口用例验证 Actor Structure 添加/删除 Body 及撤销，运行时需要可授予前台焦点的桌面会话。目前没有专用服务器可执行宿主。

暂停下重复 `step_once` 合并为一次请求；前回调失败时请求保留，修复回调后可重试；求解完成后的回调失败不重复执行已完成步。显式删除距离约束或移除任一端点都会唤醒幸存端点，解除支撑后继续受重力影响。角色、布娃娃创建同时检查 Actor 活跃状态和所属 World；共享同一 Runtime 不代表可以跨 World 使用 Actor，detach/转移后须在新场景重新创建专用句柄。
