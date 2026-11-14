# 物理场景验收

场景由正式 Runtime、World 和 `PhysicsScene` 公开接口构建，使用仓库锁定的真实 Jolt。入口为 `acceptance/physics/main.cpp`；`scenarios.inl` 提供独立模块场景，`integration.inl` 保留当前 Cook 格式代表。验收宿主显式链接 `atelier_physics` 与 `atelier_presentation`，不自行初始化 Jolt 或访问后端内部对象。

Physics 本身只依赖 World、geometry 和 Jolt。创建 `PhysicsScene` 时，宿主必须传入 `MeshReader` 函数引用；编辑器及本验收接入 `presentation::read_mesh_source`，按实际 Primitive 身份取得拥有型 `geometry::MeshSource`（source + 不可变 MeshRef），不读取或持有默认材质。插值交换值 `world::PresentedPose` 同时携带显示位姿与权威位姿；Physics 的返回 span 在下一次更新、姿态/生命周期编辑、暂停或析构时失效，插值与位姿权威规则未变。

## 运行

先按[根 README](../../../README.md#构建与端到端验收)准备锁定工具链并配置 dev/release。以下目标同时构建和执行，失败返回非零；所有检查在 Release 中仍生效。

```powershell
cmake --build --preset dev --target physics_acceptance world_acceptance
cmake --build --preset release --target physics_acceptance world_acceptance
cmake --build --preset dev --target project_scene_acceptance
cmake --build --preset release --target project_scene_acceptance

./out/dev/atelier_physics_acceptance.exe --list
./out/dev/atelier_physics_acceptance.exe --scenario cooked_assets
./out/release/atelier_physics_acceptance.exe --scenario failed_lifecycle
```

不带参数运行全部场景；`--scenario NAME` 只运行指定项，未知名称或参数返回失败。`--list` 列出当前场景，每项输出 RUN 和 PASS，最后汇总成功数与总数。数值检查按场景使用容差，不依赖可被编译移除的 `assert`。任一检查失败经统一 fatal 出口立即以非零退出，不继续运行其他场景。

## 场景与检查点

每类数学行为只保留一个代表，预期取作者输入、解析几何或不变量，不以另一份解算输出为真值。生命周期序列保留各自必要的触发条件与清理断言；故障注入不是错误文本矩阵。

| 场景名 | 类别 | 保留检查与独立依据 |
| --- | --- | --- |
| `rigid_shapes` | K3 | 一个 Box 落地，地面 y=0 加半高 .5 得支撑中心 y=.5 |
| `cooked_assets` | K1/K3 | 当前 Cook 格式一次真实磁盘往返与加载，宽 8 的立方体形状覆盖 x=3；不含旧版本、损坏、source/scale 不匹配矩阵 |
| `failed_lifecycle` | K6 | 注入 Tick 失败后 World 停止并释放全部物理体，查询不再命中；重启后失败运行排队的冲量不影响新代，close 后不残留物理体 |

`physics-cpu` 是测量入口，不属于验收或 Full 清单。编辑器的场景文档与运行接入范围另列如下。

## 官方 Physic 场景

[Physic](../../../examples/Content/Physic/README.md) 包含 9 个组件场景，目前没有逐场景自动验收；上述代表不覆盖其中的角色、约束、布娃娃与场景查询。

`project_scene_acceptance` 保留一个失败 Tick 中嵌套 stop 的停止顺序代表：End 返回前 SceneQueryComponent 的物理状态仍有效，End 后才释放。

## 边界

底层场景是无窗口真实模拟。Project Run 使用独立会话推进物理，打开工程本身不会开始模拟。上述代表不证明九个 Physic 场景的 GPU 画面、碰撞调试绘制或专用服务器宿主。

覆盖范围是已公开的基础物理 API；没有据此宣称 Convex/Compound/Heightfield、任意关节类型、骨骼动画映射、专用资产编辑或跨平台确定性已实现。当前场景不是负载基准，不给出性能预算或吞吐结论。
