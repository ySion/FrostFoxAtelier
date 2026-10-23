# Procedural：几何与程序化图

静态库 `atelier_procedural`（[cmake/procedural.cmake](../../../cmake/procedural.cmake)）依赖 runtime 与 geometry，不依赖 World、presentation 或编辑器。入口头文件：

| 头文件 | 内容 |
| --- | --- |
| [geometry.hpp](../../../src/procedural/geometry.hpp) | `Geometry` 值、层、属性列、拓扑与实例 |
| [operations.hpp](../../../src/procedural/operations.hpp) | 导入、生成、撒点、变换、Promote、Copy to Points、包围盒 |
| [instances.hpp](../../../src/procedural/instances.hpp) | 把 instance 层发布为渲染用的实例集合 |
| [bvh.hpp](../../../src/procedural/bvh.hpp) | 三角形 BVH：射线与最近点查询 |
| [graph.hpp](../../../src/procedural/graph.hpp) | `Graph`、`Node`、端口、连接与编辑入口，类型注册 |
| [nodes.hpp](../../../src/procedural/nodes.hpp) | C++ 内置节点 |
| [evaluator.hpp](../../../src/procedural/evaluator.hpp) | 增量求值与结果缓存 |
| [random.hpp](../../../src/procedural/random.hpp) | 固定算法的整数哈希与单位随机数 |

方向、分阶段计划与跨模块约定见[程序化节点图方案](../../research/procedural-graph-plan-20261022.md)。生成器组件、渲染器实例化、图文件格式、自有语言节点和网络编辑器尚未实施。

## Geometry

`procedural.Geometry` 是 Runtime 记录类型（`ASTRUCT`，没有反射字段），作为节点端口按引用传递。它是一个值：复制共享全部列，写某一列之前先把这一列复制成独占的。独占判断读引用计数，所以并行写入之前要在单线程上完成复制。

- **五层**：point、vertex（图元的角）、primitive、detail（恰好一个元素）、instance。每层是“名字 → 列”，名字在整个 Geometry 内唯一，必须是标识符。点位置是 point 层的 `P`，总是存在且是第一列。
- **列**：`Array<T>`，T 为 bool、int32、uint32、float、Vector2/3/4、Quaternion、Matrix4；String 只能放在 detail 层。`read` 找不到时返回 null，层或类型不符是错误；`write` 取得可写列；`add` 不存在时按填充值新建；`set` 接管一整列；`remove` 删除。
- **用途**：Vector3 列（以及 Vector4 颜色）带用途 point / vector / normal / color / none，决定变换方式。新列按约定名取用途：P → point，N → normal，v、up → vector，Cd → color，其余 none。
- **拓扑**：`vertex_points` 给出每个顶点引用的点；图元是首顶点的 CSR 偏移，每个图元至少三个顶点，全部图元边数相同时不存偏移。拓扑只能经 `polygons`、`remove_flagged`、`merge`、`reverse_primitives` 等 C++ 入口改写。
- **实例**：去重的资源引用表 `references`（持久键 + 宿主解析出的 `MeshRef`），加上 instance 层的保留列：每个实例的引用下标与有限仿射 Matrix4。`set_instances` 替换整个 instance 层并丢弃它的属性。
- `bytes()` 估算列与拓扑保留的内存（按容量计），共享的列按全额计算，供缓存预算使用。

## 操作

- **`import_mesh`**：每个三角形一个图元，法线和 UV 写入 vertex 层的 `N`、`uv`，材质槽号写入 primitive 层的 `material`，位置逐位相等的顶点焊接成同一个点。
- **`grid` / `line`**：XZ 平面点阵（N = +Y，以原点为中心）和含两端的等距点。
- **`scatter`**：只接受三角形图元。三角形 t 的点数与位置只取决于 `hash(seed, t)`：点数为面积 × 密度，小数部分按概率进位，重心坐标均匀采样。所以结果与线程和遍历顺序无关，改动一个三角形不影响其他三角形上的点。可选最小间距按点序淘汰过近的点，间距小到点的格坐标超出 ±2⁶² 时失败。输出 P、N（面法线）、`id`、`sourceprim`、`sourceordinal`、`sourceprimuv`。`id` 是 32 位哈希随机种子，不是身份，不保证唯一；（`sourceprim`，`sourceordinal`）唯一且局部稳定。
- **`transform`**：v · S · R · T。point 列完整变换，vector 列只用线性部分 L，normal 列乘 sign(det L) · cofactor(L) 后归一化（零向量保持为零，所以压扁到平面的奇异变换仍有平面法线），`orient` 列复合旋转，实例矩阵右乘，color 与 none 列不变。行列式为负时反转每个图元的顶点顺序，vertex 列随之重排。输入或结果出现非有限值时失败，几何已被部分改写。
- **`promote`**：把属性移到另一层，按 first / sum / average 合成每个目标元素接触到的源元素；sum 与 average 只接受 float 与 float 向量；instance 层只和 detail 交换；P 不能移动。
- **`remove_flagged`**：删除 bool 列为 true 的元素；删除点时一并删除用到它们的图元，删除图元时保留点。
- **`merge`**：按输入顺序拼接各层，属性按名字合并，缺失的元素取类型默认值，detail 取第一个有值的输入；同名属性的层、类型或用途不一致时失败。
- **`copy_to_points`**：每个点一个实例，矩阵为 S · O · T：S = scale × pscale，O 是归一化的 `orient`，没有 orient 时由 N 和 up 对齐（只有 N 时取 +Z 到 N 的最短旋转），T 取自 P。点上的 uint 属性（默认 `variant`，缺失时为 0）选择引用。detail 属性带到输出，输出没有点。非有限输入和越界的变体失败。
- **`bounds`**：点的包围盒，空几何没有。
- **`Bvh`**：对三角形图元建立的包围体层次（分箱 SAH，深层退回中位数切分，深度有界），建立时复制三角形，零面积三角形不参与。`ray` 沿单位方向找 max_distance 内最近的交点，三角形双面有效；`closest` 找 max_distance 内最近的表面点。命中给出图元号、距离、重心坐标（与撒点的 `sourceprimuv` 同一约定）和位置。三角形求交和最近点在 double 中计算，包围盒测试留在 float（溢出的距离就是无穷远），所以有限坐标不会因中间结果溢出得到非有限或超出距离限制的命中，平行于某一轴的射线不受零的符号影响。射线恰好穿过共享边时可能两侧都不命中。

## 实例发布

`publish(geometry, chunk_size, previous)` 把 instance 层按（引用，块）分组：块是生成器局部空间中平移所在的立方体 floor(t / chunk_size)。每组是一个 `Collection`，带一份共享不可变的 3×4 仿射数组（行主序、行向量约定，x、y、z 轴行加平移行），实例按源顺序排列，数组从不为空。

- 线性部分不能通过 `math::normal_transform` 的实例（零缩放、压扁）被丢弃并计入 `dropped`。这个判据与渲染器对 world 矩阵的判据是同一个函数，所以发布的内容不会让渲染器拒绝整帧。
- 与 `previous` 中同一（引用，块）的集合逐字节相同时，复用旧的数组指针，以指针为键的消费者因此保留自己的状态。
- 块大小必须有限且为正，块下标必须在 int32 范围内。

## 程序化图

`procedural::register_types(Runtime&)` 发布 Layer、Reduce、Geometry、`procedural.Geometries`（`Array<Geometry>`）、Context、Node、Graph 和内置节点；String 与 `Array<String>` 不存在时一并发布。重复调用不做任何事。

- **Graph**（`procedural.Graph`）：公开字段 `seed`；隐藏的节点强引用序列和 display 强引用槽。
- **Node**（`procedural.Node`）：节点类型的公开字段就是参数；隐藏的 `inputs` 强引用序列每个连接一项，按输入端口排序，与之平行的非反射 C++ 数组记录每个连接的（输入端口，上游输出号）。
- **端口**：`ports(runtime, type)` 从 const 同步方法 `evaluate` 的签名按参数顺序读出端口：`const procedural.Context&` 是上下文，`const procedural.Geometry&` 是输入端口，`const procedural.Geometries&` 是可接多个连接的输入端口，`procedural.Geometry&` 是输出端口。至少要有一个输出；其他参数形式使类型不能作为节点。端口目前按序号标识，端口名等语言能力 L5 提供参数名后再加入。
- **Context**（`procedural.Context`）：公开字段 `seed` 是图的种子；`mesh(key)` 经宿主的 `Resources` 解析网格，并记为当前节点结果的依赖。
- **编辑**：`add_node`（类型必须能读出端口）、`remove_node`（同时断开它的下游连接，它是 display 时清除 display）、`connect`（两端都在图内，端口存在，不成环；单输入端口替换已有连接，多输入端口追加到末尾）、`disconnect`、`set_display`（null 清除）。每个入口只写它改变的对象：节点列表和 display 写图，连接写下游节点。读取用 `contents` 和 `inputs`，都是 const 借用。图持有节点、节点持有上游，被删除的节点由别的根（例如编辑历史）保活。

内置节点：

| 节点 | 参数 | 端口 |
| --- | --- | --- |
| MeshImport | mesh | context → output |
| Grid | size、rows、columns | output |
| Line | start、end、count | output |
| Scatter | density、seed、min_distance；节点种子为 hash(图种子, seed) | context, surface → points |
| Transform | translate、rotate、scale | input → output |
| Promote | name、to、reduce | input → output |
| Remove | flag、layer | input → output |
| Merge | — | inputs（多个）→ output |
| Ray | mode（project / minimum）、direction、max_distance、normal、remove_misses | points, surface → output |
| CopyToPoints | meshes（资源键列表）、variant | context, points → output |

Ray 对 surface 的三角形建立 BVH，命中的点移到交点（minimum 模式为最近的表面点），写入 `hitprim`（int32）、`hitprimuv`、`hitdist`；未命中的点保持原位，`hitprim` 与 `hitdist` 为 -1。`normal` 打开时命中点的 N 取命中三角形按顶点顺序的法线，`remove_misses` 删除未命中的点。每次求值都重新建立 BVH。

节点失败时抛出异常，Runtime 把它转成 `invocation_failed`，消息就是失败原因。未连接的单输入端口收到空几何。

## 求值器

`Evaluator(runtime, graph, resources, cache_bytes)` 在 Runtime 线程上同步求值 display 节点及其上游，`evaluate()` 之后 `output()` 是 display 节点的第一个输出，`status(node)` 给出每个节点的错误、求值次数、缓存命中次数和最近一次 evaluate 的耗时。

- **发现变化**：先取 `change_cursor()`，再读取。图在游标之后被写过时重读节点列表、display 和种子；节点是新的或被写过时，重读它的参数和连接，算出本地键。本地键和连接都没变的写入（例如什么也没改的可写借用）不触发求值；什么都没变、资源版本也没变时直接返回。求值器只用 const 借用、`read` 和 const 接收者的 `evaluate`，不给图和节点盖章。
- **键**：节点的键是精确的字节串：类型（已发布 `TypeInfo` 的地址）、每个公开字段的内容、带上下文的节点再加图种子，以及每个连接的（输入端口，上游输出号，上游结果序号）。每次真正执行 evaluate 得到一个新的结果序号，所以上游结果变了下游的键就变；内容相同的两个节点共享同一个缓存结果。参数可以是标量、枚举、数学值和普通记录、String，以及这些类型的 Array，其他字段类型使节点失败。键只在本进程、本 Runtime 内有效，不能持久化。
- **缓存**：按键查找结果，以条目为单位按最近使用淘汰，条目大小为输出的 `bytes()` 加两份键（条目与查找表各一份）、依赖表和条目开销，总量不超过 `cache_bytes`（默认 256 MiB）。撤销把参数改回旧值时直接命中旧结果，不再求值；节点持有的当前结果不受淘汰影响。
- **资源**：节点经 Context 读到的网格连同解析结果记在结果上。`Resources::revision()` 变化后，复用结果之前重新解析这些键；解析结果不同就重新求值，相同就继续复用。
- **失败**：节点失败时它的下游不求值，状态记为输入失败，`output()` 保留上一次的结果。失败的键在资源版本不变时不再重试，键变化（例如参数改回）或资源版本变化后再试。
- **调度**：每个需要的节点每次求值最多调用一次 evaluate，按上游在前的顺序执行。环在建立连接时就被拒绝，求值时遇到环或指向图外的连接返回错误。

## 验收

`procedural_acceptance`（无窗口，[acceptance/procedural/main.cpp](../../../acceptance/procedural/main.cpp)）覆盖：撒点的精确点数、确定性、三角形之间互不影响、概率取整和最小间距；按用途变换（逆转置法线、奇异变换下的平面法线、镜像时图元反转与 vertex 列重排）；删除、合并冲突、Promote、写时复制；Copy to Points 的 S·O·T 与 N/up 对齐、NaN 与越界变体；发布的分块、丢弃与逐块复用。BVH 在 3200 个三角形的起伏高度场上，用向下、向上、近水平和受距离限制的射线以及最近点查询，与按另一套公式写成的 double 暴力解比较命中、距离和命中点；同一叶中叠放的两个三角形从两侧都取到较近的一个。Ray 节点核对最近的抬高三角形、重心坐标、命中法线、背后和范围外的未命中、删除未命中点，以及 minimum 模式取到比正下方更近的角点。图与求值器部分构建 Grid → Transform → Merge ← Line → CopyToPoints，与直接调用操作的结果逐字节比较，并核对每个节点的求值与命中次数：

- 首次求值每个节点一次，再次求值零次，求值不给任何对象盖章；
- 经 `Runtime::write` 和 `with_native` 改参数只重算该节点及下游，另一分支不动；撤销后全部命中缓存；什么也没改的可写借用不触发求值；图种子只重算带上下文的节点；
- 节点失败阻断下游并保留输出，同键不重试，参数改回后命中缓存；
- 资源版本变化但解析结果相同不重算，换了网格只重算读取它的节点，缺失的网格出现后重试；
- 内容相同的两个节点共享结果；成环、端口越界、图外节点被拒绝；单输入端口替换连接；删除节点断开下游并清除 display；缓存预算为 0 时撤销也要重算。

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset release --target procedural_acceptance
```

## 测量

`atelier_procedural_acceptance --profile-procedural <新的绝对路径.json>` 只输出数值，不属于验收。它在一块 32768 个三角形的起伏地形上构建典型生成图：MeshImport → Scatter（约 5 万点），Grid（224 × 224）→ Transform 抬高 → Ray 投射回地形，两路 Merge 后 CopyToPoints 得到约 10 万实例，再按 32 m 分块发布。报告各项 9 次的中位数（先预热 2 次）：100 万点的撒点与变换，地形 BVH 的构建与 224 × 224 条竖直射线的查询，冷启动整图求值与各节点耗时，无变化的求值，改 Scatter 种子、改 Ray 距离、改 CopyToPoints 变体属性名之后的重算，撤销到已缓存的种子，以及首次发布与逐块复用的再发布。测量时机器应空闲。
