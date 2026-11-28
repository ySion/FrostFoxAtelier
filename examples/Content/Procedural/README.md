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
   - 地形连续网格的 `"default"` 材质槽绑定 `TerraceStone.material`；散布实例由 `BasaltColumn.obj`（玄武岩石柱）与 `AmberCrystal.obj`（自发光琥珀晶簇，参与四层全局光照间接照明）按台地高度与坡度自动选择变体。

2. **三层同构参数与默认锁定预览（`.procgraph` v4）**：
   - 在 **Scene Tree** 或 **3D Viewport** 直接点击任一岛屿实体，**Parameters** 面板直接在 `ProceduralGeneratorComponent` 下方内联渲染 `Generator Parameters`（如 `Island Radius`、`Terrace Count`、`Plateau Height`、`Prop Density`），拖动滑杆仅为当前实体写入场景差量 `overrides`（接入场景 Undo/Redo 与 Play 同步）；同时 **Network** 面板默认以 **Locked Preview** 打开该 `.procgraph` 供安全检视，点击顶栏 `Unlock` 才进入共享数字资产编辑态并即时传播至全场景所有实例。
   - 图中（`.procgraph` v4，含 `Terrain Shaping` / `Outpost Scattering` 两个 Network Box 分组框与 `Terrace Outpost SOP` 便签说明）串联了内置节点（`MeshImport` → `Promote` → `Connectivity` / `Scatter` → `RandomScale` → `CopyToPoints` → `Merge` → `GraphOutput` 正式交付输出 `OUT_terrace`；按需点亮任意节点的 `T` 标记即可叠加半透明参考线框）与 [TerraceIsland.source](../../Source/Procedural/TerraceIsland.source) 中的两个工程自定义 `parallel_for` 节点：
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
   - `game.ModularBuildingLayout`（节点 `building_layout`）按 `Dimensions`、`Facade & Roof`、`Ruin Weathering` 三组直观参数，根据建筑面宽/进深、楼层数（整数量化滑杆）、层高、目标开间宽度、壁柱比例、屋脊高度与坍塌侵蚀度 `ruin_collapse`，生成阶梯台基、带入口门洞与凹凸窗框（壁柱、窗下墙、挑出窗台、窗侧柱、窗过梁、层间挑檐）的立面面框、带中庭挑空井的多层楼板面框、双坡山墙斜椽与屋脊木构折线，以及残骸候选点。
   - 下游经 C++ `Extrude`（`distance_attribute = "ext_dist"` / `inset_attribute = "ext_inset"` + 闭合底盖）与 `Sweep`（屋顶木梁扫掠）生成全闭合水密建筑构件，并由 `game.RuinRubbleGrounder` 在 `parallel_for` 中对楼板/台基做 `raycast_surface` 落点贴合与 `closest_surface_point` 墙面避让，将残骸石柱与晶簇精准安放在废墟之上。场景中的 **Grand Ruined Hall** 与 **North Sanctuary Ruin** 共用同一 `ModularRuinBuilding.procgraph` 资产并通过组件级 `overrides` 分别塑造三层主殿与双层坍塌圣所。

## 3. RuinedSettlement.procgraph（数字资产复合组装）

独立程序化数字资产 `Content/Procedural/Graphs/RuinedSettlement.procgraph`（可在 **Assets** 面板双击打开）：
- 通过 3 个通用 `procedural.AssetNode`（`west_watchtower`、`great_hall`、`east_chapel`）实例化调用 `ModularRuinBuilding.procgraph`，各自配置独立 `overrides`（4 层角楼、3 层主殿、2 层重度坍塌礼拜堂）与 `seed_offset` 并共享同一 256 MiB `Evaluator::CachePool`；图根同时暴露多目标参数 `wing_scale_y` 同步驱动两侧翼楼变换，双击或按 `Enter` 可直接沿面包屑下钻进入子资产检视。



## 门楼 Gatehouse

用 Examples 工程打开 [Gatehouse.scene](Scenes/Gatehouse.scene) 可从场景中观察木构、砌体、陶瓦和石板瓦。街道位于负 Z 侧，穿过拱道向正 Z 进入后院。正面左侧的小拱门通往主楼首层，里面有楼板、梁、窗和壁炉。

[门楼总图](Graphs/Gatehouse.procgraph) 组装[西侧主楼](Graphs/Gatehouse/Hall.procgraph)、[沿街附房](Graphs/Gatehouse/Wings.procgraph)、[双塔](Graphs/Gatehouse/Towers.procgraph)及过街楼。图级参数 `clear_width` 以米计量两侧石门框之间的净宽，默认 4.0，可调范围 2.8–4.6。[拱券 SOP](../../Source/Procedural/GateArch.source) 由它重建两端拱圈、拱腹和门框，外侧支承固定。公开参数在总图参数面板可直接调整。

过街楼的另一项图级参数 `roof_sag` 控制屋脊跨中低于两端连线的距离，默认 **0.56 米**。屋脊、前后坡与封边共用 21 个纵向截面。[SpanSag](../../Source/Procedural/GalleryRoof.source) 计算屋脊下挠，公共 [RailPanel](../../../assets/engine/procedural/rail_panel.source) 分别连接屋脊和两条檐线生成两片有厚度的屋面。Sweep 沿同一组线生成脊瓦和檐口。调节下挠时，两端支座与两侧檐口保持原位。

**用 Control 改稿与复核**

从空图搭建同类屋面时，先调用 `graph.inspect {"view":"catalog"}`，按用途找“下挠”和“封闭屋面板”，再带上选中项的 `type` 查询详情。前者接收一列有序点。后者的两条边线按其 `attribute_contracts` 中的 `port` 与 `purpose` 接入：点序沿 +X 时，Z 较小的一列在左，Z 较大的一列在右。屋脊可以位于任意一侧。

1. 打开总图后，调用 `graph.inspect` 并传入 `{"view":"node","node":"ridge_stations"}`。`controlled_fields.drop` 给出应修改的图参数 `roof_sag`。
2. 用最新的 `state_token` 调用 `op.call`，传入 `{"id":"graph.set_parameter","args":{"parameter":"roof_sag","value_json":"0.0"}}` 可把屋脊拉直。把参数改为 `clear_width`、值改为 `"4.5"` 可拓宽通道。每次编辑均可用 `edit.undo` 撤销。
3. 调用 `graph.inspect`，传入 `{"view":"node","node":"street_pitch","raycast":{"output":"panel","rays":[{"origin":[0.54,12,0.06],"direction":[0,-1,0],"max_distance":12}]}}`，从上方复测靠近跨中屋脊的高度。比较编辑前后的 `raycast.hits[0].position[1]` 即可量出实际变化。输出端口可从 `graph.inspect` 的 `catalog` 视图查得。

材色由[构件分类](../../Source/Procedural/GatehouseFinish.source)和场景生成器绑定的[表面程序](Materials/Gatehouse/Gatehouse.ms.source)共同决定。陶瓦按 35° 参考坡面每 0.215 米排一行，使过街楼缓坡与主楼陡坡上的瓦片尺度相近。

## 短木桥 Footbridge

[场景](Scenes/Footbridge.scene)中的[桥图](Graphs/Footbridge.procgraph)沿 +X 跨过溪沟，+Y 向上。六米主跨两侧各有一段 1.5 米引板，接到高程固定的岸台。编辑桥图可调整三个以米为单位的公开参数。

| 参数 | 默认 | 改稿时可观察的结果 |
| --- | ---: | --- |
| `width` | 2.0 | 控制承板和全部横铺板的总宽，主跨 30 块，引板各 8 块。两侧栏杆及支桩随边线移动。 |
| `rise` | 0.6 | 固定起端标高，改变末端高差。默认主跨坡度 10%，设为 1.2 后为 20%；铺板和扶手随坡，立柱保持竖直。末端引板随之转为下坡，仍接回东岸。 |
| `post_spacing` | 1.5 | 沿水平基线量取柱距。两端保留立柱，不能整除跨度时最后一跨较短。 |

`Line → Polyline → Resample` 沿基线划分开间。[桥架布局](../../Source/Procedural/Footbridge.source)输出两列桥面边点、柱脚点及各构件的轴线。[RailPanel](../../../assets/engine/procedural/rail_panel.source) 沿两条边线生成承板，栏柱由同一个底面贴地的 Box 经 Realize 放到各柱脚，斜撑由 Sweep 沿每一跨的两个端点制成。扶手的自定义截面是一个 XY 平面内的八边形。

用 Control 打开图，调用 `graph.inspect {"view":"catalog","type":"procedural.Sweep"}` 读取 `port_descriptions`。用 `op.call` 传入 `{"id":"graph.set_parameter","args":{"parameter":"post_spacing","value_json":"1.1"}}`，每侧立柱由 5 根增至 7 根，末跨 0.5 米。要测单根木料，向 `graph.inspect` 传入 `{"view":"node","node":"uprights","raycast":{"output":"output","rays":[{"origin":[2.5,5,0.92],"direction":[0,-1,0],"max_distance":6}]}}`。用 `edit.undo` 恢复原柱距，原位的竖射线随即穿空。


## 弧形石栏杆 ArcBalustrade

打开[场景](Scenes/ArcBalustrade.scene)，主相机面对一段 132° 的石栏杆。它由[一张 19 节点的图](Graphs/ArcBalustrade.procgraph)生成。四皮墙石交替错开半块，灰缝宽 16 mm，灰芯比石面内收 20 mm。柱座落在 1.22 m 高的压顶上，扶手底面和栏柱顶面同高 2.16 m。方柱座的局部 +Z 沿导线切向，两端各留 0.22 m。石块、灰芯、扶手和栏柱是各自闭合的构件。

| 弧线半径 `arc_radius` | 最大柱距 `post_spacing` | 栏柱 | 实际等分柱距 | 墙石 / 压顶石 |
|---:|---:|---:|---:|---:|
| 2.10 m | 0.68 m | 8 | 0.628 m | 34 / 5 |
| 2.80 m | 0.68 m | 10 | 0.668 m | 42 / 6 |
| 2.10 m | 0.48 m | 11 | 0.440 m | 34 / 5 |
| 2.80 m | 0.48 m | 14 | 0.462 m | 42 / 6 |

三个分支共用 `guide` 的 64 段开放圆弧。扶手直接沿原弧抬高并 Sweep。`post_span` 用 SliceCurve 两端各退 0.22 m，`post_pitch` 用 Resample 的 `fit_spacing` 模式排匀柱位。`post_stations` 设置柱脚高度和朝向，并把桩号换回完整导线的里程。砌石 Source 直接由 `self.polyline_length(guide, 0u)` 读取导线总弧长并决定每道灰缝的位置，一次批量截取全部石块与灰芯的原线区间，再分别抬升和扫掠。瓶形柱用竖直折线和 point float `shaft_scale` 驱动圆管 Sweep，腹部半径 0.160 m、颈部 0.076 m。柱脚和柱头复用一次 Bevel 生成的方座，整根栏柱只做一次，再沿曲线 Realize。[案例中的截面、砌法和布站](../../Source/Procedural/ArcBalustrade.source)

**用 Control 改稿。** 打开图后，用 `op.call` 传入 `{"id":"graph.set_parameter","args":{"parameter":"post_spacing","value_json":"0.48"}}`。用 `graph.inspect {"node":"post_stations","samples":16}` 查看全部柱脚的 `arclength`，用 `graph.inspect {"view":"node","node":"spindle","bindings":true}` 查看半径列的实际来源。缺列时返回 `fallback`，实际系数为 1。对 `spindle.output` 从 `[0.6,0.406,0]` 和 `[0.6,0.721,0]` 沿 `[-1,0,0]` 发射线可以复测柱腹和柱颈。执行 `edit.undo` 恢复上一次改稿。

默认弧长 4.838 m，固定 0.68 m 步长的尾段仅 0.078 m。两端退距后按最大间距均分，实际 7 跨各约 0.628 m，能容纳宽 0.33 m 的柱脚。通过 Control 对 `handrail.output` 的每个原弧段从内外两侧径向发射线，可独立核对两次调柱距前后的扶手面。车削母线的有序高度与半径目前写在案例 Source 中，Control 可发现所有截面的端口和使用方法。编辑轮廓样点是另一个独立的作者工具需求。

**已复现的射线边界读数。** 默认图的 `wall_stones.output`，从 `[2.0132889498235693,0.39,2.3572584933600957]` 沿 `[-0.6494480483301837,0,-0.7604059656000309]` 发射长度 1.55 m 的射线，落在一个截面站的共享边上，报告 1.2700 m 处的内壁。沿弧向前或向后各移 2 mm 再径向入射，两侧都在 0.7300 m 命中外壁。该件拓扑检查为零开口、零非流形边和零退化面。测量灰缝时使用两侧射线，单条共边射线的差异仍待定位。
