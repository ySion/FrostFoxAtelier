# 物理场景验收夹具

原 Physics Test 的内置网格场景保留于本目录，作为自动验收输入，不列入官方 Examples，也不随编辑器分发。场景刻意保留旧 `physics.BodyComponent` 格式，验证加载时合并到根 Primitive、普通空间根升级为 CollisionComponent，以及重新保存后的通用 `properties.body` 配置；旧类不参与运行时注册。场景含静态网格地面、动态箱/球、胶囊碰撞、Sensor、运动学平台及力/冲量目标。

`cmake --build --preset dev --target editor_physics_project_acceptance` 沿正式 Project/Scene 加载并推进真实 Jolt，验证重力、力、冲量、运动学目标与 Sensor 接触；发布配置替换为 `release`。完整系统范围见 [物理模块](../../../../docs/modules/physics/README.md)。
