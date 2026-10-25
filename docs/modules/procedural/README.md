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

本文只说明已实现能力。方向、设计依据与未实施的跨模块约定见[后续路线与未决契约](roadmap.md)。场景生成器组件、渲染器实例化、网络编辑器、持久 Cache、冻结与 Bake 尚未实施；Remap／属性噪声的目标列选择待用户决定，不计为已完成。

## Geometry

`procedural.Geometry` 是 Runtime 记录类型（`ASTRUCT`，没有反射字段），作为节点端口按引用传递。它是一个值：用 [SharedArray](../../foundation.md#共享不可变数组) 共享属性、拓扑和实例列，写入共享列时才复制成独占；已有独占列原地写。每个写入者必须独占自己的 Geometry，但无需先在单线程上复制列。这个存储能力不改变求值器在 Runtime 线程上串行执行的规则。

- **五层**：point、vertex（图元的角）、primitive、detail（恰好一个元素）、instance。每层是“名字 → 列”，名字在整个 Geometry 内唯一，必须是标识符。点位置是 point 层的 `P`，总是存在且是第一列。
- **列**：`SharedArray<T>` 持有 `Array<T>`，T 为 bool、int32、uint32、float、Vector2/3/4、Quaternion、Matrix4；String 只能放在 detail 层。`attributes()` 和 `find()` 暴露的共享列只能读取；`read` 借出 const Array，找不到时返回 null，层或类型不符是错误；`write` 先取得独占列再借出可写 Array；`add` 不存在时按填充值新建；`set` 接管一个共享列句柄；`remove` 删除。SharedArray 本身不注册为 Runtime 类型，源码借用的仍是原来的 Array 类型。
- **借用**：借用结束前，不移动、赋值或结构修改该 Geometry，也不再次 `write` 同一列；可写借用期间还不能复制该 Geometry 或出借列的句柄，只读借用允许复制。需要多列时先完成增列和独占准备，再取供计算使用的引用；Source 求值器和 Ray 节点都按这个顺序执行。
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

Ray 对 surface 的三角形建立 BVH，命中的点移到交点（minimum 模式为最近的表面点），写入 `hitprim`（int32）、`hitprimuv`、`hitdist`；未命中的点保持原位，`hitprim` 与 `hitdist` 为 -1。`normal` 打开时命中点的 N 取命中三角形按顶点顺序的法线，`remove_misses` 删除未命中的点。每次求值都重新建立 BVH。

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

`Evaluator(runtime, graph, resources, program, cache_bytes)` 在 Runtime 线程上同步求值 display 节点及其上游，`evaluate()` 之后 `output()` 是 display 节点的第一个输出，`status(node)` 给出每个节点的错误、求值次数、缓存命中次数和最近一次 evaluate 的耗时（含参数准备）。`program` 提供自有语言节点的代码，可以为空，这时自有语言节点失败（`unsupported_operation`）；它要比求值器活得久。

- **发现变化**：先取 `change_cursor()`，再读取。图在游标之后被写过时重读节点列表、display 和种子；节点是新的或被写过时，重读它的参数和连接，算出本地键。本地键和连接都没变的写入（例如什么也没改的可写借用）不触发求值；什么都没变、资源版本和 `Program::revision()` 也没变时直接返回。`Program::revision()` 变了就重新绑定自有语言节点的入口，代码身份没变的节点键也不变，不会重算。求值器只用 const 借用、`read` 和 const 接收者的 `evaluate`，不给图和节点盖章。
- **键**：节点的键是精确的字节串：类型（已发布 `TypeInfo` 的地址）、每个公开字段的内容、带上下文的节点再加图种子，自有语言节点再加入口的代码身份（`EntryInfo::code`：类的方法，或它直接、间接写到的全局函数、类型声明和常量变了才换新值，所以只改 Jitter 的方法体只重算 Jitter 节点和下游；Runtime 新注册值类型时所有 code 都换，节点缓存全部失效），以及每个连接的（输入端口，上游输出号，上游结果序号）。每次真正执行 evaluate 得到一个新的结果序号，所以上游结果变了下游的键就变；内容相同的两个节点共享同一个缓存结果。参数可以是标量、枚举、数学值和普通记录、String，以及这些类型的 Array，其他字段类型使节点失败。键只在本进程、本 Runtime 和本 Program 内有效，不能持久化。
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

## 验收

`procedural_acceptance`（无窗口，[acceptance/procedural/main.cpp](../../../acceptance/procedural/main.cpp)）覆盖：撒点的精确点数、确定性、三角形之间互不影响、概率取整和最小间距；按用途变换（逆转置法线、奇异变换下的平面法线、镜像时图元反转与 vertex 列重排）；删除、合并冲突、Promote、写时复制；Copy to Points 的 S·O·T 与 N/up 对齐、NaN 与越界变体；发布的分块、丢弃与逐块复用。BVH 在 3200 个三角形的起伏高度场上，用向下、向上、近水平和受距离限制的射线以及最近点查询，与按另一套公式写成的 double 暴力解比较命中、距离和命中点；同一叶中叠放的两个三角形从两侧都取到较近的一个。Ray 节点核对最近的抬高三角形、重心坐标、命中法线、背后和范围外的未命中、删除未命中点，以及 minimum 模式取到比正下方更近的角点。图与求值器部分构建 Grid → Transform → Merge ← Line → CopyToPoints，与直接调用操作的结果逐字节比较，并核对每个节点的求值与命中次数：

- 首次求值每个节点一次，再次求值零次，求值不给任何对象盖章；
- 经 `Runtime::write` 和 `with_native` 改参数只重算该节点及下游，另一分支不动；撤销后全部命中缓存；什么也没改的可写借用不触发求值；图种子只重算带上下文的节点；
- 节点失败阻断下游并保留输出，同键不重试，参数改回后命中缓存；
- 资源版本变化但解析结果相同不重算，换了网格只重算读取它的节点，缺失的网格出现后重试；
- 内容相同的两个节点共享结果；成环、端口越界、图外节点被拒绝；单输入端口替换连接；删除节点断开下游并清除 display；缓存预算为 0 时撤销也要重算。

列存储的边界验收包括：零图元与三角形／四边形混合拓扑，删除后转为均匀或空拓扑，与空几何合并时的顶点偏移；只修改实例副本的一个矩阵，原值不变且引用列继续共享；零元素与小列的非零填充、detail String 填充、按实际 capacity 计入 `bytes()`，共享副本不减记预算；Ray 再次处理已有结果列时覆盖输出而不改写输入。

自有语言节点部分用工程共用的加载函数 `source::engine_sources` 读取引擎文件，再加一个夹具文件组成项目快照，构建 MeshImport → Scatter → Jitter（两个，amount 不同）/ RandomScale，Grid → 夹具 Lift（不带上下文）和夹具 Shift（只读上下文的种子），五路 Merge，并按文档公式和 random.hpp 的哈希独立算出期望值：

- 源码的 `procedural.hash`、`hash2`（经夹具里的包装函数调用）与 random.hpp 逐位相同，`unit` 精确等于 (x >> 8) · 2⁻²⁴，最大输入得到 1 − 2⁻²⁴ 而不是 1；
- 输出的 P 和 pscale 与期望一致（一个 Jitter 的 axis_scale 为 (1, 0, 2)），与 Jitter 同读 Scatter 结果的 RandomScale 看到未改动的 P（Jitter 写的是副本）；求值不盖章；
- 经 `Runtime::write` 和源码方法写 `self` 改参数，都只重算该节点及下游，撤销后命中缓存；图种子经 Context 到达 Jitter、RandomScale 和 Shift（Shift 的输入与种子无关，所以重算只能来自它自己的键），不重算不带上下文的 Lift；
- 兼容发布：改 Jitter 方法体只重算两个 Jitter 节点和下游，输出用上新方法体；内容不变的发布不重算任何节点；只改 RandomScale 的空白只重算它和下游；改引擎助手只重算代码直接或间接调用了它的节点（改 `random_vector` 只重算 Jitter，改 `hash` 再加上 RandomScale，Lift 和 Shift 都不重算），输出不变；
- 立方体上撒点 → 夹具把偶数点的 N 置零 → NormalAlign → CopyToPoints，up 依次取 +Y、+Z、−Y 和零：实例矩阵与 C++ CopyToPoints 直接使用 N 和同值 up 列的结果一致。立方体的面法线沿坐标轴，这几个 up 覆盖四元数的四个分支、平行时的最短旋转和正对 −Z 的半圈；改 up 只重算 NormalAlign 和下游；
- 输入缺少读取的列、按值参数、列类型与已有列不符、没有 evaluate、evaluate 没有参数（因而可以写 `self`）的类型各自失败，只阻断下游；没有 Program 的求值器里自有语言节点失败，C++ 节点照常求值。
- 六点输入的多列夹具读取 P、改写 N 并新增八列，逐元素核对结果及旧输入／兄弟分支隔离；增列之后发现缺列或类型不符时准备失败；写过所有列后在真正的 invoke 内触发数组越界时保留旧发布、同键不重试，参数修正后重新执行并发布新值。

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset release --target procedural_acceptance
```

`procedural_graph_acceptance`（无窗口，`atelier_editor_document_acceptance --procedural-graph`，[acceptance/editor/procedural_graph.inl](../../../acceptance/editor/procedural_graph.inl)）用同一张图核对图文件：保存不盖章且文本是规范 JSON；连接按端口名和输出名、按连接顺序写出，参数含向量、四元数、枚举、String 和 `Array<String>`，隐藏的连接列表不进属性；在另一个 Runtime 中重开，求值输出逐字节相同，再次保存字节相同，名字和位置不变；加入未知节点、指向它的多输入连接、不存在的端口、类型不符和未知的参数叶后宽容加载，每项恰好报告一次，能解析的连接照常建立，保存结果与改过的文本逐字节相同，保留的节点移动后保存新位置；settings 中不符的种子和指向未加载节点的 display 保留并写回；会成环的连接保留并写回，构造失败的节点使加载失败；九种结构错误被拒绝；重名或缺少条目的文档不能保存。

```powershell
cmake --build --preset release --target procedural_graph_acceptance
```

## 测量

`atelier_procedural_acceptance --profile-procedural <新的绝对路径.json>` 只输出数值，不属于验收。它在一块 32768 个三角形的起伏地形上构建典型生成图：MeshImport → Scatter（约 5 万点），Grid（224 × 224）→ Transform 抬高 → Ray 投射回地形，两路 Merge 后 CopyToPoints 得到约 10 万实例，再按 32 m 分块发布。报告各项 9 次的中位数（先预热 2 次）：100 万点的撒点与变换，地形 BVH 的构建与 224 × 224 条竖直射线的查询，冷启动整图求值与各节点耗时，无变化的求值，改 Scatter 种子、改 Ray 距离、改 CopyToPoints 变体属性名之后的重算，撤销到已缓存的种子，以及首次发布与逐块复用的再发布。测量时机器应空闲。
