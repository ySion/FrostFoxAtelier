# Procedural

展示场景级程序化生成组件（`world.ProceduralGeneratorEntity` / `world.ProceduralGeneratorComponent`）、工程自定义 `.source` SOP 节点、2D Network 画布实时重算，以及 `CopyToPoints` 实例烘焙（Bake）的初版完整测试场景。工程入口与保存约定见 [Examples](../../README.md)。

```powershell
./out/release/atelier_editor.exe --project ./examples --scene "Content/Procedural/Scenes/ProceduralShowcase.scene"
```

## 场景结构与观察方式

1. **场景观察与 Scene Lit**：
   - 打开场景后，可在 **Entity Structure** 选中 **Main Camera** 并点击 **View from Camera**，在视口显示菜单切换到 **Scene Lit**（或直接点击 **Project → Run** 进入运行态主相机）。
   - 场景包含 3 座引用同一资产 `Content/Procedural/Graphs/TerraceOutpost.procgraph` 的程序化岛屿实体：
     - **Central Terrace Island**（使用图默认种子 `101`）
     - **West Outpost Island**（开启 `Override Graph Seed`，独立种子 `2026`）
     - **East Sanctum Island**（开启 `Override Graph Seed`，独立种子 `777`）
   - 地形连续网格的 `"default"` 材质槽绑定 `TerraceStone.material`；散布实例由 `BasaltColumn.obj`（玄武岩石柱）与 `AmberCrystal.obj`（自发光琥珀晶簇，参与 DDGI 间接照明）按台地高度与坡度自动选择变体。

2. **实时编辑 `.procgraph` 与自定义 `.source` 节点**：
   - 在 **Scene Tree** 或 **3D Viewport** 直接点击任一岛屿实体，**Network** 面板即刻在 World 上下文中呈现该实体的 `.procgraph` 节点图并支持就地编辑（也可点击 `Isolate` 图标或双击资产进入独显 SOP 视口模式）。
   - 图中（`.procgraph` v2，含 `Terrain Shaping` / `Outpost Scattering` 两个 Network Box 分组框与 `Terrace Outpost SOP` 便签说明）串联了内置节点（`MeshImport` → `Promote` → `Connectivity` / `Scatter` → `RandomScale` → `CopyToPoints` → `Merge` → `Null` 里程碑输出 `OUT_terrace`；按需点亮任意节点的 `T` 标记即可叠加半透明参考线框）与 [TerraceIsland.source](../../Source/Procedural/TerraceIsland.source) 中的两个工程自定义 `parallel_for` 节点：
     - `game.TerraceIsland`：基于连续值噪声与阶梯平滑曲线抬升阶梯台地，并用解析中心差分计算顶点法线 `N`。
     - `game.OutpostPlacement`：按台地高度与坡度法线筛选变体 `variant`、调整 `pscale`，并用正交基 `frame_rotation` 组合表面法线倾角与随机局部朝向 `orient`。
   - 修改图节点参数后，所有引用该图的岛屿实体会立即在场景中重算并刷新视口与光照几何。

3. **实例烘焙（Bake Instances to Entities）**：
   - 在场景上下文选中任一岛屿实体，打开 **Parameters** 中的 **Procedural Graph** 选择器菜单并点击 **Bake Instances to Entities**（算子 `procedural.bake`），即可将该生成器产出的全部 `CopyToPoints` 网格实例一次性转换为带独立变换与默认材质的 `<Entity> Baked` 子层级 `StaticMeshEntity`，并支持单步 `Ctrl+Z` 撤销。
