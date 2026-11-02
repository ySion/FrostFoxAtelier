# 物理场景验收

场景由正式 Runtime、World 和 `PhysicsScene` 公开接口构建，使用仓库锁定的真实 Jolt。入口为 `acceptance/physics/main.cpp`；`scenarios.inl` 提供独立模块场景，`integration.inl` 保留静态网格、当前 Cook 格式和旋转布娃娃代表。验收宿主显式链接 `atelier_physics` 与 `atelier_presentation`，不自行初始化 Jolt 或访问后端内部对象。

Physics 本身只依赖 World、geometry 和 Jolt。创建 `PhysicsScene` 时，宿主必须传入 `MeshReader` 函数引用；编辑器及本验收接入 `presentation::read_mesh_source`，按实际 Primitive 身份取得拥有型 `geometry::MeshSource`（source + 不可变 MeshRef），不读取或持有默认材质。插值交换值 `world::PresentedPose` 同时携带显示位姿与权威位姿；Physics 的返回 span 在下一次更新、姿态/生命周期编辑、暂停或析构时失效，插值与位姿权威规则未变。

## 运行

先按[根 README](../../../README.md#构建与端到端验收)准备锁定工具链并配置 dev/release。以下目标同时构建和执行，失败返回非零；所有检查在 Release 中仍生效。

```powershell
cmake --build --preset dev --target physics_acceptance world_acceptance editor_document_acceptance
cmake --build --preset release --target physics_acceptance world_acceptance editor_document_acceptance
cmake --build --preset dev --target project_scene_acceptance
cmake --build --preset release --target project_scene_acceptance

./out/dev/atelier_physics_acceptance.exe --list
./out/dev/atelier_physics_acceptance.exe --scenario special_integration
./out/release/atelier_physics_acceptance.exe --scenario lifecycle
```

不带参数运行全部场景；`--scenario NAME` 只运行指定项，未知名称或参数返回失败。`--list` 列出当前场景，每项输出 RUN 和 PASS/FAIL，最后汇总成功数与总数。数值检查按场景使用容差，不依赖可被编译移除的 `assert`。场景失败后通过 RAII 释放 World/Jolt 资源并继续报告其他场景。

## 场景与检查点

每类数学行为只保留一个代表，预期取作者输入、解析几何或不变量，不以另一份解算输出为真值。生命周期序列保留各自必要的触发条件与清理断言；故障注入不是错误文本矩阵。

| 场景名 | 类别 | 保留检查与独立依据 |
| --- | --- | --- |
| `world_mesh` | K3/K6 | 同 source 换 MeshRef：宽 8 的地面覆盖 x=3，X 减半后不覆盖；已取得的旧 Mesh 快照不被改写 |
| `rigid_shapes` | K3 | 一个 Box 落地，地面 y=0 加半高 .5 得支撑中心 y=.5 |
| `forces` | K3 | 一个已有冲量的动态体执行默认 teleport，动量清零 |
| `primitive_composition` | K3 | 子位置 (4,2,0) 经父 yaw90° 与平移 (10,2,0)，刚体初始世界位置为 (10,4,-4) |
| `primitive_authoring` | K3 | Box 半宽 .5 乘 scale4，表面 x=2；运动学子体目标世界 x=12 到达后速度为零，父 x=10 故局部 x=2 |
| `mirrored_scale` | K3 | 非对称斜面镜像后 x∈[-2,-1]，x=-1.5 时 y=1、正面朝 -X；镜像父下世界 +X 运动使子局部 x 减少且保留作者旋转/scale |
| `pose_publication` | K3 | before-step 写作者 z=3，同一步 World 与物理结果均为字面 3 |
| `contact_capacity` | K3/K6 | 一次完整的 28 个拥塞体加 4 个受影响体序列：确认进入溢出，移走拥塞体，确认退出溢出，四个原接触恢复且支撑中心仍为 y=.5 |
| `render_interpolation` | K3 | 直接写作者 x=-40 后呈现 x=-40，不沿旧位姿拖尾；不再以旧/新解算输出互比证明正常插值 |
| `cooked_assets` | K1/K3 | 当前 Cook 格式一次真实磁盘往返与加载，宽 8 的立方体形状覆盖 x=3；不含旧版本、损坏、source/scale 不匹配矩阵 |
| `fixed_step` | K6 | before/after/contact 成功 stop 后无后续回调及旧事件；前步失败不解算，后步失败保留已完成一步，重启清除待单步 |
| `lifecycle` | K6 | before/after/contact 成功 close；Tick/任务/激活/stop/close 五种失败入口完成逆序 End/注销与清理，旧命令不影响新代；销毁一个 Scene 不影响另一个 |
| `character` | K3 | 一个胶囊角色落地，radius .35 + half-height .6 得支撑中心约 .95，沿用原容差 |
| `constraint` | K3 | 固定锚点 (0,5,0)、距离区间 [2,2]，受重力与冲量后欧氏距离仍为 2 |
| `special_integration` | K3 | 一个 yaw90° 布娃娃，局部 arm(.6,0,0) 经平移 (-10,2,0) 得 x=-10、z=-.6 |
| `components` | K3/K6 | 重挂后运动学位置取作者正弦公式；Character remove/restore、stop/restart 后端代际；Ragdoll part/控制器停用释放；约束端点移除后释放并唤醒另一端 |
| `trace_queries` | K3 | line 距离 5.5/表面 z=2.5/法线 -Z；半径 .25 的 sweep 距离 2.25、球心 z=-.75、接触 z=-.5；overlap 中心为原点 |
| `query_components` | K3/K6 | 作者 start=-5、end=-5+2×10=15；remove/restore 与 stop 清缓存；首次协调前请求不能污染重启 |
| `entity_bindings` | K3/K6 | 嵌套 Character 初始 x=12、z=0；布娃娃写回保留作者显示 scale；part 失活撤销后端；位姿写入冲突后清理不泄漏 |

`physics-cpu` 是测量入口，不属于验收或 Full 清单。编辑器的场景文档与运行接入范围另列如下。

## 官方 Physic 场景

[Physic](../../../examples/Content/Physic/README.md) 包含 9 个组件场景。姿态与查询数学由上述 Physics 代表覆盖；约束目标和持有序列的场景对象映射由 Editor 的 entity composition 保存重开代表覆盖。这些示例不含独有外部资产身份，不另外保留批量文档模拟验收。

`--examples` 文档验收只保留 Sponza 的独有资产身份往返，不承担 Physic 全场景验证。`project_scene_acceptance` 用已配置物理的 Primitive 保留脚本位姿所有权冲突后的停止清理代表；Play 变更另保留 Ragdoll 拒绝作者移动后原位姿不变。独立 Project Run 的布娃娃场景还运行 180 帧并输出最终场景。

`project_scene_acceptance` 还通过正式 JIT 绑定查询派生组件 Tick，验证原生继承方法、一次请求、布尔/计数/距离/Vector3 返回、保存后瞬态清空和代码拥有者释放。

## 边界

底层场景是无窗口真实模拟。Project Run 使用独立会话推进物理，打开工程本身不会开始模拟。上述代表不证明九个 Physic 场景的 GPU 画面、碰撞调试绘制或专用服务器宿主。

覆盖范围是已公开的基础物理 API；没有据此宣称 Convex/Compound/Heightfield、任意关节类型、骨骼动画映射、专用资产编辑或跨平台确定性已实现。当前场景不是负载基准，不给出性能预算或吞吐结论。
