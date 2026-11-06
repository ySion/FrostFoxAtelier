# Procedural：几何与程序化图

静态库 `atelier_procedural`（[cmake/procedural.cmake](../../../cmake/procedural.cmake)）依赖 runtime、geometry 与 source，不依赖 World、presentation 或编辑器。入口头文件：

| 头文件 | 内容 |
| --- | --- |
| [geometry.hpp](../../../modules/engine/procedural/include/procedural/geometry.hpp) | `Geometry` 值、层、属性列、拓扑与实例 |
| [operations.hpp](../../../modules/engine/procedural/include/procedural/operations.hpp) | 导入、生成、撒点、变换、Promote、Copy to Points、Realize、包围盒 |
| [instances.hpp](../../../modules/engine/procedural/include/procedural/instances.hpp) | 把 instance 层发布为渲染用的实例集合 |
| [bvh.hpp](../../../modules/engine/procedural/include/procedural/bvh.hpp) | 首角扇形 BVH：射线与最近点查询 |
| [graph.hpp](../../../modules/engine/procedural/include/procedural/graph.hpp) | `Graph`、`Node`、端口、连接与编辑入口，类型注册 |
| [nodes.hpp](../../../modules/engine/procedural/include/procedural/nodes.hpp) | C++ 内置节点 |
| [evaluator.hpp](../../../modules/engine/procedural/include/procedural/evaluator.hpp) | 增量求值与结果缓存 |
| [random.hpp](../../../modules/engine/procedural/include/procedural/random.hpp) | 固定算法的整数哈希与单位随机数 |

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
- **`box` / `sphere` / `tube`**：以原点为中心，按下述固定拓扑生成闭合多面体，N 统一为 vertex 层。尺寸、半径、高度须有限且为正；Sphere 经向至少 3 段、纬向至少 2 个区间，Tube 至少 3 段。预检总角数须小于 2³²−1，不截断或自动降分辨率。使用实际 float 坐标，表面消费者仍执行公共凸面判据，不为构造输出豁免精度校验。
- **`Geometry::connect_points` / Polyline**：只接受纯点输入，不得带任何图元或 instance；按点序连接为一个开放图元，不焊接，不自动闭合，没有 closed 选项。0 点空进空出（保留 detail，与未连端口相容），1 点报错。全部 point 与 detail 列及其用途原样共享保留，不生成 N；已有空 vertex/primitive 列保留定义，分别扩为点数/1 个类型默认值，instance 列仍为空，不静默删列。`Line → Polyline` 先生成等距点，再连接相邻点；作者手填点数组的绘制曲线节点后移到 S2，见 roadmap。
- **`group_box`**：在各轴闭区间 `[minimum,maximum]` 内选 point，写入/替换同名 bool 列；边界须有限且逐轴有序。名字遵守 Geometry 标识符规则，已有异层/异类型列报错。GroupBox 节点在输入的 COW 副本上操作，不改输入；这是输出组名，不能用空名字表示全选，不支持表达式或反选。
- **`scatter`**：接受三角形及凸平面多边形，不接受折线；按下述公共首角扇形视图采样。原图元 p、局部子三角 j 的随机源 s 在 j=0 时为 `hash(seed,p)`，j>0 时为 `hash(hash(seed,p),j)`；点数为面积 × 密度，以 `hash(s,0)` 按小数部分概率进位，第 k 个点使用 `hash(s,2*k+1)` 与 `hash(s,2*k+2)` 均匀采样重心坐标。输出按 p/j/k 顺序，k 在每个子三角内从零开始，不把展开下标或线程号混入随机源。可选最小间距在全部候选上按这个顺序稳定淘汰，所有列一起压紧；格坐标超出 ±2⁶² 时失败。输出 P、N、`id`、`sourceprim`、`sourceordinal`、`sourceprimuv`；仅输入包含超过三角的面时在末尾追加 uint `sourcetri`（其他三角面的值为 0）。纯三角输入的六列顺序、算术与值不变。`id` 在 j=0 时为 `hash(p,k)`，否则为 `hash(hash(p,j),k)`，它只是非唯一随机种子；（p,j,k）是局部稳定来源。`sourceprimuv` 是子三角 j 内的 (b1,b2)，不是原多边形坐标。
- **`transform`**：v · S · R · T。point 列完整变换，vector 列只用线性部分 L，normal 列乘 sign(det L) · cofactor(L) 后归一化（零向量保持为零，所以压扁到平面的奇异变换仍有平面法线），`orient` 列复合旋转，实例矩阵右乘，color 与 none 列不变。行列式为负时只反转 polygon 的顶点顺序及 vertex 列，polyline 的方向不变。输入或结果出现非有限值时失败，几何已被部分改写。
- **`promote`**：把属性移到另一层，按 first / sum / average 合成每个目标元素接触到的源元素；sum 与 average 只接受 float 与 float 向量；instance 层只和 detail 交换；P 不能移动。
- **`remove_flagged`**：删除 bool 列为 true 的元素；删除点时一并删除用到它们的图元，删除图元时保留点。
- **`merge`**：按输入顺序拼接各层，属性按名字合并，缺失的元素取类型默认值，detail 取第一个有值的输入；同名属性的层、类型或用途不一致时失败。
- **`copy_to_points`**：每个点一个实例，矩阵为 S · O · T：S = scale × pscale，O 是归一化的 `orient`，没有 orient 时由 N 和 up 对齐（只有 N 时取 +Z 到 N 的最短旋转），T 取自 P。点上的 uint 属性（默认 `variant`，缺失时为 0）选择引用。detail 属性带到输出，输出没有点。非有限输入和越界的变体失败。
- **`realize`**：有序 Geometry 源在选中的目标点上生成真实几何副本，与资源 CopyToPoints 共用放置规则，但不生成或递归展开 instance；分组、属性和所有权合同见下节。
- **`bounds`**：点的包围盒，空几何没有。
- **`Bvh`**：对公共扇形视图建立的包围体层次（分箱 SAH，深层退回中位数切分，深度有界），建立时复制三角形，零面积三角形不参与。`ray` 沿单位方向找 max_distance 内最近的交点，三角形双面有效；`closest` 找 max_distance 内最近的表面点。Hit 给出原图元号 `primitive`、局部子三角号 `triangle`、距离、子三角内重心坐标和位置。三角形求交和最近点在 double 中计算，包围盒测试留在 float（溢出的距离就是无穷远），所以有限坐标不会因中间结果溢出得到非有限或超出距离限制的命中，平行于某一轴的射线不受零的符号影响。射线恰好穿过共享边时可能两侧都不命中。

### Realize：真实几何复制

`realize(sources, points, group, variant, target_attributes)` 按选中目标的点序复制其选定源，不焊接、不去重。group 默认空表示全选，非空须为已有 point bool 列；只筛目标点，不筛源属性。variant 默认列名 `variant`，须为 point uint，缺列选源端口 0；选中目标的值越界时报错。目标输入的图元与 instance 不是复制内容；任何源包含 instance 都失败，不静默跳过未选源的 instance，也不解析资源。

- 放置为 S·O·T，和 CopyToPoints 共用输入列读取与矩阵计算；orient 优先于 N/up。源列（包括 P）按其 Usage 使用 Transform 的同一公式：point 完整变换，vector 不平移，normal 用余子式，color/none 不变；输出保留用途，不以 P 的名字强制改为 point。源非 detail 的 orient 复合放置旋转。负缩放只纠正 polygon 绕序，vertex 列一起反序，开放线方向保留。奇异缩放沿 Transform 的余子式法线规则，不自动删除副本。
- 只合并实际用到的源 schema，point/vertex/primitive 列保持其层、类型和用途；缺列元素按类型默认值填充，**不变换默认值**。同名异层、异类型或异用途失败，不猜测 Promote。源 material 只保留原值，不构造跨资源材质表。
- target_attributes 默认空，是显式目标 point 列名单；每个值广播到该副本所有点，覆盖同层同型同用途的源列。禁止 P，显式缺列或冲突报错。广播值已经处于目标空间，不再套放置变换；其余目标 point 列不隐式传播。
- detail 按实际用到的**源端口顺序**取每个名字的首值，不按目标遇见源的顺序；再由目标 detail 补缺，同名类型/层/用途冲突仍报错。detail 原样共享，不参与放置。无选中点时没有副本，但保留目标 detail。
- 预检点/角/图元总数均小于 2³²−1，再预留各输出缓冲并批量填充，不先复制 N 个 Geometry 再 merge。借用只读输入，输出拥有新拓扑和逐元素列，detail 继续写时复制；失败不发布局部结果，输入和兄弟分支不变。不造 Entity、GUID 或 MeshRef，资源实例发布路径不变。

### 构造拓扑的持久语义

实现位于 [constructors.cpp](../../../modules/engine/procedural/src/constructors.cpp)。已保存图虽不存 Geometry 缓冲，重算仍依赖点序、面序、首角及 N 所在层；同类型不能静默更换这些规则。后续三角化须显式操作，不把 Box 六面改成十二面。

- **Box**：8 点的 bit0/bit1/bit2 分别取 X/Y/Z 上界，0 取下界。六面按 −X、+X、−Y、+Y、−Z、+Z 排列，角号依次为 `(0,4,6,2)`、`(1,3,7,5)`、`(0,1,5,4)`、`(2,6,7,3)`、`(0,2,3,1)`、`(4,5,7,6)`。每面四个 vertex N 都取该面轴向法线。
- **Sphere**：Y 为极轴，M=`segments`、L=`latitudes`；先北极、再从北到南的 L−1 圈（纬角 πj/L），最后南极；每圈经角 φ=2πi/M，从 +X 向 +Z。共 `2+(L−1)M` 点、`ML` 面、`M(4L−2)` 角，共享缝和极点。先北极三角 `(north,ring1_next,ring1_i)`，再逐圈四边形 `(upper_i,upper_next,lower_next,lower_i)`，最后南极三角 `(south,last_i,last_next)`；每角 N 是其实际点位置的单位径向。
- **Tube**：Y 轴实心带盖，M=`segments`；先下圈 b、后上圈 t，位置为 `(r cosφ,±h/2,r sinφ)`，经角同 Sphere。侧面依次 `(b_i,t_i,t_next,b_next)`，下盖 `(b_0,b_1,…,b_M−1)`，上盖 `(t_0,t_M−1,…,t_1)`。共 `2M` 点、`M+2` 面、`6M` 角，不加盖心点。侧面 vertex N 是实际位置在 XZ 平面的单位径向，两盖为 −Y/+Y；不支持空心或去盖模式。

### 公共凸面剖分

模块内部 [triangles.cpp](../../../modules/engine/procedural/src/triangles.cpp) 是 Scatter、BVH/Ray、Volume 共用的数值判据与首角扇形视图，不生成第二份拓扑；借用期间 Geometry 必须保持不变且存活。图元 p 的第 j 个三角取原角 `(first, first+j+1, first+j+2)`，顺序为 p 再 j；折线明确失败。Scatter/BVH 的三角输入保留原先零面积跳过规则；Volume 要求三角面也通过非退化面校验。多边形都必须有可分辨的平面、简单凸边界及非零边，合法多边形中的零面积扇形片不算退化面。

所有几何判据用 double 计算。每个原多边形 p 的距离容差 δ_p = 8 × float ε × max（该面各角最大绝对坐标分量，该面 AABB 对角线长），两个输入量都不依赖首角或扇形划分，没有固定米制下限；8 是为 float 输入和变换舍入预留的安全系数，不是容许任意翘曲的建模参数。`polygon_scale` 同时为凸面校验与 Volume 表面带提供这个尺度和容差。首角扇形的有向面积向量长度必须大于 δ_p × AABB 对角线长，点到所得平面的距离不超过 δ_p，投影边长须大于 δ_p。采用保守凸性规则：投影相邻边的有向叉积必须非负，即使负值来自舍入也拒绝，不用距离容差放过凹角；正向共线角仍有效，允许零面积扇形子三角。接近折返的角（点积非正、叉积不超过 δ_p × 两邻边长度之和）也拒绝。在此非负转角前提下，转角总和必须为 2π（double 累加容差 64 × ε × 角数），既排除多圈星形，也不让正反小回环抵消成一圈。检查为线性时间，不另建边对相交矩阵。适用于细节显著大于其绝对坐标 float 精度的模型；远离原点或极薄的不可分辨面报错，不静默修补。后续构造与拓扑工具复用此判据，不另定容差。

### Volume Scatter：有向覆盖与固定格

[volume.cpp](../../../modules/engine/procedural/src/volume.cpp) 的 `volume_scatter` 使用串行 BVH 精确有向穿越分类；默认 density=1 点/m³、origin=(0,0,0)、cell_size=1 m、seed=0。输入是非空闭合壳集合，不接受 instance 或折线。所有点须有限，各面通过公共非退化判据；按**原多边形边界边**检查每条无向边恰好两用且方向相反，并检查每个被引用点的角环只有一个连通分量。不按位置焊接，不修补开口，不做自交检测。整体有向体积必须为正（平移至首角后用 double 补偿求和），允许反向内腔，不逐壳要求正值；未经转换的 Houdini 顺时针网格会被拒绝。

每个候选以最终存入 P 的 float 位置分类。对闭合有向三角链上不在表面的 q，绕数 w 是整数；从 q 沿射线到无穷远，每个正向交点贡献 sign(n·d)，全部贡献之和就是 w。因此 **w ≥ 0.5 等价于有向穿越和 ≥ 1**，无需逐候选扫描全部三角形。不取绝对值/奇偶，不提前接受部分和。重叠正壳的 w=2 保留，外壳加反向内壳的内腔 w=0 排除；分离负壳内部 w=−1 也排除，不猜测作者意图。零面积扇形片贡献零并跳过。

边界先于最终阈值：查询点 q 到任一原多边形 p 的**欧氏距离 ≤ δ_p** 时判为表面并排除，包括公共边、扇形对角线和顶点。各非零面积扇形片共用原多边形的 δ_p，以到有界三角形的最近距离判断，邻域取并集；不使用无限平面与外扩半平面的交集，细长扇形的延长尖角不会抹掉远处内点。距离计算与 BVH 共用 `closest_on_triangle` 的 double 实现，平面距离只作宽阶段。δ_p 来自上述公共 `polygon_scale`，不是精确实数谓词；分辨不出的贴面内点也不保留，不随机挪点、不把表面假定成 w=0.5。

δ_p 的输入尺度不依赖首角；精确算术下，多边形的距离邻域也与首角无关。但当前距离经过扇形片的 double 重心插值计算，距离与 δ_p 只差舍入量的边界点可能随首角变化。此为 C R2 暴力版已有的数值限制；加速保持同一输入的叶谓词，不承诺这类边界点在循环换首角后逐位不变。

一次求值复用现有 SAH BVH，同时持有固定 p/j 顺序的有效三角快照，结束即释放，不跨求值缓存。新增查询不改变 Ray/closest 的最近命中行为：

- `on_surface_band` 是存在性查询。节点按子树最大 δ_p 扩大有效查询范围；比较 q 到原始 float 盒两侧的 double 差值，向下舍入后仍大于剪枝阈值才跳过，而不是仅对 `盒边界 ± δ` 的最终结果取相邻值。剪枝阈值是向上舍入的 `δ + δ·2^-27`：额外项等于 32 × double ε × 原面尺度，覆盖最近点重建略微越出顶点盒和距离舍入的误差；不改变叶子的 δ。令 u=double ε/2，返回权重非负且和至多 1+8u，凸包越界及重建各至多 16u×尺度，距离比较再用 8uδ，总计小于 34u×尺度，低于预留的 64u×尺度。`closest_on_triangle` 明确禁止 va/vb/vc 的乘加融合与重结合，以保留上述权重范围证明所需的逐次舍入。叶子用各自原多边形的 δ_p 和原有界距离谓词，任一命中就排除。不能拿最近面的距离只比它的 δ，也不能取全局最小 δ：更远的大尺度面仍可能覆盖 q。
- `signed_crossings` 用保守 double slab 遍历全部正向交点，不按最近 t 裁剪。叶子再检查各三角自己的射线盒，避免同叶无关三角影响重试。固定八个非轴向方向取 √2/√3/√5 的确定二进制近似及符号排列；常量可由 float 精确表示，谓词全部在 double 中计算。不按运行、候选或遍历序号随机选方向。
- **符号认证不是 δ 带。** 每次 double 加、减、乘、除后，用相邻 double 向外包住结果；区间组合包含最初的坐标减法和相消误差，不按已相消结果乘一个经验 ε。有限 float 坐标和方向精确扩展到 double，三次乘积在其范围内。保持既有 precise 编译，不启用 fast-math 或重结合。令 a/b/c 为相对 q 的顶点，认证 u=det(b,c,d)、v=det(c,a,d)、w=det(a,b,d)：有两个可信异号即在面外；三者可信同号时，其和就是 n·d，det(a,b,c)/(u+v+w) 的符号判定前后向，正向贡献共同符号。区间含零且不能证明在面外时重试，涵盖边、顶点、共面和扇形对角线；不凭未认证的边侧把交点随意分给某一片。这些局部谓词及整数和不依赖树的分组和遍历顺序。
- **射线盒兼容认证**：先用普通 double 减法和除法，给每个交界参数 t 加减 `abs(t)·2^-49`；该余量覆盖原逐操作外扩端点相对 t 的误差（四种 IEEE 舍入下小于 9u，u=2^-53）以及最终加减的舍入。单调 min/max 给出原 near/far 的上下界，能证明原布尔结果才返回，否则执行原 Interval 盒测试；零差也回退，零方向沿用原范围比较。保持原盒结果、三角访问和全部重试／兜底计数，不扩大行列式的认证集合。此处是几何顶点盒，不替代上述带 δ_p 和重建余量的表面带盒。
- **仅八个方向全部不确定**时，对该候选直接复用原 p/j 顺序的立体角补偿求和：`2 atan2(det(a,b,c), |a||b||c| + dot(a,b)|c| + dot(b,c)|a| + dot(c,a)|b|)`，总和 ≥ 2π 即保留。没有规模阈值、开关或第二套表面带；固定有限方向不保证任意合法网格都有非退化方向。这里“精确”表示没有近似 fast-winding 或 parity 替代，不声称表面容差和兜底浮点求和是任意精度实数运算。

典型总工作约为 O(T log T + C·(log T + I + D))，I 是实际穿越/叶工作，D 是表面带和重试成本；重叠壳、细长大盒或全退化仍可能 O(C·T)。这不是并行墙钟复杂度或性能保证。尚无最小间距。各方向尝试/重试与兜底次数只经现有 Recorder 和下述 profile 上报，不增加观察 API。

壳校验、原序三角快照和 BVH 构建仍串行。分类按连续候选序号每 256 个划一个逻辑块：cell 前缀保留零计数格，一个高密度 cell 可以跨块；一次 `parallel_for` 融合生成 float P、表面带和射线／兜底，按候选序号写固定 P 与字节状态。每块只写自己的统计槽，不共享计数器。join 后宿主合并整数计数并提交 Recorder，再按块 retained 前缀精确分配六列，用第二次 `parallel_for` 稳定填充。无完成顺序 append，无每候选堆分配；BVH 与三角快照只读共享。worker 只借用固定普通值和输出区间，不碰 Runtime；发布 Geometry 留在成功 join 后。取消、异常或非有限 P 均不发布部分结果。P／状态 scratch 最多 13 MiB，cell 前缀约 4 MiB（替代原 counts），另有块统计；六列最终输出最多另占 32 MiB，不把 scratch 当作总峰值。

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
| Box | size 默认 (1,1,1) | output |
| Sphere | radius=1、segments=32、latitudes=16（纬向区间数） | output |
| Tube | radius=1、height=2、segments=32 | output |
| Polyline | — | points → output |
| GroupBox | group="selected"、minimum=(−.5,−.5,−.5)、maximum=(.5,.5,.5) | input → output |
| Scatter | density、seed、min_distance；节点种子为 hash(图种子, seed) | context, surface → points |
| VolumeScatter | density=1、seed=0、origin=(0,0,0)、cell_size=1 | context, surface → points |
| Transform | translate、rotate、scale | input → output |
| Promote | name、to、reduce | input → output |
| Remove | flag、layer | input → output |
| Merge | — | inputs（多个）→ output |
| Ray | mode（project / minimum）、direction、max_distance、normal、remove_misses | points, surface → output |
| CopyToPoints | meshes（资源键列表）、variant | context, points → output |
| Realize | group=""、variant="variant"、target_attributes=[] | sources（有序多个）, points → output |

Ray 对 surface 的公共凸面扇形建立 BVH，命中的点移到交点（minimum 模式为最近的表面点），写入 `hitprim`、`hittri`（都是 int32）、`hitprimuv`、`hitdist`；未命中的点保持原位，两个编号与距离都为 -1。`hitprimuv` 是局部子三角 `hittri` 内的 (b1,b2)，不是原多边形坐标。`normal` 打开时 N 取命中子三角的三个原角计算的法线，不按 `3*hitprim` 寻址；`remove_misses` 删除未命中的点。每次求值都重新建立 BVH。

原生节点的 `evaluate(...) const noexcept` 返回 `atelier::Result<void>`，反射仍发布为业务结果 void，端口和 BorrowSafe 约束不变。业务失败在节点边界转换为 `invocation_failed` 并保留原始原因，不通过异常传递；`Resources::mesh` 和 `Context::mesh` 同样通过 noexcept Result 返回资源拒绝。未连接的单输入端口收到空几何。

求值器只在成功后发布候选 Geometry，失败保留旧发布并阻断下游，修正输入后可恢复。直接调用节点不额外承诺可写引用的事务回滚：较晚失败时 output 可能已部分改变，仍可销毁；调用者如需保旧，应像求值器一样提供独立候选。

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
| random.source | 全局函数 `procedural.hash`、`hash2`、`unit`、`random_vector`，与 [random.hpp](../../../modules/engine/procedural/include/procedural/random.hpp) 算法相同，所以源码节点和 C++ 节点取到相同的随机数；`random_vector(x)` 由 x 连续哈希三次，每个分量均匀落在 [-1, 1) |
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

图文件的编解码在编辑器里（[editor/graph_format.hpp](../../../modules/editor/core/include/editor/graph_format.hpp)，`atelier_editor_core`），procedural 不依赖 editor。`load_graph(runtime, text)` 把 version 1 的图文件打开到已注册 procedural 类型的 Runtime 中，得到 `GraphDocument`：图的根、每个节点的 `GraphNode`（名字、节点根、画布位置、保留的原始 JSON）、保留的 settings 和加载诊断（`world::Diagnostic`，field 是从文件根算起的路径）。`save_graph(runtime, document)` 写出规范文本。名字和位置是文档数据，不放进节点对象，所以移动节点不盖章；保存只做 const 读取。

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

- **Realize**：两个手写三角形/开放折线源，目标先选源 1 再选源 0、第三点不选；手算两组非对称 S·O·T、余子式法线和旋转复合，核对 12 点/12 角/4 图元、镜像只翻面、vertex 标签、primitive material、point 覆盖/缺列默认值及源端口优先的 detail；改输出后输入仍保持原值。原生图另串通 Box→VolumeScatter→GroupBox→Realize（按 sourceordinal 选 Sphere/Tube）→Transform→Merge，与折线分支合并为 83 点/77 图元/283 角，不重复每个构造的数学验收。
- **Volume**：同一离线 lowbias32 表的两个格产生 8/7 候选；核对重叠正 Box 全部保留、反向内腔排除、一个面与已知候选精确重合时排除边界，连同 P/id/格来源/ordinal 及压紧顺序。Sphere M4/L3/r2 与 Tube M5/r1/h2 的实际 float 几何经绕 (1,2,3) 旋转 .74 rad、平移 (.125,−.25,.125) 后进入 Volume；独立逆 Rodrigues 变换后的分段方形截面/五边形半空间给真值，分别保留候选 7、11 和候选 11，不用解析球/圆柱代替多面体。Sphere 链同时经真实 Graph/Runtime 节点与图 seed 混合。
- **原生构造**：Sphere M=4/L=3/r=2 经 Runtime 节点求值，10 点/12 面/40 角、固定角序、径向 N；体积真值为中段 12 加两端各 2，共 16。Box 2×4×6 的手写八角/六面与体积 48；Tube M=5/r=1/h=2 的手写角序、离线五边形坐标、侧/盖法线与体积 5 sin72°。实际 Sphere/Tube 的 float 面进入 BVH，最近点对照已知极点/盖面。Polyline 对照三点两段，GroupBox 的五点对照 `true,true,true,false,false`，包含下/上界且替换已有全 true 组；各构造只留一个代表，不做参数矩阵。
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

`procedural_graph_acceptance`（无窗口，`atelier_editor_document_acceptance --procedural-graph`，[acceptance/editor/procedural_graph.inl](../../../acceptance/editor/procedural_graph.inl)）以手写当前 v1 JSON 为金值，读取 Grid → Transform、Box → VolumeScatter、Sphere/Tube、Line → Polyline → GroupBox 及 Realize 的非默认参数、位置、seed、display 与连接；Realize 显式核对 sources 的连接顺序、points 端口和 group/variant/target_attributes，再保存为相同规范字节。只保留 Missing 类型变成 null 节点且冻结 JSON 无损往返，以及构造失败不能吞成缺失类型两条边界。九种严格加载反例和额外宽容矩阵不再作为验收；严格结构规则仍如上，旧版本升级属于外部工具，不在加载器内做兼容迁移。

```powershell
cmake --build --preset release --target procedural_graph_acceptance
```

## 测量

`atelier_procedural_acceptance --profile-procedural <新的绝对路径.json>` 只输出数值，不属于验收。它在一块 32768 个三角形的起伏地形上构建典型生成图：MeshImport → Scatter（约 5 万点），Grid（224 × 224）→ Transform 抬高 → Ray 投射回地形，两路 Merge 后 CopyToPoints 得到约 10 万实例，再按 32 m 分块发布。报告各项 9 次的中位数（先预热 2 次）：100 万点的撒点与变换，地形 BVH 的构建与 224 × 224 条竖直射线的查询，冷启动整图求值与各节点耗时，无变化的求值，改 Scatter 种子、改 Ray 距离、改 CopyToPoints 变体属性名之后的重算，撤销到已缓存的种子，以及首次发布与逐块复用的再发布。测量时机器应空闲。

同一入口另有两档闭合 Volume 负载：**Sphere(r=5,M=32,L=32)** 的 1,984 扇形三角，以及 **M128/L128** 的 32,512 三角；两者都为 1,000 格、10,000 筛选前候选（origin=0/h=1/density=10/seed=3）。小档使用 `volume_*` 键，大档使用 `volume_large_*`；报告输入参数、构建配置、预热/样本数、格数、C、T、实际保留点数和各方向 attempt/retry/fallback 计数，固定输入各轮计数须一致。`retry_rate` 为首方向重试数/C，表面排除的候选仍计入 C。

正式报告给 `ms`（整次）、`build_ms`（仅建 BVH）、`generate_classify_ms`（融合生成／分类 fork→join 及计数合并）和 ns/候选；`remaining_ms` 先逐样本算整次−build−classify，再取中位数，包含壳准备、前缀、分配、稳定填充及列装配。旧串行版的 classify 包含输出 append，不能拿其历史数字直接算本片并行收益；并行收益须比较同一新内核的真 P1 与默认 P 未插桩版本。三个子步骤没有产品 marker；只允许另行批准的临时真 P1 诊断细分，并报告插桩扰动，不把各 worker 重叠时长相加冒充墙钟。

计时复用 [模块阶段标记](../../../modules/engine/procedural/include/procedural/timing.hpp) 与 foundation Recorder，由 profile 宿主安装并绑定。worker 仅累计普通整数，host 在 classify 仍打开、所有任务 join 后批量提交零时长事件；不逐点读时钟，无 Recorder 求值不读时钟，没有新增观察接口。采样只能在另行批准的独占时段执行，不得把复杂度或预算上限当实测性能。
