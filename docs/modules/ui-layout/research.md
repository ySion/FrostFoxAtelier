# Flex/Grid 原始资料核对

访问日期：2026-10-04。核对起点：`f42d071b937ad0c9c81e2315c9a8b0ff49ddc00d`。
本记录保留该起点的算法依据和优化建议，不是待办清单；严格内部 Flex 快路径、Grid 跨度查询及批次增量已按[审计记录](audit.md)落地。当前行为由 [Flex/Grid 合同](flex-grid.md)定义。浏览器兼容不作为验收条件。

## 优先建议

### 1. Flex 快路径也应严格避开触界候选

W3C 的伸缩步骤先按权重分配，再检查 min/max 并冻结，收缩权重乘内容基准尺寸；它保证每轮冻结至少一项。逐轮扫描的直接实现最坏可有 n 轮，因此 O(n²) 是对此实现方式的推导，并非规范要求。[Flexbox §9.7](https://www.w3.org/TR/css-flexbox-1/#resolve-flexible-lengths)

本项目 `flex_allocate` 在未夹限且候选满足边界时使用 O(n) 路径，否则用断点排序和平衡权重树，时间 O(n log n)、临时空间 O(n)。这符合项目的 `clamp(basis ± λ×weight)` 契约。源码起点仍允许候选恰好等于边界，而 `allocate_slots` 已要求严格内部：极大权重的候选可能舍入到 max/min，掩盖仍应分配给小权重的剩余量。建议将两处快路径判定统一为严格内部，并用真实 Flex grow/shrink 行复现触界舍入；这是数值正确性修复，不改变公开语义。[本地分配入口](../../../src/ui_layout/flex.inl)、[共享分配](../../../src/ui_layout/layout.cpp)

CSS 在未冻结权重和小于 1 时可以只消耗部分余量；本项目正权重作为相对权重并分完整池。该差异应保留明确说明，不能因参考规范而悄悄替换既有 Expand/Flex 语义。[Flexbox §9.7](https://www.w3.org/TR/css-flexbox-1/#resolve-flexible-lengths)

### 2. 保留 sparse 单调游标与碰撞跳跃

W3C sparse 只向前搜索，不回填已越过的空洞；已锁定行的项必须放在该行此前项之后。Taffy 官方实现用 `line_area_collision_jump` 在阻挡处跳到下一候选，而非每次只推进一格。[Grid 放置算法](https://www.w3.org/TR/css-grid-1/#auto-placement-algo)、[Taffy placement.rs](https://github.com/DioxusLabs/taffy/blob/main/src/compute/grid/placement.rs)

本项目 `grid_collision` 返回实际占用格；移动轴跳到冲突格之后是安全的，因为中间候选仍覆盖该格。锁定副轴的跨度项必须推进所有覆盖轨道的游标。已有机制应保留，继续使用容量受限位表；不需要加入第二套空间索引。一次碰撞查询成本约为 `row_span × ceil((column_span+63)/64)` 次字操作，实际有边界对齐项；总成本还乘候选探测数，不能宣称自动放置恒为 O(n)。这是本地代码复杂度推导。[grid_collision / grid_mark_area](../../../src/ui_layout/grid.inl)

只有真实场景显示跨列方向的位表遍历持续占主导时，再评估按 AutoFlow 选择位表方向。当前不建议为两种方向维护两份占位状态。

### 3. 跨格批次保持同时提交，按需减少全轨扫描

W3C 的 planned increase 对每条轨道取同批项目的最大增量，再统一提交，目的就是避免项目顺序影响尺寸。相同跨度项目批处理也有原始依据。[Grid 跨格轨道求解](https://www.w3.org/TR/css-grid-1/#algo-spanning-items)、[额外空间分配](https://www.w3.org/TR/css-grid-1/#distribute-extra-space)

本地 `grid_size_axis` 采用相同的不变量，且 Fixed 不增长、max 是硬上限。应保留。当前每个不同跨度批次都清零并提交所有 T 条轨道；B 个批次额外有 O(B×T) 成本，跨度分配另有各项覆盖轨道的成本。若高轨道数、多跨度、稀疏覆盖的真实输入显著受此影响，可在同一 `grid_increases` 上复用 touched-track 列表，只清零和提交本批实际更新轨道；用首次触及标记保证每轨只入表一次。辅助存储上限 O(T)，不改批次顺序、不逐项提交。[grid_size_axis](../../../src/ui_layout/grid.inl)

这属于有条件优化，先由现有性能入口定位；轨道少、覆盖密时全轨线性扫描更简单且可能更快。

### 4. 缓存应由完整约束决定，局部调度等待真实收益

Yoga 官方增量布局说明：样式或孩子变化会标脏节点及祖先，未脏子树仅在父约束未变时才可跳过；其缓存源码进一步比较测量模式、可用尺寸与上次测量结果。[Yoga 增量布局](https://www.yogalayout.dev/docs/advanced/incremental-layout)、[Yoga Cache.cpp](https://github.com/facebook/yoga/blob/main/yoga/algorithm/Cache.cpp)

本项目同步叶测量的宽度、内容/provider 版本和基线请求都可能改变结果。建议维持 `Cached`/`MeasureQuery` 的完整身份与失败后不提交新缓存；固定尺寸叶的基线请求也必须参与失效。现有 `flex_allocate_line` 对同一次 compute 的相同池复用分配是低成本有效机制。不要只按节点未修改跳过受到新父宽度影响的文字；也不要把 Yoga 的持久树调度直接移植为第二套入口。[缓存与提交](../../../src/ui_layout/layout.cpp)、[行计划](../../../src/ui_layout/flex.inl)

通用子树跳过需同时证明父约束、共享列、基线、祖先自然需求与失败事务依赖，维护成本高于单纯缓存叶测量；本阶段优先 VirtualList 和已明确的短路。

### 5. Scratch 优化以执行链存活区间为依据

Grid 列尺寸确定后再计算各项布局是规范明确的顺序；宽度影响文字自然高度，因此省去已确定高度且无消费者的提前行求解，不意味着跳过最终文字高度或行布局。[Grid 布局算法](https://www.w3.org/TR/css-grid-1/#layout-algorithm)

本地自然 Grid 查询可能在父 `shares` 尚未收集完成时发生，独立 `grid_shares/grid_events/grid_weight_sums` 避免覆盖父的活跃数据。应保留这个真实存活区间隔离；同类型容器完成后复用容量即可，无需长期第二树或每节点分配器。纯显式放置不创建位表、无 Grid/Flex 时不创建算法状态也是现有高价值短路。[Grid scratch](../../../src/ui_layout/grid.inl)、[参与节点工作区](../../../src/ui_layout/layout.cpp)

缓冲复用节省逐帧分配，但保留高水位；折叠并不释放节点描述。这些是当前所有权取舍，不能从零新增分配推导为零内存成本。

## 保留的原生边界

- CSS 自动 min-content、分数权重部分填充、百分比和宽高循环、writing-mode、auto margins、dense 回填均不作为本阶段缺陷补齐。Flex 内容盒收缩权重和 Grid 同批增量可借鉴原理，但不意味着完整 CSS 结果等价。[Flexbox 算法](https://www.w3.org/TR/css-flexbox-1/#layout-algorithm)、[Grid 算法](https://www.w3.org/TR/css-grid-1/#layout-algorithm)
- 本项目跨格 max 是硬界；W3C 存在特定阶段允许超 growth limit 的更复杂规则。不能把规范的后续增长阶段直接加入原生 min/max。[Grid 额外空间分配](https://www.w3.org/TR/css-grid-1/#distribute-extra-space)
- 未确定高度依赖的容器基线、Grid 基线组及一般循环保留 `unsupported_dependency`。后续由真实控件需求决定能力扩展，并继续走 `LayoutDocument` 的原提交入口。

来源为 W3C 原始规范、Yoga 官方文档与官方源码、Taffy 官方源码。上游 `main` 链接用于当日核查，可能变化；本地复杂度与建议均注明为代码分析，最终收益需要项目端到端输入测量。
