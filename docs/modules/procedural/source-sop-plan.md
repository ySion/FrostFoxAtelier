# Source SOP 语言、拓扑接口与自省演进计划

状态：方案已考据定稿，分批待实施。当前已实现的语法、数学内建、几何操作与 Control 指令仍分别以 [LANGUAGE.md](../../../LANGUAGE.md)、[数学规范](../../math.md)、[程序化模块](README.md) 和 [Editor Control](../editor/control.md) 为唯一权威；本页不代表以下待推进项已经上线，每批落地后须将正式行为迁入对应权威文档并更新本页状态。

## 背景与目标

围绕“LLM 主力开发规则 + 人类 120 FPS 观察与滑杆微调”的程序化工作流，仓库已通过 [jitter.source](../../../assets/engine/procedural/jitter.source)、[spatial.source](../../../assets/engine/procedural/spatial.source)、[rail_panel.source](../../../assets/engine/procedural/rail_panel.source)、[ArcBalustrade.source](../../../examples/Source/Procedural/ArcBalustrade.source)、[Footbridge.source](../../../examples/Source/Procedural/Footbridge.source)、[GateArch.source](../../../examples/Source/Procedural/GateArch.source)、[ModularRuinBuilding.source](../../../examples/Source/Procedural/ModularRuinBuilding.source) 与 [TerraceIsland.source](../../../examples/Source/Procedural/TerraceIsland.source) 验证了 `.source` 正式类 SOP 架构。真实案例同时暴露出四类具体断层：

1. **基础向量、整数与标架样板重复**：缺少向量广播构造、同维 `float ↔ double` 显式转换、逐分量乘除、整数域 `min/max/clamp`、定义明确的 `round`、`length_squared/distance_squared` 以及“局部 `+Y` 朝上优先、前向投影到 `+Z`”的建筑正交标架，导致多处手拆 `.x/.y/.z`、绕道 `float` 强转或重复手写正交化。
2. **输入图元拓扑与折线弧长不可读**：`.source` 可借用各层属性列、查询 `Spatial` BVH 并调用 `read_curve_slices`，但不能按图元读取类型、有序顶点及点编号，也不能直接读取单条折线总弧长，迫使 [rail_panel.source](../../../assets/engine/procedural/rail_panel.source) 依赖原始点表顺序、[ArcBalustrade.procgraph](../../../examples/Content/Procedural/Graphs/ArcBalustrade.procgraph) 额外旁接 `SampleCurve` 仅取末端里程；同时 L3 节点无法只读借用自身的 `String` 参数字段作为动态输出列名。
3. **业务失败缺少问题元素定位**：`fail("...")` 仅携带文本与源码行列，求值器汇总 `root_error` 与 `graph.inspect` 序列化时还会丢弃源码位置且无层/元素索引，Control 调用方无法直接获知“第 17 个截段区间反了”。
4. **静态轮廓数据与公共函数契约缺少零分配/可查询载体**：顶层 `const` 仅支持标量与枚举，石栏杆车削母线被迫连写 28 次 `.add` 动态扩容；Editor Control 能查 SOP 节点目录，但尚不能查询 `.source` 公共辅助函数、`procedural.Node` 原生 `BorrowSafe` 方法及值类型的签名、单位、轴约定与 `parallel_for` 可用性。

---

## 分批推进总览

各批次按依赖与即时收益排序，每批独立编译、独立通过双配置端到端验收并迁移现有真实调用点：

| 批次 | 范围摘要 | 直接受益的现有案例与验收入口 | 状态 |
| --- | --- | --- | --- |
| **Batch 1（S-SOP-1）** | 向量广播与显式精度转换、`cwise_mul/div`、整数 `min/max/clamp` 与有符号 `abs`、IEEE `round`、`length_squared/distance/distance_squared`、`radians/degrees/PI/TAU`、`0x`/`u` 整数字面量；`orient.source` 建筑标架/轴角/欧拉角与 `remap.source` `unlerp/fit/fit01/efit` | 简化 `jitter.source`、`spatial.source`、`FortifiedOutpost.source`、`TerraceIsland.source`、`ArcBalustrade.source`、`ModularRuinBuilding.source`；覆盖 `jit_acceptance`、`material_program_resources_acceptance`、`procedural_acceptance` | 已实现 |
| **Batch 2（S-SOP-2）** | `procedural.Node` 只读图元拓扑与折线总长 `BorrowSafe` 接口（`count_elements`、`primitive_span`、`read_primitive_points`、`polyline_length`）；同步值区间只读借用 `self.<String 字段>` | `RailPanel` 兼容共享端点且反向的双折线；`ArcBalustrade` 移除冗余 `guide_end` 端口与 `SampleCurve` 节点；L3 节点支持面板配置输出属性名 | 已实现 |
| **Batch 3（S-SOP-3）** | `fail("...", [layer,] index)` 结构化元素诊断、16 B 冷路径 `BusinessFailure` 与 `parallel_for` 首错原子捕获、原生 `BorrowSafe` 元素错误透传、`Evaluator`/`graph.inspect` 结构化 `diagnostic`；宿主接受退出时记录触发入口与在途任务 ID | `ArcBalustrade` 第 17 个截段区间反转时 Control 直接定位该元素与源码行列；编辑器退出日志完整可追溯 | 已实现 |
| **Batch 4（S-SOP-4）** | 模块级 `.rdata` 零分配只读常量数组/结构体表；全局函数与 `struct`/`enum` `[DESC]` 元数据、`Program::functions()` 导出与 `graph.inspect {"view":"library"}` | `ArcBalustrade` 28 站车削母线与石工截面改为零分配常量表；Control 可直接查询 `orient`/`remap`/`spatial` 等公共函数签名、优先轴与 `parallel_safe` | 待实施 |
| **按需子项（S-SOP-1b）** | 内联 FMA 多项式 `math.tan`、`math.asin`、`math.acos`（含 `float`/`double` 归约与 `material_footprint` 导数保护） | 独立数值精度扫描与材质局部足迹验证，不阻塞前四批推进 | 候选按需 |

---

## 各批次精确契约与实现落点

### Batch 1（S-SOP-1）：基础数学运算、字面量与 `orient` / `remap` 标准库

#### 1.1 编译器、JIT 与材质 HLSL 契约（`language` / `jit` / `source`）

- **单标量广播构造**：`vector2/3/4(s)` 与 `vector2d/3d/4d(s)` 要求标量实参与目标向量分量精度严格一致，不开放隐式 `float ↔ double` 或整数转浮点；同步支持 `class` 向量字段默认值常量折叠。
  - JIT（[math_lowering.hpp](../../../modules/execution/jit/src/detail/math_lowering.hpp)）：降低为 `CreateVectorSplat`。
  - 材质子集（[material.cpp](../../../modules/execution/language/src/material.cpp)）：`Coverage` 将单通道依赖复制到全部向量通道；HLSL 生成 `((floatN)(arg))`（微分通道对 `.v/.d0/.d1` 分别转型），规避 DXC 对 `float3(scalar)` 构造语法的报错。
- **同形状显式精度转换**：`vector2/3/4[d](v)`、`quaternion[d](q)`、`matrix4[d](m)` 接受同形状 `float` 或 `double` 数学值（含同精度恒等转换），与原生 `atelier::math` 的 `explicit operator Basic...<U>()` 对齐。JIT 直接对 `<N x float/double>` 执行 `CreateFPCast`；材质子集仍沿既有类型检查拒绝任何 `double` 子表达式。
- **逐分量乘除**：新增 `math.cwise_mul(a, b)` 与 `math.cwise_div(a, b)`（支持同精度向量或单侧同精度标量广播，接入 CPU JIT 与材质 HLSL/`ms_dmul`/`ms_ddiv`）。保持运算符 `*` 不作用于 `vector * vector`，使 `*` 在四元数和矩阵上维持纯粹的代数乘积语义。
- **模长平方、距离与角度弧度**：
  - `math.length_squared(v)`（支持向量与四元数，返回 `dot(v, v)`，不做预缩放）、`math.distance_squared(a, b)`（`b - a` 仅求值一次）、`math.distance(a, b)`（复用 `MathLowering::norm(b - a, false)` 最大分量预缩放，与 `math.length(b - a)` 精度严格一致）。三者在 `MathOperation` 中归入归约组，使材质 `Coverage` 正确收集全部分量依赖并输出单标量通道。
  - `math.radians(x)`、`math.degrees(x)` 作为逐分量运算支持 `float`/`double` 标量与向量；`math.PI`、`math.TAU`（`float`）与 `math.PI_D`、`math.TAU_D`（`double`）在解析器 `atom()` 中生成精确 `literal` 节点，自动支持顶层 `const`、类字段默认值与材质着色器。
- **整数域 `min` / `max` / `clamp` 与有符号 `abs`**：
  - `math.min`、`math.max`、`math.clamp` 新增同型 `int` 与 `uint` 支持，沿用二元整数运算的无类型非负常量适配规则（如 `int idx` 可直接写 `math.clamp(idx, 0, count - 1)`，混用已定型 `int` 与 `uint` 仍编译期拒绝）；降低为 `llvm.smin/smax/umin/umax`，`clamp(x, lo, hi)` 保持 `max(lo, min(x, hi))`（`lo > hi` 得 `lo`）。
  - `math.abs` 新增 `int` 重载（拒绝无意义的 `uint`）：**明确规定 `x == -2147483648`（`INT_MIN`）报告 `integer_overflow` 执行错误**（在 `SourceLowering` 中经 `check_value(x != INT32_MIN)` 保护后再调用 `llvm.abs.i32`，绝不静默返回负值或触发 LLVM `poison`）。
- **最近偶数舍入 `math.round`**：
  - 定义固定为 **IEEE-754 round-ties-to-even**（`round(+0.5) = +0.0`、`round(+1.5) = +2.0`、`round(-0.5) = -0.0`、`round(-1.5) = -2.0`，保留 `±0`、`±Inf`、`NaN`）。
  - 降低为 `llvm.roundeven.*`（在 FMA3/SSE4.1 目标基线上对应单条 `vroundps`/`vroundpd` 指令，零 CRT 外部符号依赖、支持满宽 AVX2 循环向量化），并与材质 HLSL `round()`（DXIL `Round_ne`）语义完全一致；`material_footprint` 视为分段常量（导数为零）。
- **整数字面量词法扩展**（[lexer.hpp](../../../modules/execution/language/include/language/lexer.hpp)、[source.cpp](../../../modules/execution/language/src/source.cpp)、[declarations.cpp](../../../modules/execution/language/src/declarations.cpp)）：
  - 支持十六进制整数字面量 `0x`/`0X`（不进入浮点 `.` 或 `e/E` 分支）以及十进制/十六进制整数尾缀 `u`/`U`。
  - 带 `u`/`U` 尾缀时标记 `typed_constant = true`（严格定型为 `uint`，拒绝赋给 `int` 或取一元负号）；无尾缀 `0x...` 与十进制整数字面量共用相同的范围与 `int` 上下文适配规则；`enum` 项同步支持十六进制与 `u`/`U`。

#### 1.2 引擎 `.source` 标准库契约（[orient.source](../../../assets/engine/procedural/orient.source) 与 [remap.source](../../../assets/engine/procedural/remap.source)）

全部函数均为纯值 `parallel_safe` 函数，遵循项目右手系、行向量矩阵（旋转矩阵第 0/1/2 行为局部 `+X/+Y/+Z` 世界像）与 Hamilton 四元数乘积（`rotate(a * b, v) == rotate(a, rotate(b, v))`）约定：

- **`procedural.rotation_between(vector3 from, vector3 to) -> quaternion`**：
  - 任一输入非有限时返回非有限 `quaternion(NaN, NaN, NaN, NaN)`；任一输入为零向量时返回单位四元数 `(0, 0, 0, 1)`。
  - 单位化后若反向共线（`1 + dot(a, b) < 1e-7f`），取 `ref = abs(a.y) < 0.9f ? (0,1,0) : (1,0,0)`、绕单位轴 `normalize(cross(ref, a))` 旋转 `180°`——保证 `from = +Z, to = -Z` 时得 `(1, 0, 0, 0)`，与既有 `procedural.shortest_arc` 逐位一致；`from = +Y, to = -Y` 时得 `(0, 0, 1, 0)`，保持 `+Z` 前向不翻转。
- **`procedural.align_to_up(vector3 up, vector3 forward) -> quaternion`**：
  - 与 `align_to_normal`（优先保证 `+Z` 沿法线）显式分工：**优先保证局部 `+Y` 严格平行于 `up`**，局部 `+Z` 取归一化后的 `forward` 在垂直于 `up` 平面上的正向投影（`x = normalize_or_zero(cross(y, normalize_or_zero(forward)))`，`z = cross(x, y)`）。
  - `up` 非有限返回 `NaN` 四元数；`up == (0,0,0)` 返回单位四元数；当 `forward` 为零、与 `up` 共线或非有限导致 `x == (0,0,0)` 时，退化为 `procedural.rotation_between(vector3(0.0f, 1.0f, 0.0f), y)`（因此 `up = +Y` 时精确退回单位四元数 `(0,0,0,1)`，坡面法线 `y` 按无扭转最短弧倾斜）。
- **`procedural.unit_axis_angle(vector3 unit_axis, float radians) -> quaternion`** 与 **`procedural.euler_degrees(vector3 degrees) -> quaternion`**：
  - 前者直接暴露原生 `math::from_axis_angle`（调用者提供单位轴与弧度角）；后者在 `double` 域按外旋 `X → Y → Z`（`q_z * q_y * q_x`）合成后再窄化为 `quaternion`，与 `procedural.Transform.rotate` 完全同源。
- **`procedural.unlerp` / `fit` / `fit01` / `efit`**：
  - 统一采用 `double` 中间差商消除有限 `float` 端点相减溢出，且**对任一非有限输入参数统一返回 `NaN`**（显式检查有限性，避免 GLSL `math.clamp(NaN, 0, 1) -> 0` 把非有限输入静默变成 `out_min`）。
  - `unlerp(value, in_min, in_max)`：全有限且 `in_min == in_max` 时返回 `0.0f`，其余返回无夹值比例。
  - `fit(value, in_min, in_max, out_min, out_max)` 与 `fit01(value, out_min, out_max)`：全有限且 `in_min == in_max` 时返回 `out_min`；其余将比例 `t` 夹取到 `[0, 1]`（自动兼容 `in_min > in_max` 的反向区间），按 `float((1.0d - tc) * double(out_min) + tc * double(out_max))` 计算，保证两端点零 ULP 漂移。
  - `efit(value, in_min, in_max, out_min, out_max)`：不夹值线性外推（全有限且 `in_min == in_max` 返回 `out_min`）；`procedural.Remap` 节点复用同一宽精度映射核心，并保留其既有的 `input_min == input_max` 或结果超出有限 `float` 时整节点 `fail` 的合同不变。

---

### Batch 2（S-SOP-2）：只读图元拓扑、折线弧长访问与 `self.<String>` 只读借用

#### 2.1 `procedural.Node` 只读拓扑与弧长接口（[geometry.hpp](../../../modules/engine/procedural/include/procedural/geometry.hpp)、[graph.hpp](../../../modules/engine/procedural/include/procedural/graph.hpp)、[curve.cpp](../../../modules/engine/procedural/src/curve.cpp)）

由于 `Geometry` 在等角数图元（包括单条折线、纯三角网格、纯四边网格）下省略 `offsets_` 缓冲（`offsets_.empty() && uniform_ > 0`），且内部 `PrimitiveKind` 为 1 字节枚举，采用 `procedural.Node` 上的只读 `AFUNCTION(BorrowSafe)` 方法按需零拷贝/小缓冲读取最为紧凑，无需扩张编译器参数角色：

- 注册 32 位枚举 `procedural.PrimitiveType : uint32_t { polygon = 0, polyline = 1 }` 与平凡记录 `procedural.PrimitiveSpan { uint32_t first_vertex; uint32_t vertex_count; PrimitiveType kind; }`。
- 新增 `AFUNCTION(BorrowSafe)` 只读方法（均不触碰输出 `NativeCook`，对同一次求值内的 `[Port] const Geometry&` 输入零拷贝读取；单个输入端口可同时绑定 `[Port] const Geometry& g` 与 `[In("P", Port = "g")] const Array<vector3>& P`）：
  1. `uint count_elements(const Geometry& geometry, Layer layer)`：返回指定层元素数。
  2. `PrimitiveSpan primitive_span(const Geometry& geometry, uint primitive)`：经 `geometry.primitive(primitive)` 统一还原 `{first, count, kind}`，图元越界返回 `invocation_failed`。
  3. `void read_primitive_points(const Geometry& geometry, uint primitive, Array<uint>& points)`：将图元 `primitive` 的有序顶点对应点下标写入调用方 `points`（覆盖旧内容；越界报错，不分配中间 `std::vector`）。若调用方在进入 `parallel_for` 前将该局部 `Array<uint>` 准备好，现行编译器已支持在 `parallel_for` 内只读索引该数组。
  4. `float polyline_length(const Geometry& geometry, uint primitive)`：校验图元存在且为 `polyline`，与 [curve.cpp](../../../modules/engine/procedural/src/curve.cpp) `measure_edges` 共用同一套 `double` 逐段累加、零长段跳过、非有限/溢出及总长零值拒绝逻辑（不分配 `ArcEdge` 数组），返回 `static_cast<float>(length)`（米），与 `PolylineArc::distance(float)` 的末端精确吸附规则逐位兼容。

#### 2.2 同步值区间内只读借用 `self.<String 字段>`（[bridge.cpp](../../../modules/execution/source/src/detail/bridge.cpp)、[source.cpp](../../../modules/execution/language/src/source.cpp)、[source_backend.cpp](../../../modules/execution/jit/src/source_backend.cpp)）

- 在 `bind_type` 与 `validate_environment` 中向 `self` 目录开放 `ValueType::string` 字段，严格标记为**只读**（`writable_storage() == false`，禁止赋值、`.clear/.append` 或传给 `String&`）。
- 允许在同步值区间内执行 `self.attr.size()`、`self.attr[i]` 以及将 `self.attr` 作为 `const String&` 传给同模块 `.source` 函数与 `BorrowSafe` 原生方法（如 `self.set_float(self.attribute, Layer::point, values, 0u)`）；凡访问 `self.<String 字段>` 的函数自动置 `uses_scoped_values = true` 以强制 `preserves_borrows` 检查，且只读借用不计入对 `self` 的写阻断。
- **并发与生命周期安全**：`ExecutionScope` 期间禁止 GC 且对象槽不搬迁；`NativeCallback` 期间 `Runtime::write_at` / `restore` 已拒绝改写非平凡字段；在 `validate_parallel_loops` 中明确拒绝在 `parallel_for` 体内直接访问 `self` 的 `String` 字段。

---

### Batch 3（S-SOP-3，已实现）：元素级结构化 `fail(...)` 诊断与宿主退出审计

#### 3.1 结构化失败与 `parallel_for` 首错捕获（`runtime` / `language` / `jit` / `source` / `procedural` / `editor`）

- **分层解耦表示**：在 [runtime.hpp](../../../modules/execution/runtime/include/runtime/runtime.hpp) 的 `atelier::Error` 上新增不依赖 `procedural` 的可选字段 `std::optional<ErrorElement> element`（`struct ErrorElement { std::string layer; std::uint32_t index; };`）。
- **`.source` `fail` 语法扩展**：
  - `fail("message");`（无元素定位）
  - `fail("message", index);`（`index` 为 `uint` 或 `int`；若所属节点声明了 `[RunOver]` 层字段，`Evaluator` 在未显式给层时补齐当前运行层名称）
  - `fail("message", layer, index);`（`layer` 为底层类型 `uint` 的非 flags 枚举表达式，如 `procedural.Layer::primitive`；运行时在错误出口通过枚举反射表还原层名）
  - 若运行期 `int` 型的 `index < 0` 或 `uint(index) == 0xffffffffu`（`Geometry` 保留哨兵），不掩盖作者的 `fail` 消息，仅省略无效 `element` 索引（或记明越界）。
- **零热路径开销的 16 B `BusinessFailure`**：
  - `BusinessFailure`（[source_backend.hpp](../../../modules/execution/jit/internal/jit/source_backend.hpp)）扩为 16 字节 `{ reason, location, layer, element }`（默认哨兵 `0xffffffffu`），仅在 `Statement::Kind::fail` 的冷失败边求值并写入，正常循环与算术热路径不增加任何指令。
  - `parallel_for_service`（[bridge.cpp](../../../modules/execution/source/src/detail/bridge.cpp)）在栈上 `ParallelCall` 内增设 `std::atomic_flag failure_claimed` 与 `BusinessFailure first_failure`，首个失败分块原子写入完整 16 字节现场后取消任务组，工作线程零堆分配、零 `Runtime` 访问。
  - 原生 `NativeFailure` 同步拷贝 `Result` 错误中的 `Error::element`；`procedural::read_curve_slices` 在区间校验失败时附带问题区间序号 `i`。
- **求值器、Control 与 UI 呈现**：
  - 修复 [evaluator.cpp](../../../modules/engine/procedural/src/evaluator.cpp) 生成 `state.root_error` 时遗失 `source` 与 `element` 的问题，使 `display_error()`、`target_error()` 及 `AssetNode` 子图错误保留最上游根因的源码位置与元素坐标。
  - [editor_control.cpp](../../../modules/editor/application/src/application/editor_control.cpp) `graph.inspect` 保持 `status.error`、`display_error`、`inspected_error` 字符串字段完全兼容，并在失败节点 `status` 与顶层附加 `diagnostic: { message, node, source?: { path, line, column }, element?: { layer, index } }`；编辑器节点与状态栏提示同步附带简洁元素与行列后缀。

#### 3.2 编辑器退出入口与在途任务日志（[window.cpp](../../../modules/editor/application/src/application/window.cpp)、[application.cpp](../../../modules/editor/application/src/application/application.cpp)、[editor_control.cpp](../../../modules/editor/application/src/application/editor_control.cpp)）

- 在 `Window` 区分关闭请求来源（`wm_syscommand_close`、`wm_close`、`wm_quit`、`operator:app.quit`；取消关闭时复位）。
- 在 `Application` 确认关闭、销毁 `EditorControl` 之前，以 `logging::Severity::info` 记录一条 `application.closing` 结构化日志，包含 `quit_source`、`play_running` 以及当前在途 Control 任务的 `active_control_job_id` / `kind` / `phase`（若存在），既不触发日志冒烟的错误门禁，又能在外部关闭窗口打断长耗时导出时直接定位来源。

---

### Batch 4（S-SOP-4）：零分配只读常量表与 Control `.source` 库自省

#### 4.1 模块级 `.rdata` 只读常量表（[source.cpp](../../../modules/execution/language/src/source.cpp)、[source_backend.cpp](../../../modules/execution/jit/src/source_backend.cpp)）

- 顶层 `const` 扩展支持内建数学值、平凡 `struct`（`{ f0, f1, ... }`）及元素满足 `parallel_value_type(T)`（标量、枚举、数学值、平凡 `struct`）的只读数组：`const Array<vector2> PROFILE = { vector2(0.100f, 0.130f), ... };`。
- 编译期将各元素常量表达式折叠为按真实 `TypeLayout` 对齐的字节表；该常量表达式定型为只读 `Array<T>`（`writable_storage() == false`，拒绝任何修改方法或可变 `Array<T>&` 绑定）。
- JIT 将元素字节发射为模块内部只读全局变量 `@__source_const_data.<id>`，并配套发射 24 字节只读数组头 `@__source_const_array.<id> = { ptr @__source_const_data.<id>, i64 N, i64 0 }`（与 `atelier::Array<T>` 及合成 `DescribedArray` 的只读内存布局完全一致）：
  - 每次 SOP 求值**零堆分配、零析构清理**；
  - `PROFILE.size()` 直接折叠为常量 `N`，`PROFILE[i]` 边界检查后直接 GEP 读取 `.rdata`；
  - 可直接作为 `const Array<T>&` 传给 `.source` 函数与 `self.emit_*` / `self.read_curve_slices` / `self.set_*`，并可在 `parallel_for` 内免捕获、免别名重叠检查安全并发读取。

#### 4.2 `[DESC]` 元数据扩充与 `graph.inspect {"view":"library"}`（`language` / `source` / `editor`）

- 放开 `.source` 语法中对顶层全局函数、`struct`/`enum` 类型声明及 `struct` 字段的 `[DESC = "..."]` 属性：
  - `struct` / `enum` 及其字段的 `DESC` 直接发布进 `Runtime` 的 `TypeDefinition::description` 与 `FieldDefinition::description`；
  - 顶层全局函数在 `Program` 编译模块（含 `engine/procedural` 单元）时收集为只读 `source::FunctionInfo`（包含限定名、返回与参数类型签名、形参名、`parallel_safe`、可写引用是否改变容器结构以及 `description`；仅修改函数 `DESC` 不改变 ABI，保持热更兼容）。
  - 为 `assets/engine/procedural/*.source` 的公开库函数补齐单位、优先轴、区间夹取/外推差异及退化回退说明（未写 `DESC` 的引擎内部辅助函数如 `bvh_stack16_set` 默认不在公共摘要中展开，显式按名查询时仍可检视）。
- 在 [editor_control.cpp](../../../modules/editor/application/src/application/editor_control.cpp) 的 `graph.inspect` 中新增只读视图 `{"view": "library", "prefix"?: "...", "symbol"?: "..."}`（无须打开图即可调用，不新增第 7 个图级工具）：
  - `functions`：来自 `Program::functions()` 的引擎公开 `.source` 函数与工程全局函数；
  - `node_methods`：来自 `Runtime` `procedural.Node` 的原生 `BorrowSafe` 方法（`count_elements`、`primitive_span`、`read_primitive_points`、`polyline_length`、`read_curve_slices`、`emit_*`、`set_*`）；
  - `value_types`：`procedural.*` 与工程声明的 `struct` / `enum` 字段及枚举项说明。

---

## 延后项与明确不做的边界

- **待新案例驱动再定型的随机与噪声扩展**：现有 `procedural.random_vector`（`[-1, 1)^3` 立方体分布）与 `sample_value_noise` 属于已发布生成内容契约，保持逐位不变。球面均匀方向、圆盘面积均匀采样待首个实际散布/植被案例接入时新增独立函数；公共多倍频 `fbm` 待第二个地形或风化资产需要共享同一倍频、幅值衰减与归一化区间时再固化参数。
- **内联 FMA 反三角与正切（`S-SOP-1b`）**：由于 `.source` 精确超越函数不链接外部 CRT 符号且承诺在支持 FMA3 的主机上标量与 SIMD 循环逐位一致（见[超越函数契约](../../math.md#超越函数)），`math.tan`、`math.asin`、`math.acos` 必须以无调用内联 minimax FMA 归约实现并完成 ULP 边界核验与材质微分奇点保护，单独排片验证。
- **不引入的语言特性**：不开放隐式 `int ↔ float` 或 `float ↔ double` 转换，不开放 `vector * vector` 运算符重载，不为单一已跑通内核引入可变定长栈数组语法，也不引入通用函数重载或闭包。
