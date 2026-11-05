# Procedural：几何与程序化图

静态库 `atelier_procedural`（[cmake/procedural.cmake](../../../cmake/procedural.cmake)）依赖 runtime、geometry 与 source，不依赖 World、presentation 或编辑器。入口头文件：

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

引擎附带的自有语言节点与随机库在 [assets/engine/procedural](../../../assets/engine/procedural)。

本文只说明已实现能力。方向、设计依据与未实施的跨模块约定见[后续路线与未决契约](roadmap.md)。场景生成器组件、渲染器实例化、网络编辑器、持久 Cache、冻结与 Bake 尚未实施；Remap／属性噪声的目标列选择方向已定，统一字段绑定尚未实现，见 roadmap 的 S0 合同。

## Geometry

`procedural.Geometry` 是 Runtime 记录类型（`ASTRUCT`，没有反射字段），作为节点端口按引用传递。它是一个值：用 [SharedArray](../../foundation.md#共享不可变数组) 共享属性、拓扑和实例列，写入共享列时才复制成独占；已有独占列原地写。每个写入者必须独占自己的 Geometry，但无需先在单线程上复制列。这个存储能力不改变求值器在 Runtime 线程上串行执行的规则。

- **五层**：point、vertex（图元的角）、primitive、detail（恰好一个元素）、instance。每层是“名字 → 列”，名字在整个 Geometry 内唯一，必须是标识符。点位置是 point 层的 `P`，总是存在且是第一列。
- **列**：`SharedArray<T>` 持有 `Array<T>`，T 为 bool、int32、uint32、float、Vector2/3/4、Quaternion、Matrix4；String 只能放在 detail 层。`attributes()` 和 `find()` 暴露的共享列只能读取；`read` 借出 const Array，找不到时返回 null，层或类型不符是错误；`write` 先取得独占列再借出可写 Array；`add` 不存在时按填充值新建；`set` 接管一个共享列句柄；`remove` 删除。SharedArray 本身不注册为 Runtime 类型，源码借用的仍是原来的 Array 类型。
- **借用**：借用结束前，不移动、赋值或结构修改该 Geometry，也不再次 `write` 同一列；可写借用期间还不能复制该 Geometry 或出借列的句柄，只读借用允许复制。需要多列时先完成增列和独占准备，再取供计算使用的引用；Source 求值器和 Ray 节点都按这个顺序执行。
- **用途**：Vector3 列（以及 Vector4 颜色）带用途 point / vector / normal / color / none，决定变换方式。新列按约定名取用途：P → point，N → normal，v、up → vector，Cd → color，其余 none。
- **拓扑**：`vertex_points` 给出每个顶点引用的点；图元是首顶点的 CSR 偏移，全部图元角数相同时不存偏移。独立保留 COW 列 `kind` 区分 polygon（至少三角、隐式闭合）与 polyline（至少两角、只连接相邻角、不隐式闭合），不能从角数推断，也不能经属性绑定改写。`polygons` 明确生成面，`primitives` 接收每图元的 kind；`primitive(p)` 同时返回角范围和 kind。删除、合并保留 kind，`reverse_primitives()` 显式反转两类图元及 vertex 属性；镜像变换只纠正面的绕序。
- **实例**：去重的资源引用表 `references`（持久键 + 宿主解析出的 `MeshRef`），加上 instance 层的保留列：每个实例的引用下标与有限仿射 Matrix4。`set_instances` 替换整个 instance 层并丢弃它的属性。
- `bytes()` 估算列与拓扑保留的内存（按容量计），共享的列按全额计算，供缓存预算使用。

## 操作

- **`import_mesh`**：每个三角形一个图元，法线和 UV 写入 vertex 层的 `N`、`uv`，材质槽号写入 primitive 层的 `material`，位置逐位相等的顶点焊接成同一个点。
- **`grid` / `line`**：XZ 平面点阵（N = +Y，以原点为中心）和含两端的等距点。
- **`scatter`**：接受三角形及凸平面多边形，不接受折线；按下述公共首角扇形视图采样。原图元 p、局部子三角 j 的随机源 s 在 j=0 时为 `hash(seed,p)`，j>0 时为 `hash(hash(seed,p),j)`；点数为面积 × 密度，以 `hash(s,0)` 按小数部分概率进位，第 k 个点使用 `hash(s,2*k+1)` 与 `hash(s,2*k+2)` 均匀采样重心坐标。输出按 p/j/k 顺序，k 在每个子三角内从零开始，不把展开下标或线程号混入随机源。可选最小间距在全部候选上按这个顺序稳定淘汰，所有列一起压紧；格坐标超出 ±2⁶² 时失败。输出 P、N、`id`、`sourceprim`、`sourceordinal`、`sourceprimuv`；仅输入包含超过三角的面时在末尾追加 uint `sourcetri`（其他三角面的值为 0）。纯三角输入的六列顺序、算术与值不变。`id` 在 j=0 时为 `hash(p,k)`，否则为 `hash(hash(p,j),k)`，它只是非唯一随机种子；（p,j,k）是局部稳定来源。`sourceprimuv` 是子三角 j 内的 (b1,b2)，不是原多边形坐标。
- **`transform`**：v · S · R · T。point 列完整变换，vector 列只用线性部分 L，normal 列乘 sign(det L) · cofactor(L) 后归一化（零向量保持为零，所以压扁到平面的奇异变换仍有平面法线），`orient` 列复合旋转，实例矩阵右乘，color 与 none 列不变。行列式为负时只反转 polygon 的顶点顺序及 vertex 列，polyline 的方向不变。输入或结果出现非有限值时失败，几何已被部分改写。
- **`promote`**：把属性移到另一层，按 first / sum / average 合成每个目标元素接触到的源元素；sum 与 average 只接受 float 与 float 向量；instance 层只和 detail 交换；P 不能移动。
- **`remove_flagged`**：删除 bool 列为 true 的元素；删除点时一并删除用到它们的图元，删除图元时保留点。
- **`merge`**：按输入顺序拼接各层，属性按名字合并，缺失的元素取类型默认值，detail 取第一个有值的输入；同名属性的层、类型或用途不一致时失败。
- **`copy_to_points`**：每个点一个实例，矩阵为 S · O · T：S = scale × pscale，O 是归一化的 `orient`，没有 orient 时由 N 和 up 对齐（只有 N 时取 +Z 到 N 的最短旋转），T 取自 P。点上的 uint 属性（默认 `variant`，缺失时为 0）选择引用。detail 属性带到输出，输出没有点。非有限输入和越界的变体失败。
- **`bounds`**：点的包围盒，空几何没有。
- **`Bvh`**：对公共扇形视图建立的包围体层次（分箱 SAH，深层退回中位数切分，深度有界），建立时复制三角形，零面积三角形不参与。`ray` 沿单位方向找 max_distance 内最近的交点，三角形双面有效；`closest` 找 max_distance 内最近的表面点。Hit 给出原图元号 `primitive`、局部子三角号 `triangle`、距离、子三角内重心坐标和位置。三角形求交和最近点在 double 中计算，包围盒测试留在 float（溢出的距离就是无穷远），所以有限坐标不会因中间结果溢出得到非有限或超出距离限制的命中，平行于某一轴的射线不受零的符号影响。射线恰好穿过共享边时可能两侧都不命中。

### 公共凸面剖分

模块内部 [triangles.cpp](../../../src/procedural/triangles.cpp) 是 Scatter、BVH/Ray 共用的数值判据与首角扇形视图，不生成第二份拓扑；借用期间 Geometry 必须保持不变且存活。图元 p 的第 j 个三角取原角 `(first, first+j+1, first+j+2)`，顺序为 p 再 j；折线明确失败。三角输入保留原先零面积跳过规则，多边形则必须有可分辨的平面、简单凸边界及非零边。

所有几何判据用 double 计算。距离容差 δ = 8 × float ε × max（最大绝对坐标分量，距首点的最大距离），没有固定米制下限；8 是为 float 输入和变换舍入预留的安全系数，不是容许任意翘曲的建模参数。首角扇形的有向面积向量长度必须大于 δ × 尺度，点到所得平面的距离不超过 δ，投影边长须大于 δ。采用保守凸性规则：投影相邻边的有向叉积必须非负，即使负值来自舍入也拒绝，不用距离容差放过凹角；正向共线角仍有效，允许零面积扇形子三角。接近折返的角（点积非正、叉积不超过 δ × 两邻边长度之和）也拒绝。在此非负转角前提下，转角总和必须为 2π（double 累加容差 64 × ε × 角数），既排除多圈星形，也不让正反小回环抵消成一圈。检查为线性时间，不另建边对相交矩阵。适用于细节显著大于其绝对坐标 float 精度的模型；远离原点或极薄的不可分辨面报错，不静默修补。后续构造与拓扑工具复用此判据，不另定容差。

## 实例发布

`publish(geometry, chunk_size, previous)` 把 instance 层按（引用，块）分组：块是生成器局部空间中平移所在的立方体 floor(t / chunk_size)。每组是一个 `Collection`，带一份共享不可变的 3×4 仿射数组（行主序、行向量约定，x、y、z 轴行加平移行），实例按源顺序排列，数组从不为空。

- 发布接口仍使用 `shared_ptr<const vector<Affine>>`，不随内部 Geometry 列迁移成 SharedArray。
- 线性部分不能通过 `math::normal_transform` 的实例（零缩放、压扁）被丢弃并计入 `dropped`。这个判据与渲染器对 world 矩阵的判据是同一个函数，所以发布的内容不会让渲染器拒绝整帧。
- 与 `previous` 中同一（引用，块）的集合逐字节相同时，复用旧的数组指针，以指针为键的消费者因此保留自己的状态。
- 块大小必须有限且为正，块下标必须在 int32 范围内。

## 程序化图

`procedural::register_types(Runtime&)` 发布 Layer、Reduce、Geometry、`procedural.Geometries`（`Array<Geometry>`）、Context、Node、PointNode、Graph 和内置节点；String、`Array<String>` 和除 String 外每种列类型的 Array（`procedural.Bools`、`Ints`、`Uints`、`Floats`、`Vector2s`、`Vector3s`、`Vector4s`、`Quaternions`、`Matrix4s`）不存在时一并发布，源码节点借用的列就是这些类型。重复调用不做任何事。

- **Graph**（`procedural.Graph`）：公开字段 `seed`；隐藏的节点强引用序列和 display 强引用槽。
- **Node**（`procedural.Node`）：节点类型的公开字段就是参数；隐藏的 `inputs` 强引用序列每个连接一项，按输入端口排序，与之平行的非反射 C++ 数组记录每个连接的（输入端口，上游输出号）。
- **端口**：`ports(runtime, type)` 从 const 同步方法 `evaluate` 的签名按参数顺序读出端口：`const procedural.Context&` 是上下文，`const procedural.Geometry&` 是输入端口，`const procedural.Geometries&` 是可接多个连接的输入端口，`procedural.Geometry&` 是输出端口。至少要有一个输出；其他参数形式使类型不能作为节点。端口名就是 `evaluate` 的参数名（`MethodInfo::parameter_names`），每个发布版本重新读取；输入、输出端口的参数没有名字时类型不能作为节点，上下文参数可以不命名。图中的连接按端口序号存储，所以兼容更新只改参数名时现有连接仍然有效。PointNode 的子类型端口固定为 `input` → `output`，读端口不需要 Program；PointNode 本身不能作为节点。
- **Context**（`procedural.Context`）：字段 `seed` 是图的种子，C++ 节点用 `seed()` 读取；`mesh(key)` 经宿主的 `Resources` 解析网格，并记为当前节点结果的依赖。全部成员同一访问级别，类型保持标准布局，所以 `seed` 有生成器给出的偏移，源码按偏移直接读 `context.seed`；`mesh` 不是 Runtime 方法，源码不能调用。
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

Ray 对 surface 的公共凸面扇形建立 BVH，命中的点移到交点（minimum 模式为最近的表面点），写入 `hitprim`、`hittri`（都是 int32）、`hitprimuv`、`hitdist`；未命中的点保持原位，两个编号与距离都为 -1。`hitprimuv` 是局部子三角 `hittri` 内的 (b1,b2)，不是原多边形坐标。`normal` 打开时 N 取命中子三角的三个原角计算的法线，不按 `3*hitprim` 寻址；`remove_misses` 删除未命中的点。每次求值都重新建立 BVH。

节点失败时抛出异常，Runtime 把它转成 `invocation_failed`，消息就是失败原因。未连接的单输入端口收到空几何。

### 自有语言节点

自有语言的属性节点（类似 Houdini 的 Wrangle）是派生自 `procedural.PointNode` 的源码 class，参数是 class 的字段，算法是同步方法 `void evaluate(...)`。求值器从 Program 取入口 `<类名>.evaluate`，按参数名把参数绑定到输入几何的点属性列：

- `const procedural.Context&`（最多一个）是上下文；
- `Array<T>& name` 是输出列：输入有 point 层的同名同类型列就写它的独占副本，没有就按 T 的默认值（零，四元数为单位旋转）新建，新列按约定名取用途，所以用途与类型不符（例如 `Array<float>& N`）时失败；
- `const Array<T>& name` 是输入列，借用共享的列本身，不复制；没有这一列时失败。
- 其他参数形式、没有参数、没有 `evaluate` 方法、带结果或不是同步方法的类型，求值时失败（`invalid_definition`），它的下游被阻断，其他节点照常求值。至少借用上下文或一列，`evaluate` 才是同步值区间，编译器在值区间里拒绝写 `self`。

输出就是输入几何加上改写过的列。借用的列不能改变长度，所以点数与输入相同；改变点数的操作仍由 C++ 节点完成。`evaluate` 读 `self` 的字段（每次读取都是一次 Runtime 调用，循环外先存到局部），不写 `self`，也不给节点盖章。调用在 Runtime 线程上串行执行。

[assets/engine/procedural](../../../assets/engine/procedural) 里的文件是引擎源码模块，`ProjectCode::snapshot` 把它们并进每个工程快照（编辑器和 Play 同一处），快照路径带 `engine/` 前缀；`ProjectCode` 第一次构建时在工程的 Runtime 中注册本模块的原生类型，因为这些文件里的类派生自 `procedural.PointNode`。这里只能声明 `procedural.` 下的名字，工程文件不能在 `procedural.` 下声明；函数声明和调用都写全名，工程源码也可以调用，例如 `procedural.hash(x)`：

| 文件 | 内容 |
| --- | --- |
| random.source | 全局函数 `procedural.hash`、`hash2`、`unit`、`random_vector`，与 [random.hpp](../../../src/procedural/random.hpp) 算法相同，所以源码节点和 C++ 节点取到相同的随机数；`random_vector(x)` 由 x 连续哈希三次，每个分量均匀落在 [-1, 1) |
| orient.source | 全局函数 `procedural.frame_rotation`（三根正交轴构成的旋转，按 `math.rotation_matrix` 的行约定）、`shortest_arc`（+Z 到给定单位向量的最短旋转，正对 −Z 时绕 X 转半圈）和 `align_to_normal`（Copy to Points 从 N 与 up 推出的朝向：+Z 沿 N，+Y 朝 up；up 为零、与 N 平行或不是有限值时取最短旋转；N 为零时不旋转，不是有限值时结果也不是有限值） |
| jitter.source | `procedural.Jitter`：amount、axis_scale（按轴缩放）、seed；P 的每个分量加上 `random_vector(hash2(hash2(图种子, seed), id))` 的对应分量 × amount × axis_scale，读 `id`，所以偏移跟随点 |
| random_scale.source | `procedural.RandomScale`：minimum、maximum、seed；pscale 取 lerp(minimum, maximum, unit(hash2(hash2(图种子, seed), id)))，读 `id` |
| normal_align.source | `procedural.NormalAlign`：up；按 `align_to_normal(N, up)` 写 orient，所以 NormalAlign 之后的 Copy to Points 与直接用 N 和同值 up 列的结果相同，中间可以再改 orient |

参数用源码字段的元数据（Label、DESC、Min/Max、LockMin）声明，Parameters 面板经 `FieldInfo` 读取。

## 求值器

`Evaluator(runtime, graph, resources, program, cache_bytes)` 在 Runtime 线程上同步求值 display 节点及其上游，`evaluate()` 之后 `output()` 是 display 节点的第一个输出，`status(node)` 给出每个节点的错误、求值次数、缓存命中次数及可选耗时 `time`。`evaluate(record_timings = false)` 默认不读计时时钟；传入 `true` 才记录实际执行节点的耗时，包含参数准备及失败执行。`time` 表示该节点最近一次实际执行，而非本轮图求值：从未执行或最近一次执行未开启计时时为空；无变化、缓存命中、失败键抑制和未进入执行的绑定/上游失败均保留原值，不沿缓存继承别的节点耗时。计时开关不进入键、不判脏，也不强制重算。`program` 提供自有语言节点的代码，可以为空，这时自有语言节点失败（`unsupported_operation`）；它要比求值器活得久。

- **发现变化**：先取 `change_cursor()`，再读取。图在游标之后被写过时重读节点列表、display 和种子；节点是新的或被写过时，重读它的参数和连接，算出本地键。本地键和连接都没变的写入（例如什么也没改的可写借用）不触发求值；什么都没变、资源版本和 `Program::revision()` 也没变时直接返回。`Program::revision()` 变了就重新绑定自有语言节点的入口，代码身份没变的节点键也不变，不会重算。求值器只用 const 借用、`read` 和 const 接收者的 `evaluate`，不给图和节点盖章。
- **键**：节点的键是精确的字节串：类型（已发布 `TypeInfo` 的地址）、每个公开字段的内容、带上下文的节点再加图种子，自有语言节点再加入口的代码身份（`EntryInfo::code`：类的方法，或它直接、间接写到的全局函数、类型声明和常量变了才换新值，所以只改 Jitter 的方法体只重算 Jitter 节点和下游；Runtime 新注册值类型时所有 code 都换，节点缓存全部失效），以及每个连接的（输入端口，上游输出号，上游结果序号）。每次真正执行 evaluate 得到一个新的结果序号，所以上游结果变了下游的键就变；内容相同的两个节点共享同一个缓存结果。参数可以是标量、枚举、数学值和普通 plain 记录、String，以及这些类型的 Array，其他字段类型使节点失败。普通记录按公开字段递归编码，不把 padding 字节当作参数；数学值保留紧凑字节表示。键只在本进程、本 Runtime 和本 Program 内有效，不能持久化。
- **缓存**：按键查找结果，以条目为单位按最近使用淘汰，条目大小为输出的 `bytes()` 加两份键（条目与查找表各一份）、依赖表和条目开销，总量不超过 `cache_bytes`（默认 256 MiB）。撤销把参数改回旧值时直接命中旧结果，不再求值；节点持有的当前结果不受淘汰影响。
- **资源**：节点经 Context 读到的网格连同解析结果记在结果上。`Resources::revision()` 变化后，复用结果之前重新解析这些键；解析结果不同就重新求值，相同就继续复用。
- **失败**：节点失败时它的下游不求值，状态记为输入失败，`output()` 保留上一次的结果。失败的键在资源版本不变时不再重试，键变化（例如参数改回）或资源版本变化后再试。
- **调度**：每个需要的节点每次求值最多调用一次 evaluate，按上游在前的顺序执行。环在建立连接时就被拒绝，求值时遇到环或指向图外的连接返回错误。

## 图文件

图文件的编解码在编辑器里（[editor/graph_format.hpp](../../../src/editor/graph_format.hpp)，`atelier_editor_core`），procedural 不依赖 editor。`load_graph(runtime, text)` 把 version 1 的图文件打开到已注册 procedural 类型的 Runtime 中，得到 `GraphDocument`：图的根、每个节点的 `GraphNode`（名字、节点根、画布位置、保留的原始 JSON）、保留的 settings 和加载诊断（`world::Diagnostic`，field 是从文件根算起的路径）。`save_graph(runtime, document)` 写出规范文本。名字和位置是文档数据，不放进节点对象，所以移动节点不盖章；保存只做 const 读取。

```json
{
  "nodes": {
    "grid":   {"inputs": {}, "position": [0.0, 0.0], "properties": {"columns": 4, "rows": 3, "size": {"x": 3.0, "y": 2.0}}, "type": "procedural.Grid"},
    "merged": {"inputs": {"inputs": ["moved.output", "line.output"]}, "position": [400.0, 60.0], "properties": {}, "type": "procedural.Merge"}
  },
  "settings": {"display": "merged", "seed": 7},
  "version": 1
}
```

- **规范形式**：`nodes` 以名字为键，名字符合 `[A-Za-z_][A-Za-z0-9_]*`，`json_format::parse` 拒绝重复键。每个节点写 `type`（Runtime 限定类型名）、`position`、`properties`（全部公开字段，与 Scene 共用 `encode_properties`，所以节点默认值以后改变也不改变旧图的含义）和 `inputs`（总是写出；未连接的端口不出现；单输入端口写 `"上游.输出名"`，多输入端口写数组并保留连接顺序）。`settings` 是图的公开字段加 `display`（节点名，没有 display 时不写）。键按名字排序，`dump(2)` 后加换行，所以未改动的文档再次保存逐字节相同。节点的连接、图的节点列表和 display 是隐藏字段，不出现在属性里。
- **宽容加载**：与 Scene 相同，放不进的内容保留、报告并在保存时写回。类型不存在或不能作为节点（读不出端口）的节点整项保留（`unknown_type`），它的位置仍可移动；已知节点和 settings 中解析不了的参数叶经带 `PropertyReport` 的 `decode_properties` 保留（`mismatch`、`unknown_field`，可无损转换的整数报 `converted`）；端口名或上游节点、输出名解析不了的连接按端口保留（`reference_dropped`），多输入端口中能解析的连接照常建立，原数组整体保留，所以顺序不变；会成环的连接同样保留，按节点名顺序先建立的一侧成立；display 指向没有加载的节点时同样保留。保存时保留的 JSON 覆盖在写出的值上。其他 Runtime 失败（例如节点构造失败）使加载失败，保留原错误码。
- **结构错误**直接失败：版本不是 1、未知键、`nodes` 或 `settings` 不是对象、节点名非法、缺少 `type`、`position` 不是两个有限数、`properties` 不是对象、`inputs` 不是以端口名为键的对象或连接不是 `"节点.输出"` 形式、display 不是节点名。保存时每个图节点必须恰有一个条目，名字合法且不重复，位置有限。
- 保留的值在文档中一直覆盖写出的值；网络编辑器接入时，编辑一个保留了值的参数或端口要先丢弃这份保留，与 Scene 的规则相同；改名或删除节点时，还要改写或删除保留的连接、display 和保留节点里指向它的名字。

验收的宽容用例只保留“缺失节点类型 → null 节点并保留冻结数据”的往返。未知 leaf、连接、settings 和不能作为节点的已知类型等额外宽容仍是上述现行产品行为，待单独审查；缩减验收不改变这些路径，也不引入旧版本兼容加载。

图文件不保存派生的 Geometry 缓冲；kind 与扇形视图不改变 JSON v1，也不修改资源保存格式。

## 验收

`procedural_acceptance`（无窗口，[acceptance/procedural/main.cpp](../../../acceptance/procedural/main.cpp)）保留独立真值和必要生命周期序列，不以被测操作自己的输出生成期望：

- **凸面与折线**：前置四边形后的五边形首角扇形面积为 4、8、3，密度 2 产生 8、16、6 点；固定射线命中 p=1/j=1 的重心、距离 3、N=+Z，旧 `3*p` 寻址会得 +Y。表面副本与三点开放折线合并，镜像仅翻面、显式反转同时反线，删除图元/线中点后的 kind、角索引与 vertex 标签对照手写数组，原值不变。
- **数学与布局**：面积 1、3 的两个三角形按密度 4 分别得到 4、12 点，位置在三角形内且法线为 +Z；一个最小间距样本核对点对距离。非均匀缩放与 Y 轴 90° 旋转的点、向量、法线用解析式，镜像用输入三角形的角顺序与属性作真值。删除、空／混合拓扑合并用固定索引，Promote 的平均值来自源网格顶点；Copy to Points 用手写 S·O·T 矩阵及 N/up 轴，发布分块用已知 floor 结果和 3×4 布局。
- **概率取整**：面积 1、密度 2.25，取固定种子 0…2235。每次只能得到 2 或 3 点，总平均与 2.25 的误差严格小于 .03。在独立均匀随机的模型下，向上取整数 X 服从 Binomial(2236, .25)，接受区间为 492…626，精确二项求和覆盖率约 99.902133%；2236 是达到 99.9% 覆盖的最小种子数。实际固定种子运行是确定性的，不把统计模型称为实测分布或耗时证据。
- **BVH 与 Ray**：18 个高度场三角形超过保留叶的 16 上限，一个斜射线和一个最近点查询各与独立 double 暴力公式比较。Ray 用可解析的抬高三角形核对命中与未命中、覆盖已有结果列但不改旧输入、删除未命中点，以及 minimum 模式的已知最近角点。
- **ABI 与隔离**：plain 记录默认 false/0，经 Runtime 写入 `{true, 3}` 后原生节点产生三点；实例副本只写一个矩阵，原矩阵不变。Source 从 `source::engine_sources` 和保留夹具构建：`hash`／`hash2` 对照离线 lowbias32 常量，`unit(0x80000000)` 为 .5；六点 ColumnBatch 读 P、写 N、新增八列，结果按网格坐标和加法公式核对，旧输入与 Lift 兄弟分支保持各自的解析值。
- **故障与清理**：原生图 Grid → Transform → Merge ← Line → CopyToPoints 先发布已知 14 个实例，Transform 失败后旧发布仍可读，参数恢复后继续求值；Source 写过所有列后在 invoke 内越界，旧发布仍可读，修正后得到新值；删除 display 后清除输出及节点状态。三种操作序列各保留一份，不附加缓存计数、容量观察或拒绝矩阵。

跨模块工程 Source 消费的 K5 验收由 Editor 的 `project_execution.cpp` 负责，此入口不重复。`Geometry::bytes()` 的生产缓存预算接口不随容量观察用例删除。

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset release --target procedural_acceptance
```

`procedural_graph_acceptance`（无窗口，`atelier_editor_document_acceptance --procedural-graph`，[acceptance/editor/procedural_graph.inl](../../../acceptance/editor/procedural_graph.inl)）以手写当前 v1 JSON 为金值，读取 Grid → Transform 的非默认参数、位置、seed、display 与连接，再保存为相同规范字节。只保留 Missing 类型变成 null 节点且冻结 JSON 无损往返，以及构造失败不能吞成缺失类型两条边界。九种严格加载反例和额外宽容矩阵不再作为验收；严格结构规则仍如上，旧版本升级属于外部工具，不在加载器内做兼容迁移。

```powershell
cmake --build --preset release --target procedural_graph_acceptance
```

## 测量

`atelier_procedural_acceptance --profile-procedural <新的绝对路径.json>` 只输出数值，不属于验收。它在一块 32768 个三角形的起伏地形上构建典型生成图：MeshImport → Scatter（约 5 万点），Grid（224 × 224）→ Transform 抬高 → Ray 投射回地形，两路 Merge 后 CopyToPoints 得到约 10 万实例，再按 32 m 分块发布。报告各项 9 次的中位数（先预热 2 次）：100 万点的撒点与变换，地形 BVH 的构建与 224 × 224 条竖直射线的查询，冷启动整图求值与各节点耗时，无变化的求值，改 Scatter 种子、改 Ray 距离、改 CopyToPoints 变体属性名之后的重算，撤销到已缓存的种子，以及首次发布与逐块复用的再发布。测量时机器应空闲。
