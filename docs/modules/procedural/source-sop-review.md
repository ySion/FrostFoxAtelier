# Source SOP 四批次增强审查台账（`d1f3e687` / `c5762ba1` / `c9eba988`）

状态：`d1f3e687` 三路专项审查（逻辑链正确性 `R1-C1`–`R1-C7`、冗余/绕路收敛 `R1-D1`–`R1-D5`、性能与代码生成强化 `R1-P1`–`R1-P4`）已全部修复并通过双配置验收；`c5762ba1`（Batch 3）与 `c9eba988`（Batch 4）专项审查待开展。关联演进计划见 [source-sop-plan.md](source-sop-plan.md)。

---

## 第一轮审查：`d1f3e687`（Batch 1 + Batch 2：基础数学、标准库、只读拓扑与 `self.<String>`）

### 一、逻辑链正确性与契约漏洞（已完成）

| 编号 | 级别 | 模块 / 位置 | 问题描述与复现机理 | 修复方案 | 状态 |
| --- | --- | --- | --- | --- | --- |
| `R1-C1` | P1 | [orient.source](../../../assets/engine/procedural/orient.source) (`rotation_between`, `shortest_arc`) | `float32` 单位化后如 `from=(1,1,6), to=(-1,-1,-6)` 的 `dot(a,-a) = -0.999999881f`，`w = 1 + dot = 1.19e-7f >= 1e-7f` 跳过 `180°` 分支且 `cross(a,-a) == 0`，退化为 `0°` 单位四元数；`shortest_arc` 未守卫 `NaN` 输入且对未归一化 `(0,0,-c)` 可能返回恒等四元数。 | `rotation_between` 升至 `vector3d`/`quaterniond` 计算点积与叉积（反平行阈值 `1e-12d`）；`shortest_arc` 补齐 `math.is_finite` 守卫与负 Z 轴共线判定。 | 已完成 |
| `R1-C2` | P1 | [material.cpp](../../../modules/execution/language/src/material.cpp) (`Hlsl::expression`) | `Hlsl::expression` 对返回向量的 `Kind::math_intrinsic` 未给 `float` 标量实参加 `(floatN)` 广播转型。同一材质同时生成标量版与向量版 `ms_clamp` / `ms_lerp` / `ms_cwise_mul` / `ms_cwise_div` 时触发 DXC `-HV 2021` 重载歧义。 | 对向量结果 `math_intrinsic` 的 `ValueType::f32` 实参统一包一层 `((floatN)(arg))`（对齐 `differential_expression`）。 | 已完成 |
| `R1-C3` | P1 | [graph.cpp](../../../modules/engine/procedural/src/graph.cpp) (`inspect_node`) | 合成隐式主输入端口 `"input"` 时直接置 `explicit_input_port.push_back(true)`，导致使用 `[Out]`/`[InOut]` 的 COW 节点无法同时声明 `[Port] const Geometry& input` 或 `[Port] const Spatial& input` 读取自身拓扑/弧长/BVH（报错 `duplicate input port 'input'`）。 | 隐式 `"input"` 初始化为 `explicit_input_port.push_back(false)`，允许绑定至多一个只读 `Geometry`/`Spatial` `"input"` 端口并保持 `port_descriptions` 规则不变。 | 已完成 |
| `R1-C4` | P2 | [orient.source](../../../assets/engine/procedural/orient.source) (`align_to_up`) | 非轴对齐共线向量（如 `up=(0.3,0.9,-0.2), forward=0.7*up`）各自归一化后差 1 ULP，`cross(y,f)` 产生 `~1.5e-8f` 残差被 `normalize_or_zero` 放大为随机横滚方向，未走共线退化分支。 | 在归一化前检查 `!(math.length_squared(raw_x) > 1e-12f)`，共线及非有限 `forward` 确定性回退至 `rotation_between((0,1,0), y)`。 | 已完成 |
| `R1-C5` | P2 | [rail_panel.source](../../../assets/engine/procedural/rail_panel.source) (`RailPanel`) | 闭合折线（如 `CirclePrimitive::closed_polyline`，`order[0] == order[n-1]`）同属 `PrimitiveType::polyline`，传入同心闭合环时在环内部生成两片重合反向端盖墙，破坏封闭流形。 | 显式拦截首尾同点闭合折线或首尾同站闭合点环，拒绝并提示开放折线契约。 | 已完成 |
| `R1-C6` | P2 | [graph.cpp](../../../modules/engine/procedural/src/graph.cpp) (`set_output_column`) | `self.set_vec3("P", ...)`（或动态 `self.<String>` 参数取 `"P"`）直接覆盖 `Geometry` 的 `P` 列，绕过 L3 `validate_emitted_polygons` 平面凸性校验。 | 在 `set_output_column` 中显式拒绝保留位置列 `"P"`（L3 顶点坐标统一由 `emit_*` 提交校验）。 | 已完成 |
| `R1-C7` | P2 | [curve.cpp](../../../modules/engine/procedural/src/curve.cpp)、[source.cpp](../../../modules/execution/language/src/source.cpp)、[ModularRuinBuilding.source](../../../examples/Source/Procedural/ModularRuinBuilding.source)、[LANGUAGE.md](../../../LANGUAGE.md) | 跨批次交界耦合：(1) `measure_polyline`（`polyline_length` 等共用）四类图元错误漏填 `ErrorElement{"primitive", p}`；(2) `eval_math_lanes` 常量折叠在 `degrees`（差 1 ULP）、`min`/`max`/`clamp`、`step`/`smoothstep`、`length`/`distance`/`normalize_or_zero`（未预缩放及非有限值）、`sign(-0.0)`、`pow` 及 `rotate`/`transpose`/`transform_*` 上偏离 `MathLowering` 与 `docs/math.md`；(3) `ruin_angle` `Max = 6.283185` 比 `math.TAU` 小 1 ULP；(4) `LANGUAGE.md` 两处旧表述未同步 `self.<String>` 与 `round`。 | 一次性补齐 `measure_polyline` 的 `ErrorElement`、对齐 `eval_math_lanes` 与 `MathLowering` 逐运算合同、将 `ruin_angle` 上限改为 `math.TAU` 并同步 `LANGUAGE.md`。 | 已完成 |

---

### 二、冗余代码、绕路代码与可折叠实现（已完成）

| 编号 | 模块 / 位置 | 收敛前冗余 / 分叉 | 收敛实现 | 状态 |
| --- | --- | --- | --- | --- |
| `R1-D1` | [lexer.hpp](../../../modules/execution/language/include/language/lexer.hpp)、[declarations.cpp](../../../modules/execution/language/src/declarations.cpp)、[source.cpp](../../../modules/execution/language/src/source.cpp) | `Lexer::next` 浮点/整数后缀分支重复；`Parser::numeric_literal` 首尾两处 `u32` `from_chars` 分叉且与枚举字面量解析重复；`Parser::math_call` 百行 `if/else if` 链；常量折叠 `float`/`double` 与 `int32`/`uint32` 5 处重复循环。 | 提取 `parse_uint32_literal` 共用十进制/十六进制/`u` 后缀解析并收敛 `numeric_literal` 为单一整数出口；合并 `Lexer` 后缀判定；`math_call` 折叠为 43 项 `constexpr` 查表；常量折叠 5 处按通道/整数类型模板化去重。 | 已完成 |
| `R1-D2` | [source_backend.cpp](../../../modules/execution/jit/src/source_backend.cpp)、[program_module.inl](../../../modules/execution/source/src/detail/program_module.inl) | `value_address` 中 `Kind::field` (`ValueType::string`) 与 `expression(id)` 托管字段分支重复；向量字段默认叶子分 3 路组装。 | `value_address` 将 `Kind::field` 统一并入 `managed_value(node.type)` 的 `expression(id)` 委派；向量默认叶子收敛为单循环求值 + `resize` 广播/补零。 | 已完成 |
| `R1-D3` | [geometry.hpp](../../../modules/engine/procedural/include/procedural/geometry.hpp)、[geometry.cpp](../../../modules/engine/procedural/src/geometry.cpp)、[constructors.cpp](../../../modules/engine/procedural/src/constructors.cpp)、[curve.cpp](../../../modules/engine/procedural/src/curve.cpp)、[graph.cpp](../../../modules/engine/procedural/src/graph.cpp) | `curve.cpp` 存在 4 行透传 `measure_edges`；`graph.cpp` `primitive_span` 与 `read_primitive_points` 重复越界诊断；多处手工构造 `Array<PrimitiveKind>` 填充 `polyline`。 | 新增与 `Geometry::polygons` 对称的 `Geometry::polylines` 并统一替换 `graph.cpp` / `curve.cpp` / `constructors.cpp` 折线构造样板；移除 `measure_edges`；提取 `checked_primitive` 复用图元越界检查。 | 已完成 |
| `R1-D4` | [TerraceIsland.source](../../../examples/Source/Procedural/TerraceIsland.source)、[FortifiedOutpost.source](../../../examples/Source/Procedural/FortifiedOutpost.source) | 调用 `procedural.align_to_up` 前手工预做 `math.normalize_or_zero` 与零向量分支兜底（`align_to_up` 内部已统一归一化并确定性回退）。 | 移除 `OutpostPlacement`、`OutpostLayoutSkeleton` 门柱与 `OutpostTacticalScatter` 朝向的冗余预归一化与零向量兜底。 | 已完成 |
| `R1-D5` | [noise.source](../../../assets/engine/procedural/noise.source)、[remap.source](../../../assets/engine/procedural/remap.source)、[GalleryRoof.source](../../../examples/Source/Procedural/GalleryRoof.source) 等示例 | `noise.source` 保留单行透传 `lattice_bits` 与展开零向量；`GalleryRoof` 用 `dot(span, span)`；示例残余 22 处 `uint(0)`..`uint(3)` 旧转型；部分元素循环内 `fail` 缺元素下标。 | 内联 `math.as_uint` 移除 `lattice_bits`（同步 [README.md](README.md)）并用 `vector3()`；`GalleryRoof` 改为 `math.length_squared`；全部 22 处 `uint(...)` 字面量改为 `0u`..`3u`/`918273u`；补齐元素循环 `fail` 索引。 | 已完成 |

---

### 三、性能隐患与代码生成强化（已完成）

| 编号 | 模块 / 位置 | 瓶颈机理 | 强化实现 | 状态 |
| --- | --- | --- | --- | --- |
| `R1-P1` | [spatial.source](../../../assets/engine/procedural/spatial.source) (`raycast_surface`, `closest_surface_point`, `closest_polyline`) | 射线逐节点重复执行最多 3 次 `1.0d / d`；三类 BVH 查询均在入栈前测过子节点包围盒后，出栈时又读取 `SpatialNode` 并重做第二次完整 3D AABB 求交/距离测试。 | 对齐 C++ `bvh.cpp`：射线入口预计算一次 `inv_dir`；`BvhStack64` 改为携带 `BvhEntry { double entry; uint node; }`，出栈仅做标量比较 `top.entry > best_dist` 即可跳过被收缩距离剪枝的子树与节点内存读取。 | 已完成 |
| `R1-P2` | [graph.cpp](../../../modules/engine/procedural/src/graph.cpp)、[curve.cpp](../../../modules/engine/procedural/src/curve.cpp)、[ArcBalustrade.source](../../../examples/Source/Procedural/ArcBalustrade.source)、[GateArch.source](../../../examples/Source/Procedural/GateArch.source) | `read_primitive_points` 每次 `resize` 先清零再 `memcpy`；曲线切片 `std::vector<Slice>` 与石工/拱门多组 `Array` 未按已知容量预分配。 | `read_primitive_points` 改为 `points.clear(); points.append(...)` 复用容量并免去清零；`slice_curves`、`read_curve_slices` 及 `ArcBalustrade`、`GateArch` 按精确上界补齐 `reserve`。 | 已完成 |
| `R1-P3` | [rail_panel.source](../../../assets/engine/procedural/rail_panel.source) (`RailPanel`) | 每跨顶面非折叠检查算过 2 次叉积后丢弃重算，底面与竖直矩形侧壁/端盖逐三角独立做 3D 叉积与预缩放归一化。 | 顶面直接复用 `c0`/`c1`，底面按对应对角线取 `-top_n1`/`-top_n0`，竖直侧壁与端盖按平行四边形恒等式共享同一水平法线，每跨从 8 次叉积 + 8 次 `normalize_or_zero` 降至 2 次叉积 + 4 次 `normalize_or_zero`。 | 已完成 |
| `R1-P4` | [math_lowering.hpp](../../../modules/execution/jit/src/detail/math_lowering.hpp)、[source_backend.cpp](../../../modules/execution/jit/src/source_backend.cpp) | `norm(input, true)` 生成未使用的 `has_nan` 链，`finite`/`has_nan` 及数学 `==`/`!=` 逐通道串行归约；`math.abs(int)` 在 `INT_MIN` 守卫后仍传 `is_int_min_poison = false`。 | `norm` 在 `normalize` 分支省略 `has_nan` 并将 `finite`/`has_nan` 与数学 `==`/`!=` 收敛为 `CreateAndReduce`/`CreateOrReduce`；`math.abs(int)` 传 `true` 暴露非负值域；经评估排除 `self.<String>` 入口 `sequence_view` 缓存以保证别名重分配/SSO 生命周期安全与 `DiagnosticCounters` 契约一致。 | 已完成 |

---

## 第二轮审查：`c5762ba1`（Batch 3：元素级 `fail` 诊断穿透与宿主关窗审计）

状态：待开展。

---

## 第三轮审查：`c9eba988`（Batch 4：`.rdata` 零分配常量表、`[DESC]` 扩展与 `graph.inspect {"view":"library"}`）

状态：待开展（其中与 `d1f3e687` 数学内建交叉的 `eval_math_lanes` 常量折叠问题已提前纳入 `R1-C7` 处理）。
