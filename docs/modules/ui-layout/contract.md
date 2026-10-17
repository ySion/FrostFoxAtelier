# 自研布局核心：生产合同

状态：基础消费链、共享列/换行/查询、原生Flex与二维Grid均已落地。生产 Interface 为 [`src/ui_layout/layout.hpp`](../../../src/ui_layout/layout.hpp)，实现为相邻 `layout.cpp`及其私有算法文件；新增合同见[Flex/Grid](flex-grid.md)，实际运行证据见[验收记录](acceptance.md)。本页是当前行为合同；必要的历史实测与上游来源见[研究入口](../../research/ui-layout-reassessment/README.md)。

## 1. Module、目标与依赖

`atelier::ui_layout::LayoutDocument` 将一棵有序树、确定 viewport 和同步叶测量转换为完整逻辑几何。实现为自有 C++23 内核，不使用 Yoga/Taffy/Clay，不依赖 Runtime、LLVM/JIT、RHI、Windows/文字头文件或任务系统。正式静态目标为 `atelier_ui_layout`，公开包含路径为 `ui_layout/layout.hpp`。

进程沿项目正式入口使用共享 mimalloc；这不改变库、调用者和 Adapter 的所有权。DirectWrite 由[生产 UI 的文字服务](../ui/README.md#cpu绘制与文字寿命)和真实验收 Adapter 使用，布局核心不链接它。最小消费者不链接 DirectWrite、Runtime、LLVM 或 RHI；具体构建图与运行情况见验收。没有验证非 Windows 构建。

## 2. 生命周期、身份与结构编辑

- Document 同步、单写者，不可复制/移动。构造时传入固定 `Measure` 函数、非拥有的 context 和明确 `Limits`。Document、provider、context 以及被测内容必须在整个计算期间保持有效；内容和测量规则的版本在计算期间不得改变。
- 根随 Document 创建，根深度0、计入节点容量。`root()` 返回稳定局部身份；根不能移除、移动或通过普通 `update/input` 操作。`set_root(RootInput)` 设置根容器种类（row/column/overlay）、padding、gap及可选FlexSpec flow；flow只适用于row/column，overlay要求gap为0且不设置flow。根框尺寸唯一由 `compute(viewport, provider_revision)` 给定。
- `LayoutNodeId` 私有表示为 Document identity＋slot/generation，不是业务对象、Runtime ObjectId、序列化ID或可持久地址。跨 Document、空ID及失效代际均返回 `invalid_node`；原生地址复用不会复活旧身份。
- `create(parent,input,position)` 在指定容器插入节点；默认追加。`update` 更新整个纯值输入，`input` 按值读取普通节点输入。叶不能拥有孩子；有孩子的容器不能直接改成叶。
- `move(node,parent,position)` 保留整棵子树的活身份。**position 是先移除该节点后、目标孩子序列中的插入位置**；默认追加。拒绝环、叶父、非法位置以及任一后代将超深的移动。
- `remove` 移除整棵子树并使所有旧身份失效。可复用slot推进generation；代际耗尽的slot永久退役，不回绕。Document identity、输入revision和发布revision也不回绕。极端耗尽分支不是已通过数十亿次迭代的实测承诺。
- 单次编辑先完成校验和可能失败的分配，再提交逻辑变化；失败不改变输入revision/结构。成功编辑只推进输入revision，旧发布仍可读取。多次调用可合并到下一次compute，但不承诺跨多次成功调用的批次回滚。完全相同的update/set_root和实际顺序不变的move不推进revision。
- 测量回调期间所有编辑及计算重入返回 `busy`；读取旧view/input/stats允许。**从回调销毁 Document/provider 是违反寿命前提，不是可以事后返回busy的操作。** 当前不提供close、并发编辑或异步布局。
- `NodeInput.collapsed` 保留该节点及子树的输入和身份，但计算时整棵子树不参与测量、间距或几何。`node_count()`仍包含折叠节点。恢复后按当前版本/约束使用已有缓存；折叠不是绘制可见性或业务对象销毁。
- 含列配置的描述在编辑提交前完成可能失败的复制；分配异常不会留下半份新描述。

### 完整树批量提交

`set_tree(span<const TreeNodeInput>)` 在同一节点池中提交最终树，返回与输入一一对应的节点 ID。每项包含可选旧 `node`、父项下标和现有 `NodeInput`；不描述隐式 root，也不修改其设置。

- 空 `node` 创建节点，活的旧 ID 保留身份。没有出现在批次中的旧节点全部退役；保留的孩子可以移出被删除的祖先，祖先与后代也可在合法最终树中交换上下关系。
- `parent` 为空表示 root，否则必须引用本批较早的条目；同父条目的出现次序就是兄弟顺序，不要求整份输入严格深度优先。预检拒绝重复、外来、失效或 root 身份、向前/自身父引用、最终叶父和超深结构。
- 容器改成叶以最终孩子序列为准。容量包含永久退役槽；新节点可复用本批省略的节点槽位，并推进代际。保留节点的 ID 不变，旧 ID 不会复活。
- 全部校验、描述复制、结果存储和容量准备在逻辑提交前完成。失败保持输入、身份、输入 revision、几何失效标志和旧发布；准备阶段的缓冲容量与分配统计可以增长。`Error.tree_index` 在能够归属到具体输入条目时返回其下标。
- 同值且同结构的完整批次不推进 revision。有效变更只推进一次输入 revision，并使几何失效；下一次 `compute` 全部成功才发布几何和新测量缓存。普通内容属性更新仍优先用 `update`，保留原有几何复用路径。

拓扑处理时间和临时索引空间为 O(P + N)，P 是节点池历史槽位数，N 是本批节点数；另计描述比较及发生变化的描述复制成本。只覆写改变的父节点孩子序列，不逐项查找、移位或遍历旧子树。该复杂度是实现分析，尚不构成整次 UI 准备耗时的实测结论。

## 3. 尺寸、约束与依赖顺序

外框和局部坐标使用有限 `float` 逻辑值，padding属于外框。宽高、padding、gap、min/max非负；对齐系数在[0,1]，锚点/pivot/offset允许有限的外推或负值。核心不做像素舍入。返回矩形只保证各字段有限，不承诺任意深树的绝对坐标、极大坐标的像素精度或渲染可见性。

| 轴规则 | 含义 |
|---|---|
| `Axis::fixed(size)` | 确定外框大小；不符合min/max或小于padding时拒绝，不暗中改写 |
| `Axis::content(min,max)` | 该容器的独立内容需求，再应用显式界限；不隐含收缩到父可用空间 |
| `Axis::expand(weight,min,max)` | 正有限权重；主轴参与整池分配，交叉轴使用父给定的可用区域 |

未给maximum就是无显式上限；不接受Inf作为上限，也不使用魔法大数模拟无界查询。`Axis.value` 对Fixed为尺寸、对Expand为权重、对Content必须为0。硬minimum至少覆盖padding。

只有measured节点接受文字content字段。`NodeInput.gap` 用于row/column的主轴项间距、table的行间距以及wrap的行内项间距；`cross_gap` 用于table的列间距、wrap的行间距，以及显式设置flow的row/column的Flex分行间距。`Kind::grid` 只使用 `GridSpec.column_gap/row_gap`，其 `NodeInput.gap/cross_gap` 必须为0。overlay、box和measured的gap/cross_gap也必须为0；未适用的定位字段按后文各自模式解释。

### 主轴整池分配

从父内容空间扣除Fixed/Content项及普通流间距，剩余池 `P=max(0,available-fixed-gap)`。Expand槽满足 `s_i=clamp(λ×weight_i,min_i,max_i)`，上下限允许时总和等于P；这不是“每项先给min，再分剩余”。min总和过大时保持硬最小值并溢出；所有项到max后保留空白。

槽大小与外框是否填满分开：horizontal_alignment/vertical_alignment使用Alignment::fit(offset)时按独立自然需求限制实际框，并以offset在槽内摆放；Alignment::stretch(offset)保留填满政策。Fixed/Content框不因fill标志变成Expand。兄弟重排按相同身份及浮点容差比较，不保证跨机器bitwise一致。自然内容不是Expand的隐式硬min；需要不压缩的内容应选择Content或给出显式min。Axis::flex的basis/grow/shrink与字段迁移见[Flex合同](flex-grid.md#flex)。

### 具体容器与求解

- row沿横向累加普通流槽及gap，column沿纵向累加；交叉轴按align定位。普通流的anchor/pivot/offset不参与定位。
- `out_of_flow=true` 或父为overlay时排除普通流分配及自然需求汇总，以父**内容区域**计算 `padding + content_size×anchor - box_size×pivot + offset`。没有兄弟依赖或任意约束图。
- overlay只接受Fixed或填满型Expand，由父框确定尺寸；不以子项反推自身Content，也不支持需要其独立自然需求的组合。直接非法模式在编辑时拒绝；依赖组合无法求解时compute返回 `unsupported_dependency`，保留旧发布。
- 按需请求独立宽度，再解析实际宽度、所需自然高度、分配高度和位置。遍历及删除用显式存储，不随树深度递归C++调用栈。明确的Fixed高度或确定且填满的Expand高度不因存在文字就查询未使用的自然高度；Content祖先所需的贡献仍要测量。
- Content容器行高可以超过viewport；只有显式Fixed/max改变框高。裁剪、滚动、省略、文字绘制和稳定滚动条反馈归调用侧。首基线已接入同一测量合同；比例反馈和一般循环约束求解未提供，可解范围见[Flex/Grid](flex-grid.md)。

### 共享列 `table`

`NodeInput::table(columns, column_gap, row_gap)` 使用非空的 `Axis` 列规则；`columns` 只允许用于 table。普通流孩子必须是 `row`，宽度为默认 `Axis::expand()` 且填满、水平padding和行自身gap为0。垂直padding与行高仍可设置。根继续使用row/column/overlay；table作为普通容器接入。

- 各行的非out-of-flow孩子依次对应列。折叠单元保留列序号但不贡献自然需求；折叠行整行退出。缺失的末尾单元保留空列，超过列数明确报错。行和单元的out-of-flow孩子走既有锚定规则。
- Fixed列使用确定宽度；Content列取全部参与单元的最大独立外宽，再应用列min/max；Expand列按既有整池权重规则分配剩余空间。Content列的max限制最终列宽，随后按这个宽度测量文字自然高度。
- 单元在列槽中继续应用自身Fixed/Content/Expand、min/max、fill与align。显式硬尺寸可能溢出列槽；不会通过隐式裁剪或缩小硬min掩盖冲突。行自然高度为参与单元自然高度的最大值加垂直padding，table沿纵向采用既有列容器分配。
- table的独立自然宽度是各列独立需求与列间距之和；Expand列在该查询阶段使用参与单元的最大自然需求，并应用自身界限。无参与单元的列仍保留其显式尺寸/min。
- 行结构与统一宽度不相容时，compute返回带行身份和`InputField::table_row`的错误，保留旧发布。单次编辑仍允许暂时不完整的表格，使批量增删可合并至下次compute。

### 换行与重复格 `wrap`

`NodeInput::wrap(item_gap,line_gap)` 的gap用于同一行，cross_gap用于相邻行。自然模式按孩子独立宽度顺序打包；Expand孩子的自然宽度限制到当前可用宽度后应用显式min/max，Content保持自然需求。超宽项独占一行并允许溢出，零宽viewport也保证前进。

正`item_width`选择等宽重复槽；`stretch_items=false`保持固定槽宽，true先计算`max(1,floor((available+gap)/(item_width+gap)))`个槽，再平分可用宽度，槽宽至少为item_width。`grid(minimum_item_width, item_gap, line_gap)`构造后一种政策，宽度必须正且有限。尾行保留同样槽宽，不再次把少数项拉满；每一行用`line_align`在[0,1]之间对齐。

宽度安排后才测量自然高度，行高取参与孩子的自然/固定高度最大值；Expand高度在该行内填满或按fill/align限制。即使wrap自身高度确定，仍按需计算各行高度。out-of-flow孩子不分行、不贡献自然大小；折叠孩子退出分行和间距。wrap的独立宽度为不换行时的槽总宽与间距，重复格已知槽宽时不额外询问未使用的叶独立宽度。

## 4. 真实叶测量与缓存

`MeasureQuery` 为内容key/revision、provider revision及可选内容宽度。宽度缺省表示独立不换行需求，存在表示确定内容宽度下的自然尺寸；宽度可以为0，**不附带viewport高度限制**。返回的内容宽度不改变已解析外框宽度。

`MeasureResult` 返回MeasuredSize或`MeasureError`；原provider可继续return Size。自然宽高必须有限非负，可选first_baseline必须有限且在[0,height]内；MeasureQuery.first_baseline表示本次需要基线。provider失败保留其32位code，并在布局Error中标明触发节点；无效尺寸或基线返回 `invalid_measurement`。普通回调异常转为 `measurement_failed`；`std::bad_alloc`继续传播，不伪装为测量成功或零框。

provider拥有实际文字、字体与排版对象；core只存纯值键和结果。所有影响度量的变化必须推进内容或provider版本。每个活节点分别缓存独立查询和最近一次确定宽度查询；查询种类、宽度、内容key/revision、provider revision共同决定命中。同一计算内的新度量放入私有日志，重复相同查询可复用；不保证每叶只调用一次。节点移除清除其已提交缓存，slot复用不继承旧节点缓存。

可变排版对象只是Adapter的实现；对象复用不等于内部shaping次数不变，也不提供旧绘制版本租期。失败只回滚布局拥有的缓存，不撤销provider自己的对象、副作用或内容更新。

## 5. 发布、借用与拥有式结果

- 几何及新测量先写工作存储。全部阶段和输出数值检查成功后，在没有后续分配/回调的提交段一起更新测量缓存与完整几何；publication只在此时推进。
- 失败不发布半树，不覆盖已发布缓存；下一次重试不能命中上次失败的新条目。错误后的输入revision仍可以领先于旧发布的input_revision，因为已成功完成的编辑不会被计算失败撤销。
- `LayoutView` 包含publication、对应input_revision/provider_revision、viewport和按可见有序树先序排列的连续Box span；根在首位，根parent为空身份，其余parent是局部身份，x/y相对父外框原点。publication为0且boxes为空表示尚未成功计算。
- view借用经过编辑和失败计算仍有效；**下一次成功发布或Document析构结束借用**。输入、viewport和provider revision均不变时直接复用同一发布，不遍历/复制几何。
- 只改变content_key/content_revision且该节点未参与上一成功几何的度量时，compute可复用几何/索引并发布新的input_revision/publication。缓存命中的度量也记录依赖；失败计算不更新依赖记录，复用发布不丢失其他节点依赖。统计见geometry_reuses。这仍是成功发布，即使地址没有变化，也按既有规则结束旧借用。
- `find(node)` 常数时间查询该次发布，返回Box指针或nullptr；以完整document/slot/generation身份校验。空、外国或该发布不存在的身份返回nullptr。输入侧删除、折叠、槽复用及失败计算不改变旧发布的查询；新发布的索引与几何在同一无失败提交段切换。索引容量按节点池高水位保留。
- `snapshot()` 显式复制几何、版本和查询索引，可跨发布及Document析构读取。快照复制/移动保留查询能力，`snapshot.view()`借用快照本身。查询索引对应原始boxes顺序，调用方若修改公开boxes则不再保证查询完整性；find仍校验边界和身份。快照不保活业务对象、字体、排版对象、渲染包或GPU资源。
- 核心不做DPI和像素处理。composition应先累加未舍入绝对边界，再根据目标政策派生绘制数据；本增量不定义clip/hit-test/half-pixel政策，也不宣称RHI渲染通过。

## 6. 资源、错误与成本

`Limits.max_nodes` 必须在[1, UINT32_MAX−1]，包含根及代际耗尽的退役slot；超限构造参数抛`invalid_argument`。`max_depth`明确给出，根深度0；创建及移动先检查节点容量/所有受影响后代深度。没有隐含产品节点数或帧预算。构造时Document身份耗尽抛`overflow_error`，其他内存分配异常正常传播。

输入校验拒绝NaN/Inf、负尺寸、非法枚举/范围及结构；计算中检查中间值/输出转换，无法表达的量返回`numeric_overflow`。这不是对所有极端数值组合的数学完备性证明。错误码及附带节点/provider_code见公开头文件。

当前编辑是真实节点池上的创建/更新/移动/删除或完整树批量提交。静态输入及没有几何度量依赖的逐项内容变化可以复用结果；其余变化仍计算参与布局的实例树，不是通用局部调度。工作数组按参与节点初始化，折叠子树保留描述但不承担逐次初始化；索引扩容、节点编辑和快照复制仍有各自成本。稳定叶缓存减少provider调用，不承诺O(变更节点数)、零分配、硬帧预算或并行。Stats的node_visits计三次可见节点尺寸/定位遍历，不包含编辑验证、先序构造、独立宽度遍历、provider内部工作和整个进程成本。

`Stats.buffer_allocations/buffer_bytes_allocated/buffer_bytes/peak_buffer_bytes`记录内部节点池、孩子数组、批量计划、工作和发布缓冲的实际分配次数、累计申请字节、当前保留字节与峰值；仍使用进程默认分配器。它们排除公开NodeInput列描述的std::vector复制、返回的 ID 数组、快照、provider、Document拥有者本身与进程其他分配，不得称为全进程或全部布局内存。移除释放孩子数组和列描述，批量复用槽位时可保留即将使用的孩子数组容量；节点池/工作/索引向量保留高水位容量，没有自动shrink或硬内存预算。

`NodeInput`提供row/column/overlay/box/measured/table/wrap/grid构造辅助；它们直接产生同一描述，不创建另一个节点体系。输入错误附带`InputField`，`describe(ErrorCode/InputField)`返回静态说明，无需分配。构造辅助的非法参数由create/update/set_tree共用的校验拒绝。

共同工程和异常规则遵循[工程指南](../../engineering.md)；本增量的真实覆盖与未测项见[acceptance.md](acceptance.md)。

## 7. 逻辑集合与可见范围

同一公开头文件中的VirtualList负责逻辑行高、前缀索引、extent、范围和滚动锚点，完整契约见[可见范围机制](virtual-list.md#virtuallist契约)。它不创建业务身份或另一个几何求解器；调用方把选出的行实例化到现有LayoutDocument，并管理自己的内容及绘制资源。未测行采用调用方估计，不能把估计extent宣称为全部文字已排版后的精确高度。
