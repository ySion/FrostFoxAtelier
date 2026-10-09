# 物理场景验收夹具

原 Physics Test 的内置网格场景保留于本目录，作为自动验收输入，不列入官方 Examples，也不随编辑器分发。场景含静态网格地面、动态箱/球、胶囊 Body、Sensor、运动学平台及力/冲量目标。

`cmake --build --preset dev --target editor_physics_project_acceptance` 沿正式 Project/Scene 加载并推进真实 Jolt，验证重力、力、冲量、运动学目标与 Sensor 接触；发布配置替换为 `release`。完整系统范围见 [物理模块](../../../../docs/modules/physics/README.md)。
