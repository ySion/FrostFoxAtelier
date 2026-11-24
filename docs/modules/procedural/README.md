# Procedural：几何与程序化图

静态库 `atelier_procedural`（[cmake/procedural.cmake](../../../cmake/procedural.cmake)）依赖 runtime、geometry 与 source，不依赖 World、presentation 或编辑器。入口头文件：

| 头文件 | 内容 |
| --- | --- |
| [geometry.hpp](../../../modules/engine/procedural/include/procedural/geometry.hpp) | `Geometry` 值、层、用途（含 `Usage::orientation`）、属性列、拓扑与实例 |
| [operations.hpp](../../../modules/engine/procedural/include/procedural/operations.hpp) | 导入、生成、挤出、扫掠、倒角、撒点、射线投射、变换、Promote、Copy to Points、Realize、包围盒 |
| [instances.hpp](../../../modules/engine/procedural/include/procedural/instances.hpp) | 把 instance 层发布为渲染用的实例集合 |
| [bvh.hpp](../../../modules/engine/procedural/include/procedural/bvh.hpp) | 首角扇形 `Bvh` 与供 `.source` 并行只读查询的 `Spatial` 加速缓冲及 16 槽共享缓存 |
| [graph.hpp](../../../modules/engine/procedural/include/procedural/graph.hpp) | `Graph`、统一 `Node` 基类（含受控 L3 `emit_*/set_*` 构建方法）、实例级 `ports`、`AssetSubgraph` 绑定与编辑入口 |
| [nodes.hpp](../../../modules/engine/procedural/include/procedural/nodes.hpp) | C++ 内置节点（含边界节点 `GraphInput`/`GraphOutput` 与通用数字资产节点 `AssetNode`） |
| [evaluator.hpp](../../../modules/engine/procedural/include/procedural/evaluator.hpp) | 增量求值、跨实例共享 `CachePool`、递归子图求值与失败保旧接口 |
| [random.hpp](../../../modules/engine/procedural/include/procedural/random.hpp) | 固定算法的整数哈希与单位随机数 |

引擎附带的自有语言节点与随机库在 [assets/engine/procedural](../../../assets/engine/procedural)。

本文只说明已实现能力。方向、设计依据与未实施的跨模块约定见[后续路线与未决契约](roadmap.md)。编辑器与呈现层已接入 `.procgraph` v4 数字资产（`AssetNode`、`GraphInput`/`GraphOutput`、多目标暴露参数 `parameters` 与 `graph.extract_asset` 一键提取）、SOP 视口上下文、Houdini 式二维 Network 连线画布（含默认锁定预览与面包屑下钻）、Geometry Spreadsheet、`world.ProceduralGeneratorEntity` / `world.ProceduralGeneratorComponent` 场景生成器组件（含内联 `overrides` 与私有克隆图求值）、实例 `procedural.bake` 与 Control 操作链（见[编辑器模块](../editor/README.md)与[项目场景](../world/project-scenes.md)）；渲染器 GPU 大批量常驻实例化区段、持久磁盘 Cache 与磁盘冻结尚未实施，节点已支持会话冻结。

## Geometry

宿主提供的不可变 `geometry::Mesh` 通过 `Mesh::create` 创建，返回 `std::expected<MeshRef, geometry::MeshError>`；错误只有诊断 message，不依赖 Runtime。失败候选被丢弃，`import_mesh(const Mesh&)` 仍只读取已发布资源。固定 `cube_mesh()` 保持 MeshRef 返回值，工厂失败按内部不变量 fatal；程序化 Geometry 自身的值、COW 与发布合同不变。

`procedural.Geometry` 是 Runtime 记录类型（`ASTRUCT`，没有反射字段），作为节点端口按引用传递。它是一个值：用 [SharedArray](../../foundation.md#共享不可变数组) 共享属性、拓扑和实例列，写入共享列时才复制成独占；已有独占列原地写。每个写入者必须独占自己的 Geometry，但无需先在单线程上复制列。这个存储能力不改变求值器在 Runtime 线程上串行执行的规则。

- **五层**：point、vertex（图元的角）、primitive、detail（恰好一个元素）、instance。每层是“名字 → 列”，名字在整个 Geometry 内唯一，必须是标识符。点位置是 point 层的 `P`，总是存在且是第一列。
- **列**：`SharedArray<T>` 持有 `Array<T>`，T 为 bool、int32、uint32、float、Vector2/3/4、Quaternion、Matrix4；String 只能放在 detail 层。`attributes()` 和 `find()` 暴露的共享列只能读取；`read` 借出 const Array，找不到时返回 null，层或类型不符是错误；`write` 先取得独占列再借出可写 Array；`add` 不存在时按填充值新建；`set` 接管一个共享列句柄（若同名列已存在，其所在层与元素类型必须匹配，且未显式传入 `usage` 时自动保留原有用途）；`remove` 删除。SharedArray 本身不注册为 Runtime 类型，源码借用的仍是原来的 Array 类型。
- **借用**：借用结束前，不移动、赋值或结构修改该 Geometry，也不再次 `write` 同一列；可写借用期间还不能复制该 Geometry 或出借列的句柄，只读借用允许复制。需要多列时先完成增列和独占准备，再取供计算使用的引用；求值器与 Ray 操作都按这个顺序执行。
- **用途**：列带用途 `point` / `vector` / `normal` / `color` / `orientation` / `none`，决定变换方式。新列按 `Geometry::admit_name` 的约定名取用途：P → point，N → normal，v、up → vector，Cd → color（Vector4），orient → orientation（Quaternion），其余 none。
- **拓扑**：`vertex_points` 给出每个顶点引用的点；图元是首顶点的 CSR 偏移，全部图元角数相同时不存偏移。独立保留 COW 列 `kind` 区分 polygon（至少三角、隐式闭合）与 polyline（至少两角、只连接相邻角、不隐式闭合），不能从角数推断，也不能经属性绑定改写。`polygons` 明确生成面，`primitives` 接收每图元的 kind；`primitive(p)` 同时返回角范围和 kind。删除、合并保留 kind，`reverse_primitives()` 显式反转两类图元及 vertex 属性；镜像变换只纠正面的绕序。
- **实例**：去重的资源引用表 `references`（持久键 + 宿主解析出的 `MeshRef`），加上 instance 层的保留列：每个实例的引用下标与有限仿射 Matrix4。`set_instances` 替换整个 instance 层并丢弃它的属性。
- `bytes()` 估算列与拓扑保留的内存（按容量计），共享的列按全额计算，供缓存预算使用。

## 操作

- **`import_mesh`**：每个三角形一个图元，法线和 UV 写入 vertex 层的 `N`、`uv`，材质槽号写入 primitive 层的 `material`；将 `-0.0f` 规范化为 `+0.0f` 后，位置逐位相等的顶点焊接成同一个点。
- **`grid` / `line`**：`grid(GridSettings)` 支持 `zx`（+Y）、`xy`（+Z）、`yz`（+X）三参考平面上的 `quads`（默认四边形网格）、`triangles`（三角形网格）与 `points`（纯点阵）；表面模式要求 `rows/columns >= 2` 且尺寸为正，写出 `vertex` `N` 与 `[0,1]` 仿射 `uv`；双参数便捷入口 `grid(size, rows, columns)` 保持纯点阵语义。`line` 输出含两端的等距散点。
- **`box` / `sphere` / `tube` / `wedge` / `circle` / `torus`**：按下述拓扑生成几何，表面均写出 `vertex` `N` 与 `vertex` `uv`。`box` 支持三轴细分（共享棱与角点构成单一闭合流形壳）及 `base_on_ground`（底面贴齐 `center.y`）；`tube` 支持独立 `radius_top`/`radius_bottom`（一端为 0 时生成共享尖顶三角锥面）、中空 `inner_radius`（凸梯形环盖）、多层 `rows` 与扇形 `arc_degrees`（自动封闭径向切口）；`wedge` 生成闭合楔形体或双坡屋顶棱柱（`peak_ratio` 覆盖 `0..1`）；`circle` 在三参考平面上输出闭合凸多边形（小于 360° 为过圆心的凸三角扇）、闭合折线或供 `Sweep` 拱门/弯墙直接消费的开放圆弧折线（`open_arc`）；`torus` 生成 `minor_radius < major_radius` 的无自交圆环或带盖弯管。预检总角数均须小于 2³²−1，表面消费者仍执行公共凸面判据。
- **`Geometry::connect_points` / Polyline**：只接受纯点输入，不得带任何图元或 instance；按点序连接为一个开放图元，不焊接，不自动闭合，没有 closed 选项。0 点空进空出（保留 detail，与未连端口相容），1 点报错。全部 point 与 detail 列及其用途原样共享保留，不生成 N；已有空 vertex/primitive 列保留定义，分别扩为点数/1 个类型默认值，instance 列仍为空，不静默删列。`Line → Polyline` 先生成等距点，再连接相邻点；作者手填点数组的绘制曲线节点后移到 S2，见 roadmap。
- **`Group`** 输出一列命名的 `bool` 属性。选 `normal` 时，用局部空间点位和多边形绕序计算面积法线；与 `direction` 的夹角 ≤ `max_angle`（0–180°）即入 primitive 组。边界包含在内，折线的条件值为假，零面积面报出首个图元索引。选 `existing` 时直接读取 `source_group` 的层和成员。选 `range` 时，读取 `source_attribute` 指定的 float 列，在同一层对 `minimum ≤ 值 ≤ maximum` 写入成员。边界可相等；界值和列值须有限，反向区间报错，非有限列值报告首个元素索引。默认 `replace` 写入条件结果；`unite / intersect / subtract` 计算“基组 ∪ 条件 / 基组 ∩ 条件 / 基组 − 条件”。输出可与任一操作数同名。显式命名的组须存在、为 bool，参与组合的组须同层。已有列替换遵守 Geometry 层与类型契约。
- **屋面示例**：对宽 2、高 1 的对称 `Wedge` 用 +Y、45° 得到两片 `roof_faces`；以右坡法线 (1,1,0)、0° 得到 `protected_side`。在 `Group` 里选 `existing`、`subtract`，以 `roof_faces` 为基组、`protected_side` 为条件，输出 `work_area`。下游 `Extrude.group = work_area` 只抬高左坡。Scene View 的 Guides → Highlight Group 可以检查该列，Geometry Info HUD 显示成员数和所在层的总数。
- **算子用途**：在 Source 的 `class` 前写 `[DESC = "..."]`，说明接受什么输入、做什么、输出什么。编辑器 Tab 搜索可按说明中的关键词筛选，其行提示与 Control 的 `graph.inspect {"view":"catalog"}` 共用 Runtime 的类型说明。显式几何端口可在 `[Port, DESC = "..."]` 或原生 `APARAM(Port, DESC = "...")` 声明拓扑、坐标和接线条件；画布节点的提示与精确类型目录共读端口说明。
- **命名属性契约**：类型目录和网络节点提示使用同一份声明。Source 的列参数可在 `In/Out/InOut/Group` 上加单行 `DESC` 说明用途，例如 `[In("id", Optional, DESC = "缺列时使用点序号。")]`。绑定器传入空数组，点序号回退由节点内核实现。原生 `Scatter` 声明所读的坡面组和写出的点列；`Realize / CopyToPoints` 声明具名的放置输入。任意源几何的列传播和 `Realize.target_attributes` 的显式广播仍由各自算子定义。Scatter 的 `density` 是每平方米的候选点数，输入几何上即便有同名列也不会自动加权。
- **屋面覆盖（选组灰模验收）**：[MossRoof.procgraph](../../../examples/Content/Procedural/Graphs/MossRoof.procgraph) 用 `Wedge → Group → Scatter` 在朝 −X 的 45° 坡面生成固定锚点。`cover_noise` 按点位写出 `moss_cover`；`moss_patch` 按闭区间生成同名点组，`RandomScale → Realize` 仅在组内放置代表苔斑的扁球，便于计数和辨认位置。保持上界 1，降低 `moss_patch.minimum` 会让苔斑向周围扩展，已有锚点保持原位。示例默认下界 0.55。
- **`group_box`**：在各轴闭区间 `[minimum,maximum]` 内选 point，写入/替换同名 bool 列；边界须有限且逐轴有序。名字遵守 Geometry 标识符规则，已有异层/异类型列报错。GroupBox 节点在输入的 COW 副本上操作，不改输入；这是输出组名，不能用空名字表示全选，不支持表达式或反选。
- **Connectivity** 按共享点索引将多边形、开放折线和散点划分为连通块，在 point 层写入 uint 属性（默认 `piece`）。编号从 0 开始，按每块最小点索引排序。仅位置重合的独立点保持分离，孤立点各有一个编号；实例层与其余属性原样传递。需要逐图元编号时接 Promote，目标层选 primitive，归约选 first。
- **`scatter`**：接受三角形及凸平面多边形，不接受折线；按下述公共首角扇形视图采样。原图元 p、局部子三角 j 的随机源 s 在 j=0 时为 `hash(seed,p)`，j>0 时为 `hash(hash(seed,p),j)`；点数为面积 × 密度，以 `hash(s,0)` 按小数部分概率进位，第 k 个点使用 `hash(s,2*k+1)` 与 `hash(s,2*k+2)` 均匀采样重心坐标。输出按 p/j/k 顺序，k 在每个子三角内从零开始，不把展开下标或线程号混入随机源。可选最小间距通过紧凑双数组开放寻址空间哈希在全部候选上按这个顺序稳定淘汰，所有列一起压紧；格坐标超出 ±2⁶² 时失败。输出 P、N、`id`、`sourceprim`、`sourceordinal`、`sourceprimuv`；仅输入包含超过三角的面时在末尾追加 uint `sourcetri`（其他三角面的值为 0）。纯三角输入的六列顺序、算术与值不变。`id` 在 j=0 时为 `hash(p,k)`，否则为 `hash(hash(p,j),k)`，它只是非唯一随机种子；（p,j,k）是局部稳定来源。`sourceprimuv` 是子三角 j 内的 (b1,b2)，不是原多边形坐标。
- **`transform` / `match_size`**：底层 `transform(geometry, translate, rotation, scale)` 执行 `v · S · R · T`；`TransformSettings` 与 `Transform` 节点采用围绕局部 `pivot` 的缩放加角度制欧拉旋转（`X → Y → Z` 顺序）再平移。point 列完整变换，vector 列只用线性部分 L，normal 列乘 sign(det L) · cofactor(L) 后归一化（零向量保持为零，所以压扁到平面的奇异变换仍有平面法线），`Usage::orientation` 四元数列复合旋转，实例矩阵右乘，color 与 none 列不变；行列式为负时只反转 polygon 的顶点顺序及 vertex 列，polyline 的方向不变。`match_size` 按包围盒执行逐轴 `none / min / center / max` 对齐（默认 XZ 居中、Y 下界贴齐 0）与可选等比/非等比 `scale_to_fit`（连接 `reference` 几何且非空时自动对齐参考几何包围盒）。
- **`promote`**：把属性移到另一层，按 first / sum / average 合成每个目标元素接触到的源元素（向 detail 归约或自 detail 广播直接走单次线性快路径）；sum 与 average 只接受 float 与 float 向量；instance 层只和 detail 交换；P 不能移动。
- **`remove_flagged`**：删除 bool 列为 true 的元素；删除点时一并删除用到它们的图元，删除图元时保留点。
- **`merge`**：按输入顺序拼接各层，属性按名字合并，缺失的元素取类型默认值，detail 取第一个有值的输入；同名属性的层、类型或用途不一致时失败。
- **`copy_to_points`**：每个点一个实例，矩阵为 S · O · T：S = scale × pscale，O 是归一化的 `orient`，没有 orient 时由 N 和 up 对齐（只有 N 时取 +Z 到 N 的最短旋转），T 取自 P。点上的 uint 属性（默认 `variant`，缺失时为 0）选择引用。detail 属性带到输出，输出没有点。非有限输入和越界的变体失败。
- **`realize`**：有序 Geometry 源在选中的目标点上生成真实几何副本，与资源 CopyToPoints 共用放置规则，但不生成或递归展开 instance；分组、属性和所有权合同见下节。
- **`extrude`**：对选中（`group` 支持 primitive 或 point bool 组，空表示全选）的凸平面多边形沿面法向挤出 `distance`（可由 primitive float 列 `distance_attribute` 逐面覆盖），并在原平面沿角平分线内缩 `inset`（负值为外扩，可由 `inset_attribute` 逐面覆盖；内缩致边折叠或反向时直接报错）。`individual=true` 时逐面独立生成带硬边 `vertex` `N`、米制侧面 `uv` 与来源 primitive/detail 属性的棱台；`output_back=true` 时每个独立棱台各自封闭底盖，无 4 面共边非流形，可直接送入 `Boolean` 与 `VolumeScatter`；`distance=0` 且 `inset!=0`（须 `output_back=false`）在原面内切出边框四边形环与中心面板。`individual=false`（要求 `inset=0` 且挤出距离非零、同号）合并选中区域的共享内边，仅沿外边界生成侧壁。可选写出 primitive bool 组 `front_group`、`side_group`。输出经公共凸面判据统一复核。
- **`sweep`**：沿 `backbone` 中的每条开放 `polyline` 扫掠 `SweepProfile::rectangle`（`width × height` 硬边法线）、`tube`（`radius × segments`，`segments >= 8` 平滑径向法线）或 `custom`（`cross_section` 端口单图元 XY 截面，polygon 为闭合、polyline 为开放）。标架采用 `up` 参考向量稳定斜接加共线处平行输运，可选按折线点上的正有限 float 列 `scale_attribute`（默认 `"pscale"`，缺列按 1）逐站缩放截面；闭合截面且 `end_caps=true` 时在首尾生成外向封盖以构成水密流形壳。三维扭转或起伏导致侧面四点超出公共平面容差 δ_p 时，自动沿对角线剖分为两个共点三角形，确保输出全部满足公共凸平面判据。写出 `vertex` `N`、沿弧长与环周的 `vertex` `uv`，并继承折线 primitive 与 detail 属性。
- **`Spatial`**：`Spatial::acquire(geometry)` 按与 `Bvh::acquire` 相同的 16 槽底层 `SharedArray` 指针元组缓存，将任意含多边形或开放折线的 `Geometry` 编译为紧凑 32 字节 `SpatialNode` 只读序列（同时包含表面三角 SAH/中位数 BVH 与折线有向线段归一化弧长 BVH，保证树深不超过 54 层），供 `.source` `[Port] const procedural.Spatial&` 在 `parallel_for` 内零 Runtime 交互并发查询。
- **`bounds`**：点的包围盒，空几何没有。
- **`ray`**：在 `surface` 上按 `RayMode::project`（沿方向投射射线）或 `RayMode::minimum`（最近表面点）查询并将命中的点移到命中处，写入 int32 `hitprim`、`hittri`、Vector2 `hitprimuv` 与 float `hitdist`；未命中点保持原位且编号/距离为 -1；`normal` 开启时从命中子三角原角计算单位法线写入 point 层 `N`；`remove_misses` 开启时移除未命中点。内部经 `Bvh::acquire(surface)` 复用缓存 BVH。
- **`Bvh`**：对公共扇形视图建立的包围体层次（分箱 SAH，深层退回中位数切分，深度有界），建立时复制三角形，零面积三角形不参与。`Bvh::acquire(surface)` 提供线程安全的 16 项 LRU 共享缓存，以底层 `P`、`vertex_points`、`primitive_offsets`、`primitive_kinds` 的缓冲指针与长度元组为键，并持有这些 `SharedArray` 的浅拷贝以防止 ABA 地址复用；当仅调整下游射线或采样参数而表面缓冲未变时，零拷贝命中同一 `shared_ptr<const Bvh>`。`ray` 沿单位方向找 max_distance 内最近的交点，三角形双面有效；`closest` 找 max_distance 内最近的表面点。Hit 给出原图元号 `primitive`、局部子三角号 `triangle`、距离、子三角内重心坐标和位置。三角形求交和最近点在 double 中计算，包围盒测试留在 float（溢出的距离就是无穷远），所以有限坐标不会因中间结果溢出得到非有限或超出距离限制的命中，平行于某一轴的射线不受零的符号影响。射线恰好穿过共享边时可能两侧都不命中。

### Realize：真实几何复制

`realize(sources, points, group, variant, target_attributes)` 按选中目标的点序复制其选定源，不焊接、不去重。group 默认空表示全选，非空须为已有 point bool 列；只筛目标点，不筛源属性。variant 默认列名 `variant`，须为 point uint，缺列选源端口 0；选中目标的值越界时报错。目标输入的图元与 instance 不是复制内容；任何源包含 instance 都失败，不静默跳过未选源的 instance，也不解析资源。

- 放置为 S·O·T，和 CopyToPoints 共用输入列读取与矩阵计算；orient 优先于 N/up。源列（包括 P）按其 Usage 使用 Transform 的同一公式：point 完整变换，vector 不平移，normal 用余子式，color/none 不变；输出保留用途，不以 P 的名字强制改为 point。源非 detail 的 orient 复合放置旋转。负缩放只纠正 polygon 绕序，vertex 列一起反序，开放线方向保留。奇异缩放沿 Transform 的余子式法线规则，不自动删除副本。
- 只合并实际用到的源 schema，point/vertex/primitive 列保持其层、类型和用途；缺列元素按类型默认值填充，**不变换默认值**。同名异层、异类型或异用途失败，不猜测 Promote。源 material 只保留原值，不构造跨资源材质表。
- target_attributes 默认空，是显式目标 point 列名单；每个值广播到该副本所有点，覆盖同层同型同用途的源列。禁止 P，显式缺列或冲突报错。广播值已经处于目标空间，不再套放置变换；其余目标 point 列不隐式传播。
- detail 按实际用到的**源端口顺序**取每个名字的首值，不按目标遇见源的顺序；再由目标 detail 补缺，同名类型/层/用途冲突仍报错。detail 原样共享，不参与放置。无选中点时没有副本，但保留目标 detail。
- 预检点/角/图元总数均小于 2³²−1，再预留各输出缓冲并批量填充，不先复制 N 个 Geometry 再 merge。借用只读输入，输出拥有新拓扑和逐元素列，detail 继续写时复制；失败不发布局部结果，输入和兄弟分支不变。不造 Entity、GUID 或 MeshRef，资源实例发布路径不变。

### 构造拓扑的持久语义

实现位于 [constructors.cpp](../../../modules/engine/procedural/src/constructors.cpp)。已保存图虽不存 Geometry 缓冲，重算仍依赖点序、面序、首角及 N 所在层；同类型不能静默更换这些规则。后续三角化须显式操作，不把 Box 六面改成十二面。

- **Box**：默认 `1×1×1` 未细分时，8 点的 bit0/bit1/bit2 分别取 X/Y/Z 上界，0 取下界；六面按 −X、+X、−Y、+Y、−Z、+Z 排列，角号依次为 `(0,4,6,2)`、`(1,3,7,5)`、`(0,1,5,4)`、`(2,6,7,3)`、`(0,2,3,1)`、`(4,5,7,6)`，并附逐面 `[0,1]` 仿射 `vertex uv`。开启三轴细分时沿六面边界共享整数格点，维持同一六面大组顺序与外向闭合流形。
- **Sphere**：Y 为极轴，M=`segments`、L=`latitudes`；先北极、再从北到南的 L−1 圈（纬角 πj/L），最后南极；每圈经角 φ=2πi/M，从 +X 向 +Z。共 `2+(L−1)M` 点、`ML` 面、`M(4L−2)` 角，共享缝和极点。先北极三角 `(north,ring1_next,ring1_i)`，再逐圈四边形 `(upper_i,upper_next,lower_next,lower_i)`，最后南极三角 `(south,last_i,last_next)`；每角 N 是其实际点位置的单位径向，附经纬 `vertex uv`。
- **Tube**：默认实心等径单层整圆（`radius_top == radius_bottom > 0`、`inner_radius = 0`、`rows = 1`、`arc_degrees = 360`、`end_caps = true`）保持原持久拓扑：先下圈 b、后上圈 t，共 `2M` 点、`M+2` 面、`6M` 角，侧面 `(b_i,t_i,t_next,b_next)`，下盖 `(b_0,…,b_M−1)`，上盖 `(t_0,t_M−1,…,t_1)`。一般参数下，尖顶（半径为 0）合并为单顶点并输出三角形锥面；中空（`inner_radius > 0`）追加反向内壁并以凸梯形环片封盖；部分张角（`< 360°`）在 `end_caps` 开启时封闭首尾径向切面。
- **Wedge / Circle / Torus**：`Wedge` 沿 Z 切分为 `nz+1` 组等截面三角形 `(A_k, B_k, C_k)`，依次输出底面、右坡、左坡四边形与 −Z/+Z 山墙三角形；`Circle` 在右手平面基 `(u, v, n)`（`u × v = n`）下按角度采样，整圆多边形输出单 `M` 边形，部分扇形输出过圆心的 `M` 个凸三角形，`open_arc` / `closed_polyline` 输出单条 `polyline`；`Torus` 要求 `0 < minor_radius < major_radius`，主环平行弦构成严格共面的等腰梯形四边形，部分张角且 `end_caps = true` 时封闭两端管口。

### 公共凸面剖分

Scatter 的 `group` 留空时在整个有效表面撒点；命名组必须是输入几何的 primitive 布尔列。全假组选出零点，并保留相同的输出列结构。只对组内面生成候选再执行最小间距淘汰，因此组外点不会挤掉组内点。源面与扇形三角仍按原输入编号；相同种子下，未做间距淘汰时组内候选的位置、ID 和重心坐标与全表面采样对应项一致。输入表面的几何有效性校验覆盖全部图元。

模块内部 [triangles.cpp](../../../modules/engine/procedural/src/triangles.cpp) 是 Scatter、BVH/Ray、Volume 共用的数值判据与首角扇形视图，不生成第二份拓扑；借用期间 Geometry 必须保持不变且存活。图元 p 的第 j 个三角取原角 `(first, first+j+1, first+j+2)`，顺序为 p 再 j；折线明确失败。Scatter/BVH 的三角输入保留原先零面积跳过规则；Volume 要求三角面也通过非退化面校验。多边形都必须有可分辨的平面、简单凸边界及非零边，合法多边形中的零面积扇形片不算退化面。

本节通用输入多边形的判据用 double 计算。每个原多边形 p 的距离容差 δ_p = 8 × float ε × max（该面各角最大绝对坐标分量，该面 AABB 对角线长），两个输入量都不依赖首角或扇形划分，没有固定米制下限；8 是为 float 输入和变换舍入预留的安全系数，不是容许任意翘曲的建模参数。`polygon_scale` 同时为凸面校验与 Volume 表面带提供这个尺度和容差。首角扇形的有向面积向量长度必须大于 δ_p × AABB 对角线长，点到所得平面的距离不超过 δ_p，投影边长须大于 δ_p。采用保守凸性规则：投影相邻边的有向叉积必须非负，即使负值来自舍入也拒绝，不用距离容差放过凹角；正向共线角仍有效，允许零面积扇形子三角。接近折返的角（点积非正、叉积不超过 δ_p × 两邻边长度之和）也拒绝。在此非负转角前提下，转角总和必须为 2π（double 累加容差 64 × ε × 角数），既排除多圈星形，也不让正反小回环抵消成一圈。检查为线性时间，不另建边对相交矩阵。适用于细节显著大于其绝对坐标 float 精度的模型；远离原点或极薄的不可分辨面报错，不静默修补。后续构造与拓扑工具复用此判据，不另定容差。

### Bevel 实体倒角

`procedural.Bevel` 的 `surface` 输入可以接一个闭合外向的凸件，也可以接多个分离的凸件。`angle_degrees` 按相邻面的外法线夹角选棱，共面剖分缝会跳过。`radius` 是米制圆角半径，两面垂直时等于两侧的退距。`segments=1` 生成平倒角，增加至 2–8 段可逼近圆弧。半径为零或无选中棱时原样输出。请参阅[几何算法与适用范围](bevel.md)。

### Boolean 实体建模

SOP 的两个 Geometry 端口与 C++ 的 `solid_boolean(a, b, operation)` 走[同一实现](../../../modules/engine/procedural/src/boolean.cpp)。输入为无自交、外向的闭合实体边界，内腔取相反朝向。求解前检查边的成对方向、点的单一面扇和总有向体积。调用方负责输入内部没有自交或重叠壳。空 Geometry 可作为空集，空结果不携带会影响下次运算的格网；仅有点、折线、实例或开放壳会报告错误。并、交、差都返回实体的体积边界，面接触的交集为空，共面的重合表面优先使用 A。仅沿一条边接触的两个独立实体可以保留各自的点环。

- **求交与分类**　两个新输入共用以 2 的幂为步长、最长轴至多 2²⁵ 格的局部坐标系，坐标取最近格点，未被面引用的散点不参与定标。每个轴上的首次舍入误差至多半格。闭壳在其整个包围盒外的有向穿越相消，多壳输入的包围盒树据此整体剔除无贡献的壳。包含样点的外壳与内腔都参与分类。包围盒树筛出相关面，各面只对有实际交段或共面重叠的候选做平面切分。非共面交段若在源面内部终止，先用端点所属的目标边支撑平面建立局部分界，再仅切分交段覆盖的单元，避免近共面三角形的支撑平面延长后留下浮点显示不了的窄条。交点由三个支撑平面确定，使用齐次整数保存。以分片首角的闭包围盒查询射线候选，再用整个分片的符号扰动样点精确判定边界两侧的有向穿越数。只有确认整个源面与对侧边界分离时，才沿其共享点复用内外归属。首次进入 Boolean 的网格中，格上共面的凸面在点、角属性为常量时可整面裁切；格上平行四边形也支持仿射浮点属性。随角变化的法线和需要原扇形的属性继续按源三角取样。共线边界角沿用原面规则。宽整数的转换、位宽上界和包围盒取整反例见[算术推导与可执行证书](boolean-arithmetic.md)。算法借鉴了 EMBER 的平面表示与局部排列，目前采用串行求解。[论文与补充材料](https://www.graphics.rwth-aachen.de/publication/03339/) · [作者完整讲座](https://www.youtube.com/watch?v=-myVogx5mHY)
- **输出与连续运算**　保留凸多边形，按同一直线上的端点补齐两侧边界，以成对有向边恢复共享点。发布前检查 float P 的首角扇形朝向及闭壳拓扑。Boolean 输出还按现有点、角和图元索引携带不可变的精确坐标与支撑平面。后续 Boolean 可共享这些数据，来自不同格子的结果在位宽允许时做精确换基。写 P、翻转面或改动表面拓扑后，下一次 Boolean 以当前网格重新量化；改属性继续共享精确边界。由普通几何文件重新导入也从导入坐标开始。退化输入、超出位宽或 float 无法显示的面和边会返回错误。Ray、Scatter 可直接读取已验证的输出扇形；Volume Scatter 保留自己的严格尺度检查。
- **属性与选择**　每个输出面都有 uint 图元列 `boolean_operand`（0 为 A，1 为 B）与 `boolean_primitive`（本次操作的源图元）。该名字再次经过 Boolean 时刷新归属。图元属性按来源逐面复制，包括完整 32 位材质、标签与组。点和角上的浮点标量、向量按源面插值，离散值取权重最大的源元素；差集反向面的法线随面反转，UV 仍使用各自的角。新共享点需要单一值时使用第一个贡献面，A 先于 B。缺列用类型默认值；同名列层、类型或用途不符时报错。Detail 取首先定义该列的输入。
- **验收**　覆盖共面重叠、嵌套内腔、相同实体、接触、空集、图端口、切面材质及角 UV；两个半径 1、中心相差 0.5 的 12×6 球网格连续 30 轮重算 `(A−B)∪(A∩B)`，逐轮验证表面积、体积并对最终结果执行射线查询。该规模用于验证精度，百万三角面的交互性能尚未测量。

**移动刀具与诊断。** 平移验收依次经过间隙、点接触、边接触、面接触和薄层穿透，再从正交方向切第二刀并检查两组切面的材质和本次操作数归属。旋转验收在共面姿态两侧以解析三角楔的体积核对差集和交集。24×12 球面移动到三个不同的偏移量时检查切分后的合计体积，并将其中一组的差集与交集拼回以检查面积及体积。编辑器求值另覆盖薄刀被拒绝、下游保留最后有效预览及恢复厚度后的重算和缓存命中。错误给出发生阶段，原始面精度错误还包括操作数和图元。

实测原场景可通过 [Editor Control](../editor/control.md) 的 `graph.inspect` 读取目标节点的 `status.boolean`。C++ 可向同一个 `solid_boolean` 传入 `BooleanStats*`。四段耗时依次覆盖输入准备、局部切分与内外分类、交线缝合、拓扑与属性发布。`input_faces` 计进入求解的面数，`candidate_pairs` 为两个操作方向累计的三维包围盒重叠数，`ray_candidates` 是各分片实际发出的分类射线经 BVH 筛选后访问的非平行面总数，已复用归属的面无需发射线，`splits` 是实际新增的单元数。`peak_face_scratch_bytes` 取各源面切分时局部单元、角及交段缓冲的最大同时持有动态容量，包含尚未释放的父单元与切分中的两个子单元；整次求解的峰值内存还包括包围盒树、按输入点保存的内外归属、保留面片、缝合表、发布缓冲和分配器开销。

每个 Evaluator 为 Boolean 的 A、B 输入各保留一个准备槽。一次实际求解首次遇到某个上游输出和公共格点时只记录其身份，连续第二次遇到时才保留精确面和包围盒树，后续求解可直接使用。格点的原点和步长必须完全一致。改变上游属性也会产生新的输出身份，按当前属性重新判断是否需要保留扇形对角线。准备数据计入 Evaluator 的结果缓存预算，预算为零或空间不足时照常求解。独立调用 `solid_boolean` 在调用结束时释放准备数据。`prepared_input_hits` 的最低位表示 A 命中，次低位表示 B 命中。结果缓存直接返回完整几何时保留上次实际求解的统计。

Release 下运行 `atelier_procedural_acceptance --profile-boolean <新的绝对路径.json>` 可重放两类刀具。盒阵列的每个操作数使用 12 或 96 个闭盒（按三角扇计 144 或 1,152 个输入三角形），分别测局部接触与大量共面重叠，每组预热两次。双球差集使用 24×12、48×24、96×48 三档半径为 1 的单闭壳网格，每边各有 528、2,208、9,024 个三角形。裁切球的 X 偏移循环取 0.375、0.5、0.625，三个姿态各自预热一次。每组记录 21 次实际 Boolean 调用的耗时、阶段计数、面数、体积、表面积与几何签名；签名覆盖有序的点坐标、面拓扑、操作数、源面以及各角法线。变换、计算签名和核对体积均在计时区间外。报告保留失败刀位及错误原因，任一刀位失败会使进程以失败状态结束。

同一报告还用真实 Evaluator 对 96×48 双球分别执行零预算和默认预算的相同轨迹。每趟先用相交操作观察输入，再依次计算三个刀位的差集。报告分别记录图求值、Boolean 节点与准备阶段的耗时，并逐刀对照直接求解的几何签名。

### Volume Scatter：有向覆盖与固定格

[volume.cpp](../../../modules/engine/procedural/src/volume.cpp) 的 `volume_scatter` 使用串行 BVH 精确有向穿越分类；默认 density=1 点/m³、origin=(0,0,0)、cell_size=1 m、seed=0。输入是非空闭合壳集合，不接受 instance 或折线。所有点须有限，各面通过公共非退化判据；按**原多边形边界边**检查每条无向边恰好两用且方向相反，并检查每个被引用点的角环只有一个连通分量。不按位置焊接，不修补开口，不做自交检测。整体有向体积必须为正（平移至首角后用 double 补偿求和），允许反向内腔，不逐壳要求正值；未经转换的 Houdini 顺时针网格会被拒绝。

每个候选以最终存入 P 的 float 位置分类。对闭合有向三角链上不在表面的 q，绕数 w 是整数；从 q 沿射线到无穷远，每个正向交点贡献 sign(n·d)，全部贡献之和就是 w。因此 **w ≥ 0.5 等价于有向穿越和 ≥ 1**，无需逐候选扫描全部三角形。不取绝对值/奇偶，不提前接受部分和。重叠正壳的 w=2 保留，外壳加反向内壳的内腔 w=0 排除；分离负壳内部 w=−1 也排除，不猜测作者意图。零面积扇形片贡献零并跳过。

边界先于最终阈值：查询点 q 到任一原多边形 p 的**欧氏距离 ≤ δ_p** 时判为表面并排除，包括公共边、扇形对角线和顶点。各非零面积扇形片共用原多边形的 δ_p，以到有界三角形的最近距离判断，邻域取并集；不使用无限平面与外扩半平面的交集，细长扇形的延长尖角不会抹掉远处内点。距离计算与 BVH 共用 `closest_on_triangle` 的 double 实现，平面距离只作宽阶段。δ_p 来自上述公共 `polygon_scale`，不是精确实数谓词；分辨不出的贴面内点也不保留，不随机挪点、不把表面假定成 w=0.5。

δ_p 的输入尺度不依赖首角；精确算术下，多边形的距离邻域也与首角无关。但当前距离经过扇形片的 double 重心插值计算，距离与 δ_p 只差舍入量的边界点可能随首角变化。此为 C R2 暴力版已有的数值限制；加速保持同一输入的叶谓词，不承诺这类边界点在循环换首角后逐位不变。

一次独立 `volume_scatter` 默认（`cache_bvh = false`）按次建立 SAH BVH 并持有固定 p/j 顺序的有效三角快照；图节点 `VolumeScatter` 显式开启 `cache_bvh = true`，在表面未变时经 `Bvh::acquire` 复用已构建的 BVH。新增查询不改变 Ray/closest 的最近命中行为：

- `on_surface_band` 是存在性查询。节点按子树最大 δ_p 扩大有效查询范围；比较 q 到原始 float 盒两侧的 double 差值，向下舍入后仍大于剪枝阈值才跳过，而不是仅对 `盒边界 ± δ` 的最终结果取相邻值。剪枝阈值是向上舍入的 `δ + δ·2^-27`：额外项等于 32 × double ε × 原面尺度，覆盖最近点重建略微越出顶点盒和距离舍入的误差；不改变叶子的 δ。令 u=double ε/2，返回权重非负且和至多 1+8u，凸包越界及重建各至多 16u×尺度，距离比较再用 8uδ，总计小于 34u×尺度，低于预留的 64u×尺度。`closest_on_triangle` 明确禁止 va/vb/vc 的乘加融合与重结合，以保留上述权重范围证明所需的逐次舍入。叶子用各自原多边形的 δ_p 和原有界距离谓词，任一命中就排除。不能拿最近面的距离只比它的 δ，也不能取全局最小 δ：更远的大尺度面仍可能覆盖 q。
- `signed_crossings` 用保守 double slab 遍历全部正向交点，不按最近 t 裁剪。叶子再检查各三角自己的射线盒，避免同叶无关三角影响重试。固定八个非轴向方向取 √2/√3/√5 的确定二进制近似及符号排列；常量可由 float 精确表示，谓词全部在 double 中计算。不按运行、候选或遍历序号随机选方向。
- **符号认证不是 δ 带。** 每次 double 加、减、乘、除后，用相邻 double 向外包住结果；区间组合包含最初的坐标减法和相消误差，不按已相消结果乘一个经验 ε。有限 float 坐标和方向精确扩展到 double，三次乘积在其范围内。保持既有 precise 编译，不启用 fast-math 或重结合。令 a/b/c 为相对 q 的顶点，认证 u=det(b,c,d)、v=det(c,a,d)、w=det(a,b,d)：有两个可信异号即在面外；三者可信同号时，其和就是 n·d，det(a,b,c)/(u+v+w) 的符号判定前后向，正向贡献共同符号。区间含零且不能证明在面外时重试，涵盖边、顶点、共面和扇形对角线；不凭未认证的边侧把交点随意分给某一片。这些局部谓词及整数和不依赖树的分组和遍历顺序。
- **射线盒兼容认证**：先用普通 double 减法和除法，给每个交界参数 t 加减 `abs(t)·2^-49`；该余量覆盖原逐操作外扩端点相对 t 的误差（四种 IEEE 舍入下小于 9u，u=2^-53）以及最终加减的舍入。单调 min/max 给出原 near/far 的上下界，能证明原布尔结果才返回，否则执行原 Interval 盒测试；零差也回退，零方向沿用原范围比较。保持原盒结果、三角访问和全部重试／兜底计数，不扩大行列式的认证集合。此处是几何顶点盒，不替代上述带 δ_p 和重建余量的表面带盒。
- **仅八个方向全部不确定**时，对该候选直接复用原 p/j 顺序的立体角补偿求和：`2 atan2(det(a,b,c), |a||b||c| + dot(a,b)|c| + dot(b,c)|a| + dot(c,a)|b|)`，总和 ≥ 2π 即保留。没有规模阈值、开关或第二套表面带；固定有限方向不保证任意合法网格都有非退化方向。这里“精确”表示没有近似 fast-winding 或 parity 替代，不声称表面容差和兜底浮点求和是任意精度实数运算。

典型总工作约为 O(T log T + C·(log T + I + D))，I 是实际穿越/叶工作，D 是表面带和重试成本；重叠壳、细长大盒或全退化仍可能 O(C·T)。这不是并行墙钟复杂度或性能保证。尚无最小间距。各方向尝试/重试与兜底次数只经现有 Recorder 和下述 profile 上报，不增加观察 API。

壳校验、原序三角快照和 BVH 构建仍串行。分类按连续候选序号每 256 个划一个逻辑块：cell 前缀保留零计数格，一个高密度 cell 可以跨块；一次 `parallel_for` 融合生成 float P、表面带和射线／兜底，按候选序号写固定 P 与字节状态。每块只写自己的统计槽，不共享计数器。join 后宿主合并整数计数并提交 Recorder，再按块 retained 前缀精确分配六列，用第二次 `parallel_for` 稳定填充。无完成顺序 append，无每候选堆分配；BVH 与三角快照只读共享。worker 只借用固定普通值和输出区间，不碰 Runtime；发布 Geometry 留在成功 join 后。取消或非有限 P 的任务 Failure 均不发布部分结果，后者在 join 后转成原有 procedural 错误。P／状态 scratch 最多 13 MiB，cell 前缀约 4 MiB（替代原 counts），另有块统计；六列最终输出最多另占 32 MiB，不把 scratch 当作总峰值。

- 固定格使用几何局部坐标，不按包围盒推导 h。各轴范围为 `floor((min−origin)/h)` 到 `ceil((max−origin)/h)−1`，min/max 只统计被图元角引用的点；游离点不扩大格范围或消耗候选预算，但仍须通过全部输入点的有限性校验。只接触边界的外格不参与。格坐标先检查 int32，再转换；按 x/y/z 字典序（z 最快）枚举。
- 设 H(a,b) 为现有 lowbias32 组合 hash，U 为高 24 位映射到 [0,1)。格随机源 `s=H(H(H(seed,uint32(x)),uint32(y)),uint32(z))`，负坐标使用模 2³² 编码。λ=density·h³，格内数目 `floor(λ) + (U(H(s,0)) < frac(λ))`。第 k 候选 id=`H(s,k)`，三个位置通道为 `U(H(id,1/2/3))`；以 double 计算 `origin+(cell+u)h`，最终舍入成 float P，再分类。节点 seed 为 H(图 seed, 节点 seed)。这是持久重算语义，不混入枚举下标、线程号或 std::hash。
- 输出按格/ordinal 顺序稳定压紧为六列：P、uint id、int sourcecellx/sourcecelly/sourcecellz、uint sourceordinal。无 N，不携带输入属性。id 有碰撞，不是身份；格/ordinal 只在 origin/h/seed 固定时表示局部来源。改密度不重排已有 ordinal 的候选位置；几何远处变化不重生成原格候选，压紧后的点下标可以改变。
- **相交格数和筛选前实际候选数分别不得超过 1,048,576**，小于 Geometry element_limit。先检查格范围/乘积，再用真实概率进位累计候选；不以格数×ceil(λ) 误拒，不等筛选后再计数。超限整节点失败，既有求值器保留旧发布；不截断、不自动调格或密度。候选 float 溢出也报错。上限不是交互性能承诺。

## 实例发布

`publish(geometry, chunk_size, previous)` 把 instance 层按（引用，块）分组：块是生成器局部空间中平移所在的立方体 floor(t / chunk_size)。每组是一个 `Collection`，带一份共享不可变的 3×4 仿射数组（行主序、行向量约定，x、y、z 轴行加平移行），实例按源顺序排列，数组从不为空。

- 发布接口仍使用 `shared_ptr<const vector<Affine>>`，不随内部 Geometry 列迁移成 SharedArray。
- 线性部分不能通过 `math::normal_transform` 的实例（零缩放、压扁）被丢弃并计入 `dropped`。这个判据与渲染器对 world 矩阵的判据是同一个函数，所以发布的内容不会让渲染器拒绝整帧。
- 与 `previous` 中同一（引用，块）的集合逐字节相同时，复用旧的数组指针，以指针为键的消费者因此保留自己的状态。
- 块大小必须有限且为正，块下标必须在 int32 范围内。

## 程序化图

`procedural::register_types(Runtime&)` 发布 Layer、Reduce、RayMode、BooleanOperation、ResampleMode、SweepProfile、OrientationPlane、GridPrimitive、CirclePrimitive、AxisJustify、FallbackShape、Geometry、`procedural.Geometries`（`Array<Geometry>`）、Spatial、Context、统一基类 `procedural.Node`、Graph 和内置节点；String、`Array<String>` 和除 String 外每种列类型的 Array（`procedural.Bools`、`Ints`、`Uints`、`Floats`、`Vector2s`、`Vector3s`、`Vector4s`、`Quaternions`、`Matrix4s`）不存在时一并发布，源码节点借用的列就是这些类型。重复调用不做任何事。

- **Graph**（`procedural.Graph`）：公开字段 `seed`；隐藏的节点强引用序列和 display 强引用槽。
- **Node**（`procedural.Node`）：所有 C++ 与 `.source` 程序化节点统一派生自 `procedural.Node`（不再设 `PointNode`）；节点类型的公开字段就是 UI/持久化参数；隐藏的 `inputs` 强引用序列每个连接一项，按输入端口排序，与之平行的非反射 C++ 数组记录每个连接的（输入端口，上游输出号）。`.source` 节点直接使用语言级 `fail("...")` 语句报告业务失败（返回 `invocation_failed` 与源码位置，`Node::fail` 已退休）并用 `parallel_for` 并行计算属性列。`procedural.Node` 自身无 `process` 方法，不能直接作为图节点实例。
- **统一 `process` 与三类参数/输出机制**：`ports(runtime, type)` 直接从 `Runtime` 元数据（原生类型读 `TypeInfo::methods`，`.source` 类读 `TypeInfo::described_methods`，无需 `source::Program*`）检查名为 `process` 的无返回值同步方法（原生节点须为 `const` 方法；`procedural.Node` 通过 `Meta("source.contract_method", "process")` 固定派生类 `process` 的端口契约，不影响普通非节点 `.source` 类的 `process` 热更）：
  1. **Geometry 与 Spatial 端口角色**：参数带 `Port`（或无角色属性并直接声明端口类型引用）：`const procedural.Geometry&` 为单输入端口，`const procedural.Geometries&` 为有序多连接输入端口，`const procedural.Spatial&` 为经 `Spatial::acquire` 构建缓存的只读空间查询输入端口（分派前完成序列投影与别名校验，可在 `.source` `parallel_for` 内并发查询），`procedural.Geometry&` 为显式输出端口。所有输入与输出端口参数必须具名；图连接按端口序号存储。`.source` 节点可通过 `procedural.Node` 上的受控 `BorrowSafe` L3 构建方法（`self.emit_points`、`emit_polylines`、`emit_polygons`、`emit_quads` 及 `set_bool/int/uint/float/vec2/vec3/vec4/quat(..., output_port = 0)`）将局部栈上拥有型 `Array<T>` 批量提交到显式 `[Port] procedural.Geometry&` 输出端口，宿主在提交时原子校验点有限性、各层列长、约定 `Usage` 及多边形公共凸面判据 δ_p（主列流模式下禁止对 0 号 `output` 调用 `emit_*/set_*`，以保护活动可写列借用）。
  2. **属性列与组流式角色（Column SOP / 正式类 Wrangle）**：声明不带 `Port = "..."` 的 `[In]`、`[Out]`、`[InOut]` 或 `[Group]` 参数时，自动建立 0 号主输入端口 `input` 与 0 号主输出端口 `output`；而 `[In(..., Port = "road")]` 与 `[Group(..., Port = "road")]` 则从具名辅助单输入端口 `"road"` 只读借用上游列（可与同名 `[Port] const procedural.Spatial& road` 共享同一个图输入端口，且在无主列流参数时不额外生成空置的 `input`/`output` 端口）。整节点至少须有一个输出端口：
     - `[In]`（`const Array<T>&`）：从输入几何只读借用共享列，零拷贝；缺失或异层/异型报错。附加 `Optional`（如 `[In("id", Optional)]`）时，若列缺失则借入零分配空 `Array<T>` 哨兵（内核用 `col.size() == n` 判断是否存在），若同名列存在但层或类型不符仍报错。
     - `[Out]`（`Array<T>&`）：若同层同名同类型列已存在，对其取得 COW 独占副本；若不存在则按类型默认值新建（新列按 `Geometry::admit_name` 赋予约定 `Usage`，类型与约定用途冲突如 `Array<float>& N` 时报错）。
     - `[InOut]`（`Array<T>&`）：要求输入几何已存在同层同名同类型列，取得 COW 独占副本原地读写；缺失报错。
     - `[Group]`（`const Array<bool>&`）：组名来自字面量或 `From = "group_field"`。组名为空字符串时借入零分配空 `Array<bool>` 哨兵表示全选（`mask.size() != n`，不分配全 `true` 临时数组，也不写回几何）；非空时须为输入几何同层已有的 `bool` 列。
     - **列名与运行层解析**：省略列名与 `From` 时默认取形参名（如 `[InOut] Array<vector3>& P`）；也可显式写字面名 `[Out("pscale")]` 或绑定节点的 `String` 字段 `[Out(From = "attribute")]`（字面名与 `From` 互斥）。节点至多可声明一个带 `[RunOver]`（`AFIELD(RunOver)`）的公开 `procedural.Layer` 字段作为动态默认层；否则默认 `Layer::point`；单个参数还可用 `Layer = "detail"` 等静态覆盖层。
- **执行顺序与安全保证**：每次列模式执行先记录主输入各层元素数、复制节点 `String` 选择字段并读取 `[RunOver]` 层，拒绝两个可写参数指向同一列；先从输入几何解析全部 `[In]` 与非空 `[Group]`，再在输出副本上准备 `[InOut]`/`[Out]` 的增列与独占缓冲（因此同名 `[In]` + `[Out]` 安全读取旧输入列，不会读到新建零列或半写缓冲），最后同步调用整节点 `process`。调用返回后、进入缓存与发布前，求值器核对每个 `[InOut]`/`[Out]` 可写列（含 `P`）的长度未发生改变，并在写入了 `P` 时统一校验全部点位置有限。
- **Context**（`procedural.Context`）：字段 `seed` 是图的种子，C++ 节点用 `seed()` 读取；`mesh(key)` 经宿主的 `Resources` 解析网格，并记为当前节点结果的依赖。全部成员同一访问级别，类型保持标准布局，所以 `seed` 有生成器给出的偏移，源码按偏移直接读 `context.seed`；`mesh` 不是 Runtime 方法，源码不能调用。
- **编辑**：`add_node`（类型必须能读出端口）、`restore_node`（把已由外部根保活的同一 `ObjectId` 节点在指定下标恢复回图成员，供编辑器增量撤销/重做复用缓存）、`remove_node`（同时断开它的下游连接，它是 display 时清除 display）、`connect`（两端都在图内，端口存在，不成环；单输入端口替换已有连接，多输入端口追加到末尾）、`disconnect`、`set_display`（null 清除）。每个入口只写它改变的对象：节点列表和 display 写图，连接写下游节点。读取用 `contents` 和 `inputs`，都是 const 借用。图持有节点、节点持有上游，被删除的节点由别的根（例如编辑历史）保活。

内置 C++ 节点：

| 节点 | 参数 | 端口 |
| --- | --- | --- |
| MeshImport | mesh | context → output |
| Grid | primitive_type=quads（quads / triangles / points）、orientation=zx（zx / xy / yz）、size=(10,10)、center=(0,0,0)、rows=10、columns=10 | output |
| Line | start、end、count | output |
| Box | size=(1,1,1)、center=(0,0,0)、base_on_ground=false、divisions_x/y/z=1 | output |
| Sphere | radius=1、segments=32、latitudes=16（纬向区间数） | output |
| Tube | radius_top=1、radius_bottom=1、inner_radius=0、height=2、center=(0,0,0)、base_on_ground=false、segments=32、rows=1、arc_degrees=360、end_caps=true | output |
| Wedge | size=(2,1,2)、peak_ratio=0.5、center=(0,0,0)、base_on_ground=false、divisions_z=1 | output |
| Circle | primitive_type=polygon（polygon / closed_polyline / open_arc）、orientation=zx、radius=(1,1)、center=(0,0,0)、segments=32、start_degrees=0、arc_degrees=360 | output |
| Torus | orientation=zx、major_radius=1、minor_radius=0.25、center=(0,0,0)、base_on_ground=false、major_segments=32、minor_segments=16、arc_degrees=360、end_caps=true | output |
| Polyline | — | points → output |
| Resample | mode=spacing、spacing=1 m 或 segments=8、preserve_corners=false | curves（折线）→ output |
| Group | group="selected"、criterion=normal、direction=(0,1,0)、max_angle=45°、source_group=""、source_attribute=""、minimum=0、maximum=1、operation=replace、base_group="" | input → output |
| GroupBox | group="selected"、minimum=(−.5,−.5,−.5)、maximum=(.5,.5,.5) | input → output |
| Connectivity | attribute="piece"（point uint） | input → output |
| Scatter | density、seed、min_distance、group=""（primitive bool）；节点种子为 hash(图种子, seed) | context, surface → points |
| Boolean | operation 默认 unite，可选 intersect、subtract（A 减 B） | a, b → output |
| VolumeScatter | density=1、seed=0、origin=(0,0,0)、cell_size=1 | context, surface → points |
| Transform | translate、rotate（X→Y→Z 角度制欧拉角）、scale、pivot | input → output |
| MatchSize | justify_x=center、justify_y=min、justify_z=center、target_position=(0,0,0)、scale_to_fit=false、uniform_scale=true、target_size=(1,1,1) | input, reference → output |
| Promote | name、to、reduce | input → output |
| Remove | flag、layer | input → output |
| Merge | — | inputs（多个）→ output |
| Null | —（原样直通零拷贝传递） | input → output |
| Ray | mode（project / minimum）、direction、max_distance、normal、remove_misses | points, surface → output |
| CopyToPoints | meshes（资源键列表）、variant | context, points → output |
| Realize | group=""、variant="variant"、target_attributes=[] | sources（有序多个）, points → output |
| Extrude | group=""、distance=0.5、inset=0、distance_attribute=""、inset_attribute=""、individual=true、output_back=true、front_group=""、side_group="" | surface → output |
| Sweep | profile=rectangle（rectangle / tube / custom）、width=1、height=1、radius=0.5、segments=8、up=(0,1,0)、end_caps=true、scale_attribute="pscale" | backbone, cross_section → output |
| GraphInput | port_name="input"、order=0、preview_fallback=empty（empty / grid / box / sphere / line；仅独立打开该图且无外层注入时回退） | output |
| GraphOutput | port_name="output"、order=0（零拷贝透传正式交付输出，与作者调试 `Display [D]` 解耦） | input → output |
| AssetNode | seed_offset=0（`0` 原样透传外层种子，非零经 `procedural::hash` 确定性混合）；动态端口由 `ports(runtime, node_id)` 从所绑定的 `AssetSubgraph` 读取 | 动态 inputs → 动态 outputs |

原生节点的 `process(...) const noexcept` 返回 `atelier::Result<void>`，反射仍发布为业务结果 void，端口和 BorrowSafe 约束不变。业务失败在节点边界转换为 `invocation_failed` 并保留原始原因，不通过异常传递；`Resources::mesh` 和 `Context::mesh` 同样通过 noexcept Result 返回资源拒绝。未连接的单输入端口收到空几何。

### 变截面扫掠

Sweep 在每个骨架点读取 `scale_attribute` 指定的 point float，乘到截面的每个线性尺寸。默认列名为 `pscale`，空名或缺列时系数为 1。Control 的 `graph.inspect` 可在 `view: node` 下启用 `bindings` 查看绑定列、上游及回退状态。圆管截面达到 8 段时，侧面按实际相邻环的周向和纵向计算平滑角法线。直线上的锥台因此也能反映半径变化，端盖继续使用独立的平面法线。[弧形石栏杆案例](../../../examples/Content/Procedural/README.md#弧形石栏杆-arcbalustrade)

### 沿路径布点与保形重采样

`Resample` 对每条折线分别从起点计算弧长。间距模式按 `0, spacing, 2 × spacing, …` 取样并保留精确终点，最后一段可以较短。段数模式沿原始弧长均分。默认相邻采样点直接相连，可能削平两点之间的转角；开启 `preserve_corners` 会额外插入原线的转折点，后面的采样位置不动。因此开启后实际段数可以增加。在直段内增加共线控制点不会额外布点。

例如一段向东 3 m 再向北 2 m 的街墙：间距 2 m 时柱位在原线的 0、2、4、5 m；扫掠墙体时开启保留转角，则再插入 3 m 处的墙角。输出的点层 `arclength` 为原线累计长度，`curveu` 为其占总长的比例，`tangent` 为原线的单位切向；转角取出边方向，开放终点取入边方向。闭合线按原有的首尾共用点索引表示，接缝点的弧长和比例为 0，首尾顶点可以有不同的顶点属性。

浮点和向量属性沿边线性插值，插值法线归一化；整数、组、旋转和矩阵选取最近端点，平局取边起点。图元和 detail 属性沿用原值。已有同名的三个输出属性须匹配点层及类型。各曲线独立生成点，未被折线使用的点不参与；空几何产生空输出，纯散点需先连接 `Polyline`。相邻重合控制点不占弧长。整条零长、非折线、实例、闭环不足三个采样点、超出几何容量或相邻样点在输出精度下重合时返回错误。小于终点位置精度的末尾短段与终点合为一站。

### 自有语言节点与正式类 Wrangle

具名输入只由一个列绑定定义时，该绑定的 `DESC` 同时是端口接线说明。多个列共用输入时，各列保留各自的属性说明；显式 `Port` 的说明描述整个端口。可在 Control 的精确类型目录读取 `port_descriptions.inputs` 和 `port_descriptions.outputs`。

工程或引擎中的任意 `.source` `class Name : procedural.Node` 只要声明合法的同步 `void process(...)` 即成为正式 SOP 节点（公开字段为节点参数，函数体内显式书写循环或多步逻辑，不经过文本宏拼接）。[assets/engine/procedural](../../../assets/engine/procedural) 里的文件是引擎源码模块，`ProjectCode::snapshot` 把它们并进每个工程快照（带 `engine/` 前缀），并在首次构建时向工程 Runtime 注册本模块原生类型：

| 文件 | 内容 |
| --- | --- |
| random.source | 全局函数 `procedural.hash`、`hash2`、`unit`、`random_vector`，与 [random.hpp](../../../modules/engine/procedural/include/procedural/random.hpp) 算法相同；`random_vector(x)` 由 x 连续哈希三次，每个分量均匀落在 [-1, 1) |
| orient.source | 全局函数 `procedural.frame_rotation`、`shortest_arc` 与 `align_to_normal`（+Z 沿 N，+Y 朝 up；up 为零、与 N 平行或非有限时取最短旋转；N 为零时不旋转，非有限时结果非有限） |
| jitter.source | `procedural.Jitter`：`group`、`amount`、`axis_scale`、`seed`；校验参数有限性，优先读 `[In("id", Optional)]`，缺 `id` 列时回退到点序号 `i`，只偏移组内点 |
| random_scale.source | `procedural.RandomScale`：`group`、`minimum`、`maximum`、`seed`；校验范围端点有限性，优先读可选 `id` 列否则回退点序号 `i`，写组内 `pscale`；分组模式下若上游已有 `pscale` 则组外点保留原值，若缺列新建则组外点写单位缩放 `1.0` |
| normal_align.source | `procedural.NormalAlign`：`group`、`up`；校验 `up` 与输入 `N` 有限性，按 `align_to_normal(N, up)` 写组内 `orient`（自动获得 `Usage::orientation`）；若上游已有 `orient`，组外点保留原值。新建 `orient` 时，组外点遵循 `CopyToPoints` 的规则：零法线使用单位旋转，非零法线才读取可选的点层 `up` 并检查其有限性 |
| noise.source | `procedural.Noise`：`[RunOver] layer`（默认 `point`）、`source_attribute="P"`、`attribute="noise"`、`group=""`、`seed=0`、`frequency=1`、`offset=(0,0,0)`；3D value noise，`lattice_bits` 通过零开销 `math.as_uint` 编码二补码格坐标 |
| remap.source | `procedural.Remap`：`[RunOver] layer`（默认 `point`）、`source_attribute="noise"`、`attribute="pscale"`、`group=""` 及四端点；double 中间线性映射，允许反向与外推，不夹值 |
| [rail_panel.source](../../../assets/engine/procedural/rail_panel.source) | `procedural.RailPanel` 将两列同向、等点数的点层 `P` 按点序逐站配对。沿 +X 前进时，较小 Z 接 `rail_a`、较大 Z 接 `rail_b`。两轨可有不同弧长。输出 `panel` 为封闭三角网格，带顶点层 `N`；`thickness` 默认 0.09 米，沿图局部负 Y 量取。 |
| spatial.source | 结构体 `procedural.SurfaceHit`、`procedural.CurveSample` 及 `parallel_safe` 空间查询函数 `procedural.raycast_surface`、`closest_surface_point`、`closest_polyline`（遍历 `[Port] const procedural.Spatial&` 的 64 层近端优先 BVH，使用双精度计算相交条件、距离和剪枝界限） |

### Noise / Remap 数值契约

两者均派生自 `procedural.Node`，支持 `[RunOver]` 动态选择几何层、可选组遮罩以及同名列读写（读旧输入），不改变元素数。只读缺列或异层/异型报错；组外旧值不变，新建输出列的组外值为 0。没有选中元素时仍校验节点参数，不读取未选元素的数值。

Noise 在输入空间按 float 分步计算 `q=position*frequency+offset`，不自动转世界空间或融合乘加。frequency 是每单位坐标的格数，允许 0（常量场）及负数（镜像）；offset 是缩放后的格坐标偏移。参数须有限，选中元素的 q 每轴须在 `[−2147483648,2147483648)` 内，NaN/Inf 或越界整节点失败，不 wrap、夹坐标或重定位。格子取 `floor(q)`，经 `math.as_uint(int(floor))` 取 32 位二补码位模式；复用 H=`hash2`：`s=H(graph_seed,node_seed)`，角值 `unit(H(H(H(s,bits(x)),bits(y)),bits(z)))`；小数 f=`q−floor(q)` 的权重为 `smoothstep(0,1,f)`，八角按 X→Y→Z 三线性插值，最终 float 显式 clamp 到 `[0,1]`。

Remap 四端点须有限、输入两端不同，输出两端可以相同。按 `out_min + ((x−in_min)/(in_max−in_min))*(out_max−out_min)` 用 double 运算后转 float，避免有限 float 端点相减溢出；最终 double 结果超出有限 float 范围或非有限则整节点失败，不饱和。允许反向区间及外推：0…1→2…6 时 0.25→3、1.5→8。

## 求值器

`Evaluator(runtime, graph, resources, program, cache_bytes_or_pool)` 在 Runtime 线程上同步求值 display 节点（或 `evaluate_targets` 指定的有序 `GraphOutput` 节点集合）及其上游。多个 `Evaluator`（主图、`AssetNode` 子图求值器与场景 `ProceduralGeneratorComponent` 私有图求值器）可共享同一个 `std::shared_ptr<CachePool>`（默认总预算 256 MiB LRU），内容键一致的节点跨实例零重算共享只读 `Geometry` 输出；`AssetNode` 嵌套深度硬上限为 `Evaluator::max_subgraph_depth = 8`。`evaluate()` 之后：
- 若本轮求值与当前 display 节点上游链路均成功，`output()` 与 `last_valid_output()` 均指向最新几何输出；
- 若未设置 display，两者均为空；
- **若本轮求值发生图级错误（返回 `Error`）或当前 display 链路存在失败节点**，`output()` 均明确清空为 `nullptr`（绝不把过期的陈旧几何伪装成当前有效结果），而 `last_valid_output()` 保留此前最近一次成功的输出供编辑器预览回退。

`status(node)` 给出每个节点的错误、求值次数、缓存命中次数及可选耗时 `time`。`evaluate_node(node, record_timings)` 按需同步求值任意图内节点及其上游闭包（即使该节点不在当前 display 链上），不替换 `output()` / `last_valid_output()`，之后可通过 `node_output_count(node)` 与 `node_output(node, output_index)` 读取任意节点的已缓存输出句柄供 Geometry Spreadsheet 等检查视图使用。`evaluate(record_timings = false)` 默认不读计时时钟；传入 `true` 才记录实际执行节点的耗时，包含参数准备及失败执行。`time` 表示该节点最近一次实际执行，而非本轮图求值。`program` 提供自有语言节点的代码，可以为空，这时自有语言节点失败（`unsupported_operation`）；它要比求值器活得久。

- **发现变化**：先取 `change_cursor()`，再读取。图在游标之后被写过时重读节点列表、display 和种子；节点是新的或被写过时，重读它的参数和连接，算出本地键。本地键和连接都没变的写入不触发求值；什么都没变、资源版本和 `Program::revision()` 也没变时直接返回。`Program::revision()` 变了就重新绑定自有语言节点的 `.process` 入口，代码身份没变的节点键也不变，不会重算。求值器只用 const 借用、`read` 和同步 `process` 调用，不给图和节点盖章。
- **键**：节点的键是精确的字节串：类型（已发布 `TypeInfo` 的地址）、每个公开字段的内容、带上下文的节点再加图种子，自有语言节点再加入口的代码身份（`EntryInfo::code`），以及每个连接的（输入端口，上游输出号，上游结果序号）。每次真正执行 `process` 得到一个新的结果序号，所以上游结果变了下游的键就变；内容相同的两个节点共享同一个缓存结果。
- **缓存**：按键查找结果，以条目为单位按最近使用淘汰，总量不超过 `cache_bytes`（默认 256 MiB）。撤销把参数改回旧值时直接命中旧结果，不再求值。
- **资源**：节点经 Context 读到的网格连同解析结果记在结果上。`Resources::revision()` 变化后，复用结果之前重新解析这些键；解析结果不同就重新求值，相同就继续复用。
- **失败与根因传播**：节点失败时它的下游不求值，下游节点的 `status(node)->error` 直接携带失败输入端口名及最上游失败节点的类型与原始错误原因（`Input '<port>' failed (<RootType>: <reason>).`），避免多层链路只剩空泛提示。失败的键在资源版本不变时不再重试，键变化或资源版本变化后再试。启用 `procedural::set_bypassed(runtime, node, true)` 的节点跳过 `process` 求值（及自有语言内核绑定），将第 0 个输入端口的首个上游输出（无连接时为空 `Geometry`）直接转发至 `outputs[0]`；启用 `procedural::set_frozen(runtime, node, true, snapshot)` 的会话态冻结节点（未启用 bypass 时）直接输出从当前输入捕获的 `FrozenOutput`。快照共同保有各输出及活动上游的传递资源依赖，撤销解冻和重做冻结会恢复同一个快照。冻结时使用该快照并跳过上游分支求值，直至显式解冻（持久磁盘冻结格式尚未定型，不写入 `.procgraph`）。
- **调度**：每个需要的节点每次求值最多调用一次 `process`，按上游在前的顺序执行。每次发现变化后，对全图连接检查来源输出号；环检测仍只遍历求值目标的上游闭包。

### 通用反射编辑后的图校验

通用 `apply_reference_changes` 仍允许把 display 或 inputs 写成图外节点，不提供图的写侧保证。`contents` 返回只读 `Contents` 快照（有序 nodes 与紧凑哈希成员索引），检查 display；`inputs(runtime, contents, node)` 检查节点及其来源都属于快照，并检查引用序列和连接记录等长。成员或 display 改变后须刷新快照。读取、保存和求值遇到这些无效结构返回 `argument_mismatch`，修复后恢复；`set_display`、`disconnect` 和 `remove_node` 使用内部原始读取，不因旧的图外引用阻止修复。

输出端口有效性由持有当前端口的消费者检查：求值器复用稳定的节点计划缓存与 `state.plan`，保存沿逐边 `ports()` 读取输出名。端口统一来自 Runtime 已发布的不可变 `TypeInfo`（`methods` 或 `described_methods`）。Source 重发布即使不给下游盖章，也由 Program revision 触发全图输出检查及内核重绑定；声明或绑定故障仍记入 `NodeStatus`，只有端口已知而输出号越界才成为外层拓扑错误。

脏图只为本轮重读或重绑定的节点暂存 local、inputs、类型与 ports、读取故障，先检查全部连接及 display 闭包的环，再写回这些字段、成员和 cursor；失败不推进内部缓存状态、`last_valid_output()` 保留，同时清空当前 `output()`，连续无修改求值仍失败，修复图结构后重新发布有效输出。成员变化时，未写过的节点用已缓存的 inputs 对新集合复核，不重读 Runtime。不可变类型元数据仍可按需记入现有 types 缓存，不创建第二份端口缓存。只改一个节点参数时仅重新生成该节点的 local 及连接，其他节点原地借读；重绑定未写节点才复制其已有连接。候选量随改动节点及其数据大小增长，不是整表 O(V) 复制，不复制旧 local、NodeStatus、结果条目或 Geometry 缓冲。脏图的发现和全图拓扑检查仍需 O(V+E) 遍历。

可观察变化：图外连接由旧 `invalid_member / A connection leaves the graph.` 改为 `argument_mismatch / Graph input connects a node outside this graph.`；display 图外、引用长度不一致、来源输出越界也显式拒绝。已修复输出号越界时直接读取 `entry->outputs[connection.output]`，以及旧 cursor 在拓扑排序前推进、导致第二次无变化求值误报成功的问题。PROC-GRAPH 残留：display 闭包外的环可保存，但加载时 `connect` 会拒绝形成环的边并沿原宽容诊断保留；本片不扩大环检测范围。

## 图文件

图文件的编解码在编辑器里（[editor/graph_format.hpp](../../../modules/editor/core/include/editor/graph_format.hpp)，`atelier_editor_core`），procedural 不依赖 editor。`load_graph(runtime, text, asset_port_resolver)` 把 **version 4** 的图文件打开到已注册 procedural 类型的 Runtime 中（遵循[持久化格式规则](../../persistence.md)，不保留对旧版本的向后兼容），得到 `GraphDocument`：图的根、可选 `template_node`、有序声明式暴露参数列表 `parameters`（`name`、同 `TypeId` 多目标绑定 `targets: ["node.field", ...]` 及可选 `label`/`category`/`description`）、每个节点的 `GraphNode`（名字、节点根、`AssetNode` 的 `asset` 持久键与差量 `overrides` JSON、画布位置、`shape` 形状预设、可选 `#RRGGBB` `color`、`comment` 备注与保留的原始 JSON）、`notes`（Sticky Notes 便签）、`boxes`（Network Boxes 分组背景框）、保留的 settings 和加载诊断（`world::Diagnostic`）。`save_graph(runtime, document)` 写出规范文本。名字、位置、外观与画布注释是文档数据，不放进节点对象，所以移动节点或改配色/形状不盖章；节点 `bypass` 为 true 时在节点条目写出 `"bypass": true` 并在加载时经 `set_bypassed` 恢复。

```json
{
  "boxes": {
    "terrain": {"color": "#386b5e", "position": [-40.0, -40.0], "size": [240.0, 200.0], "title": "Terrain"}
  },
  "nodes": {
    "grid": {"inputs": {}, "position": [0.0, 0.0], "properties": {"columns": 4, "rows": 3, "size": {"x": 3.0, "y": 2.0}}, "type": "procedural.Grid"},
    "out":  {"comment": "Final", "inputs": {"input": "grid.output"}, "position": [400.0, 60.0], "properties": {"order": 0, "port_name": "output"}, "shape": "diamond", "type": "procedural.GraphOutput"}
  },
  "notes": {
    "memo1": {"color": "#d4a72c", "position": [220.0, 0.0], "size": [200.0, 96.0], "text": "Pipeline overview", "title": "Note"}
  },
  "parameters": [
    {"category": "Layout", "label": "Grid Width", "name": "grid_width", "targets": ["grid.size.x"]}
  ],
  "settings": {"display": "out", "seed": 7, "template": "grid"},
  "version": 4
}
```

- **规范形式**：`nodes`、`notes`、`boxes` 均以标识符名字为键，按名字排序；`parameters` 保持作者排列顺序；空的 `parameters` / `notes` / `boxes`、空的 `AssetNode` `overrides` 及默认外观字段（`shape: auto`、无自定义 `color`、空 `comment`、`bypass: false`、无 `template`）省略写出，`dump(2)` 后加换行，未改动文档再次保存逐字节相同。
- **宽容加载**：与 Scene 相同，已声明 `Transient` 的旧字段跳过，其余放不进的内容保留、报告并在保存时写回。类型不存在或不能作为节点的条目整项保留（`unknown_type`）；已知节点和 settings 中解析不了的参数叶经 `PropertyReport` 保留；上游数字资产增删重排 `GraphInput`/`GraphOutput` 端口后，外层连接按端口名重对齐，失效连接按端口保留入 `kept["inputs"]`（`reference_dropped`，多输入端口混有未解析端点时保留有序列表，并在后续连线、剪线、重连、串接或删除上游节点时同步增删对应端点、保留其余未解析项与相对顺序并计入撤销历史）；换绑到缺失资产或端口定义无效的资产时，节点立即清空端口，保留原始绑定错误和按名记录的连线。上游删改暴露参数名后，`AssetNode` 上未匹配的旧 `overrides` 原样保留；嵌套诊断携带实例节点、资产键和参数名，场景生成器报告到对应组件；`display` 或 `template` 指向没有加载的节点时同样保留，并在重命名或删除占位节点时同步更新与清理（含撤销恢复）。
- **结构错误**直接失败：版本不是 4、未知键、`nodes`/`settings`/`notes`/`boxes` 不是对象、`parameters` 不是数组、标识符非法、缺少 `type`、非 `procedural.AssetNode` 携带 `asset`/`overrides`、`position`/`size` 非有限或尺寸非正、非法的 `shape` 预设名或非 `#RRGGBB` 颜色、`inputs` 格式非法。

图文件不保存派生的 Geometry 缓冲。

## 验收

`procedural_acceptance`（无窗口，[acceptance/procedural/main.cpp](../../../acceptance/procedural/main.cpp)）覆盖端到端真值契约：

- **Ray**：y=0 地面上方另有一块 y=2、朝 +Y 的三角形；从 (1,5,1) 竖直向下的射线停在 (1,2,1)，命中 hitprim 2、hittri 0，距离 3、重心 uv (.25,.25)、法线 +Y。
- **原生图求值**：Grid（3×2、3 行 4 列）→ Transform(+1,0,0) → Merge ← Line → CopyToPoints 发布 14 个实例（12 个格点加 2 个线点），首个实例平移为 (−.5,0,−1)。
- **连通块**：重合但拓扑独立的面、共享端点的折线、孤立点经 Connectivity 再 Promote 得到逐图元编号，并检查输入列与拓扑的写时复制。注册节点还经过 Merge → Connectivity → CopyToPoints 的完整图求值。
- **空间与混合 C++/`.source` SOP 契约**：`import_mesh` 正负零缝焊接、`Bvh::acquire` 跨 COW 副本缓存复用与写后失效、`Geometry::set` 用途保持、`scatter` 最小间距空间哈希；`Grid → GroupBox → Group → Jitter → RandomScale → NormalAlign → 自定义 .source SOP → Transform → CopyToPoints`（验证无 `id` 列回退、组遮罩、`RandomScale` 组外单位缩放与已有 `pscale` 保留、`Usage::orientation` 旋转复合及全部实例正常发布）；原生列绑定节点篡改可写列（含 `P`）长度被求值器拦截；`Box → Noise → Promote → Remap`（验证 `[RunOver]` 图元层求值、同名读写、节点与图级失败时 `output()==nullptr` 与 `last_valid_output()` 保旧、下游根因错误传播及恢复后缓存命中）。
- **参数化基础图元、圆弧截面与对齐契约**：`Grid`（`quads`/`triangles`/`points` 与三参考平面及 `vertex uv`）、`Box`（共享棱角三轴细分闭合壳与 `base_on_ground`）、`Tube`（实心圆柱持久拓扑、尖顶圆锥、中空圆管与带径向切面封盖的扇形拱）、`Wedge`（双坡屋顶与斜坡闭合体积）、`Circle`（整圆凸多边形、大于 180° 的凸三角扇及 `open_arc → Sweep` 石拱流形）、`Torus`（闭合圆环与带盖 90° 弯管解析多面体体积）、`Transform`（角度制欧拉旋转 + 局部 `pivot`）与 `MatchSize`（默认 XZ 居中贴地及 `reference` 等比适配）。

图文件 `load_graph` / `save_graph` 对 C++ 与 `.source` 节点的规范往返、缺失类型/失效连接宽容加载（含多输入端口剪线/删节点的保留项同步与撤销），以及编辑器 `.procgraph` 资产、SOP 上下文切换、二维 Network 节点连线画布（`graph.layout`、三段式连线、外置磁吸端口、滚轮缩放、框选多选拖动、`Y` 划断连线、`Tab` 搜索创建并自动连线）、重导入网格删除后的 SOP 快照失效与 `procedural.bake` 拦截、Display 与选中节点解耦求值、`String`/标量参数撤销缓存命中、Geometry Spreadsheet 分页过滤与视口表面/辅助线生成由 `editor_document_acceptance`（[acceptance/editor/document.cpp](../../../acceptance/editor/document.cpp)）覆盖；跨模块工程 Source 消费由 `project_integration_acceptance` 覆盖。

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset release --target procedural_acceptance
```

## 测量

`atelier_procedural_acceptance --profile-procedural <新的绝对路径.json>` 只输出数值，不属于验收。它在一块 32768 个三角形的起伏地形上构建典型生成图：MeshImport → Scatter（约 5 万点），Grid（224 × 224）→ Transform 抬高 → Ray 投射回地形，两路 Merge 后 CopyToPoints 得到约 10 万实例，再按 32 m 分块发布。报告各项 9 次的中位数（先预热 2 次）：100 万点的无间距与带正 `min_distance` 撒点、变换与自有语言 SOP 内核（`Jitter`、`RandomScale`、`Noise`、`Remap`），地形 BVH 的构建与 224 × 224 条竖直射线的查询，冷启动整图求值与各节点耗时，无变化的求值，改 Scatter 种子、改 Ray 距离（复用 `Bvh::acquire` 缓存）、改 CopyToPoints 变体属性名之后的重算，撤销到已缓存的种子，以及首次发布与逐块复用的再发布。测量时机器应空闲。

同一入口另有两档闭合 Volume 负载：**Sphere(r=5,M=32,L=32)** 的 1,984 扇形三角，以及 **M128/L128** 的 32,512 三角；两者都为 1,000 格、10,000 筛选前候选（origin=0/h=1/density=10/seed=3）。小档使用 `volume_*` 键，大档使用 `volume_large_*`；报告输入参数、构建配置、预热/样本数、格数、C、T、实际保留点数和各方向 attempt/retry/fallback 计数，固定输入各轮计数须一致。`retry_rate` 为首方向重试数/C，表面排除的候选仍计入 C。

正式报告给 `ms`（整次）、`build_ms`（仅建 BVH）、`generate_classify_ms`（融合生成／分类 fork→join 及计数合并）和 ns/候选；`remaining_ms` 先逐样本算整次−build−classify，再取中位数，包含壳准备、前缀、分配、稳定填充及列装配。旧串行版的 classify 包含输出 append，不能拿其历史数字直接算本片并行收益；并行收益须比较同一新内核的真 P1 与默认 P 未插桩版本。三个子步骤没有产品 marker；只允许另行批准的临时真 P1 诊断细分，并报告插桩扰动，不把各 worker 重叠时长相加冒充墙钟。

计时复用 [模块阶段标记](../../../modules/engine/procedural/include/procedural/timing.hpp) 与 foundation Recorder，由 profile 宿主安装并绑定。worker 仅累计普通整数，host 在 classify 仍打开、所有任务 join 后批量提交零时长事件；不逐点读时钟，无 Recorder 求值不读时钟，没有新增观察接口。采样只能在另行批准的独占时段执行，不得把复杂度或预算上限当实测性能。
