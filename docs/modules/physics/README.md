# 物理模块

`atelier_physics` 使用仓库锁定的 Jolt 源码，公开接口只暴露项目的 Runtime、World、数学值和资源身份。[Jolt 使用设计方案](jolt-usage-design.md)记录目标语义与阶段边界；本文描述当前源码可用的功能。

先在 Runtime 创建对象前依次调用 `world::register_types(runtime)` 和 `physics::register_types(runtime)`。一个运行中的 World 对应一个由宿主持有的 `PhysicsScene`，场景须先于 World/Runtime 销毁；同进程多个场景共享 Jolt 全局注册。Actor 最多启用一个非空间 `BodyComponent`，形状使用显式根 `SceneComponent` 的世界位姿。Box、Sphere、Capsule 支持静态、运动学或动态体，根缩放须为 `(1,1,1)`；静态三角网格读取根 `StaticMeshComponent` 的不可变 CPU 网格，并在创建时烘入正缩放，不支持动态三角网格或复合碰撞。

`PhysicsScene::update(elapsed)` 每帧先执行一次 `World::update`，然后按 60 Hz 最多推进 8 个固定子步。`StepResult` 报告完成步号、子步数、剩余/丢弃时间、失效命令及接触通知溢出。`set_paused` 和 `step_once` 控制物理子步。`on_before_step` 每个子步在消费命令前调用，适合持续施力或角色/平台输入；`on_after_step` 在求解、位姿回写和接触收集后调用，可读取当前完成步，提交的新命令由后续子步消费。两者收到步号及固定步长，回调失败会中止当前帧推进。动态体和运动学体完成步后回写根位姿；业务可用 `BodyHandle` 提交一次性力、冲量、运动学目标或传送命令，并用 `read_body_state` 读取最近完成态。句柄含场景与重建代际，重建、停用或离场后失效；事件回调在本帧子步结束后派发，其命令从下一帧的固定步生效。

`BodySettings` 提供 16 类对象的双向 Ignore/Overlap/Block 响应、Sensor 和摩擦/恢复系数；双方 Block 才阻挡，Ignore 优先，Overlap/Sensor 不求解阻挡。接触按对象对归并为 began/persisted/ended，卸载或停用的 Exit 标记 `body_removed`。事件在本帧所有子步及位姿回写后派发，回调身份来自同一 Runtime。同步 `raycast`、`sweep_sphere`、`overlap_sphere` 返回带完成步号的命中列表，并支持类别、忽略对象、any/closest/all 与容量截断。查询只覆盖 Jolt 场景中的刚体；默认虚拟角色不进入普通 Ray/Overlap 查询。

`cook_triangle_mesh` 将静态网格生成带格式版本、Jolt 版本、源身份、缩放和摘要的二进制形状；读取时验证原始网格与 Cook 数据一致。`Project::cook_collision` 将工程 OBJ 的形状写入 `Cache/Physics`，`Project::collision` 校验并读取。编辑器场景文件只持久化 Body 配置及网格来源，工程网格加载需要对应的有效 Cook 缓存，缺失、损坏或上游 Jolt 版本变化须重新 Cook。内置网格可直接从 CPU 网格创建三角形状，不生成工程缓存。场景加载先恢复网格与 Transform，再配置依赖它们的 Body。

编辑器通过 Actor Structure 的统一 Add Component 菜单添加 Body，选择组件后 Delete 删除，操作参与场景撤销。`Session::configure_body` 提供完整配置事务，Scene v2 保存全部 Body 设置，读取兼容已有 v1 物理工程。Parameters 连续展示已反射属性；BodySettings 尚未反射，形状、Sensor、材料、类别/响应矩阵等仍通过 C++ 配置及 Scene 文件保存，没有专有 Components 页或重复按钮入口。工程三角网格的 Cook 目前使用 Project API；碰撞几何可视化、Cook 管理页尚未实现。

仓库内的 [物理验收夹具](../../../acceptance/editor/fixtures/physics-project/README.md)保留原场景作为自动验收输入，不作为官方 Examples 分发。编辑场景保持静止；Project → Run 从快照创建独立运行会话，经 Session 统一推进 World 和 Jolt。该工程的实际 Jolt 加载与步进由 `editor_document_acceptance` 验证。角色、约束、布娃娃、车辆等专用系统没有对应的编辑器场景持久格式，仍由下述无窗口场景验收。

P4 的场景 API 包含 `CharacterVirtual` 胶囊角色的移动、跳跃、坡道/台阶与移动平台，两个 Body 之间的距离约束，基于 Jolt Ragdoll 的多部件布娃娃，以及四轮车辆的油门、转向和刹车。角色由场景每固定步调用 `ExtendedUpdate`，不以自由动态刚体代替角色控制。角色和布娃娃要求已启用、单位缩放的根组件；布娃娃局部部件位置按根旋转转换。Actor 启用普通 Body 时会撤销其角色或布娃娃，使同一根只有一个物理位姿写入者；旧专用句柄立即失效。布娃娃和车辆的专用句柄也带场景与代际，销毁、停用和 World 停止后失效。当前只提供这些玩法系统的基础配置与状态读取；骨骼动画映射、车辆资产参数、关节限位编辑器和存档/网络格式尚未建立。

无窗口真实 World/Jolt 验收使用：

```powershell
cmake --build --preset dev --target physics_acceptance editor_document_acceptance
cmake --build --preset release --target physics_acceptance editor_document_acceptance
```

`physics_acceptance` 提供 15 个独立场景，逐项输出 RUN/PASS/FAIL。完整覆盖矩阵、单场景重跑方法和本轮结果见[物理验收](acceptance.md)。`editor_document_acceptance` 覆盖工程缓存、Body 配置历史和场景文档；`editor_acceptance` 的真实窗口用例验证 Actor Structure 添加/删除 Body 及撤销，运行时需要可授予前台焦点的桌面会话。目前没有专用服务器可执行宿主。

暂停下重复 `step_once` 合并为一次请求；前回调失败时请求保留，修复回调后可重试；求解完成后的回调失败不重复执行已完成步。显式删除距离约束或移除任一端点都会唤醒幸存端点，解除支撑后继续受重力影响。角色、布娃娃创建同时检查 Actor 活跃状态和所属 World；共享同一 Runtime 不代表可以跨 World 使用 Actor，detach/转移后须在新场景重新创建专用句柄。
