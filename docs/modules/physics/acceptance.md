# 物理场景验收

场景由正式 Runtime、World 和 `PhysicsScene` 公开接口构建，使用仓库锁定的真实 Jolt。入口为 `acceptance/physics/main.cpp`；`scenarios.inl` 提供独立模块场景，`integration.inl` 保留静态网格、触发、Cook 和专用系统组合消费者。验收目标只链接 `atelier_physics`，不自行初始化 Jolt 或访问后端内部对象。

## 运行

先按[根 README](../../../README.md#构建与端到端验收)准备锁定工具链并配置 dev/release。以下目标同时构建和执行，失败返回非零；所有检查在 Release 中仍生效。

```powershell
cmake --build --preset dev --target physics_acceptance world_acceptance editor_document_acceptance editor_physics_project_acceptance
cmake --build --preset release --target physics_acceptance world_acceptance editor_document_acceptance editor_physics_project_acceptance
cmake --build --preset dev --target physics_examples_acceptance project_scene_acceptance
cmake --build --preset release --target physics_examples_acceptance project_scene_acceptance

./out/dev/atelier_physics_acceptance.exe --list
./out/dev/atelier_physics_acceptance.exe --scenario vehicle
./out/release/atelier_physics_acceptance.exe --scenario world_ownership
```

不带参数运行全部场景；`--scenario NAME` 只运行指定项，未知名称或参数返回失败。`--list` 列出当前场景，每项输出 RUN 和 PASS/FAIL，最后汇总成功数与总数。数值检查按场景使用容差，不依赖可被编译移除的 `assert`。场景失败后通过 RAII 释放 World/Jolt 资源并继续报告其他场景。

## 场景与检查点

| 场景名 | 构建的物理场景 | 主要检查 |
| --- | --- | --- |
| `world_mesh` | 三角网格地面、球、箱体 | 下落与支撑高度、Entity 位姿回写、网格替换、动态三角网格拒绝、前后步回调、接触和停止清理 |
| `rigid_shapes` | Box/Sphere/Capsule 下落及运动学平台 | 重力、各形状支撑高度、休眠、静态体拒绝施力、运动学目标、非法配置不覆盖原值 |
| `forces` | 零重力下三个相同动态体 | 等效力与冲量、延迟消费且只消费一次、传送保留/清除速度、自定义重力、NaN 拒绝 |
| `primitive_composition` | Entity 子树中的网格刚体、动态碰撞子实体和视觉后代，组成顺序倒置 | 独立句柄/命令、世界姿态创建、父先子回写、查询身份、视觉跟随、只卸载目标、重建代际和旧命令拒绝 |
| `primitive_authoring` | 反射配置的缩放网格与运动学触发子实体 | 属性验证回滚、Box 缩放、owner Entity 刚体查找、运动学目标到达后停止、局部位姿转换、非法缩放拒绝并清理 |
| `mirrored_scale` | 镜像的非对称三角网格（运行时构建与 Cook 各一）、负缩放 Box、(-1,-1,-1) 动态体及其子体、嵌套零缩放空间 | 镜像位置与正面、Box 尺寸取绝对值、镜像 Character 拒绝、镜像回写保持局部 TRS、零缩放不建刚体且只在最上层节点报告一次、恢复后重建；全程 halt 策略 |
| `pose_publication` | 静态地面、已睡眠与仍下落的动态体、下落体下的睡眠子体、空间父节点下的动态体 | 刚睡眠体回写最终位姿；绕过 World 写入静态体与睡眠体的 TRS 后，下一步刚体随之传送且与 World 一致；下落体回写；移动父节点下的睡眠子体保持世界位姿；步内（`on_before_step`）写入未被本步回写还原，下一步传送刚体；显式传送的静态体回写；只改祖先缩放使其不均匀后，下一步重新检查并按对象报告 |
| `solver_determinism` | 160 个动态箱子在多线程求解池上下落堆叠 120 帧 | 两次全新运行的全部刚体状态与 World 局部 TRS 逐位一致 |
| `contacts` | 触发区与被过滤对象 | began/persisted/ended 归并、无阻挡响应、停用 Exit 原因、查询容量截断与过滤；事件保留场景与刚体代际，历史结束事件不能操作重建后的刚体 |
| `collision_matrix` | 相交的运动学体和动态体 | Ignore/Overlap/Block 双向 9 组 × Sensor 开/关，共 18 组；Ignore 优先、仅双方 Block 且非 Sensor 时分离、单次 Enter |
| `queries` | 两个不同距离的静态体 | Ray any/closest/all、命中排序/身份/材质/法线、类别与组件过滤、Sphere Sweep/Overlap、完成步号、无效参数 |
| `cooked_assets` | 磁盘 Cook 网格反复加载到新 World | 形状可查询、二进制往返、损坏/版本不符拒绝；来源/缩放不符时按当前缩放运行期重建一次（`runtime_cooks`），缓存校验仍报不符 |
| `fixed_step` | 空中自由落体 | 经 `World::update` 驱动：半帧积累、8 子步上限与丢时、暂停/单步、重复请求合并、重入拒绝、前后回调失败停止 World、30/120 Hz 宿主得到相同 60 步、pre_physics/post_physics Tick 位于全部子步前后并收到整帧时间 |
| `frame_timing` | 地面上的动态体，每帧 2.5 个固定步长 | 在帧阶段计时 Recorder 帧内调用 `World::update`：停止的 World 不记录阶段；运行后无结构错误，各 World 阶段、`physics.step/reconcile/events` 各进入 1 次，`world.schedule` 只在启动后的首帧进入，`physics.substep/solve/write_back` 次数等于 `last_step().steps`；不检查耗时数值 |
| `lifecycle` | 同时存在的两个独立 Runtime/World/场景 | 失效命令拒绝、停用/重启代际、重复 disable/stop/close、销毁一场景不影响另一场景、运行中重建场景；直接写 `body.enabled`（不经 World）在下一步建立或撤销刚体，组件停用时不建，重新启用、detach/restore 随之撤销与恢复；同一 World 上重建 40 次场景复用能力登记，物理登记不使 Primitive 计为呈现 |
| `world_ownership` | 同一 Runtime 中两个 World | 拒绝跨 World 角色/布娃娃创建、句柄限定场景、detach 清理、转移后仅新 World 可创建 |
| `character` | 胶囊角色落地、移动和跳跃 | grounded、跳跃只消费一次、输入/重复创建拒绝、销毁重建代际、根停用清理 |
| `constraint` | 固定锚点悬挂动态体 | 距离保持、无效端点/区间拒绝、约束删除失效、显式删除及端点停用后唤醒休眠体并自由落下 |
| `ragdoll` | 两部件布娃娃落到地面 | 层级拒绝、全部部件位姿/步号、重力和碰撞、重复创建/销毁拒绝、重建及 World stop 清理 |
| `vehicle` | 四轮车在平面加速、转向和刹车 | 四轮接地、位移/速度/转向、刹车减速、底盘及输入合法性、控制器重建与底盘停用 |
| `special_integration` | 角色台阶/坡道/移动平台、旋转布娃娃、受约束车辆 | 组合运动、布娃娃局部转世界、平台继承、普通 Body 替换专用系统、停用/退出清理 |
| `components` | 组件拥有的角色、车辆、布娃娃及摆锤 | 重复 reconcile 幂等、keep_world 重挂保留句柄/冲量状态/运动相位、组合停用与独立空间参考停用的区分、删除恢复、detach/重启代际、位姿写入者排他、约束释放/唤醒、跨 World 目标拒绝 |
| `trace_queries` | 触碰、首个阻挡、遮挡后目标、通道忽略对象和 Sensor | 通道/对象类型 test/single/multi、Entity/Component 忽略集合、容量摘要、旧 closest 合同、球心与接触点、初始穿透、掩码/枚举/零长度/浮点溢出边界 |
| `query_components` | 有空间父节点的查询组件和可停用目标 | 旋转/缩放与剪切空间链、独立空间参考停用时保留请求、自身忽略、自动/一次请求、暂停与立即查询、失败清空结果、组合祖先停用、删除恢复、跨 World、旧命中失效 |
| `entity_bindings` | 嵌套角色、底盘与显式车轮 Entity 绑定，metadata 与网格分离 | world/local 位姿转换、公开持有引用序列、重复/活动绑定拒绝、批姿态发布保留显示尺度、part/wheel 上原生控制器创建的原子拒绝、非零初始部件位姿的幂等、目标禁用/恢复 |
| `query_exclusions` | 组合父与同组物理部件、actual owner 自体和远近目标 | ignore_self 不猜父组、活动显式排除与调用方集合合并、重复/空绑定拒绝保留旧集合、固定步消费与清空生效 |

## 官方 Physic 场景

[Examples/Physic](../../../assets/examples/Examples/Physic/README.md) 包含 10 个组件场景。`atelier_editor_document_acceptance --physics-examples` 通过真实磁盘写读保存全部属性及约束目标，再各执行 180 个固定步，检查刚体落地、持续力/一次冲量、运动学平台、触发与碰撞矩阵、Ray/Sweep/Overlap、角色落地移动、距离约束、六部件布娃娃及车辆接地运动。每个场景还验证重复 start/update、暂停/单步、停止和重启；越界约束目标被拒绝。

`atelier_editor --acceptance-physics-examples <输出目录>` 在输出目录的 Workspace 下为每次运行创建新的官方示例副本，避免旧测试副本缺少新增场景。它使用正式 Application/Session/World/Jolt 以及 D3D12 GPU validation。每个场景输出编辑状态及实际模拟后的两张截图，检查被接受的帧和有效几何深度；布娃娃部件坐标另由文档验收与 Jolt 姿态比对。此入口直接调用应用命令和模拟器，不发送原生鼠标输入。

`--examples` 文档验收确认老用户工程仅补入缺失的官方示例目录，重复打开保留编辑且不修改模板。`project_scene_acceptance` 验证脚本 `translate_owner` 不得覆盖 已配置物理的 Primitive、Character 或 Ragdoll 的位姿所有权。独立 Project Run 的布娃娃和车辆场景还各运行 180 帧并输出最终场景。

`Raycasts.scene` 的六个查询组件经真实磁盘保存重开，验证通道最近阻挡、通道多命中截断、对象类型穿过阻挡、布尔无载荷、球扫掠与 Sensor 重叠。`project_scene_acceptance` 还通过正式 JIT 绑定查询派生组件 Tick，验证原生继承方法、一次请求、布尔/计数/距离/Vector3 返回、保存后瞬态清空和代码拥有者释放。

## 边界

底层场景是无窗口真实模拟；官方 Physic 另有上述 GPU 场景验收。[物理验收夹具](../../../acceptance/editor/fixtures/physics-project/README.md)保留旧 Body 文档兼容性；`editor_physics_project_acceptance` 从该工程加载并检查重力、力、冲量、运动学目标和 Sensor 接触。Project Run 使用独立会话推进物理，打开工程本身不会开始模拟。验收不证明碰撞调试绘制或专用服务器宿主；文档检查、GPU 捕获和原生交互的证据范围分别陈述。

覆盖范围是已公开的基础物理 API；没有据此宣称 Convex/Compound/Heightfield、任意关节类型、骨骼动画映射、专用资产编辑或跨平台确定性已实现。当前场景不是负载基准，不给出性能预算或吞吐结论。

## Entity 迁移验收状态

当前官方内容使用 Scene v6，物理姿态写入 owner Entity，布娃娃和车辆通过 parts / wheels 显式绑定子 Entity。CPU 结果、实际日志和未完成的图形验收统一记录在[编辑器验收状态](../editor/README.md#entity-迁移验收状态)，旧模型的历史 PASS 不代表当前模型通过。
