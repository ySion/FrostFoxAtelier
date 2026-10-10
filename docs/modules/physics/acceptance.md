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

不带参数运行全部 18 项；`--scenario NAME` 只运行指定项，未知名称或参数返回失败。每项输出 RUN 和 PASS/FAIL，汇总成功为 `Physics acceptance: PASS (18/18 scenarios)`。数值检查按场景使用容差，不依赖可被编译移除的 `assert`。场景失败后通过 RAII 释放 World/Jolt 资源并继续报告其他场景。

## 场景与检查点

| 场景名 | 构建的物理场景 | 主要检查 |
| --- | --- | --- |
| `world_mesh` | 三角网格地面、球、箱体 | 下落与支撑高度、根位姿回写、网格替换、动态三角网格拒绝、前后步回调、接触和停止清理 |
| `rigid_shapes` | Box/Sphere/Capsule 下落及运动学平台 | 重力、各形状支撑高度、休眠、静态体拒绝施力、运动学目标、非法配置不覆盖原值 |
| `forces` | 零重力下三个相同动态体 | 等效力与冲量、延迟消费且只消费一次、传送保留/清除速度、自定义重力、NaN 拒绝 |
| `contacts` | 触发区与被过滤对象 | began/persisted/ended 归并、无阻挡响应、停用 Exit 原因、查询容量截断与过滤 |
| `collision_matrix` | 相交的运动学体和动态体 | Ignore/Overlap/Block 双向 9 组 × Sensor 开/关，共 18 组；Ignore 优先、仅双方 Block 且非 Sensor 时分离、单次 Enter |
| `queries` | 两个不同距离的静态体 | Ray any/closest/all、命中排序/身份/材质/法线、类别与组件过滤、Sphere Sweep/Overlap、完成步号、无效参数 |
| `cooked_assets` | 磁盘 Cook 网格反复加载到新 World | 形状可查询、二进制往返、损坏/版本/来源/缩放不匹配拒绝 |
| `fixed_step` | 空中自由落体 | 半帧积累、8 子步上限与丢时、暂停/单步、重复请求合并、重入拒绝、前后回调失败、30/120 Hz 宿主得到相同 60 步 |
| `lifecycle` | 同时存在的两个独立 Runtime/World/场景 | 失效命令拒绝、停用/重启代际、重复 disable/stop/close、销毁一场景不影响另一场景、运行中重建场景 |
| `world_ownership` | 同一 Runtime 中两个 World | 拒绝跨 World 角色/布娃娃创建、句柄限定场景、detach 清理、转移后仅新 World 可创建 |
| `character` | 胶囊角色落地、移动和跳跃 | grounded、跳跃只消费一次、输入/重复创建拒绝、销毁重建代际、根停用清理 |
| `constraint` | 固定锚点悬挂动态体 | 距离保持、无效端点/区间拒绝、约束删除失效、显式删除及端点停用后唤醒休眠体并自由落下 |
| `ragdoll` | 两部件布娃娃落到地面 | 层级拒绝、全部部件位姿/步号、重力和碰撞、重复创建/销毁拒绝、重建及 World stop 清理 |
| `vehicle` | 四轮车在平面加速、转向和刹车 | 四轮接地、位移/速度/转向、刹车减速、底盘及输入合法性、控制器重建与底盘停用 |
| `special_integration` | 角色台阶/坡道/移动平台、旋转布娃娃、受约束车辆 | 组合运动、布娃娃局部转世界、平台继承、普通 Body 替换专用系统、停用/退出清理 |
| `components` | 组件拥有的角色、车辆、布娃娃及摆锤 | 重复 reconcile 幂等、停用根/控制器/部件/底盘、删除/恢复、detach/重启代际、单根写入者、约束端点释放/唤醒、跨 World 目标拒绝 |
| `trace_queries` | 触碰、首个阻挡、遮挡后目标、通道忽略对象和 Sensor | 通道/对象类型 test/single/multi、Actor/Component 忽略集合、容量摘要、旧 closest 合同、球心与接触点、初始穿透、掩码/枚举/零长度/浮点溢出边界 |
| `query_components` | 有空间父节点的查询组件和可停用目标 | 局部旋转/缩放、自身忽略、自动/一次请求、暂停与立即查询、失败清空结果、祖先停用、删除恢复、跨 World、旧命中失效、未登记请求的停止重启 |

## 官方 Physic 场景

[Examples/Physic](../../../assets/examples/Examples/Physic/README.md) 包含 10 个组件场景。`atelier_editor_document_acceptance --physics-examples` 通过真实磁盘写读保存全部属性及约束目标，再各执行 180 个固定步，检查刚体落地、持续力/一次冲量、运动学平台、触发与碰撞矩阵、Ray/Sweep/Overlap、角色落地移动、距离约束、六部件布娃娃及车辆接地运动。每个场景还验证重复 start/update、暂停/单步、停止和重启；越界约束目标被拒绝。

`atelier_editor --acceptance-physics-examples <输出目录>` 在输出目录的 Workspace 下为每次运行创建新的官方示例副本，避免旧测试副本缺少新增场景。它使用正式 Application/Session/World/Jolt 以及 D3D12 GPU validation。每个场景输出编辑状态及实际模拟后的两张截图，检查被接受的帧和有效几何深度；布娃娃部件坐标另由文档验收与 Jolt 姿态比对。此入口直接调用应用命令和模拟器，不发送原生鼠标输入。

`--examples` 文档验收确认老用户工程仅补入缺失的官方示例目录，重复打开保留编辑且不修改模板。`project_scene_acceptance` 验证脚本 `translate_owner` 不得覆盖 Body/Character/Ragdoll 的根位姿所有权。独立 Project Run 的布娃娃和车辆场景还各运行 180 帧并输出最终场景。

`Raycasts.scene` 的六个查询组件经真实磁盘保存重开，验证通道最近阻挡、通道多命中截断、对象类型穿过阻挡、布尔无载荷、球扫掠与 Sensor 重叠。`project_scene_acceptance` 还通过正式 JIT 绑定查询派生组件 Tick，验证原生继承方法、一次请求、布尔/计数/距离/Vector3 返回、保存后瞬态清空和代码拥有者释放。

## 2026-10-10 查询组件增量

在组件场景基础上增加 [SceneQueryComponent](scene-queries.md) 和 `Raycasts.scene`。dev/release 均构建成功，18/18 物理场景、Project Scene（包含查询 Source/JIT）、10/10 场景磁盘往返与模拟检查通过。完整编辑器文档在两种配置上通过，查询配置未破坏既有 Scene、UI 或源码链。

两种配置的正式编辑器 GPU 验收均通过 10/10 场景，每种输出 20 张编辑/模拟截图；`Raycasts-simulated.png` 显示完整目标布局。查询线本身未绘制，命中值由上述物理、文档和 JIT 验收判定。发布版额外验证在已有旧 Workspace 的输出目录中运行仍会创建新副本并加载当前 10 场景。

查询验收修复了球扫掠初始穿透的后端选项，并覆盖未首次 reconcile 的请求在停止时被取消。Actor 停止通知与组件移除期间的过渡状态分别处理；角色、布娃娃和车辆的既有生命周期场景继续通过。

日志位于 `out/<配置>/query-physics.log`、`query-project-scene.log`、`query-examples.log`、`query-editor-document.log`；截图及窗口报告位于 `out/<配置>/query-examples-evidence/`，使用同一 RX 6800 XT。结果证明当前查询和组件合同，不作为性能或完整 UE 功能对等的结论。

## 2026-10-10 组件与官方场景验证（查询增量之前）

使用锁定工具链，在开发与发布配置上完成以下实际运行，退出码均为 0：

| 配置 | 物理 | World | 完整编辑器文档 / Project Scene | 官方目录与 9 场景磁盘/模拟 | 编辑器 GPU 场景 |
| --- | --- | --- | --- | --- | --- |
| dev | 16/16 PASS | 9/9 PASS | PASS / PASS | PASS / 9/9 PASS | 9/9 PASS，18 张截图 |
| release | 16/16 PASS | 9/9 PASS | PASS / PASS | PASS / 9/9 PASS | 9/9 PASS，18 张截图 |

两种配置的独立运行进程还分别完成 Ragdoll、Vehicle 各 180 帧。证据在 `out/<配置>/physic-components.log`、`physic-world.log`、`physic-project-scene.log`、`physic-editor-document.log`、`physic-official-examples.log`、`physic-examples.log`、`physic-run-*.log` 和 `physics-examples-evidence/`，均为可重建产物。GPU 使用本机 AMD Radeon RX 6800 XT；窗口验收直接调用正式应用入口，未据此声称原生菜单鼠标交互已覆盖。

本轮修复并覆盖了隐藏弱引用的正确恢复、组件依赖停用后的后端清理、恢复时句柄换代，以及脚本移动指令对角色/布娃娃物理所有权的保护。旧 v1 文档验收补齐此前新增材质字段的默认值预期，继续独立检查完整格式兼容性。

## 2026-10-06 历史验证与修复

2026-10-06 在当前工作区修改上，使用 `cmake/toolchain-lock.json` 锁定工具链完成以下验收，两个构建命令退出码均为 0：

| 配置 | 物理 | World | 编辑器文档 | Physics Test 工程 |
| --- | --- | --- | --- | --- |
| dev | 15/15 PASS | 8/8 PASS | PASS | PASS |
| release | 15/15 PASS | 8/8 PASS | PASS | PASS |

两种配置的 `--list` 均列出 15 项；分别执行 `vehicle`、`world_ownership` 均为 1/1 PASS；未知场景和未知参数均返回非零。完整本地日志为 `out/review/physics-scenarios/dev.log`、`release.log`、`cli.log`，属于可重新生成的构建产物，不随源码提交。

Physics Test 工程使用正式 Project/Session 加载并保存重开，再由真实 Jolt 步进 90 帧检查 Body、重力、力、冲量、运动学目标及 Sensor 接触；本轮 dev/release 的独立目标与完整编辑器文档验收均退出 0。自动窗口检查未完成，不能据此断言场景在视口中的显示与操作已验收。

真实场景已复现并加入回归的修复：

- 暂停单步在前回调失败时曾提前消费请求；现在保留未执行请求，后回调失败仍保留已完成步。
- Jolt 移除距离约束不会自动唤醒端点；适配层显式唤醒，使显式删除和端点停用两条路径均恢复幸存刚体运动。
- 角色和布娃娃曾只验证 Runtime 内 Actor 活跃状态；现在同时验证所属 World，阻止另一场景创建第二个物理写入者。

## 边界

底层 18 项是无窗口真实模拟；官方 Physic 另有上述 GPU 场景验收。[物理验收夹具](../../../acceptance/editor/fixtures/physics-project/README.md)保留旧 Body 文档兼容性；`editor_physics_project_acceptance` 从该工程加载并检查重力、力、冲量、运动学目标和 Sensor 接触。Project Run 使用独立会话推进物理，打开工程本身不会开始模拟。验收不证明碰撞调试绘制或专用服务器宿主；文档检查、GPU 捕获和原生交互的证据范围分别陈述。

覆盖范围是已公开的基础物理 API；没有据此宣称 Convex/Compound/Heightfield、任意关节类型、骨骼动画映射、专用资产编辑或跨平台确定性已实现。当前场景不是负载基准，不给出性能预算或吞吐结论。
