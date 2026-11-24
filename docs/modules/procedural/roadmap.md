# 程序化：方向、未决契约与后续阶段

当前实现、接口和验收以[模块说明](README.md)为准。本页保留首阶段方案中尚有用的产品决定、设计依据与未实施的跨模块约定，不是已实现能力清单，也不授权启动后续阶段。

P1、共享不可变列迁移、统一 `procedural.Node` / `process(...)` 节点架构、`.source` 语言级 `parallel_for` 与显式 `fail("...")`、S2 可视化与图操作（Slice 1A + Slice 1B：`.procgraph` 资产、SOP 视口上下文切换、点/法线/朝向辅助显示、Geometry Spreadsheet、Houdini 式二维 Network 节点连线画布、`Tab` 浮动搜索自动连线、`Y` 划断连线、`graph.layout` 自动拓扑排版、独立图撤销历史与 `graph.*` Control 操作链）、S3/S4 生成式建模核心闭环（`Extrude`、`Sweep`、`.source` 多端口辅助列与 `Spatial` 并发 BVH 查询、L3 受控 `Node` 变长拓扑构建接口）、S4 子图数字资产与三层同构参数体验（`.procgraph` v4、通用 `AssetNode`、`GraphInput`/`GraphOutput`、多目标暴露参数 `parameters`、`graph.extract_asset` 一键提取、共享 `Evaluator::CachePool` 与默认锁定预览），以及 S5-1 场景生成器组件（`world.ProceduralGeneratorEntity` / `world.ProceduralGeneratorComponent` 内联 `overrides`、宿主边界同步、Scene Lit / Play 联动与实例 `procedural.bake`）已交付。属性列、可选输入、组遮罩与 `[RunOver]` 层通过方法参数属性（`APARAM` / `.source` `[In]/[Out]/[InOut]/[Group]/[Port]`）和正式 `.source` class 统一表达，不再使用 `PointNode`、`Node::fail`、`Meta("procedural.bind", ...)` 或代码片段宏拼接。尚未实现渲染器 GPU 大批量常驻实例化区段（S5-2）、持久磁盘 Cache 或持久冻结；节点会话冻结已可用。

## 方向与依据

- 第一类产品是 Houdini 式统一几何图：点、顶点、图元、实例与各层属性共用 Geometry。先解决现有网格上的撒点、属性运算与资源实例化，再做多边形构造；Substance 式贴图图推后。点和顶点不是 Runtime 对象或 Entity；生成器作为组件（`world.ProceduralGeneratorComponent`）挂在普通 Entity 上。
- Geometry 核心轻量自建，不做 CAD 内核。算法优先用自有语言；容器、求值器、BVH 和需要维护复杂拓扑/数值不变量的操作留在 C++。可靠的批量服务可以作为原生黑箱，但当前 C++ 操作并不因此自动成为语言可调用的 BorrowSafe 方法。
- 图是权威数据。人与 LLM 共用 Control 操作符和撤销历史：人用网络编辑器与 Parameters，LLM 读写文本、调用操作符并读取客观统计。生成结果不是另一套可编辑权威数据。
- 场景默认用 Cache 或冻结结果，不每次从零求值；同时允许 Play 与游戏运行时主动求值，图可以封装为可复用的数字资产。持久缓存与磁盘冻结尚未实现。
- 预期规模是单生成器约 10 万实例、图内约 100 万点；典型图（≤20 节点、≤10 万点）调参重算期望 ≤33 ms，目标画面为 120 FPS。这些是设计量级，不是未经测量的性能承诺或验收硬闸。
- `id` 是 uint32 随机种子，不是唯一身份。100 万点约有 116 对哈希碰撞的期望值；重复只使随机值相同。P1/P2 当前范围不需要 64 位列，也没有 Source Map/Set 的真实需求。实例覆盖或 Spawn Entity 若需要逐元素身份，必须另定契约；撒点的（`sourceprim`，子三角 `sourcetri`，`sourceordinal`）可作为基础，纯三角输入隐含子三角 0，但不是跨拓扑编辑的永久身份。
- 参考 Houdini 的分层、属性名与实例矩阵公式；参考 Adobe Lagrange（Apache-2.0）的保留拓扑、用途与等边数图元表示，改编代码须保留许可和署名；Blender Geometry Nodes（GPL）只参考设计，不复制或改编代码。
- 暂不做常量列（借给 Source 的 Array 需要先展开）、外部缓冲策略矩阵、稳定边编号。需要邻接的服务按需建立邻接；常量列的收益须由负载证明。没有选用 Lagrange 的网格剪枝撒点，因为它不保证点数与面积成比例。

### 人机双环定位、节点粒度与三层分工

本项目程序化服务于**“LLM 主力开发规则 + 人类 120 FPS 观察与滑杆微调”**的双环工作流，主干固定为**共享 `Runtime` + 正式 `.source` `procedural.Node` 类（LLVM O2 JIT）+ 宏观 `.procgraph`**（参考 [TerraceIsland.source](../../../examples/Source/Procedural/TerraceIsland.source)）：

1. **节点粒度准则（语义几何阶段粒度，规避两端误区）**：
   - **拒绝算术微节点连线**：不在图中建立 `Add`、`Sin`、`Dot`、`Branch` 等低级可视化编程节点；复杂数学、条件分支与循环统一写在 `.source` `process(...)` 内，避免节点图膨胀与端口连错。
   - **拒绝单节点黑盒巨石**：不把整座建筑或整张关卡塞入 1～2 个数百行的巨型 `.source` 节点。图节点的边界同时是人类视口分步检视（`D`/`T` 主副对比）、`Evaluator` 256 MiB 写时复制结果缓存、节点级 `Freeze` 以及 LLM `graph.inspect` 逐阶段断言的边界。典型单图应保持 **6～15 个可独立观察与缓存的几何变换阶段**。
2. **三层职责切分（强强联合模型）**：
   - **C++ 几何底座与高阶拓扑算子（`atelier_procedural`）**：负责精确数值谓词、邻接关系、跨层属性插值（point/vertex/primitive）、流形闭合与公共凸面容差 δ_p 保证（已有 `Boolean`、`VolumeScatter`、`Bvh/Spatial/Ray`、`Realize`、`Connectivity`、`Extrude`、`Sweep`；后续按需补齐平面裁切 `Clip`、点焊接与三角化 `Fuse/Triangulate` 等构造动词）。**不让 LLM 在 `.source` 中手写三维多边形 CSR 索引汤、封盖环或绕序修补**，防止下游 `Boolean`/`VolumeScatter` 因非流形或翘曲超 δ_p 频繁报错。
   - **`.source` 领域阶段模块（`class Name : procedural.Node`）**：保持 `.source` 强类型、无隐式转换、同步值区间零 GC 停顿的紧凑边界（不膨胀为半个 C++），专注表达与游戏业务强耦合的阶段逻辑——地形/噪声场塑形、建筑开间与楼层语法切分（Grammar/Lot Subdivision）、道路/枝干折线骨架生成、多参考输入战术打分筛选、变体与朝向状态机。公开字段即人类微调滑杆。
   - **`.procgraph` 与数字资产（`AssetNode`）**：宏观串联 C++ 拓扑算子与 `.source` 业务阶段模块；成熟子图按 `asset.generic_node` 封装为高阶复合模块供群落/关卡图调用。
3. **已排除的伪瓶颈**：`Evaluator` 在节点入口对 `.source` 节点按每次求值十余次的图粒度经 `Entry::invoke` 核对借用签名（见[原生值借用](../../native-value-borrows.md)），热点全在内部 `parallel_for` 的 1024 元素 O2 向量化块内，不为节点边界纳秒级调用另造绕过检查的 `_typed` 借用通道；图求值后台化须以实测超预算（>33 ms）和只读求值计划为前提，不破坏单线程 `Runtime` 契约。

## 当前顺序与后续依赖

S0 契约静态对齐 → S1 原生拓扑/构造/体积采样/Realize，随后接公共绑定与 Source Noise/Remap → S2 可视化与图操作 → S3 拓扑工具 → S4 子图、循环与 L3 → S5 场景生成器。原 P2 场景优先排序已被此顺序取代；以下保留的 P 阶段估价只供历史参考，不是当前排期或授权。

| 项目 | 状态与下一步 |
| --- | --- |
| 双轨成板 | 门楼变坡屋面与[短桥及两端引板](../../../examples/Content/Procedural/README.md#短木桥-footbridge)共用已发布的 `procedural.RailPanel`。短桥沿线布置继续由 `Resample` 取站点、`Realize` 放原型。 |
| S3 拓扑工具 | `Connectivity`、`Extrude`（法线外推/内缩封边/底盖及逐图元缩放属性）与 `Sweep`（最小旋转标架沿折线扫掠矩形/圆管/自定义闭合截面，三维扭曲侧面自动按对角线破分为满足 δ_p 的三角形流形）已接入 SOP 求值链，见[几何操作](README.md#操作)。后续按真实生成命题补齐 `Clip`、`Fuse/Triangulate` |
| Remap／属性噪声 | E/F 已接通共同绑定与 Source 3D value noise / point float Remap，现行合同见[Noise / Remap](README.md#noise--remap-数值契约)。其他噪声族、导数和周期仍未排期 |
| 语言能力 | 参数名与参数属性（含 `[In/Group(..., Port = "...")]` 与 SOP 定长列保护）、可写引用容器结构修改（`Array<T>&`/`String&`/`Struct&`）、自动合成 `Array<源码 struct/enum>`、`parallel_for`、复合赋值（`+=` 等）与短路 `?:`、显式 `fail("...")`、`math.is_finite`、`math.as_uint`/`math.as_int`、同步值区间读 self、字段元数据、数学函数、代码身份、引擎源码加载及按依赖复用编译单元已接入，语义见 [LANGUAGE.md](../../../LANGUAGE.md)、[数学模块](../../math.md)和[原生值借用](../../native-value-borrows.md)。面向 SOP 作者的下一批数学/标准库、图元拓扑与折线总长只读访问、结构化元素 `fail`、零分配常量表与 Control 库自省分批方案见 [Source SOP 演进计划](source-sop-plan.md) |
| Source 业务失败 | 已落地语言级 `fail("...")` 语句，替换全部内建 `.source` 节点并移除原 `Node.fail(message)`，零 Runtime `String` 分配且支持 `parallel_for` 块内失败收敛；可选元素层与索引诊断见 [Source SOP 演进计划](source-sop-plan.md#batch-3s-sop-3元素级结构化-fail-诊断与宿主退出审计) |
| L3 与多端口查询 | 已落地 `[In/Group(..., Port = "...")]` 辅助端口列借用、`[Port] const procedural.Spatial&`（`Spatial::acquire` 16 槽共享 BVH 缓存 + `spatial.source` `parallel_for` 安全并发查询）与受控 `Node` L3 拓扑构建方法（`emit_points` / `emit_polylines` / `emit_polygons` / `emit_quads` + `set_*`），合同见[模块说明](README.md#source-空间查询与受控拓扑构建l3)与显式合同表 |
| 调度 | 当前求值器在 Runtime 线程上串行执行，没有每帧时间片调度。C++ 节点迁到工作线程须先有只读求值计划、清楚的发布和借用生命周期，再由实测决定；不能只因列支持写时复制就并行调用 Runtime |
| Volume 并行化 | 原生入口已采用融合分类与稳定填充的两次 fork/join，不等于后台图求值；当前合同及双规模 profile 见[模块说明](README.md#测量)。正式只记 volume/build/classify 墙钟；三段成本用临时真 P1 诊断并单列插桩扰动，不为计时拆开产品流水线。并行收益只比较同一新内核未插桩的 P1/default P，串行加速片的 12.34× 不是并行或规模扩展证据 |
| 数字资产与参数面板 | 单轨 `.procgraph` v4 数字资产（`procedural.AssetNode` + `GraphInput`/`GraphOutput`、`graph.extract_asset` 一键提取，不建内联递归 `SubnetNode`）、声明式多目标参数提升 `parameters`、跨实例共享 `Evaluator::CachePool` 与三层同构参数面板（单节点整数量化滑杆/条件可见/局部 `Category`、图级暴露参数、场景 `ProceduralGeneratorEntity` 内联 `overrides` 与默认锁定预览）已全部落地，合同见[显式合约](#已裁决的后续合同与待确认时机)与[模块说明](README.md#图文件) |

### 核心断层闭环状态与下一阶段顺序

围绕上述人机双环定位，前两阶段（人机闭环基础设施与生成式建模闭环）已全部落地，下一阶段聚焦复合资产化与十万实例规模闭环：

1. **保状态的无感类型热重建（`editor.stateful_rebuild`，已实现）**：
   - 保持 `Runtime` 只追加偏移不变量不变；无占位对象时 `project.build` 优先走 `poll` 兼容热更快路径，确需重建时内存态无损迁移已打开 `.procgraph` 草稿（保留各自 `dirty` 基线且不覆写磁盘）与 `scratch` 图，恢复视口相机、SOP 上下文、`NetworkCanvas` 平移缩放与实体/节点选择，并在无 Undo 历史且空闲时自动静默重建。详见[工程与源码构建](../editor/projects.md#保存失败与重建)与 [Editor Control](../editor/control.md)。
2. **`graph.inspect` 轻量空间体检指标（`control.spatial_diagnostics`，已实现）**：
   - `graph.inspect` 默认在几何摘要 `spatial` 下输出多边形流形/开口边体检（`topology`）、最近邻间距分位数与重合计数（`spacing`）、实例世界对齐 AABB 重叠统计（`instance_overlaps`）及可选 `reference_node` 驱动的 `Bvh::acquire` 离地间隙分布（`surface_clearance`）。详见 [Editor Control](../editor/control.md)。
3. **`.source` 多端口只读参考几何与 `Spatial` 空间查询（`source.spatial_and_aux_ports`，已实现）**：
   - 向 `.source` `process(...)` 开放 `[In/Group(..., Port = "...")]` 辅助端口列借用（支持 `point` / `vertex` / `primitive` / `detail` 层）与 `[Port] const procedural.Spatial&` 32B 定长节点序列加速句柄（底层由 `Spatial::acquire` 16 槽共享缓冲复用三角形与折线 BVH，深度 ≤ 54）。`assets/engine/procedural/spatial.source` 提供 `raycast_surface`、`closest_surface_point` 与 `closest_polyline`，串行分派前预投影连续只读切片，可在 `parallel_for` 块内无锁并发读取，工作线程零 `Runtime` 触碰。
4. **受控变长拓扑构建 `GeometryBuilder`（L3）与高阶 C++ 构造算子协同（`source.geometry_builder`，已实现）**：
   - 复用 `.source` 栈上拥有型 `Array<T>`，在 `procedural.Node` 上提供 12 个 `AFUNCTION(BorrowSafe)` 方法（`emit_points`、`emit_polylines`、`emit_polygons`、`emit_quads` 与各类型 `set_*`），禁止在已声明主输入流列借用时覆盖 0 号输出，并在提交时原子校验有限性、各层列长、属性 `Usage` 与公共凸面判据 δ_p。配合 C++ `Extrude` 与 `Sweep` 算子，已在 [FortifiedOutpost.procgraph](../../../examples/Content/Procedural/Graphs/FortifiedOutpost.procgraph) 与 [ModularRuinBuilding.procgraph](../../../examples/Content/Procedural/Graphs/ModularRuinBuilding.procgraph) 完成端到端验证。

**推进顺序**：
1. **阶段一（人机闭环基础设施，已完成）**：已落地 `editor.stateful_rebuild` 与 `control.spatial_diagnostics`。
2. **阶段二（真实业务驱动的生成式建模闭环，已完成）**：已通过带道路排斥与扫掠围墙的哨站（`FortifiedOutpost`）与可调层高/开间的模块化建筑废墟（`ModularRuinBuilding`）双命题，端到端落地 `source.spatial_and_aux_ports`、受控 `GeometryBuilder`（L3）及 C++ `Extrude` / `Sweep` 算子。
3. **阶段三（复合资产化、核心算子扩充与十万实例规模闭环）**：S4 子图数字资产与三层同构参数体验已全部落地；下一步优先探讨并推进 **S4.5 核心几何算子与节点工具箱扩充**（补齐基础图元网格/细分、平面裁切/焊接/分格、法线/UV 投影、折线重采样、语义选组与惰性分支），随后推进[集合与渲染接口](#集合与渲染接口)中的 `SceneSnapshot` GPU 常驻实例集合区段（S5-2，消除 [scene.cpp](../../../modules/engine/presentation/src/scene.cpp) `Scene::render_items()` 的 CPU 逐实例 `RenderItem` 展开）。

### S4.5 核心几何算子与节点工具箱扩充（分批推进中：S4.5-1 参数化图元与对齐底座已实现）

在进入 S5-2 渲染侧批量加速前，针对现有内置节点在真实建模与关卡生成中的高频工作流断层，遵循上述三层分工（**不引入加减乘除算术微节点，聚焦 `.source` 不应手写的 C++ 拓扑/属性硬算子与高频工作流节点**），分批推进核心算子扩充：

| 分组 | 节点 / 升级 | 解决的工作流断层与职责边界 | 当前状态 |
| --- | --- | --- | --- |
| 1. 参数化基础图元与对齐（S4.5-1） | `Grid`（表面网格模式 + 三平面 + UV） | 补齐 `quads / triangles / points`（默认 `quads`）、`zx / xy / yz` 朝向、`center` 与 `vertex N`、`uv`，直接作为地形、楼板、墙面起手网格 | 已实现（S4.5-1） |
| 1. 参数化基础图元与对齐（S4.5-1） | `Box`（三轴细分 `divisions_x/y/z` + `base_on_ground` + UV） | 支持共享棱角点的闭合流形三轴细分、底面贴地（消除半高心算）与逐面仿射 `vertex uv` | 已实现（S4.5-1） |
| 1. 参数化基础图元与对齐（S4.5-1） | `Tube`（锥度/棱锥 + 中空管 + 扇形封盖）与 `Wedge` / `Circle` / `Torus` | 单一泛化 `Tube` 覆盖圆柱/圆锥/四棱锥屋顶/六棱柱/中空管/扇形拱；新增 `Wedge`（斜坡与双坡屋顶）、`Circle`（正多边形/闭合截面/`open_arc` 拱线）与 `Torus`（柱础圆环/弯管） | 已实现（S4.5-1） |
| 1. 参数化基础图元与对齐（S4.5-1） | `Transform`（角度制欧拉旋转 + `pivot`）与 `MatchSize` | `Transform.rotate` 统一采用角度制 `X→Y→Z` 欧拉角并支持局部 `pivot`；新增 `MatchSize`（默认 XZ 居中、底面贴齐 `Y=0`，支持 `scale_to_fit` 与 `reference` 参考端口） | 已实现（S4.5-1） |
| 2. 撒点遮罩与拓扑构造（S4.5-2） | `Scatter`（面组与密度） | primitive 布尔组选区已通过双坡屋面案例验证。出现同一选区内随湿度或距檐口距离连续变化的疏密需求时，再验证密度权重列 | 面组已实现；连续权重待案例 |
| 2. 撒点遮罩与拓扑构造（S4.5-2） | `Clip`（平面裁切 + 可选封盖 `Cap` + 切口组） | 斜屋顶山墙、断壁残垣斜切口与岩石削切；开启 `Cap` 时补齐满足 δ_p 的闭合切面，保持下游 `Boolean` / `VolumeScatter` 可用 | 候选待推进 |
| 2. 撒点遮罩与拓扑构造（S4.5-2） | [Bevel](bevel.md) | 为石构压顶、柱帽等闭合凸件按棱角阈值生成平倒角或多段圆角，保持实体封闭 | 闭合凸件已实现 |
| 2. 撒点遮罩与拓扑构造（S4.5-2） | `Fuse`（空间哈希点焊接 + 退化图元清理） | `Merge`、`Realize` 或 L3 `emit_*` 拼合几何后，按距离阈值确定性焊接重合点并清理退化边面 | 候选待推进 |
| 2. 撒点遮罩与拓扑构造（S4.5-2） | `Divide`（凸化 / 三角化 / Bricker 正交网格切分） | 多边形三角化与正交网格分块（Bricker），将不规则裁切面切成规整网格片以供噪声起伏或逐块 `Extrude` | 候选待推进 |
| 3. 法线与 UV 完备化（S4.5-2） | `Normal`（按折痕角 `Cusp Angle` 重算法线） | `.source` 变形点位置 `P` 后，按平滑/硬边角度阈值重建 `vertex` 或 `point` 法线 | 候选待推进 |
| 3. 法线与 UV 完备化（S4.5-2） | `UVProject`（Box 六向 / Planar / Cylindrical 投影） | 为 `Boolean`、`Extrude`、`Clip` 及 L3 生成几何赋予统一世界尺度（Texel Density）的 `vertex uv`，打通 Scene Lit 贴图表现 | 候选待推进 |
| 4. 曲线、选组与分支（S4.5-2） | `Resample` 与 `Polyline` 闭合创建 | `Resample` 已支持折线定距/定段、可选保留转角、切向与弧长，兼容首尾共用点的闭环；`Polyline` 从纯点创建闭合线仍为候选 | 重采样已落地；闭合创建待推进 |
| 4. 曲线、选组与分支（S4.5-2） | 按弧长取样、截段与等分布站 | [弧形石栏杆](../../../examples/Content/Procedural/README.md#弧形石栏杆-arcbalustrade)使用 SampleCurve 读里程、SliceCurve 保留切口之间的折点、Resample 按最大间距均分。Source 砌石复用同一截段实现。 | 已落地 |
| 4. 曲线、选组与分支（S4.5-2） | `Group` | 面法线夹角、已有组和 float 列闭区间三种条件、同层并交差及视口高亮已落地；`GroupBox` 提供盒选点，球形范围与开口边界选组按案例推进 | 基础选组已实现；其余候选 |
| 4. 曲线、选组与分支（S4.5-2） | `Switch`（按索引选择输入，`Evaluator` 惰性求值） | 等数字资产出现互斥造型分支时验证。结构与环校验仍检查已连的分支，仅选中输入进入本次求值与缓存键。验收应覆盖未选分支报错或修改不影响输出、切换后返回所选分支首错、撤销切换后恢复。 | 候选 |

### Volume 表面带的首角舍入限制（未排期）

当前容差尺度不依赖首角，但距离经扇形片求值：距 δ_p 只有舍入差的点，可能随首角改变而改变分类，见[现行数值政策](README.md#volume-scatter有向覆盖与固定格)。C R2 暴力版与加速版均存在此限制；加速片保持同一输入的叶判定，不扩大合同。

若未来确需逐位首角不变，可另评估直接对原多边形计算平面距离、投影内外及边界线段最小距离，不再经扇形；法线计算也须固定为循环移位不变，否则仍不能保证逐位结果。这只是候选，需总负责人决定是否另立片，未授权实现；收益限于舍入边界，不以此要求普通几何重新求值。

### Volume 后续射线优化（未排期）

盒门兼容认证保持原 Interval 盒布尔结果，不改变可认证方向或重试／兜底。后续若测量仍显示射线为主成本，可分别评估行列式过滤与遍历／剪枝优化，不在盒门片叠加。访问数和 nextafter 次数只是操作量，不是耗时占比。

扩大行列式认证集合前，必须证明新增可认证方向与旧 double 立体角兜底的最终分类相同；精确整数绕数的方向无关性本身不证明旧浮点阈值结果等价。新方案还须给出实际运算链的误差界、原有六列与计数的具体合同，以及过滤器和 Interval 同时认证时零符号冲突的开发差分。未满足这些条件前继续保留当前行列式路径，不将更少重试自动视为语义兼容。

## 程序化与引擎的显式合约

用户原则：**程序化过程中不应有隐式决策和变化。** 默认行为必须是显式参数或可查阅的合约，不按名字或数值临时猜测用途。

合约使用 `name`、`meaning`、`writer`、`reader`、`violation` 和 `status` 六个字段。`writer`/`reader` 标注未来职责，不表示相关入口已经存在；`violation` 说明违反合同的行为，不预先规定错误文本或新增拒绝矩阵。现有行为可从模块说明迁入清单作为唯一权威，不能趁整理改变语义。

### 已裁决的后续合同与待确认时机

| name | meaning | writer | reader | violation | status |
| --- | --- | --- | --- | --- | --- |
| asset.template_instance | 每个 `.procgraph` 即数字资产定义，由 `GraphInput`（含独立预览回退）与 `GraphOutput`（与作者调试 `Display [D]` 解耦）界定外部几何端口，由顶层声明式多目标 `parameters` 界定暴露参数默认值。不设内联递归 `SubnetNode`，子网复用通过 `graph.extract_asset` 提取为 `.procgraph` 并原位替换为 `AssetNode`。实例（含图内 `AssetNode` 与场景 `ProceduralGeneratorComponent`）各自持有私有运行时 `Graph` 克隆根并共享 `Evaluator::CachePool`，作者记录只存资产引用、差量 `overrides` 与输入连线；默认处于编辑锁定状态（World 模式下跟随选中 Entity 的 Network 面板默认同样锁定），解锁后直接编辑共享定义并立即传播到所有未冻结实例，不允许实例局部分叉 | 资产定义编辑器、`graph.extract_asset` 与实例创建/覆盖编辑入口 | 图资产解析、实例参数覆盖同步与共享缓存求值宿主 | 建立第二套内联子图树、混用 `Display [D]` 与正式 `GraphOutput`、偷存实例私有定义、解锁后暗中分叉、未冻结实例遗漏传播，或把编辑锁定当冻结 | 已实现（S4） |
| asset.generic_node | 图资产不发布 Runtime 类型；所有子图实例共用 `procedural.AssetNode`（存 `"asset": "project/{GUID}"`、`"overrides"` 与 `seed_offset`），端口由实例级 `ports(Runtime&, ObjectId)` 读取目标资产的有序 `GraphInput`/`GraphOutput`。`Tab` 菜单仍按工程 `.procgraph` 资产列出。上游资产重排或删改端口/暴露参数名后，外层连接按端口名重对齐，失效连线与无法匹配的旧 `overrides` 保留在 `kept` 并产生诊断，不静默丢弃或猜测改名映射 | 资产目录、`AssetNode` 注册、端口重对齐与覆盖编解码入口 | `Tab` 菜单、参数面板（图根、`AssetNode`、场景生成器）、图 v4 与场景保存/加载 | 每资产发布新 Runtime 类型、按端口静态下标错连、静默丢失失效连线或未匹配 `overrides` | 已实现（S4） |
| parameter.three_layer_ux | 单节点、`.procgraph` 暴露参数与场景 `ProceduralGeneratorEntity` 共用同构参数交互：整数 `Range` 生成基于按下起点连续位移量化的步进 `1` 滑杆；`Meta("ui.visible_when", ...)` 控制枚举/布尔分支字段显隐（已受图参数控制的字段保持可见以供解绑）；节点局部按需 `Category` 分节不改动全局 Entity 参数布局；图级 `parameters` 采用同 `TypeId` 多目标 `targets` 声明式绑定（求硬边界交集、自动同步节点改名/删除），不引入字符串通道表达式 | `ui` 控件层、`editor/core` 属性与图参数绑定层、`Workspace` 参数面板 | 单节点检视、`.procgraph` 根参数、`AssetNode` 与场景 `ProceduralGeneratorEntity` 检视 | 整数拖调产生小数拒绝或慢拖死区、开启全局分类破坏 Entity 变换行、引入脆弱字符串通道表达式，或隐藏受控字段致无法解绑 | 已实现（S4） |
| freeze.scope_and_history | 两级冻结都保留：节点冻结属于资产定义，所有实例共享；实例/生成器冻结属于场景或包含它的图。冻结只在显式解冻后失效，定义变化只提示、不清掉冻结。冻结/解冻是可撤销编辑，与编辑锁定/解锁分别命名；撤销记录编辑操作；节点冻结与解冻共享不可变快照引用，普通重算不写入历史。冻结载荷的具体存储格式另定 | 未来图定义编辑、宿主实例编辑与冻结命令 | 未来求值宿主、保存/加载与编辑历史 | 定义变化自动解冻、混淆两级归属、混用锁定与冻结，或把重算结果逐次塞入撤销历史 | 用户已裁决，未实现；不属 S1 |
| geometry.attribute_namespace | 属性名在整个 Geometry 内唯一；跨层访问只能使用显式 Promote 或拓扑数组，不引入同名属性的隐式层优先级 | Geometry 属性创建/合并、Promote；未来绑定与 Wrangle | 全部几何算法、列绑定与属性检查 | 为同名跨层列隐式选择优先层，或静默跨层回退 | 全局唯一规则已实现并保留；未来消费者同样遵守 |
| wrangle.formal_class | 自定义 SOP / Wrangle 直接采用正式 `.source` `class Name : procedural.Node` + 整节点 `void process(...)`（公开字段为节点参数，形参属性 `[In]/[Out]/[InOut]/[Group]` 绑定属性列，显式书写串行循环、`parallel_for` 或多阶段步骤，用 `fail("...")` 报告业务失败），淘汰非正式片段拼接展开。读不存在的非 Optional 属性报错，不跨层回退 | `.source` 编译器、参数属性反射与求值器 | 统一 Source/Runtime `Node`、列绑定与求值器 | 另建脚本包装世界、引入字符串宏拼接 Wrangle，或静默跨层取值 | 已实现正式类 `process` 与 `parallel_for` / `fail` 模型 |
| evaluation.host_boundary | 生成器组件挂普通 Entity，点与实例不是 Entity；求值是派生数据重算，输入变化标脏。编辑器在编辑提交后、场景提取前的宿主边界求值；加载/Play 按 Cache 或冻结结果在 Begin 前准备。运行中参数变化走请求队列，在 World::update 后、提取前统一求值。首版同步，超预算后再考虑后台 | 未来编辑提交、加载/Play 与运行时参数请求入口 | 未来生成器宿主、World 与场景提取 | 在遍历/提取中途重入求值、把每个点/实例变 Entity，或尚未确认就当作现行 World 合同 | 总参谋提出，用户未否决；S2/S5 前再确认；未实现 |
| ui.houdini_reference | 节点界面参考 Houdini，复用统一图数据、Parameters、操作符与编辑历史；不是在 S1 顺带建立节点画布 | 图编辑器与操作符 | 用户界面、Control/LLM 与文档历史 | UI 私有图成为第二份权威数据 | 已实现（Slice 1A + Slice 1B：SOP 视口上下文、Houdini 式二维 Network 连线画布、Geometry Spreadsheet、Parameters、独立图历史与 `graph.*` 操作符） |
| curve.authored_points | 作者可编辑点数组的曲线节点需要视口绘制交互和数学值数组的属性编解码；涉及 modules/editor/core/include/editor/fields.hpp:54–59 与 scene.cpp，届时由编辑器负责人实施。S1 的 Polyline 只从纯点输入按点序连开放折线，不为它扩展编解码 | 未来曲线绘制/属性编辑入口与编辑器数学值数组编码器 | 未来曲线节点、图文件与属性解码 | 把点数组压成无类型浮点列表，或在 S1 提前扩展编辑器产品 | S2，未实现 |
| editor.stateful_rebuild | `.source` 类型定义变更触发 `rebuild_required` 时，保持 `Runtime` 类型表只追加设计不变；宿主重建上下文前快照内存 `.scene` 与全部已打开程序化图草稿（含未落盘 `.procgraph` 与 `scratch` 图），并在采纳新文档后恢复视口相机姿态、活动 SOP 图、画布缩放平移与选中实体/节点 | 编辑器 `build_project`、`publish_document` 与 `project.build` Control 入口 | 重建后的 `Session`、`Workspace` 与视口 | 动摇 `Runtime` 只追加偏移不变量，或重建时丢弃未落盘 `.procgraph` 草稿、重置相机与 SOP 上下文 | 已实现 |
| control.spatial_diagnostics | `graph.inspect` 在层计数与数值统计外，默认输出多边形流形/开口边计数、最近邻间距分位数、实例 AABB 重叠统计及可选 `reference_node` 相对参考表面离地间隙，为 LLM 提供客观三维空间反馈 | `Session` 图检查与 `graph.inspect` 序列化入口 | Editor Control / LLM 协作环 | 仅凭标量均值掩盖穿模/浮空，或引入重型有状态副作用污染只读检查 | 已实现 |
| source.spatial_and_aux_ports | `.source` `process(...)` 可声明 `[In/Group(..., Port = "...")]` 只读辅助端口列借用与 `[Port] const procedural.Spatial&` 并发 BVH 空间查询；串行分派前固定只读缓冲，允许在 `parallel_for` 内并发读取而不触碰 `Runtime` | `.source` 参数绑定、`Evaluator`、`Spatial::acquire` 与 `spatial.source` | 自定义 `.source` 阶段节点 | 工作线程访问 `Runtime`、保存悬垂借用，或退回画布外连大量中间查询节点 | 已实现 |
| source.geometry_builder | `.source` 通过同步值区间栈上拥有型 `Array<T>` 构造变长点集、折线与凸面框并经 `Node` `BorrowSafe` 方法批量提交；宿主原子校验有限性、各层列长、属性 `Usage` 与 δ_p 凸面判据后写入指定输出端口，重型流形扫掠/挤出仍由 C++ `Sweep`/`Extrude` 承担 | `Evaluator`、`Node` 受控构建方法与 `.source` 节点 | 下游 `Geometry` 消费者、`Boolean`/`VolumeScatter` 与呈现层 | 向脚本暴露可变共享列句柄、跳过有限性/凸性校验、破坏 0 号主输入流列借用，或迫使脚本手拼复杂流形索引汤 | 已实现 |

### 合同依据：与 Houdini 的显式差异

以下是程序化参谋转达的总参谋 Houdini 22.0.429 `hython` 实测回执；本线未重复运行。它们用于解释设计取舍，不作为本项目验收真值；数学期望仍独立推导。

| 对应合同 | 实测参考与本项目取舍 |
| --- | --- |
| 构造绕序与未来互通 | Houdini Box 为 8 点、6 面、24 角，从外侧看顺时针（例如面 0 的点序 1,5,4,0，N 朝 −Z）。本项目保持外侧逆时针、右手定则外法线，实际点/面顺序见[构造合同](README.md#构造拓扑的持久语义)。未来 Houdini 导入/导出必须显式反转绕序，不按数据猜；C 的整体正有向体积检查应拒绝未经转换的顺时针输入，不自动修补 |
| geometry.attribute_namespace、wrangle.formal_class | Houdini primitive wrangle 读 `@P` 得面中心，`@ptnum` 是首角的点；写 `@P` 静默丢弃，点不动且无警告。故本项目保留全 Geometry 属性名唯一，跨层只能显式 Promote 或经拓扑数组访问，不按当前 Run Over 层猜测同名读写含义 |
| 显式跨层、vertex N | Houdini 几何只有 vertex N 时，point wrangle 读一次 `@N` 就平均提升为 point N 并删除 vertex N；写也会静默替换，Box 硬边丢失。本项目读不存在/异层列报错，不隐式 Promote；B 构造的 vertex N 不能被消费者静默提升，转层必须显式操作 |
| asset.template_instance：不允许局部分叉与双轨子图 | Houdini 同时维护内联匿名 Subnet 与外部 HDA 两套机制，且 HDA 实例 Allow Editing 后修改内部只改该实例、保存定义并 match 后才传播。本项目已有 `.source` 承担局部算法抽象、`NetworkBox` 承担视觉分组，故不设内联 `SubnetNode`，统一采用单轨 `.procgraph` + 一键 `graph.extract_asset`；解锁直接编辑共享定义并即时传播到未冻结实例，变体必须另存新资产；正式交付端口由 `GraphOutput` 显式声明，不与调试 `Display [D]` 耦合 |
| parameter.three_layer_ux：声明式多目标绑定与同构覆盖 | Houdini 用 `ch("../parm")` 相对路径字符串表达式提升内部参数，节点改名易断且缺乏静态类型与范围校验，关卡调参还需跨层级界面。本项目采用类型化 `targets` 多目标声明式绑定（要求同一叶 `TypeId`、自动求硬边界交集并随节点改名/删除同步），并在 `.procgraph` 根、`AssetNode` 与场景 `ProceduralGeneratorEntity` 三处直接内联渲染同一套暴露参数滑杆与差量 `overrides` |

### 仍需细化的模块合同

| 待覆盖条目 | 需要明确的边界 | 对齐方 |
| --- | --- | --- |
| LOD | 由显式资产合约决定，不按三角形数推断；`_LOD0`–`_LOD5` 只是用户举的候选形式，不是已实现的导入规则。要定义识别、缺级、选择与错误，与 meshlet/CPU 剔除方案一起审 | 渲染、编辑器、总负责人 |
| 属性约定 | P、N、v、up、Cd、pscale、scale、orient 的层、类型、单位、用途和写读双方；约定名推导用途的规则、显式用途覆盖与不匹配错误 | 程序化、运行时 |
| 随机与来源 | 当前表面撒点的 p/j/k、随机混合与子三角 UV，以及 Volume 的固定格/ordinal 与严格内部合同以模块说明为准；其他拓扑变化另定，不把这些来源或 uint id 当永久身份 | 程序化、运行时 |
| 实例变体 | `variant` 默认列名、缺列时用 0、越界失败；默认值不能伪装成推断出的用户意图 | 程序化 |
| 空间块 | publish 当前要求调用者显式传 chunk_size；生成器曾提议默认 32 m，但生成器尚未实现。将来须在生成器参数与合约中写明，不能把测量用 32 m 当作现行 API 默认 | 程序化、渲染 |
| 退化实例 | 上游非有限值使节点失败；发布时丢掉法线变换不可表示的实例并计数，不静默修复 | 程序化、渲染 |
| 身份与格式 | 图资产 GUID、节点文档名、运行时对象身份、缓存键、集合历史身份各自的有效范围；新场景版本只读最新版本、不写迁移 | 程序化、World、编辑器 |

## S5 场景与实例化：未实施的历史对齐方案

以下保留此前 P2 与渲染、性能、World、编辑器负责人的接口讨论，现已后移到 S5。以上显式合同优先；历史候选不是当前接口保证。实施前须在当时主线上复核，尤其是 LOD、剔除、材质与渲染数据尺寸；本轮不修改这些模块。

### 宿主、资源与保存

- 生成器组件（`world.ProceduralGeneratorComponent`）挂普通 Entity，引用 `.procgraph` 资产键、保存暴露参数差量 `overrides`、持有最近发布的不可变输出（基础宿主同步与组件级 `overrides` / 私有运行时图克隆已落地；S5-2 接入 `SceneSnapshot` GPU 常驻实例集合区段）。
- 图资源为 `Content/**/*.procgraph`，复用 `.asset.json` GUID 边车。现行图 JSON v4 含节点、连接、暴露参数 `parameters`、`AssetNode` `asset`/`overrides`、`NetworkBox` 与 `StickyNote`，遵循[持久化格式规则](../../persistence.md)只读写最新版本，不保留旧版兼容分支。
- 场景经组件 properties 保存。生成器若需要新的顶层载荷，先确定版本；新程序只读最新场景版本，不增加旧版本迁移。已有内联 Array 编解码按字段 form/kind 分派，不按 JSON 数组形状猜测；它扩展了此前无法保存的字段形状，不需要空迁移。
- 材质沿用 `editor::Session::mesh_surfaces(assets::Reader&, const MeshAsset&)`，网格经 `project.assets().mesh(key)` 取得。编辑器与 Play 同源：按网格槽顺序解析当前默认材质 capture，未映射槽保留导入表面，不乘组件独有参数。刷新/重载后重新解析，逐元素比较 SurfaceRef；外层 vector 每次新建，不能拿它的地址当变化。材质变化不替换局部变换数组。
- 第二刀的生成网格候选资源键为 `<图 GUID>#<输出名>`，经 set_mesh_binding 接入；碰撞缓存加入内容哈希。未实施，不是现在可解析的资源键。

| 未来模式 | 预期行为与数据归属 |
| --- | --- |
| 实时 | 参数或代码变化重算，结果在内存 |
| Cache（默认，未实现） | 以完整持久内容键查 `Cache/Procedural/<图 GUID>/<键哈希>.bin`，可重建；不能直接保存当前进程内缓存键 |
| 冻结 | 按 `freeze.scope_and_history` 分为定义中的节点冻结与宿主中的实例/生成器冻结，只显式解冻；具体载荷和文件格式尚未定，不把历史二进制边车候选当作已定格式 |
| Bake | 可撤销地转成普通 StaticMeshEntity；第二刀再支持 Mesh 资产 |

### 集合与渲染接口

Geometry 的 instance 层和按（引用，空间块）发布、逐块复用已实现，见[模块说明](README.md#实例发布)。下面是尚未接入的消费者契约：

| 项 | 对齐方案与约束 |
| --- | --- |
| 数据 | MeshInstance 增加共享不可变局部仿射数组：每项 12 float、48 B，行主序、行向量，x/y/z 轴与平移四行。null 是普通单实例，非 null 必须非空，空集合由生产者不发布；selected、surfaces、常量材质对整个集合生效 |
| 身份 | SceneSnapshot 按（组件，网格，块）管理非零 stable_key。局部数组相同则沿用；替换数组则换新键，旧集合退休、新集合插入并回到 stationary。仅生成器 world 变化沿用现行 dynamic 判定。消费者持有 shared_ptr，不能只缓存裸地址 |
| 世界矩阵 | local[i] · 生成器 world。首版 CPU 用 double 合成世界矩阵、光栅法线矩阵、Lit 逆矩阵与逐实例镜像符号，校验并入同一循环；只在数组或 world 改变时执行，所以拖动生成器是 O(N) |
| 法线与有限性 | 发布已用公共 math::normal_transform 判据拒绝退化局部矩阵。集合路径组合 normal(world) · normal(local[i])，不从可能下溢的 FP32 世界乘积重新推导法线并拒帧。数组或 world 改变时仍检查合成位置是否可表示，失败沿用渲染器整帧拒绝边界 |
| 包围盒 | 数组变化时渲染器自己合并所有变换后网格包围盒，O(N)，不信任生产者给的范围；world 变化只变换集合包围盒的 8 个角 |
| 常驻区段 | 按 stable_key 分配光栅、Lit 与 material_offsets 区段，数组/world 变化才更新，其他场景项增删不平移集合。draw 直接以区段基址 + SV_InstanceID 取实例；scene_vs、lit_gbuffer、lit_shadow 按 draw 选择取法，阴影按区间绘制而非逐实例上传 caster 下标 |
| 检查与计数 | 字段须进入 MeshChecks::same、valid_instance 及 sizeof(MeshInstance) 断言。scene_instances/scene_culled_instances 计展开实例，scene_draw_calls 计实际 draw；无集合场景数值不变。按旧数字搜索 acceptance 中断言后列入必要验证 |
| 剔除和阴影 | 首版 CPU 按集合包围盒，阴影缓存视集合为一个 caster，探针场景范围按集合算。点光 face 与级联相交时可能重绘整块，大块还会撑大深度范围；分块只降低浪费，不能代替后续显式 LOD/剔除方案 |
| 历史 | stable_key 与集合内序号混合为每实例历史表面 id，供 TAA/SSR 区分；数组更换导致历史失效 |
| 拾取 | 先按实例包围盒排除，再做三角形求交；命中选择生成器组件。实测需要时再为实例包围盒建 BVH，编辑器负责人审 |
| 扩展 | 逐实例属性以后另设与变换等长的流。共享不可变列迁移不顺带更换这里的 shared_ptr<const vector<Affine>> 接口 |

TLAS 路径仍需实测选择：

- **a：gfx 每实例句柄。** 当时的 gfx build/update 会 CPU 遍历全部实例并上传 64 B 描述；一个普通物体运动也可能使 10 万实例每帧重写约 6.4 MB。
- **b：调用方持有描述缓冲。** 候选局部 gfx 补丁接受常驻实例描述缓冲和数量；普通物体按脏区间写，集合由 compute 展开，世界/法线矩阵与 TLAS 描述一次生成。数组变化预先计算界值，world 变化用 O(1) 判界。首版数据布局应允许 a→b，不提前实施补丁。
- 展开实例数须小于 2²⁴（InstanceID 24 位）。当时布局估算每实例光栅 108 B、Lit 192 B、材质偏移 4 B、TLAS 64 B，10 万约 37 MB，另有 gfx CPU 句柄；这些是预算依据，非当前占用测量。
- 不能把每实例变成一个 CPU 渲染项：此前性能负责人以 Dungeon 3380 实例推算到 10 万，render_items + SceneSnapshot 约 20 ms、Renderer prepare 约 9 ms、静止 Scene Lit 循环约 3.7 ms，阴影规划还随灯数增加。只是当时推算，不是新实现的成绩。

### 后续性能验证口径（本轮不采样）

保留 Dungeon 原样、Dungeon_10k、Dungeon_100k 三个只差生成器的工程副本，用同一现有网格、显式 LOD，报告单实例和总三角形数；实例在可走区域，镜头只见一部分。已对齐镜头参数为 `--scene-lit-camera 6.927793 -0.362655 24.410025 3.6086 -1.3809 33.7881`。

- Renderer 的 release profile 看 clustered、clustered_orbit、docked_smooth；性能负责人届时补 clustered_motion，分别移动生成器和一个普通 Entity，覆盖 TLAS update。
- CPU 看 render.prepare、render.encode.scene.lit、render、application_step_ms；GPU 看 acceleration、gbuffer、shadow_cache、ddgi_trace/ddgi_update、direct_visibility/direct、hdr_reflections、measured_main_total_ms。Acceleration 的 CPU 包含在 .lit，按场景差值比较。
- 无窗口 scene CPU profile 看 render_items、light_items、scene_snapshot、scene_snapshot_changed，届时增加 generator_moved/replaced 情形，配合现有 procedural profile。
- 依当时机器协议持锁、独占空闲采样；基线/改动版交替各两轮取中位数，不加产品专用 marker。静止 Dungeon 应在基线波动范围内，否则补成对测量；10k→100k 报静止 CPU 斜率、运动 ns/实例、各 GPU pass 增量及实例/剔除/draw/TLAS/上传字节数。这是诊断口径，不用测量代替正确性验收，也不以估算宣称达成 120 FPS。

## S2 可视化及后续工具（旧 P3/P4/P5 参考）

目标仍为 Houdini 式紧凑节点、Parameters 独立调参、Tab 搜索添加、display 标记。可复用停靠标签页、DSL、out_of_flow/offset 布局、DrawQuad、菜单搜索、反射 Parameters、文档撤销历史、操作符/Control、虚拟 Table 和视口 OverlayShape 点显示；不因此认为节点画布已存在。

| 编号 | 未实施的缺口 | 接口审阅 |
| --- | --- | --- |
| G1 | UI 线段/曲线连接，复用抗锯齿着色，守住 UI 合批契约 | 渲染、编辑器 |
| G2 | 画布按档位缩放 DIP 尺寸与字号 | 编辑器 |
| G3 | 宿主处理平移、框选、移动、连线、断开，基于接受帧 UiFrame::find 命中 | 编辑器 |
| G4 | 节点/图参数检查、数组编辑、图文档撤销 | 编辑器 |
| G5 | Network View、Geometry Spreadsheet、Asset Browser 类型与视口 display 输出 | 编辑器 |
| G6 | graph.add_node/connect/disconnect/delete/move/set_display/freeze/bake 与 graph.inspect，返回点数、范围、属性统计及间距/穿插反馈 | 编辑器，遵循其操作符规范 |

下表保留旧阶段的范围与估价，不覆盖上面的 S0–S5 顺序，也不授权实施。S1 的原生构造/Realize 已从旧 P5 中提前，不能再按旧 P5 整体作为前置。

| 旧阶段 | 历史范围与验收目标 | 原估算，非本轮排期 |
| --- | --- | --- |
| P1 | 已交付 Geometry、图、求值器、JSON v1、原生/源码节点的验收见模块说明；Remap/噪声已随 S1-F 接入 | 原计划 14–18 人日，不代表剩余工作量 |
| P2 | 生成器、覆盖、Cache、冻结、Bake、Play；验证旋转/非均匀缩放下集合与矩阵、改参数增量发布、静止不发布、保存重开及删缓存重建、冻结不随代码变化、Bake 可撤销；GPU 读回、退化实例不拒帧、其他项增删不重写集合、只替换变化块；另做性能对照 | 场景/格式 6–9；渲染集合/TLAS a 8–12；视实测追加 TLAS b 5–8，GPU 剔除/LOD 6–10 人日 |
| P3 | G1–G6；窗口验收操作符和拖拽连线、Parameters、连线中点和视口像素、表格行数、撤销/重做、保存重开及 inspect 统计 | 14–20 人日 |
| P4 | 子图、从选中节点封装数字资产，L3 批准后迁移改变点数的基元；验证展开前后及 C++/Source 输出一致 | 6–10 人日，依赖 L3 |
| P5 | 基本体、挤出、焊接、法线、投影 UV、实例嵌套/Realize、Mesh 输出和碰撞缓存；参考已有 OBJ 耳切三角化保留边界 mask | 20–30 人日起，另立方案 |

布尔、细分、减面、UV 展开、凸分解不混入 P1；候选参考 Manifold、OpenSubdiv、meshoptimizer、xatlas、V-HACD，按真实需要逐项核实许可、维护状态和接入收益。
