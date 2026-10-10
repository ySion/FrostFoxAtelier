# 官方 Examples

编辑器无参数启动时打开内置 **Sponza**，使用 Scene Lit + DDGI、内庭相机和覆盖完整建筑的探针体积。所有官方场景放在同一个 **Official Examples** 工程中，资源浏览器显示 **Project → Examples → Sponza / Scene Lit Room / Physic / Dungeons**；启动时展开到 Sponza 文件夹。顶部 **Project → Examples** 菜单切换对应场景，沿现有流程处理未保存修改。

源模板由本目录维护，构建时部署到可执行文件旁的 `examples/`。资源定位只依据可执行文件位置，运行目录不必是仓库，也不需要额外下载 Sponza。克隆仓库时需 `git lfs pull` 取得 FBX、OBJ 与贴图原文件。分发编辑器时同时携带 `examples/`、字体和现有运行库。

首次打开时，在 `%LOCALAPPDATA%/FrostFoxAtelier/OfficialExamples/` 建立整个工程的可编辑副本。后续启动和菜单切换复用该副本，Ctrl+S 只保存当前场景，构建和更新模板不覆盖用户的已保存修改。新发布的官方示例仅在对应文件夹完全不存在时整体补入；已有目录内不合并、不替换文件。副本损坏时报告错误，不自动清空或替换。需要一份全新模板时，可先把用户副本移到其他位置，再打开 Examples。Dev / Release 默认共用当前用户的副本。

| 场景 | 内容 |
| --- | --- |
| [Sponza](Examples/Sponza/README.md) | 完整建筑、三盏真实光源、动态 DDGI；默认启动场景 |
| [Scene Lit Room](<Examples/Scene Lit Room/README.md>) | 材质、面光源、自发光和屏幕反射的紧凑观察场景 |
| [Physic](Examples/Physic/README.md) | 10 个物理组件场景：刚体、力、触发/查询、角色、约束、布娃娃及车辆；`--physic` 可直接打开 |
| [Dungeons](Examples/Dungeons/README.md) | Synty 地牢 Demo 及两个资源总览，保留全部非脚本源资源；Smooth 显示颜色贴图，`--dungeons` 可直接打开 |

工程目录中，`Config` 保存工程配置，`Source` 保存共享业务源码，`Content` 可存普通内容，`Examples/<场景名>/` 收纳每个示例的场景与资源。可在 Examples 下增加新的场景文件夹，刷新资源后通过资源浏览器打开；Save Scene As 支持保存到 Content 或 Examples。网格身份仍由原 `.asset.json` 保持，移动目录不修改场景中的资源身份。显示模式和相机由 Examples 菜单入口设置，不改变 Scene 文件格式。显式 `--project` / `--new-project` 保留原语义。

运行 `cmake --build --preset dev --target editor_examples_acceptance` 验证默认启动、原生菜单切换、示例保存重开、用户副本与模板隔离、工作目录独立性；发布配置换成 `release`。证据位于 `out/<配置>/examples-evidence/`。原独立 Physics Test 工程已迁出，旧 Body 场景作为 [验收夹具](../../acceptance/editor/fixtures/physics-project/README.md) 保留；未被场景使用的 Synty 动画移到 [独立资源目录](../animations/README.md)。

`atelier_editor --snapshot-examples <输出目录>` 使用正式应用命令和 D3D12 捕获，检查真实目录树、场景切换、在新增 Examples 子目录中保存重开以及模板隔离；它不发送原生输入，也不代替原生菜单与 GPU validation 验收。
