# Examples 工程

这是独立的普通文件夹工程，不随编辑器部署，也不是引擎启动依赖。编辑器无参数启动为空文档；通过 **File → Open Project** 选择本目录，或在仓库根目录运行：

```powershell
./out/release/atelier_editor.exe --project ./examples
./out/release/atelier_editor.exe --project ./examples --scene "Content/Scene Lit Room/Scenes/Main.scene"
```

`Config/Project.json` 的启动场景为 Sponza。资源浏览器按 **Project → Content → Sponza / Scene Lit Room / Physic / Dungeons** 展示真实目录，双击 `.scene` 切换场景。保存直接写入所打开的工程；需要隔离修改时，先自行复制整个工程到其他位置。编辑器不创建、读取或删除 `%LOCALAPPDATA%/FrostFoxAtelier/OfficialExamples/`。

| 内容 | 入口 |
| --- | --- |
| [Sponza](Content/Sponza/README.md) | Intel New Sponza 建筑与布帘、天空贴图、三盏真实光源和 DDGI |
| [Scene Lit Room](<Content/Scene Lit Room/README.md>) | 材质、面光源、自发光和屏幕反射的紧凑观察场景 |
| [Physic](Content/Physic/README.md) | 9 个物理组件场景：刚体、力、触发/查询、角色、约束及布娃娃 |
| [Dungeons](Content/Dungeons/README.md) | Synty 地牢 Demo 及两个资源总览，保留非脚本源资源 |

`Config` 保存工程配置，`Source` 保存共享业务源码（当前仅有占位文件），`Content/<名称>/` 收纳场景与资源，`Cache` 是可重建的本地缓存。新增场景保存到 Content；资源身份由 `.asset.json` 保持。克隆时需 `git lfs pull` 取得模型、glTF buffer、HDR 与贴图原文件；各资产许可见其目录。未被场景使用的 Synty 动画在[独立资源目录](../assets/animations/README.md)。

14 个场景均为 Scene v9，包含普通主相机与 Environment 引用；**Project → Run** 使用场景主相机，不自动取景。编辑视口仍使用默认/当前视图：选择 Main Camera 后 **View from Camera** 可观察同一视点；用显示菜单选择 Scene Lit 或 Smooth。打开场景或使用 `--scene-lit-camera` 不拟合 DDGI，只有 **World Settings → Coverage → Fit Scene** 明确修改作者体积并进入撤销历史。

`editor_examples_acceptance` 是无窗口内容验收：逐一严格加载全部场景，检查资源身份、主相机、材质槽及临时保存重开；不修改仓库场景。对应 Sponza、Scene Lit Room、Physic、Dungeons 的窗口验收使用普通工程入口，只在必要的隔离内容副本保存。
