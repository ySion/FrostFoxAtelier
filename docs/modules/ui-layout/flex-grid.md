# 原生 Flex 与二维 Grid

本页是Flex/Grid当前行为的权威说明，设计理由见[decisions](decisions.md)，实际验收和采样见[acceptance](acceptance.md)。这是原生UI的尺寸语义，不承诺CSS兼容。求解入口仍为LayoutDocument，核心没有新增文字库、Runtime、RHI或第三方solver依赖。

## Flex

```cpp
auto toolbar = NodeInput::row(8);
toolbar.flow = FlexSpec{
    .wrap = Wrap::wrap,
    .justify = Distribution::space_between,
    .align_items = Alignment::baseline()
};
toolbar.cross_gap = 6;
auto search = NodeInput::measured(search_key, content_revision);
search.width = Axis::flex(Basis::content(), 1, 1, 80, 320);
```

row/column通过flow选择弹性分行与对齐；孩子在父主轴使用Axis::flex时，也会启用同一算法的默认nowrap配置。RootInput也可设置flow，根宽高仍来自compute的viewport。Flex规则只能放在父主轴；放到交叉轴、Grid轨道或脱离普通流的节点上会明确拒绝。

### 尺寸

- Basis::fixed指定基准外框，Basis::content请求自然尺寸。
- grow_weight分配增长余量；shrink_weight乘以基准内容尺寸分配收缩量，padding不参与收缩权重。它们均为非负相对权重，零表示该方向不伸缩。
- min/max是硬边界，实际尺寸不小于padding。触及边界后将剩余量重新分配给还能变化的项；无法满足则保持硬边界并溢出。
- 不先将minimum加到basis上。旧Expand依然满足clamp(λ×weight,min,max)，小数权重也分完整池。
- Fixed/Content继续保持自己的尺寸规则。需要同一行的卡片等高时设置height=Axis::expand()，再使用stretch；不会隐式把Content改成Expand。

无夹限的分配使用线性路径；需要处理边界时使用断点及平衡权重和，避免反复扫描和极端权重消减。两项basis为200/100、收缩权重均1、池240时得到160/80；第一项min180时得到180/60；收缩权重改成2/1时得到152/88。

### 摆放、分行

justify支持start、center、end、space_between、space_around、space_evenly。align_lines另支持stretch。负余量的分布对齐回退到start；justify不接受stretch。gap用于行内，cross_gap用于行间。

Wrap::no_wrap、wrap、wrap_reverse决定分行及行方向；reverse改变主轴摆放方向。分行先使用受限basis，再在每行内分配空间。reverse不改变树序、业务身份或发布的先序顺序。

横向换行支持宽度决定真实文字高度。纵向换行支持根viewport或Fixed高度、确定或填满的容器宽度，孩子宽度必须Fixed/Content且独立可解。Content容器宽度、按自然需求缩窄容器，或孩子Expand宽度导致分列反向改测量宽度等组合返回unsupported_dependency并保旧。当前不声称解决一般宽高循环。

### 统一对齐描述与基线

旧fill_width/fill_height、align_x/align_y已迁移为horizontal_alignment和vertical_alignment，不保留第二套字段。两轴各用一个Alignment：

- 默认值继承Flex父的交叉轴align_items；其余情况使用stretch。
- Alignment::stretch(offset)填满可扩展槽，保留0到1的槽内偏移系数。
- Alignment::fit(offset)限制到自然尺寸，并按系数对齐；0、0.5、1分别为起端、居中、末端。
- Alignment::baseline()请求首基线，只适用于横向Flex交叉轴。显式子项设置覆盖父默认。

例：原fill_width=false且align_x=0.5改写为horizontal_alignment=Alignment::fit(0.5f)。原仅align_y=0.5、保留fill改写为vertical_alignment=Alignment::stretch(0.5f)。这是源代码字段迁移，已有场景的几何、身份与发布语义保持。

MeasureQuery.first_baseline表示本次需要基线；MeasureResult现在包含MeasuredSize，原provider仍可return Size。first_baseline从内容框顶部计量，必须有限、非负且不超过返回自然高度；缺失时回退到返回自然内容底边。空非文字盒回退到最终分配盒底；容器传播首行参与内容的基线，加上孩子实际局部y（包含padding和对齐偏移）。确定高度时复用同一排列入口解析基线，后续不重复排列。基线只在被请求时测量、纳入失效依赖，并与尺寸缓存共同发布。固定文字框在基线布局里仍可能需要度量。

尚未确定的容器高度如果会通过居中、反向或分行摆放影响传播基线，则返回unsupported_dependency；Grid/overlay的容器基线传播和Grid内基线组也未开放。可使用真实文字叶、可确定基线的自然高行或Fixed高容器组合；不会以错误近似代替缺失能力。

纵向Flex或out_of_flow项目请求首基线时返回unsupported_dependency/alignment，包括从父继承的基线。脱流项目可以显式设置vertical_alignment=Alignment::stretch()覆盖继承，再使用原锚定路径；折叠项目退出本次校验和布局。

## 二维 Grid

```cpp
auto form = NodeInput::grid(GridSpec{
    .columns = {Axis::content(80, 180), Axis::expand()},
    .rows = {Axis::content(), Axis::content()},
    .column_gap = 12,
    .row_gap = 6
});
auto help = NodeInput::measured(help_key);
help.cell = Cell{.row = 1, .column = 0, .column_span = 2};
```

grid(GridSpec)构造Kind::grid。原grid(float,...)继续构造等宽重复槽wrap，保留其Kind、尾行槽宽和输入读回语义。table也保留原来的row节点、列序号、身份与局部坐标。三者使用同一描述/发布体系，重复槽和无跨度表格保留专用快路径；二维Grid的新增算法位于同一Impl的grid.inl。

### 放置

Cell行列从0开始，两个位置均可省略；span默认1且不能为0。支持显式位置、单轴指定、全自动位置。显式项目允许重叠，自动项目避开占用；绘制和命中顺序仍由消费层决定。

AutoFlow::row/column选择自动放置方向。采用顺序前进的sparse政策，不回填当前游标已经跳过的空洞。锁定副轴的项目从起始轨此前自动放置的末端继续搜索；成功后推进其覆盖的所有副轴轨道游标。未声明的隐式行列分别使用implicit_row/implicit_column，默认Content。collapsed不参与放置和需求，out_of_flow使用原有锚定规则。Cell只对Grid父有效，其他父下的非默认Cell会明确报错。

Limits.max_grid_tracks默认4096，限制每轴轨道数；max_grid_cells默认1048576，限制自动占位区域的行列乘积，位表存储按64位对齐。纯显式放置不建立占位表，仍受轨道限制。所有位置+跨度和占位容量先校验，超限不截断输入、不破坏旧发布。

### 轨道与跨格

Fixed固定大小；Content列使用独立无换行需求，Content行使用最终列宽下的自然高度；Expand按相对权重分配确定空间的剩余量。数值min/max始终有效。没有CSS自动min-content或任意intrinsic minmax，不能把width=0当作最小可断行宽度。

跨格按跨度升序分批，同批各项读取相同初值；每轨收集最大计划增量后一起提交。Fixed不增长，达到max的轨道冻结，剩余量分给仍可增长轨道。这样结果不依赖同跨度孩子的遍历顺序，但不承诺全局最小轨道总宽。

确定尺寸轴上，跨Expand的项目不通过自然需求撑大Content轨道；先得到区域，再测量真实内容。Grid本身计算自然需求时，Expand暂按有界Content参与，最终有确定空间时再按权重分配。文字依赖保持列宽→文字高度→行高，不通过反复试排解决循环。

## 工作数据与失败

Flex/Grid只创建本次compute的行或轨道计划，不创建第二棵长期节点树。没有相应容器的旧布局不会分配该算法的专用状态数组。缓冲容量复用，统计覆盖这些工作缓冲；描述中的轨道vector、provider、文字帧和拥有式快照仍单独计量。

Grid自然需求查询可以发生在父级空间分配期间，因此其轨道分配使用独立可复用scratch，避免覆盖父级中间数据。Flex和Grid最终仍经原来的缓存/几何提交点发布；失败不提交局部结果。

自动放置在冲突时沿移动轴跳过仍覆盖该冲突格的候选位置；其复杂度与探测形状、跨度有关，不宣称总是O(N)。全显式无跨度、已确定轨道、重复槽和无饱和分配各走较短路径。比例候选触及min/max时保留断点求解，避免舍入吞掉小权重余量；确定高度的Grid省去无消费者的行求解，Flex同一行分配池不变则复用本次结果。规模较大的数据仍使用VirtualList；通用局部调度和异步求解未加入。

Grid跨度是否贡献自然需求为常数时间查询：同规则轴直接判定，混合规则轴使用本次计划的紧凑计数前缀，含隐式轨道。需求已满足的跨度不建立分配临时数据；同跨度批次只提交收到正增量的轨道，仍保持共同初值和逐轨最大增量。正缺口依然按原顺序累计轨道大小，随后构建分配输入；没有浮点前缀差分或逐项提交。

## 正式验收与复现

当前46项布局场景继续覆盖数值边界、全部分布、反向/换行、真实混字号基线、跨行跨列、自动定位、容量、嵌套、内容更新与失败保旧；Flex/Grid场景位于[flex_grid.inl](../../../acceptance/ui_layout/flex_grid.inl)，审计补充位于[audit.inl](../../../acceptance/ui_layout/audit.inl)。复合消费者用真正Grid外壳和Flex工具栏，文字仍来自真实DirectWrite。已运行版本见[审计记录](audit.md)，新增完整树批次场景的运行状态见[验收记录](acceptance.md)。

在正式环境中构建ui_layout_acceptance的dev/release目标；使用`./out/release/atelier_ui_layout_acceptance.exe --flex-grid-performance`测量新场景。结果分开报告整次操作、compute、内部测量和消费协调，不能把回调外全部工作都称为纯布局。最新数字与可执行文件记录在[验收](acceptance.md)。
