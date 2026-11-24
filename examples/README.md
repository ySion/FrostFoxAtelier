# Examples 工程

这是独立的普通文件夹工程，不随编辑器部署，也不是引擎启动依赖。编辑器无参数启动为空文档；通过 **File → Open Project** 选择本目录，或在仓库根目录运行：

```powershell
./out/release/atelier_editor.exe --project ./examples
./out/release/atelier_editor.exe --project ./examples --scene "Content/Scene Lit Room/Scenes/Main.scene"
```

`Config/Project.json` 的启动场景为 Sponza。资源浏览器按 **Project → Content → Sponza / Scene Lit Room / Physic / Procedural / Dungeons** 展示真实目录，双击 `.scene` 切换场景。保存直接写入所打开的工程；需要隔离修改时，先自行复制整个工程到其他位置。编辑器不创建、读取或删除 `%LOCALAPPDATA%/FrostFoxAtelier/OfficialExamples/`。

| 内容 | 入口 |
| --- | --- |
| [门楼 Gatehouse](Content/Procedural/Scenes/Gatehouse.scene) | 可进入的木石城门、过街楼与双塔。通过总图调整通道净宽和屋脊下挠量，用 Control 射线复测。[模型与编辑说明](Content/Procedural/README.md#门楼-gatehouse) |
| [短木桥 Footbridge](Content/Procedural/Scenes/Footbridge.scene) | 带扶手、交替斜撑与横铺木板的六米木桥。调整桥宽、两端高差和柱距，观察同一组站点如何驱动桥面与栏杆。[模型与编辑说明](Content/Procedural/README.md#短木桥-footbridge) |
| [弧形石栏杆 ArcBalustrade](Content/Procedural/Scenes/ArcBalustrade.scene) | 瓶形栏柱、连续扶手和四皮错缝石墙共用一条圆弧。可分别调整弧线半径和最大柱距。[模型与编辑说明](Content/Procedural/README.md#弧形石栏杆-arcbalustrade) |
| [Sponza](Content/Sponza/README.md) | Intel New Sponza 建筑与布帘、天空贴图、三盏真实光源和 DDGI |
| [Scene Lit Room](<Content/Scene Lit Room/README.md>) | 材质、面光源、自发光和屏幕反射的紧凑观察场景 |
| [Physic](Content/Physic/README.md) | 9 个物理组件场景：刚体、力、触发/查询、角色、约束及布娃娃 |
| [Procedural](Content/Procedural/README.md) | 阶梯岛屿散布、带道路排斥与扫掠围墙的哨站、可调层高/开间的模块化建筑废墟（自定义 `.source` SOP + `Sweep`/`Extrude`）、2D Network 联动与实例 Bake |
| [Dungeons](Content/Dungeons/README.md) | Synty 地牢 Demo 及两个资源总览 |

`Config` 保存工程配置，`Source` 保存共享业务源码与自定义程序化节点（如 `Source/Procedural/` 下的 `TerraceIsland.source`、`FortifiedOutpost.source`、`ModularRuinBuilding.source`），`Content/<名称>/` 收纳场景与资源，`Cache` 是可重建的本地缓存。新增场景保存到 Content；资源身份由 `.asset.json` 保持。克隆时需 `git lfs pull` 取得模型、glTF buffer、HDR 与贴图原文件；各资产许可见其目录。

16 个场景均为 Scene v10，包含普通主相机与 Environment 引用；材质使用 v2 MS/MI 作者格式，组件不再保存固定的颜色、粗糙度、金属度和发光字段。Sponza 引用网格默认 MI；Physic、Scene Lit Room、Procedural 与 Dungeons 的 MI 保存作者外观，父材质为内置 StandardPBR。未赋材质的槽显示 DefaultWorldGrid，不自动生成白色 MI。

**Project → Run** 使用场景主相机，不自动取景。编辑视口仍使用默认/当前视图：选择 Main Camera 后 **View from Camera** 可观察同一视点；用显示菜单选择 Scene Lit 或 Smooth。打开场景或使用 `--scene-lit-camera` 不拟合 DDGI，只有 **World Settings → Coverage → Fit Scene** 明确修改作者体积并进入撤销历史。

`editor_log_smoke_acceptance` 复制本工程的 `Config`、`Source` 与启动场景所在的 `Content/Sponza`，在真实编辑器中打开启动场景 Sponza，做一次添加/保存/撤销并在 Play 运行中关闭编辑器，检查日志零错误后自动删除临时工程副本，不修改仓库场景；`project_scene_acceptance` 只读校验 `Content/Procedural/Scenes/` 下两个程序化示例场景的严格加载、流形拓扑与零穿模/零浮空空间诊断；它们不承担示例工程的渲染效果矩阵。其他示例场景目前没有自动验收。
