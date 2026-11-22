# Procedural

展示场景级程序化生成组件（`world.ProceduralGeneratorEntity` / `world.ProceduralGeneratorComponent`）、工程自定义 `.source` SOP 节点（含 `parallel_for` 并发 `Spatial` BVH 查询与受控变长拓扑构建 L3）、C++ 高阶构造算子（`Extrude` / `Sweep`）、2D Network 画布实时重算，以及 `CopyToPoints` 实例烘焙（Bake）的官方示例目录。工程入口与保存约定见 [Examples](../../README.md)。

```powershell
./out/release/atelier_editor.exe --project ./examples --scene "Content/Procedural/Scenes/ProceduralShowcase.scene"
./out/release/atelier_editor.exe --project ./examples --scene "Content/Procedural/Scenes/GenerativeModelingShowcase.scene"
```

## 1. ProceduralShowcase.scene（阶梯台地与散布）

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

## 2. GenerativeModelingShowcase.scene（生成式哨站与模块化建筑废墟）

展示 `.source` 多输出 L3 受控变长拓扑构建（`emit_points` / `emit_polylines` / `emit_quads`）、多端口 `Spatial` BVH 并发查询（`raycast_surface` / `closest_surface_point` / `closest_polyline`）与 C++ `Sweep` / `Extrude` 流形算子的协同建模：

1. **Fortified Outpost（`Content/Procedural/Graphs/FortifiedOutpost.procgraph` + [FortifiedOutpost.source](../../Source/Procedural/FortifiedOutpost.source)）**：
   - `game.TerraceIsland` 生成起伏阶梯地形后，`game.OutpostLayoutSkeleton` 经 `[Port] const procedural.Spatial& terrain` 射线投射采样地形高程，一次构建 4 个变长输出端口：贴合地形起伏的 S 形引道折线、带大门豁口的多边形棱堡围墙折线、墙顶城垛雉堞折线段与城门/棱角扶壁立柱点集。
   - 下游由 3 个 C++ `Sweep` 分别扫掠生成闭合流形路面、围墙主体与雉堞城垛，再由 `game.OutpostTacticalScatter` 在 `parallel_for` 中同时查询地形法线、道路折线（排斥路幅并在路缘切向排列柱廊）与城墙曲面（避让墙体净空）筛选散布点，经 `CopyToPoints` 实例化零穿模、零浮空的石柱与晶簇。

2. **Modular Ruin Building（`Content/Procedural/Graphs/ModularRuinBuilding.procgraph` + [ModularRuinBuilding.source](../../Source/Procedural/ModularRuinBuilding.source)）**：
   - `game.RuinArchitectureGrammar` 根据可调建筑面宽/进深、楼层数、层高、目标开间宽度、壁柱比例、屋脊高度与坍塌侵蚀度 `ruin_collapse`，按建筑语法切分出阶梯台基、带入口门洞与凹凸窗框（壁柱、窗下墙、挑出窗台、窗侧柱、窗过梁、层间挑檐）的立面面框、带中庭挑空井的多层楼板面框、双坡山墙斜椽与屋脊木构折线，以及残骸候选点。
   - 下游经 C++ `Extrude`（`distance_attribute = "ext_dist"` / `inset_attribute = "ext_inset"` + 闭合底盖）与 `Sweep`（屋顶木梁扫掠）生成全闭合水密建筑构件，并由 `game.RuinRubbleGrounder` 在 `parallel_for` 中对楼板/台基做 `raycast_surface` 落点贴合与 `closest_surface_point` 墙面避让，将残骸石柱与晶簇精准安放在废墟之上。

