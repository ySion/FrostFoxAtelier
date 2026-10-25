# POLYGON Dungeons

官方 Examples 的地牢示例，来源为用户提供的 **POLYGON - Dungeons Pack - Art by Synty 1.10.2**。美术由 **Synty Studios** 制作。原始包的 SHA-256、逐资源摘要及移除的脚本记录在 [source-manifest.json](source-manifest.json)。

运行 `atelier_editor --project ./examples --scene Content/Dungeons/Scenes/Dungeon.scene`。资源位于 **Project / Content / Dungeons**，选择 Main Camera 后用 View from Camera 观察 Demo，显示菜单可选择 Smooth 或 Scene Lit。三个场景均保存了主相机，Play 不自动取景。保存直接写入打开的工程，详见 [Examples](../../README.md)。

| 场景 | 模型实例 | 灯光 | 用途 |
| --- | ---: | ---: | --- |
| [Dungeon.scene](Scenes/Dungeon.scene) | 3425 | 192 | 完整 Demo 地牢 |
| [DungeonOverview.scene](Scenes/DungeonOverview.scene) | 626 | 1 | 地牢建筑和道具总览 |
| [GenericOverview.scene](Scenes/GenericOverview.scene) | 380 | 1 | 通用角色、地形和道具总览 |

`Models/` 包含 2245 个源网格生成的 2314 份 OBJ/MTL 材质组合和稳定资源 UUID，涵盖全部 1268 个 FBX 的网格，包括未在 Demo 中出现的模型和 LOD。可通过 Asset Browser 的 Add to Scene 复用。`Models/Textures/` 为这些材质提供 62 张共享颜色贴图；多个模型复用同一张解码后的图像。

`Originals/Assets/` 保留包内的非脚本资源：1268 个 FBX、152 个材质文件、168 张原始贴图、1303 个 Prefab、703 个 `.asset`、3 个 Unity 场景，以及动画、Shader Graph、光照数据和 `.meta`。包内 9 个 C# 脚本及引用这些脚本的 MonoBehaviour 已移除。`.meta` 留在磁盘，不单列于资源浏览器。Originals 中的 Unity 文件用于保留源数据；引擎直接使用 Models 和 Scenes 下的转换结果。

`Import/models.json` 记录每个转换网格的来源、材质槽、三角形数与资源身份；`materials.json` 包含 152 个源材质及 6 个内嵌/内置材质；`*-instances.json` 保留原始层级路径和变换；`summary.json` 给出原始转换规模与 Unity 相机，不包含另行编写的 Main Camera 和默认 Environment 两个根实体。场景中 52 个镜像实例以负缩放直接引用原网格，不另建镜像网格变体；所有实例保留原始世界摆放。场景模型以独立 Entity 保存世界 TRS，未恢复 Unity Prefab 的编辑关系。

**Smooth** 显示原始颜色贴图。192 盏灯已转为可编辑 LightComponent，切换 Scene Lit 可使用引擎光照及材质贴图。此导入没有重建 Unity Shader Graph、透明水面、后处理、烘焙光照、骨骼动画或粒子行为；Demo 中 73 个粒子系统保留在源文件中，不作为运行组件执行。因此画面不等同于 Unity 原效果。

复现导入见 [转换工具说明](../../../tools/import-dungeons/README.md)。验收命令：

```powershell
cmake --build --preset dev --target editor_dungeons_acceptance
cmake --build --preset release --target editor_dungeons_acceptance
```

无窗口验收逐个加载 2314 个模型、检查三角形数与贴图共享、核对源资源数量并保存重开三个场景。真实窗口验收验证普通工程入口、Content 目录树、三个场景的 D3D12 截图及只含 Models/Scenes 的隔离副本保存重开。证据位于 `out/<配置>/dungeons-evidence/`；该专项不发送原生输入，也不启用 GPU validation。
