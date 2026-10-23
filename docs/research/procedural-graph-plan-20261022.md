# 程序化节点图：首阶段方案

状态：**已批准**（2026-10-22），P1 实施中。方向与产品取舍由用户确定，技术方案经总负责人审阅。§2 Geometry 核心（参考 Houdini、Lagrange、Blender 细化）和 §5.1 实例集合（已与渲染、性能负责人对齐）经总负责人批准；目标内容的三角形数待用户定（§9）。分支 `procedural` 基于 a0d3441c，即运行时 ec-8 栈的 tip（5badb653 写代号 → a0d3441c）。P1 已完成 Geometry 核心、C++ 操作、BVH 与 Ray 节点、实例发布、图与求值器（端口名来自 L5）和 `--profile-procedural`，当前行为以[程序化模块](../modules/procedural/README.md)为准；规范化 JSON v1 的编解码接口已与编辑器负责人对齐（§5.4），内联数组经 World 负责人同意、不算场景格式版本变化；自有语言节点的基类 PointNode、按参数名的列绑定和按代码身份的重算已接上（L4a、L1、代码身份和 L2 已交付并合入），Jitter、RandomScale 和 NormalAlign 已用自有语言写成，参数带 L2 的元数据；Remap 和属性噪声要能在参数里选择目标属性，选择方式待定（§9）。

## 0. 结论

- **Houdini 式的统一几何图。** 点、顶点、图元、实例及各层属性共用一种 Geometry 类型。第一刀交付点、属性与实例化：在现有网格上撒点、做属性运算，把现有资源实例化到点上。第二刀交付多边形构造。贴图图推后。
- **点和顶点不是 Entity。** 节点处理的是属性和数据表。Entity 相当于 UE 的 Actor：生成器组件挂在普通 Entity 上，输出实例集合。以后可以加 Spawn Entity 这类构造 Entity 的节点，首阶段不引入。
- **Geometry 核心自建，轻量，不做 CAD 内核。** 工业库和优秀实现作为参考，按需自建；难算法逐项决定参考还是接入。
- **自有语言优先。** 节点算法默认用自有语言编写。Geometry 容器、求值器、BVH 等引擎服务，以及暂时只能由 C++ 完成的改变点数的基元，作为可靠的黑箱供自有语言调用。
- **同一份权威数据，两种参与方式。** 图是 Runtime 对象，以规范化 JSON 存储。LLM 经 Control 操作符搭结构、取得客观反馈；人在 Houdini 式网络编辑器和 Parameters 中调节图级暴露参数，必要时下钻修改。两边走同一组操作符，都可以撤销。
- **生成结果以图为权威。** 场景默认使用 Cache 或冻结结果，提供冻结与 Bake 操作符，游戏运行时也可以求值。图可以封装为可复用的数字资产。

## 1. 已定事项

| 主题 | 决定 |
| --- | --- |
| 方向 | Houdini 式统一几何图；自建轻量几何核心，不做 CAD；工业库作参考，按需自建；第一刀点/属性/实例化，第二刀多边形构造；贴图图推后 |
| 协作 | 图为权威数据，以文本结构存储，便于 LLM 读写；经 Control 操作符编辑（Control 基础设施归编辑器负责人，`graph.*` 操作符由本线程按其规范实现）；算法写成自有语言节点；图级暴露参数供人调节 |
| 权威与缓存 | 图为权威；场景默认用 Cache 或冻结结果，不每次从零求值；提供冻结与 Bake；允许运行时求值；图可封装、可模板化（数字资产） |
| 实现语言 | 自有语言优先，不要求全部用自有语言；可靠的 C++ 工具和函数作为黑箱；重视 JIT 带来的性能，请运行时负责人对通用数学函数做极致、激进的优化 |
| 格式 | 规范化 JSON：节点以唯一名字为键，参数用字段名，连接写成名字路径，确定性排序与缩进 |
| 规模 | 单生成器约 10 万实例、图内约 100 万点、典型图（≤20 节点、≤10 万点）单次调参重算 ≤33 ms。这是预期量级，不是硬闸，靠可靠的实现达到。画面要在 120 FPS 下画出这些实例。8192 Entity 的上限以后会突破 |
| 界面 | Houdini 式网络编辑器：节点紧凑，参数在 Parameters，Tab 搜索添加节点，display 标记 |
| 运行时求值 | 求值器放在引擎库，Play 和游戏运行时都可以求值 |
| 写代号 | P1 直接用写代号，不另写不依赖写代号的实现 |
| 归属与审阅 | `src/material` 的数据和 API 归编辑器负责人，涉及 Surface 与捕获语义时渲染负责人也审；跨模块的依赖工作由本线程实现，对应负责人审 diff，动手前先对齐接口 |
| 语言能力 | 直接找运行时负责人排期，L5、L4 先做；L1 要过 Oracle，验收写明“只读、不改变借用与 GC 规则” |
| 机器 | 构建与窗口验收都守 GPU/窗口锁 |

## 2. Geometry 核心

```text
procedural.Geometry   原生记录，C++ 实现：按值语义，列级写时复制；坐标 float32，与 geometry::Mesh 一致
  五层：point / vertex / primitive / detail / instance，每层是“名字 → 列”，名字在整个 Geometry 内唯一
  列：Runtime 注册的 Array<T>，T ∈ {bool, int, uint, float, vector2, vector3, vector4, quaternion, matrix4}；
      detail 层每列一个元素，另允许 String；float 向量列带用途：point / vector / normal / color / none
  拓扑：顶点 → 点的 Array<uint>；图元 → 首顶点的 CSR 偏移 Array<uint>（长度为图元数 + 1）。
        全部图元边数相同时不存偏移，按“图元号 × 边数”计算；点云没有顶点和图元
  实例：references（去重的资源键表，首刀只有网格）+ instance 层的保留列 reference: Array<uint>、transform: Array<matrix4>
  约定名（沿用 Houdini）：P、N、v、up、Cd、pscale、scale、orient、id
```

- **列是 Runtime 数组**，自有语言节点直接借用这份存储，不复制，也不建镜像（[原生值借用](../native-value-borrows.md)）。列以共享的不可变缓冲保存；宿主在调用节点之前，在单线程上把节点要写的列复制成独占的，节点执行期间不发生复制。所以并行执行的 C++ 节点之间不会争抢首次写入（Lagrange 的整属性写时复制在这一步没有加锁）。下游节点只复制自己改写的列，其他列共享。
- **拓扑和实例保留列不参与属性绑定**：自有语言节点按参数名只能绑定普通属性列；拓扑经 Geometry 的只读接口读取，只有 C++ 服务能改写。这道写入屏障保证拓扑不会被属性节点改坏。
- **按用途变换**：point 列做完整仿射变换；vector 列只用线性部分；normal 列乘 sign(det) × 余子式矩阵（即行列式乘逆转置，矩阵奇异时仍有定义；det = 0 时 sign 取 +1），再归一化，长度为 0 的结果写 (0,0,0)，不产生 NaN。压扁到平面这类奇异变换因此仍能得到正确的平面法线。`orient` 复合上 Transform 节点的旋转参数，不从矩阵里分解。instance 层的 `transform` 右乘变换矩阵；color 和 none 列不变。变换的行列式为负时，反转每个图元的顶点顺序，vertex 层的列随拓扑一起重排，保持面的朝向。新列的用途按约定名决定（P → point，N → normal，v、up → vector，Cd → color），其他名字为 none，节点可以显式声明。用途显示在 Geometry Spreadsheet 中。
- **`id` 是随机种子，不是身份**：撒点等节点生成的 `id` 是 32 位哈希，用于派生逐元素的随机数和变体选择，不保证唯一（100 万点期望约 116 对重复，n²/2³³）。重复只会让两个点取到同样的随机值。第一阶段没有需要逐元素身份的功能：渲染历史用集合的 stable_key 加实例序号（§5.1），实例覆盖和 Spawn Entity 需要身份时另立方案；撒点输出的（来源图元号，三角形内序号）两列本身唯一、局部稳定，可以作为那时的基础。这样也不必给 Runtime 加 64 位整数列。
- **组就是 bool 列**：按属性删除、按组选择都读 bool 列，不另设组的概念。
- **层之间不隐式转换**：节点只绑定自己所在层的列，跨层用 Attribute Promote 节点显式转换（平均、取首个、求和等）；也不采用 Houdini 那种 vertex 覆盖 point 的同名优先级。
- **边不存储**：挤出、焊接等需要邻接关系的服务调用时按需建立，不提供稳定的边编号。
- **实例**：Copy to Points 把点转成实例。`reference` 由点上的 uint 属性（默认名 `variant`，缺失时为 0）对节点参数里的网格列表取下标；`transform` 按 Houdini 的公式由点属性合成。我们的矩阵也是行向量约定（`v * M`），公式可以原样使用：有 `orient` 时为 S·O·T；没有时用 N 和 up 求出对齐矩阵 L，为 S·L·T。其中 S = scale × pscale，T 取自 P。pivot、trans、rot、transform 这几个属性按需再加。参与合成的点属性出现非有限值时，节点失败（下游不求值，已发布输出保留上一版本）；pscale = 0 这类零缩放是合法的隐藏写法，矩阵照常合成，发布时丢掉（§5.1）。生成器按 `reference` 和空间块把 instance 层分组成实例集合（§5.1）。实例引用另一个 Geometry（嵌套）和实例展开成多边形（Realize）放在第二刀。
- **0 层服务（C++，第一刀）**：
  - 导入 `geometry::Mesh`。它是逐三角形的顶点表：导入时每个三角形成为一个图元，法线和 UV 进 vertex 层，位置逐位相等的顶点焊接成同一个点，材质槽写入 primitive 层的 `material`。
  - 表面撒点。每个三角形以 hash(种子, 三角形号) 作为自己的随机源，点数取面积 × 密度后按概率取整，在三角形内均匀采样重心坐标。结果与线程数和遍历顺序无关，改动一个三角形不影响其他三角形上的点。输出 P、N、`id`（由三角形号和序号哈希得到），以及来源图元号、三角形内序号和重心坐标，供后续节点插值属性。可选最小间距：先按密度过采样，再用均匀网格按点序淘汰距离过近的点（Blender 的 Poisson 模式也是淘汰法）。
  - 变换、包围盒、Attribute Promote、BVH 射线与最近点查询。
  - 这些服务以批量接口作为节点基类上的 `BorrowSafe` 原生方法提供，一次调用处理整列。
- **1 层（第二刀）**：基本体、合并、挤出、焊接、法线、投影 UV、实例的嵌套与展开，转成 `geometry::Mesh`，复用 OBJ 导入中保留边界 mask 的耳切三角化。**2 层**（布尔、细分、减面、UV 展开、凸分解）逐项决定参考还是接入，候选参考有 Manifold、OpenSubdiv、meshoptimizer、xatlas、V-HACD，许可证与维护状态届时核实。
- **参考与许可**：
  - Houdini 公开文档：分层、约定名、实例矩阵公式。
  - Adobe Lagrange（Apache-2.0）：拓扑作为保留数据加写入屏障、按用途变换、图元边数相同时不存偏移。改编其代码时保留许可证与署名。
  - Blender Geometry Nodes（GPL）：CSR 图元偏移、属性名唯一、实例层加引用表、逐三角形种子撒点。只参考设计，不复制、不改编代码。
  - 不采用的做法：Blender 的常量列（源码节点直接借用 Array，常量列要先展开，收益等实测再定）；Lagrange 的外部缓冲策略矩阵（列只由 Geometry 拥有）；Lagrange 的网格剪枝撒点（点数不与面积成比例）。

## 3. 节点、端口和参数对应到 Runtime 类型体系

```text
procedural.Graph   原生对象：图设置为 AFIELD（seed）；隐藏的节点强引用序列与 display 强引用槽；图级暴露参数（§5.3）
procedural.Node    原生对象基类：隐藏的 inputs 强引用序列，每个连接一项，按输入端口排序；平行的非反射 C++ 数组记录（输入端口，上游输出号）
  ├─ C++ 黑箱节点直接派生：evaluate(const procedural.Context&, const procedural.Geometry& in…,
  │                                  const procedural.Geometries& 多输入, procedural.Geometry& out…) const
  └─ procedural.PointNode   自有语言属性节点（类似 Houdini Wrangle）。端口固定为 input → output，不经 Program 就能读出：
        evaluate(const procedural.Context& context, Array<T>& 写入列…, const Array<T>& 读取列…)
        宿主按参数名绑定 point 层的属性列：可写引用是输出列（不存在就按默认值创建），只读引用是输入列（缺失时节点报错）
        改 primitive、detail 层的 PrimitiveNode / DetailNode 等有真实节点需要时再加
参数 = 节点类的公开字段（Label/Category/Min/Max/DESC 等元数据）；端口与列绑定来自 evaluate 签名；节点实例 = Runtime 对象
```

自有语言节点示例（assets/procedural/jitter.source；哈希与随机库 L6 之前由使用方放进快照）：

```source
class procedural.Jitter : procedural.PointNode {
    [Min = 0.0, Max = 1.0, LockMin, Label = "Amount", DESC = "每个轴上偏移的最大距离，单位 m。"]
    float amount = 0.1f;
    [Min = 0.0, Max = 1.0, LockMin, Label = "Axis Scale", DESC = "按轴缩放偏移；某一分量为 0 时不沿该轴移动。"]
    vector3 axis_scale = vector3(1.0, 1.0, 1.0);
    [Label = "Seed", DESC = "与图的种子混合后得到本节点的随机种子。"]
    uint seed = 0;

    void evaluate(const procedural.Context& context, Array<vector3>& P, const Array<uint>& id) {
        const vector3 reach = self.axis_scale * self.amount;   // 每次读 self 都是一次宿主调用，循环外先存到局部
        const uint node_seed = hash2(context.seed, self.seed);
        for (uint i = 0; i < P.size(); i = i + 1) {
            const vector3 r = random_vector(hash2(node_seed, id[i]));   // 来自 random.source
            P[i] = P[i] + vector3(r.x * reach.x, r.y * reach.y, r.z * reach.z);
        }
    }
}
```

- **点数由宿主固定**。属性节点只改元素，不改长度，这符合现行可写借用规则。撒点、删除、复制、合并这类改变点数的基元由 C++ 实现，L3 落地后迁到自有语言。
- **evaluate 只读 self**：节点状态保存在求值器里，节点方法不写自己的字段。
- **端口名**：来自 L5，每个代码版本重新读取。Runtime 对象里的连接按端口序号存储，所以兼容更新改了端口名，现有连接仍然有效，下次保存时写出新名字。文件里的连接按名字保存：加载时名字解析不到的连接按宽容加载保留并原样写回。输入或输出参数没有名字的类型不能作为节点，读端口时拒绝。
- **连接存在下游节点的 inputs 强引用序列中**（Runtime 只有强引用序列）。改连接只给下游节点盖章；图持有节点、节点持有上游，被删除的节点由编辑器历史持有的根保活；环在建立连接时拒绝。
- **调用路径**：C++ 节点经 `Runtime::call`，自有语言节点经 `Program` 的 `类名.evaluate`，每个节点每次求值调用一次。原生 `evaluate` 是 const 接收者，调用不盖章；源码方法读 `self` 走 `Runtime::read`，也不盖章。宿主不在持有节点对象可写 `with_native` 的期间调用 evaluate。求值器构造时接收宿主的 `const source::Program*`，所以 procedural 链接 atelier_source；读端口、建图和图文件都不需要 Program。
- **代码身份**：自有语言节点的键加上入口的 `EntryInfo::code`。运行时按 unit（一个类的全部方法，或全局函数那一组）比较实际编译输入，相同就沿用上次的值，不同才换新值；`Program::revision()` 是成功发布的次数，求值器看到它变了才重新绑定入口。判定是保守的：改某个类的方法（含空白）只换这个类的 code，改全局助手、struct、enum、常量，或 Runtime 新注册了值类型，换掉所有 code，新增类不影响别的类；撤销改动拿到新值，不回到旧值。code 在整个进程内唯一，不同 Program、Runtime 之间不会撞值。这满足验收 ⑥。`evaluate` 必须至少借用上下文或一列：这样它才是同步值区间，编译器拒绝写 `self`，结果只取决于键里的内容。
- **种子与确定性**：节点种子 = hash(图种子, 节点 `seed` 参数, 元素 `id`)，不使用 ObjectId 等非持久身份。哈希与随机函数是 L6 引擎源码库中算法固定、有文档的函数，语言不为它们增加内建。同样的输入得到同样的结果，但不承诺跨 CPU 逐位一致。
- **内置节点的分发**：内置节点用自有语言编写，随引擎源码模块进入每个工程快照（L6）。P1 的验收先用夹具快照。

### 3.1 语言能力（运行时负责人已接，按此顺序逐项交付）

顺序：L5 → L4a → L1 → 代码身份 → L2 → L4b → L6，L3 最后。每项单独提交、过 Oracle。涉及语义的 L1、L3 和 L4 的 NaN/精度，先由运行时负责人交总负责人审。

| 编号 | 能力与表示 | 用途 | 需要的时间点 |
| --- | --- | --- | --- |
| L5 | 参数名放在签名旁边，不放进 `FunctionSignature`，兼容比较不变：原生方法用 `MethodDefinition/MethodInfo::parameter_names`，由反射生成器从 C++ 声明取名；源码入口用 `EntryInfo`。改参数名算兼容更新 | 端口名；按参数名绑定属性列 | 已交付（`runtime-param-names` 37872568，已合入 `procedural`），端口名已接上 |
| L4a | 逐元素函数 floor/ceil/trunc/fract/abs/min/max/clamp/lerp/smoothstep/step/sign（另有已有的 sqrt），支持 float 标量和 float vector2/3/4，按分量计算，降为 intrinsic 或 select 后内联，不开 fast-math，可以向量化。min/max 按 GLSL 定义（min(x,y) = y < x ? y : x），所以 clamp 遇到 NaN 输入会返回下界；fract 结果在 [0,1)；没有整数版本 | 噪声、重映射 | 已交付（`runtime-math-builtins` 5707ca53，已合入 `procedural`） |
| L1 | `analyze_calls` 放行一种情况：接收者是 `self`、结果为标量/枚举/数学值的字段读取。写 self、取对象引用、调用非 BorrowSafe 方法照旧拒绝。读取走 `Runtime::read`，不盖章，值区间内也没有 GC 安全点。验收写明“只读、不改变借用与 GC 规则”。源码执行只在 Runtime 线程上进行 | 节点读参数 | 已交付（`runtime-self-reads` c5b6a47c，已合入 `procedural`，bdff0c02） |
| 代码身份 | `EntryInfo::code` 与 `Program::revision()`，判定见 §3 的“代码身份”；仍然每次发布都重新编译，复用旧 Version 等实测需要再说 | 验收 ⑥ 只重算代码变了的类型 | 已交付（`runtime-entry-codes` 2b278be4，已合入 `procedural`，d9809d57） |
| L2 | 源码类字段支持 float vector2/3/4 与已注册的枚举，元数据 `[Min, Max, LockMin, HardMax, Label, Category, DESC]` 写进已有的 `FieldDefinition`；向量的范围按分量生效；默认值用常量构造（如 `vector3(0.0f, 1.0f, 0.0f)`），按字节比较，范围或默认值变化返回 `rebuild_required` | 向量/颜色/枚举参数与面板范围 | 已交付（`runtime-class-fields` ed2b6e89，已合入 `procedural`，323e3cac） |
| L4b | float 的 sin/cos/exp/log、atan2(y, x)、pow(x, y)，参数为标量或 vector2/3/4，标量广播到每个分量，double 编译期拒绝。纯 IR 多项式，内联、可向量化，误差 ≤ 4 ULP（sin/cos 在 \|x\| ≤ 2²⁰ 内，超出后结果有限且绝对值不超过 1）；特殊值：log(0) = −∞，负数取 log 得 NaN，exp 溢出得 +∞，pow 底数为负得 NaN，pow(x, 0) = pow(1, y) = 1。所有 FMA3 主机逐位相同，标量与向量化循环逐位相同，与 `<cmath>` 不逐位相同；JIT 要求 FMA3。详见 [math.md](../math.md#超越函数) | 旋转、衰减、曲线 | 已交付（`runtime-math-transcendental` 701b9f0f，已合入 `procedural`，de359783） |
| L6 | 引擎文件在仓库 `assets/engine/procedural/*.source`，构建时复制到可执行文件旁的 `engine/procedural/`，开发树构建直接读源码树。`atelier_source` 提供共用加载函数：给出引擎根目录（默认是 exe 旁的 `engine/`，开发树构建用源码树），返回带 `engine/` 前缀路径、按内容哈希定版本的文件；`ProjectCode::snapshot` 把它们并进每个工程快照（编辑器和 Play 同一处），`procedural_acceptance` 用同一函数再追加夹具。全部引擎模块固定带上，不按需启用。命名空间由目录决定：`engine/procedural/` 下的文件只能声明 `procedural.` 下的类型和函数，其他文件不能在 `procedural.` 下声明任何东西；只看声明出来的名字，不看基类，所以 `class mygame.Foo : procedural.PointNode` 允许。这条规则在 `Program::request(SourceSnapshot)` 收集快照时检查，发布前就失败，所以所有宿主走同一条规则。全局函数支持限定名，引擎函数必须带限定名，声明与调用都写全名（`uint procedural.hash(uint x)`、`procedural.hash(x)`），工程函数照旧可用短名；同一个限定名不能既是类型又是函数。L6 合入后由本线程把 `assets/procedural/` 的文件搬到 `assets/engine/procedural/`，改成限定名，验收改用共用加载函数 | 内置节点、哈希与随机库 | P3 前；形状已与运行时负责人对齐，方案待总负责人审 |
| L3 | 倾向按值返回拥有型 Array，结果直接构造在调用方存储中；P4 前单独出方案，过 Oracle | 改变点数的节点迁到自有语言 | P4 |

依据：带聚合借用的函数里，[`analyze_calls`](../../src/language/source.cpp#L245-L292) 只允许 `self` 作为 BorrowSafe 调用的接收者（第 259 行）；类字段只能是五种标量（[LANGUAGE.md](../../LANGUAGE.md#项目快照与原生派生类)）；数学内建没有 floor/sin 等（[math.md](../math.md#源码运算)）。

## 4. 求值模型

```text
宿主边界（编辑器帧内，操作符和编辑处理完之后、渲染准备之前）
 1. cursor_now = runtime.change_cursor()                  先取游标，再读取
 2. 图在 cursor_prev 之后被写过：重读节点列表、display 和种子
 3. 新节点和 changed_since(节点, cursor_prev) 的节点：用 const 借用和 read 读参数与连接，算出本地键；本地键和连接都没变的是误报
 4. 有变化（或资源版本变了）时，从 display 按上游在前的顺序：
    键 = 本地键（类型身份 + 公开字段内容）+ 图种子（只对带上下文的节点）+ 各连接的（端口, 上游输出号, 上游结果序号）
    当前结果的键不变就沿用；否则先查缓存，未命中才执行 evaluate，得到新的结果序号
 5. 发布 display 的输出；cursor_prev = cursor_now
```

- **写代号**：节点就是对象，粒度合适。读取一律用 const 借用或 `read`，求值器不能给自己盖章。节点状态（错误、求值与命中次数）保存在求值器侧，按 ObjectId 索引，不写回节点。代码发布不盖章，所以自有语言节点接入时把代码版本写进键里。写代号不持久化，持久缓存只用内容哈希。
- **键**：键是精确的字节串，相等就是内容相等，没有哈希碰撞；类型身份用已发布 `TypeInfo` 的地址，只在本 Runtime 内有效。每次执行 evaluate 得到新的结果序号，上游结果一变下游的键就变；内容相同的节点共享同一个结果。
- **缓存**：内存缓存按键寻址，以条目为单位在字节预算内按最近使用淘汰；撤销把参数改回旧值时直接命中旧键。
- **资源**：节点经 Context 解析的网格连同解析结果记在结果上，宿主的资源版本变化后重新解析核对，结果不同才重算。
- **调度**：Runtime 线程上同步执行，有每帧时间预算，节点是最小单元。C++ 节点只读不可变的求值计划，可以移到工作线程；源码节点留在 Runtime 线程。是否这样做由实测决定。
- **错误**：建立连接时拒绝成环。节点失败时，下游不求值，已发布输出保留上一版本；同一个键在资源版本不变时不重试。
- **测量**：P1 提供 `--profile-procedural`，规模数字以实测为准。

## 5. 接入场景、身份与保存

### 5.1 生成器与实例集合

- **生成器组件**：`procedural.GeneratorComponent : SpatialComponent`，属于 presentation 层，World 负责人审。它引用图资产键，保存本实例的暴露参数值，持有最近一次发布的不可变输出。presentation 仍是一个组件一个槽位：生成器的 RenderItem 带一份共享的不可变集合列表，SceneSnapshot 把它展开成每个集合一个 `MeshInstance`，开销按集合数计。SceneSnapshot 按（组件，网格，块）给每个集合分配 stable_key：集合的局部数组 shared_ptr 不变就沿用旧键，换了才分配新键。
- **输出形式**：发布新输出时，把 display 输出的 instance 层按（`reference`，块）分组，每组是一个实例集合：一个 MeshRef 加一份共享的不可变局部变换数组。块在生成器局部空间中按实例的平移划分，边长是生成器参数，默认 32 m。这样视锥剔除、阴影失效范围、级联拟合和 TLAS 都按块受益，渲染项数只与块数有关。实例的世界矩阵 = local[i] · 生成器世界矩阵（行向量约定，先局部后世界）。Geometry 中的 `transform` 是 matrix4 保留列，只由 C++ 服务写入，构造上就是仿射，发布时去掉恒为 (0,0,0,1) 的那一列。首刀实例只带变换。
- **发布**：分组、分块、格式转换和下面两项检查都在发布时一次完成，O(N)。
  - **生成器内容不触发渲染器的整帧拒绝**：局部矩阵的判据与渲染器对 world 的判据是同一段代码。把 `renderer.cpp` 里 `prepare_instance` 的行列式与 GPU 法线矩阵可表示性检查提成 `math` 中的纯函数，两边共用，渲染负责人审。行列式为 0 或法线矩阵在 GPU 上不可表示的实例（pscale = 0 等零缩放、压扁成面的缩放），发布时按约定丢掉，丢掉的数量写进求值统计和 `graph.inspect`。非有限值在上游已让节点失败，到不了发布。集合路径的法线矩阵由 normal(world) 与 normal(local[i]) 组合：两者都已通过同一判据并按最大元素缩放，组合结果不从 FP32 乘积重新推导，也不再据此拒帧，否则两个各自合法的极端缩放（例如都是 1e-20）相乘会下溢。剩下的只有位置：局部矩阵与 world 都合法，但相乘后超出 float 范围（两者的缩放都极端）。这和普通 Entity 缩放极端属于同一类，仍由渲染器现行边界处理。
  - **逐块复用**：每个（网格，块）与上次发布的同一块逐字节比较，相同就复用旧的 shared_ptr，SceneSnapshot 因而沿用旧键、渲染器不重写这一块的常驻区段。块内实例按源点顺序排列，相同内容得到相同字节。只改动一个块的参数修改、撤销后命中 Cache 的重新发布，都只替换变了的块。
- **材质**：与普通网格相同，入口是编辑器负责人在 app-editor-gltf 分支 d2340350 提交的 `Result<std::shared_ptr<const std::vector<material::SurfaceRef>>> editor::Session::mesh_surfaces(Project&, const MeshAsset&)`，MeshAsset 由 `Project::mesh(key)` 取得。它放在 Session 而不是 presentation，因为解析需要 Project 目录和 Session 里组件也在绑定的那份共享材质对象；Play 也用 editor::Session，所以编辑器和 Play 走同一个入口。结果按网格的槽顺序排列：映射了默认材质的槽是该材质当前的 capture，没有映射的槽是导入时的表面；不乘组件独有的 material 参数；不可用的默认材质用替代品，只有 capture 失败才报错。两边对同一个共享材质对象调用 `material::capture`，内容不变时返回同一个 Surface，所以与没有覆盖的组件逐指针相同。没有订阅：变化来自材质被编辑（capture 发布新 Surface）、刷新后映射改变和网格重载，后两种在重新调用 `project.mesh` 后体现。宿主在更新生成器时取一次，外层 vector 每次都是新的，所以与上次逐元素比较指针（`std::ranges::equal`），变了才替换这些集合的 surfaces，局部变换数组不动。
- **一个集合是一个渲染项**：presentation 和 SceneSnapshot 都只按集合计，不把集合展开成逐实例的 CPU 循环。按性能负责人在 Dungeon（3380 个实例）上的实测推算到 10 万：每个实例单独做一个渲染项的话，render_items 加 SceneSnapshot 约 20 ms，Renderer prepare 约 9 ms，Scene Lit 静止循环约 3.7 ms，阴影缓存规划还要乘上灯数，所以不可行。

渲染器接口已与渲染负责人对齐；渲染器需要新增的能力由本线程实现、渲染负责人审：

| 项 | 约定 |
| --- | --- |
| 字段 | `MeshInstance` 增加 `std::shared_ptr<const std::vector<…>>` 局部变换数组。每个实例 12 个 float（48 B），即 3×4 仿射：沿用 `world` 的行主序与行向量约定，按 x 轴、y 轴、z 轴、平移四行存储。null 表示普通单实例；非 null 时数组必须非空，空输出由生产者直接不发这一项。集合要有非零 stable_key；selected、surfaces 和常量材质对整个集合生效。新字段同时加进 `MeshChecks::same()`（按 shared_ptr 比较）和 `valid_instance`，并更新 `sizeof(MeshInstance)` 的 static_assert（这三处来自性能负责人的 perf-render-delta 5b796b29，P2 以合入它之后的 main 为准） |
| 缓存身份 | 渲染器的缓存持有这个 shared_ptr 本身，不只记裸指针。否则旧数组释放后，新数组可能分配在同一地址，命中过期缓存 |
| 包围盒与校验 | 数组变化时，渲染器自己算集合的局部包围盒（所有实例变换后网格包围盒的并集，O(N)），不信任生产者给的值；world 变化时只变换包围盒的 8 个角。合成后世界矩阵的有限性在数组或 world 变化时检查，失败时整帧拒绝，与现行边界一致；法线矩阵按上面的组合规则得到，不再检查 |
| 合成 | 首版在 CPU 上用 double 逐实例合成世界矩阵、光栅用的法线矩阵（normal(world) · normal(local[i])）、Lit 用的逆矩阵和逐实例镜像符号（同一集合可以混有镜像和非镜像的实例），校验并入同一循环，只在数组或 world 变化时执行。所以拖动生成器的每一帧是 O(N)，需要实测。改用 TLAS b 时，合成、法线矩阵和 TLAS 实例描述由同一个展开 compute 完成；此时校验改为在数组变化时预先算出界值，world 变化时只做 O(1) 判界 |
| 常驻区段 | 集合的光栅实例（108 B/实例）、Lit 实例（192 B/实例）和按实例展开的 `material_offsets`（4 B/实例，shader 不用改）放在常驻区段，只在数组或 world 变化时写入，不跟着每帧的实例表上传。区段按 stable_key 分配位置，场景里其他项的增删不会让集合整体平移。draw 直接用区段基址 + `SV_InstanceID` 取实例，不经过 draw_instances 间接表；`scene_vs.hlsl`、`lit_gbuffer.hlsl`、`lit_shadow.hlsl` 按 draw 切换取法。阴影任务按区间发 draw，不逐实例上传 caster 下标 |
| 剔除与阴影 | CPU 只按集合包围盒剔除；阴影缓存把集合当作一个 caster；探针场景包围盒按集合算。按集合计有三项代价：点光的 face 与集合包围盒相交时，重绘全部实例；级联投影变化时，与级联相交的集合每次都画全部实例；大集合会撑大接收面的深度范围，级联拟合退回保守分段。分块输出就是为了压低这三项 |
| 静止与动态 | 局部数组更换的块（重新求值后内容变了）按“旧集合退休 + 新集合插入”处理，换新的 stable_key，回到 stationary 层；内容没变的块复用旧 shared_ptr，键、层次和常驻区段都不动。生成器 world 变化沿用现行规则：插入后变过的物体标为 dynamic |
| 历史身份 | 渲染器把 stable_key 与实例序号混合成每个实例的历史表面 id，TAA、SSR 能区分同一集合里的实例。局部数组更换后 stable_key 换新，历史随之失效 |
| TLAS | 先用 gfx 每实例句柄（a）。gfx 的 build 和 update 每次调用都在 CPU 上遍历 TLAS 的全部实例，重写并上传全部 64 B 实例描述；Scene Lit 只要有任何物体变换就会 update。所以只要有一个物体在动，每帧都是 O(总实例数)，10 万实例约上传 6.4 MB。预计 120 FPS 的 Play 需要 b：在 gfx 局部补丁里加一个入口，由调用方提供常驻实例描述缓冲和数量来 build 和 update；普通物体在 CPU 上按脏区间写，集合由 compute 从局部数组展开。数据布局现在就按 b 设计，从 a 换到 b 时接口不变。展开后的总实例数要低于 2^24（InstanceID 只有 24 位） |
| 统计 | `scene_instances` 和 `scene_culled_instances` 按展开后的实例计（整个集合被剔除时计 N 个），`scene_draw_calls` 按实际 draw 计。没有集合的场景数值不变；P2 仍按计数规则用旧数字搜索 acceptance/ 中的断言（materials.inl、viewport_modes.inl、main.cpp、scene_lit.inl） |
| 拾取 | 编辑器拾取现在对每个渲染项的每个三角形暴力求交。集合先按实例的局部包围盒剔除，实测太慢再在实例包围盒上建 BVH；命中实例时选中生成器组件。这部分在编辑器负责人的模块，由本线程实现、编辑器负责人审 |
| 不必特殊处理 | 自发光：D1 2.1 之后，网格发光只体现在 G-buffer 的自发光项和 DDGI 探针命中上，没有逐实例的发光采样，按集合判断一次即可。按场景计的路径不随实例数增长：light_frame、按槽位增量重建的 SceneSnapshot、按几何共享的 BLAS、DDGI、探针场景包围盒 |
| 以后 | 逐实例属性做成与变换数组平行、等长的属性流，shader 侧新增实例属性输入，届时单独提方案 |

- **显存**：10 万实例约 37 MB（光栅 108 B、Lit 192 B、材质偏移 4 B、TLAS 描述 64 B，再加上 gfx 每个实例句柄的 CPU 对象）。
- **GPU 成本与目标内容**：没有逐实例剔除时，块的包围盒只要可见，GBuffer 就画块内全部实例，阴影 face 或级联每次重绘也一样，所以 10 万实例的主要成本可能在 GPU。目标内容（每个实例的三角形数）由用户定（§9），用它实测。超过几百个三角形时，逐实例 GPU 剔除和 LOD 是 P2 的必需项。
- **测量口径（性能负责人确定）**：
  - 场景：工程副本里三个只差生成器的场景：Dungeon 原样、Dungeon_10k、Dungeon_100k。两个生成器用同一个网格，三角形数按用户定的目标内容选（优先用 Dungeon 现成的道具），报告单实例与总三角形数。实例撒在可走区域，镜头内只看到一部分。相机统一为 `--scene-lit-camera 6.927793 -0.362655 24.410025 3.6086 -1.3809 33.7881`。
  - 入口一：`atelier_editor --profile-render-performance`（release，有窗口，持锁），用例 clustered、clustered_orbit、docked_smooth，以及性能负责人在 P2 时添加的 clustered_motion。clustered_motion 分别移动生成器 Entity 和一个普通 Entity；后者等同于 Play 里有一个刚体在动，覆盖 TLAS 每帧 update 的开销。CPU 看 render.prepare、render.encode.scene.lit、render、application_step_ms；GPU 看 gpu_passes 中的 acceleration、gbuffer、shadow_cache、ddgi_trace/ddgi_update、direct_visibility/direct、hdr_reflections，以及 measured_main_total_ms。
  - 入口二：`atelier_editor_document_acceptance --profile-scene-cpu`，看 render_items、light_items、scene_snapshot、scene_snapshot_changed，并由本线程新增 scene_snapshot_generator_moved 与 scene_snapshot_generator_replaced 两个 stage，配合 `--profile-procedural`。
  - 规则：基线版和改动版交替各跑 2 次，取中位数；持锁，机器空闲（没有构建，负载低于 30%）。Acceleration 的 CPU 包含在 .lit 里，按与 Dungeon 的差值比较。不往产品里加 marker。
- **交付对照**（只用于决策和诊断，不作为验收通过条件）：
  - Dungeon 静止帧不退步：clustered 与 docked_smooth 的上述 CPU 字段和 measured_main_total_ms，以及 scene_snapshot、render_items。改动版的中位数应落在基线两次的波动范围内，超出就补一对重测。
  - Dungeon_10k 和 Dungeon_100k：静止帧和运动帧的同组字段，加上 scene_instances、被剔除实例数、scene_draw_calls、TLAS 实例数、GPU 实例表字节数。报告与 Dungeon 的差值，以及 10k→100k 的斜率：静止帧 CPU 斜率应约为 0，运动帧报 ns/实例，GPU 报每个 pass 的增量。GPU 能否达到 120 FPS 由渲染侧的 D1 负责，这里只看增量。

### 5.2 Cache、冻结与 Bake

| 模式 | 行为 | 数据位置 |
| --- | --- | --- |
| 实时 | 参数或代码变化就重算 | 内存 |
| Cache（默认） | 加载时按完整键查找，命中就不求值 | `Cache/Procedural/<图 GUID>/<键哈希>.bin`，可以重建 |
| 冻结 | 固定当前输出，图或代码之后再变也不重算，直到解冻 | Content 中场景旁的二进制边车，是作者数据；格式在 P2 定，涉及场景格式版本时报总负责人 |
| Bake | 操作符：把输出转成普通 StaticMeshEntity；第二刀起也可以转成 Mesh 资产，可撤销 | 普通场景数据 |

### 5.3 封装与暴露参数（数字资产）

图级暴露参数供人调节，生成器实例保存覆盖值，图资产以后可以作为子图节点复用。这是类型语义的契约，**P2 前另交方案给总负责人审**。建议的表示是：暴露参数是图上的参数记录列表，生成器保存显式覆盖（沿用 CONTEXT 的“实例覆盖”语义）；图资产不作为类型定义的生产者。原因是已发布的 Runtime 类型不可变，作者频繁增删暴露参数会不断触发重建。

### 5.4 身份与格式

| 项 | 影响 |
| --- | --- |
| 图资产 | P2 与生成器一起做：新资源种类 `Content/**/*.procgraph`（AssetKind 加在枚举末尾）+ 现有 `.asset.json` GUID 边车；Project 的边车规则由编辑器负责人审 |
| 图文件 | 格式 v1，规范化 JSON：`version`、`settings`（`seed`、`display`）、`nodes`。`nodes` 以名字为键（按名字排序，名字符合 `[A-Za-z_][A-Za-z0-9_]*`，`json_format::parse` 拒绝重复键），每项含 `type`（Runtime 限定类型名）、`position`、`properties`（写全部参数，节点默认值以后改变也不改变旧图的含义）和 `inputs`（总是写出，可以是 `{}`；未连接的端口不出现；单输入写 `"scatter_floor.points"`，多输入写数组并保留连接顺序）。`dump(2)` 后加换行，重新保存时字节不变。v1 不写暴露参数，P2 的契约方案再加 `parameters` 并决定是否升到 v2。名字和画布位置是文档数据（editor 的 GraphDocument），不放进节点对象，所以移动节点不盖章。宽容加载：未知节点类型原样保留并写回；指向它或从它出发的连接，以及端口名解析不到的连接，留在下游节点的 `inputs` 里写回；已知节点解析不了的参数叶（例如热重载改了字段名）作为保留值留在 GraphDocument 里，报诊断，保存时写回。只有结构错误严格拒绝：版本、`nodes` 不是对象、名字非法、`inputs` 形状错 |
| 场景 | 生成器是一种新组件类型，经现有 `properties` 保存；需要新增载荷时再报总负责人定格式版本 |
| Runtime 类型与计数 | 新增 `procedural.*` 类型、组件类型、标签页和 Control 键；实施时按旧数字搜索 acceptance/，把命中的断言列入必要测试 |
| 第二刀网格 | `mesh_source` 用 `<图 GUID>#<输出名>` 键，经 `set_mesh_binding` 绑定；物理碰撞缓存键加入内容哈希 |

模块落点：新建静态库 `src/procedural/`（`atelier_procedural`），包含 Geometry、Graph/Node 类型、C++ 黑箱节点、求值器，依赖 runtime、math、geometry。生成器组件放在 presentation，World 负责人审。图文件编解码是 `editor/graph_format.hpp/.cpp`，放在 `atelier_editor_core`，这个库链接 `atelier_procedural`，procedural 不依赖 editor；编辑器负责人审。它与 Scene 共用属性编解码：`properties.hpp` 声明 `encode_properties`（引用经 `std::function<Json(ObjectId)>` 编码，分配 id、归档材质等副作用留在调用方的回调里）和带诊断报告的宽容 `decode_properties`，定义在 `scene.cpp` 的 `decode_properties` 旁边；节点的连接和图的节点列表、display 是隐藏字段，不在类型的公开字段里，所以节点与图的属性就是各自的公开字段，不需要另行过滤。共用编解码新增元素为标量、枚举或字符串的内联 Array（不嵌 record，也不嵌数组），写成 JSON 数组。解码时在调用方的存储里把整个数组构造成一个值，作为一个叶整体写入：对象经 `Runtime::write`，Play 的 live set 经 `World::write`（类型规则和准入一起检查，被拒时整值恢复），写入钩子也只收到这一个叶。硬范围按元素检查：严格解码遇到越界或元素类型不符就报错，不夹值；宽容加载时元素越界用夹值后的数组，元素类型不符则字段取类型默认值，两种情况都保留原数组并原样写回。`author_fields` 输出数组条目，Parameters 不编辑数组（G4 再做）。World 负责人已同意这种值形状，并判定不算场景格式版本变化：此前 `author_fields` 遇到数组就报错，含数组字段的类型既不能保存也不能加载，所以现有文件里不可能有内联数组；新程序读现有文件结果不变，不含数组字段的文件写出来逐字节相同；旧程序打开含数组的新文件，无论版本号是多少都会失败，升版本反而让旧程序连不含数组的新文件也拒绝；用户已定只保留最新版本，为一个空迁移升版本，过后还要再删一次。编解码按字段的 form 和 kind 分派，不靠 JSON 形状判断（引用序列本来就是 JSON 数组）。

## 6. 编辑器：Houdini 式网络编辑器

**可复用**：停靠标签页；DSL 组件；布局 `out_of_flow` + `offset`；圆角加边框的 DrawQuad；弹出菜单与搜索框；反射驱动的 Parameters；按文档的撤销历史；操作符注册表与 Control；虚拟 Table，可以做 Houdini 的几何表格（Geometry Spreadsheet），人和 LLM 都能逐点检查属性；Renderer 的 `OverlayShape` 等点线段画成圆点，可在视口里显示点。

**缺口（本线程实现，对应负责人审 diff；动手前先对齐接口）**：

| 编号 | 内容 | 审阅 |
| --- | --- | --- |
| G1 | UI 帧的线段/曲线图元，用来画连线：复用 `OverlayShape` 的抗锯齿线段着色，守住 UI“一层一次实例绘制”的合批约定 | 渲染 + 编辑器 |
| G2 | 画布缩放：按档位缩放节点的 DIP 尺寸与字号 | 编辑器 |
| G3 | 画布手势：由宿主处理输入，用接受帧 `UiFrame::find` 命中，处理平移、框选、移动、连线和断开 | 编辑器 |
| G4 | 以节点和图参数为检查目标的 Parameters，以及图文档的撤销历史 | 编辑器 |
| G5 | Network View 与 Geometry Spreadsheet 标签页、Asset Browser 新类型、视口中显示 display 节点 | 编辑器 |
| G6 | 操作符 `graph.add_node / connect / disconnect / delete / move / set_display / freeze / bake`，Control 的 `graph.inspect`：返回点数、包围盒、属性统计、间距与穿插检查 | 编辑器（按操作符规范） |

## 7. 分阶段计划、端到端验收与工作量

测试遵循 AGENTS.md：只做必要的端到端验收，dev/release 都构建，测试默认在 release 运行；改到核心运行时或构建链时 dev 也重跑。期望值独立推导，选用能区分常见错误实现的非对称输入。构建与窗口验收都守 GPU/窗口锁。

| 阶段 | 内容 | 端到端验收（必要项） | 本线程工作量 | 依赖 |
| --- | --- | --- | --- | --- |
| P1 图与几何核心 | Geometry、Graph/Node、求值器（写代号、精确内容键、条目级 LRU）、规范化 JSON v1；C++ 黑箱：网格导入、Grid/Line 点、Scatter（含最小间距）、Transform、Attribute Promote、按 bool 列删除、Merge、Copy to Points、实例集合发布（分块、丢掉退化实例、逐块复用；矩阵判据从 renderer 提到 math 共用，渲染负责人审）、BVH 批量查询；自有语言节点：Jitter、属性随机、属性噪声、Remap、法线对齐；`--profile-procedural` | 新增无窗口目标 `procedural_acceptance`：①两个面积为 1 和 3 的三角形，密度取 4，使面积×密度为整数：两个三角形上的点数分别正好是 4 和 12，点都在各自的三角形内；同种子结果相同，换种子结果不同；只移动第二个三角形时，第一个三角形上的点不变；开启最小间距 d 后，任意两点距离都 ≥ d；②Grid(5×4)（带 N、Cd）→ Transform（旋转、非均匀缩放加平移）：P 与公式逐点一致，N 等于逆转置变换后归一化的结果（与只用线性部分的结果不同），Cd 不变；对导入的网格做镜像变换后，每个图元的顶点顺序反转；③按 bool 列删除后，点数与独立推导的结果一致；④Copy to Points：点带 orient、pscale 和非均匀 scale 时，实例矩阵等于 S·O·T；没有 orient 时按 N 与 up 对齐；`variant` 选择网格；按（网格，块）分组后，各组实例数正确，跨块边界的实例落在正确的块里；pscale = 0 和压扁成面的实例在发布时被丢掉并计数，点属性含 NaN 时 Copy to Points 失败、保留旧输出；只改变一个块内实例的参数修改，重新发布后其他块的 shared_ptr 不变；⑤经 `Runtime::write` 改参数、经 `with_native` 改 C++ 节点参数、经源码方法写 `self` 改自有语言节点参数，都只重算该节点及其下游（核对调用次数），撤销后零次重算；⑥兼容热更 Jitter 方法体，只重算该类型的节点及下游；⑦（放在 `editor_document_acceptance --procedural-graph`，配同名 CMake 目标）保存后在新 Runtime 重开，输出相同，再次保存字节相同；未知节点类型、解析不了的参数叶和指向未知节点的连接原样写回；共用属性编解码的数组经测试类型（Array<String>、带范围的 Array<float>）做 Scene 保存、重开和 live set 往返，严格解码拒绝元素类型不符，宽容加载把整个数组作为保留值写回；⑧成环被拒绝，节点越界失败时保留旧输出 | 14–18 人日 | L5、L4、L1、L2 |
| P2 接入场景 | 生成器组件、presentation 输出实例集合、渲染器实例化（按 §5.1 的对齐结论）、暴露参数与实例覆盖（先交契约方案）、Cache、冻结、Bake、Play 加载 | 扩展 `world_acceptance` 或 `procedural_acceptance`：集合数与每实例世界矩阵正确（生成器 Entity 带旋转和非均匀缩放）；改覆盖值后集合更新、版本前进，静止时版本不变；保存场景后重开结果相同，删掉 Cache 后重新生成也相同；冻结后改图或改代码不影响输出；Bake 产生的 Entity 数量与矩阵一致且可撤销；渲染器 GPU 读回核对实例画面；含 pscale = 0 实例的生成器正常出图、不触发整帧拒绝；场景中其他项增删时，集合的常驻区段不重写；改一个只影响一个块的参数，其他块的 stable_key 不变、常驻区段不重写；附性能负责人确定的测量 | 生成器、格式与场景接入 6–9 人日；渲染器实例化（字段、常驻区段、shader 取法、阴影区间、历史 id、统计、拾取，TLAS a）8–12 人日；按实测追加：TLAS b 5–8 人日，逐实例 GPU 剔除与 LOD 6–10 人日 | 渲染、World、性能、编辑器负责人 |
| P3 网络编辑器 | G1–G6、Geometry Spreadsheet、视口点显示 | 新增窗口目标 `editor_graph_acceptance`，持锁运行：经操作符和合成拖拽建图连线；在 Parameters 改参数；导出 PNG，核对连线中点像素和视口实例；核对表格行数；撤销/重做；保存后重开；`graph.inspect` 的统计正确 | 14–20 人日 | L6；编辑器与渲染负责人审 |
| P4 封装与迁移 | 子图作为节点、从选中节点创建数字资产；L3 落地后把改变点数的基元迁到自有语言 | 扩展 `procedural_acceptance`：子图展开前后结果相同；迁移后输出与 C++ 版本一致 | 6–10 人日 | L3 |
| P5 多边形构造 | 1 层算子、Mesh 输出键、碰撞缓存；2 层逐项决定 | 届时另立方案 | 20–30 人日起 | World、渲染负责人 |

## 8. 跨模块对齐

| 负责人 | 事项 | 状态 |
| --- | --- | --- |
| 运行时和语言 | L5 → L4a → L1 → 代码身份 → L2 → L4b → L6，L3 最后 | L5、L4a、L1、代码身份、L2、L4b 已交付并合入；L6 形状已对齐、方案待总负责人审；`math.is_finite` 排在 L6 之后；其余见 §3.1 |
| 渲染 | 实例集合接口、TLAS 路径、阴影 caster 粒度、自发光与逐实例属性；矩阵判据提到 math；G1 着色与合批 | 实例部分已对齐，见 §5.1；矩阵判据（fab31153）已审通过；G1 在 P3 前对齐 |
| 性能 | 120 FPS 下实例场景的测量口径与交付对照；P2 时请其添加 clustered_motion 用例 | 已对齐，见 §5.1 |
| World 和物理 | Scene properties 新增内联数组的值形状；生成器组件与 presentation 的接口；以后的生成碰撞体 | 数组已同意，不算格式版本变化（§5.4），文档段落由其审；其余 P2 前对齐 |
| 编辑器 | G1–G6、图文件编解码落点、数字资产契约方案（同时交总负责人）；生成器从网格资产解析槽位默认材质的接口（与新材质方案同一来源） | 材质入口已交付（`Session::mesh_surfaces`，d2340350，见 §5.1）；共用属性编解码与内联 Array（335cb844）、图文件编解码（ed47199a）已审通过（§5.4）；其余 P2/P3 前对齐 |

## 9. 风险与未决

- **语言能力的时间线**：P1 依赖的 L1、L2、L4（含 L4b 超越函数）、L5 都已交付；剩下 L6（引擎源码并入工程快照，P3 前）和 L3（P4 前）。
- **Remap 与属性噪声的目标属性**：列按 `evaluate` 的参数名绑定，所以节点只能处理代码里写定的属性名；Houdini 的 Attribute Remap、Attribute Noise 在参数里选属性。建议加一个原生基类，带 String 参数 `attribute`，求值器按它的值绑定一个约定的列参数（源码 class 不能声明 String 字段，但能继承原生字段）。这扩展了节点契约，**待用户定方向，再交总负责人审**；另一种做法是保持写定属性名（Wrangle 式），由用户为每个属性写自己的节点。
- **数字资产契约**：§5.3 的表示要在 P2 前定。如果改为让图资产发布 Runtime 类型，需要先解决类型版本与重建问题。
- **实例渲染**：10 万实例在 120 FPS 下的 CPU/GPU 成本按 §5.1 实测。主要不确定项有两个：一是只要有物体在动，TLAS 每帧都按总实例数重写，Play 多半需要 TLAS b；二是没有逐实例剔除时 GBuffer 和阴影重绘的 GPU 开销。第二项取决于目标内容的三角形数，**待用户定**。

## 附：拟在 P1 交付时加入 CONTEXT.md 的术语

**程序化图（Procedural Graph）**：一组节点及其连接，求值后产生几何或生成资源；图资产具有持久身份，节点是同一 Runtime 中的对象。**几何（Geometry）**：点、顶点、图元、实例与各层属性构成的值，属性列是 Runtime 数组；点和顶点不是对象，也不是 Entity。**节点（Node）**：类型来自原生注册或语言编译的图对象，公开字段是它的**参数（Parameter）**，`evaluate` 的引用参数是它的**端口**或属性列绑定。**连接（Connection）**：下游输入端口对上游节点某个输出的引用，由下游节点持有。**求值（Evaluation）**：按内容键判定并执行脏节点的过程，即 Houdini 的 cook。**生成器（Generator）**：挂在 Entity 上、引用图资产并保存暴露参数覆盖值的组件，持有最近发布的实例集合。**冻结（Freeze）**：固定生成结果，图与代码之后再变化也不重算，直到解冻。
