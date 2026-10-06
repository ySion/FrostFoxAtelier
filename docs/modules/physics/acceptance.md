# 物理场景验收

场景由正式 Runtime、World 和 `PhysicsScene` 公开接口构建，使用仓库锁定的真实 Jolt。入口为 `acceptance/physics/main.cpp`；`scenarios.inl` 提供独立模块场景，`integration.inl` 保留静态网格、触发、Cook 和专用系统组合消费者。验收目标只链接 `atelier_physics`，不自行初始化 Jolt 或访问后端内部对象。

## 运行

先按[根 README](../../../README.md#构建与端到端验收)准备锁定工具链并配置 dev/release。以下目标同时构建和执行，失败返回非零；所有检查在 Release 中仍生效。

```powershell
cmake --build --preset dev --target physics_acceptance world_acceptance editor_document_acceptance editor_physics_project_acceptance
cmake --build --preset release --target physics_acceptance world_acceptance editor_document_acceptance editor_physics_project_acceptance

./out/dev/atelier_physics_acceptance.exe --list
./out/dev/atelier_physics_acceptance.exe --scenario vehicle
./out/release/atelier_physics_acceptance.exe --scenario world_ownership
```

不带参数运行全部 15 项；`--scenario NAME` 只运行指定项，未知名称或参数返回失败。每项输出 RUN 和 PASS/FAIL，汇总成功为 `Physics acceptance: PASS (15/15 scenarios)`。数值检查按场景使用容差，不依赖可被编译移除的 `assert`。场景失败后通过 RAII 释放 World/Jolt 资源并继续报告其他场景。

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

## 本轮验证与修复

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

这些是无窗口、可重跑的真实模拟场景。[Physics Test 工程](../../../examples/physics-test/README.md)提供编辑器可打开的基础 Body 配置场景；`editor_physics_project_acceptance` 从该工程加载场景并用真实 Jolt 检查重力、力、冲量、运动学目标和 Sensor 接触。当前编辑器已通过独立 Project Run 会话推进物理，打开工程本身不会开始模拟。这里的场景验收不证明窗口操作、碰撞调试绘制或专用服务器宿主。`world_acceptance` 验证 World 契约，文档验收不能替代窗口验收。

覆盖范围是已公开的基础物理 API；没有据此宣称 Convex/Compound/Heightfield、任意关节类型、骨骼动画映射、专用资产编辑或跨平台确定性已实现。当前场景不是负载基准，不给出性能预算或吞吐结论。
