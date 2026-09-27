# 自研布局核心：生产首增量合同

状态：用户已授权按首步探索后的合同落地。生产 Interface 为 [`src/ui_layout/layout.hpp`](../../../src/ui_layout/layout.hpp)，实现为相邻 `layout.cpp`；实际运行证据唯一见[验收记录](acceptance.md)。本页上半部分是当前行为合同，页尾旧 Yoga 文本只保留为历史，不形成第二套实现要求。

## 1. Module、目标与依赖

`atelier::ui_layout::LayoutDocument` 将一棵有序树、确定 viewport 和同步叶测量转换为完整逻辑几何。实现为自有 C++23 内核，不使用 Yoga/Taffy/Clay，不依赖 Runtime、LLVM/JIT、RHI、Windows/文字头文件或任务系统。正式静态目标为 `atelier_ui_layout`，公开包含路径为 `ui_layout/layout.hpp`。

进程沿项目正式入口使用共享 mimalloc；这不改变库、调用者和 Adapter 的所有权。DirectWrite 仅在真实验收 Adapter 中使用。最小消费者不链接 DirectWrite、Runtime、LLVM 或 RHI；具体构建图与运行情况见验收。没有验证非 Windows 构建。

## 2. 生命周期、身份与结构编辑

- Document 同步、单写者，不可复制/移动。构造时传入固定 `Measure` 函数、非拥有的 context 和明确 `Limits`。Document、provider、context 以及被测内容必须在整个计算期间保持有效；内容和测量规则的版本在计算期间不得改变。
- 根随 Document 创建，根深度0、计入节点容量。`root()` 返回稳定局部身份；根不能移除、移动或通过普通 `update/input` 操作。`set_root(RootInput)` 只设置容器种类、padding、gap；根框尺寸唯一由 `compute(viewport, provider_revision)` 给定。
- `LayoutNodeId` 私有表示为 Document identity＋slot/generation，不是业务对象、Runtime ObjectId、序列化ID或可持久地址。跨 Document、空ID及失效代际均返回 `invalid_node`；原生地址复用不会复活旧身份。
- `create(parent,input,position)` 在指定容器插入节点；默认追加。`update` 更新整个纯值输入，`input` 按值读取普通节点输入。叶不能拥有孩子；有孩子的容器不能直接改成叶。
- `move(node,parent,position)` 保留整棵子树的活身份。**position 是先移除该节点后、目标孩子序列中的插入位置**；默认追加。拒绝环、叶父、非法位置以及任一后代将超深的移动。
- `remove` 移除整棵子树并使所有旧身份失效。可复用slot推进generation；代际耗尽的slot永久退役，不回绕。Document identity、输入revision和发布revision也不回绕。极端耗尽分支不是已通过数十亿次迭代的实测承诺。
- 单次编辑先完成校验和可能失败的分配，再提交逻辑变化；失败不改变输入revision/结构。成功编辑只推进输入revision，旧发布仍可读取。多次调用可合并到下一次compute，但不承诺跨多次成功调用的批次回滚。完全相同的update/set_root和实际顺序不变的move不推进revision。
- 测量回调期间所有编辑及计算重入返回 `busy`；读取旧view/input/stats允许。**从回调销毁 Document/provider 是违反寿命前提，不是可以事后返回busy的操作。** 当前不提供close、并发编辑或异步布局。

## 3. 尺寸、约束与依赖顺序

外框和局部坐标使用有限 `float` 逻辑值，padding属于外框。宽高、padding、gap、min/max非负；对齐系数在[0,1]，锚点/pivot/offset允许有限的外推或负值。核心不做像素舍入。返回矩形只保证各字段有限，不承诺任意深树的绝对坐标、极大坐标的像素精度或渲染可见性。

| 轴规则 | 含义 |
|---|---|
| `Axis::fixed(size)` | 确定外框大小；不符合min/max或小于padding时拒绝，不暗中改写 |
| `Axis::content(min,max)` | 该容器的独立内容需求，再应用显式界限；不隐含收缩到父可用空间 |
| `Axis::expand(weight,min,max)` | 正有限权重；主轴参与整池分配，交叉轴使用父给定的可用区域 |

未给maximum就是无显式上限；不接受Inf作为上限，也不使用魔法大数模拟无界查询。`Axis.value` 对Fixed为尺寸、对Expand为权重、对Content必须为0。硬minimum至少覆盖padding。纯容器/盒不接受文字content字段，gap只用于row/column；未适用的定位字段按后文各自模式解释。

### 主轴整池分配

从父内容空间扣除Fixed/Content项及普通流间距，剩余池 `P=max(0,available-fixed-gap)`。Expand槽满足 `s_i=clamp(λ×weight_i,min_i,max_i)`，上下限允许时总和等于P；这不是“每项先给min，再分剩余”。min总和过大时保持硬最小值并溢出；所有项到max后保留空白。

槽大小与外框是否填满分开：`fill_width/fill_height=false` 时按独立自然需求限制实际框，并以相应align系数在槽内摆放。Fixed/Content框不因fill标志变成Expand。兄弟重排按相同身份及浮点容差比较，不保证跨机器bitwise一致。自然内容不是Expand的隐式硬min；需要不压缩的内容应选择Content或给出显式min。

### 具体容器与求解

- row沿横向累加普通流槽及gap，column沿纵向累加；交叉轴按align定位。普通流的anchor/pivot/offset不参与定位。
- `out_of_flow=true` 或父为overlay时排除普通流分配及自然需求汇总，以父**内容区域**计算 `padding + content_size×anchor - box_size×pivot + offset`。没有兄弟依赖或任意约束图。
- overlay只接受Fixed或填满型Expand，由父框确定尺寸；不以子项反推自身Content，也不支持需要其独立自然需求的组合。直接非法模式在编辑时拒绝；依赖组合无法求解时compute返回 `unsupported_dependency`，保留旧发布。
- 按需请求独立宽度，再解析实际宽度、所需自然高度、分配高度和位置。遍历及删除用显式存储，不随树深度递归C++调用栈。明确的Fixed高度或确定且填满的Expand高度不因存在文字就查询未使用的自然高度；Content祖先所需的贡献仍要测量。
- Content容器行高可以超过viewport；只有显式Fixed/max改变框高。裁剪、滚动、省略、文字绘制和稳定滚动条反馈归调用侧。本增量不提供共享轨道、换行格、基线、比例反馈或循环约束求解。

## 4. 真实叶测量与缓存

`MeasureQuery` 为内容key/revision、provider revision及可选内容宽度。宽度缺省表示独立不换行需求，存在表示确定内容宽度下的自然尺寸；宽度可以为0，**不附带viewport高度限制**。返回的内容宽度不改变已解析外框宽度。

`MeasureResult` 返回有限非负Size或`MeasureError`。provider失败保留其32位code，并在布局Error中标明触发节点；无效尺寸返回 `invalid_measurement`。普通回调异常转为 `measurement_failed`；`std::bad_alloc`继续传播，不伪装为测量成功或零框。

provider拥有实际文字、字体与排版对象；core只存纯值键和结果。所有影响度量的变化必须推进内容或provider版本。每个活节点分别缓存独立查询和最近一次确定宽度查询；查询种类、宽度、内容key/revision、provider revision共同决定命中。同一计算内的新度量放入私有日志，重复相同查询可复用；不保证每叶只调用一次。节点移除清除其已提交缓存，slot复用不继承旧节点缓存。

可变排版对象只是Adapter的实现；对象复用不等于内部shaping次数不变，也不提供旧绘制版本租期。失败只回滚布局拥有的缓存，不撤销provider自己的对象、副作用或内容更新。

## 5. 发布、借用与拥有式结果

- 几何及新测量先写工作存储。全部阶段和输出数值检查成功后，在没有后续分配/回调的提交段一起更新测量缓存与完整几何；publication只在此时推进。
- 失败不发布半树，不覆盖已发布缓存；下一次重试不能命中上次失败的新条目。错误后的输入revision仍可以领先于旧发布的input_revision，因为已成功完成的编辑不会被计算失败撤销。
- `LayoutView` 包含publication、对应input_revision/provider_revision、viewport和按有序树先序排列的连续Box span；根在首位，根parent为空身份，其余parent是局部身份，x/y相对父外框原点。publication为0且boxes为空表示尚未成功计算。
- view借用经过编辑和失败计算仍有效；**下一次成功发布或Document析构结束借用**。输入、viewport和provider revision均不变时直接复用同一发布，不遍历/复制几何。
- `snapshot()` 显式复制纯几何和版本，可跨发布及Document析构读取。它不保活业务对象、字体、排版对象、渲染包或GPU资源；composition必须另外保持消费版本一致。
- 核心不做DPI和像素处理。composition应先累加未舍入绝对边界，再根据目标政策派生绘制数据；本增量不定义clip/hit-test/half-pixel政策，也不宣称RHI渲染通过。

## 6. 资源、错误与成本

`Limits.max_nodes` 必须在[1, UINT32_MAX−1]，包含根及代际耗尽的退役slot；超限构造参数抛`invalid_argument`。`max_depth`明确给出，根深度0；创建及移动先检查节点容量/所有受影响后代深度。没有隐含产品节点数或帧预算。构造时Document身份耗尽抛`overflow_error`，其他内存分配异常正常传播。

输入校验拒绝NaN/Inf、负尺寸、非法枚举/范围及结构；计算中检查中间值/输出转换，无法表达的量返回`numeric_overflow`。这不是对所有极端数值组合的数学完备性证明。错误码及附带节点/provider_code见公开头文件。

当前编辑是真实节点池上的创建/更新/移动/删除，但**变化后的几何仍全树计算，不是局部增量调度**。稳定叶缓存减少provider调用，不承诺O(变更节点数)、零分配、硬帧预算或并行。Stats的node_visits计三次尺寸/定位遍历，不包含编辑验证、先序构造、provider内部工作和整个进程成本。移除释放死节点的孩子存储，活节点及工作向量可复用容量；没有字节级内存预算或所有分配故障点注入证明。

共同工程和异常规则遵循[工程指南](../../engineering.md)；本增量的真实覆盖与未测项见[acceptance.md](acceptance.md)。

## 历史附录：旧 Yoga 合同，非当前要求

以下保留旧文本供研究追溯；第三方依赖、旧表示及旧范围不覆盖上述自研合同。

### 第一里程碑行为合同

> **历史合同：旧 Yoga 候选方案，已被自研方向取代。** 本页保留供反例和方案对照，不是必须兼容的产品要求，也不得直接据此实施。当前方向与首版设计见 [decisions.md](decisions.md) 的 S01–S08。

状态：以下为当时的实现要求，未发布为自研 C++ Interface。自研行为合同须依据当前方案审查后重写到本页；不得把下文第三方依赖、表示或范围自动继承为新合同。共同规则仍沿用 [engineering.md](../../engineering.md)。

#### 1. Module 与依赖

LayoutDocument 将有序树、逻辑约束和叶测量转换为完整未舍入几何快照。第一实现使用私有 Yoga；核心仅依赖标准 C++ 与该实现，没有 Runtime、LLVM、RHI、Windows/DWrite、字体库和任务系统依赖。

内容生产、文本排版、DPI、业务事件与渲染由调用侧 Adapter 负责。核心接受明确内容键/版本和同步测量 Interface，不查询全局窗口、字体或业务对象。

#### 2. 所有权与身份

| 数据/资源 | 拥有者 | 有效期与限制 |
|---|---|---|
| Yoga config/nodes/style/children/cache | 一个 LayoutDocument | 配置先建后释放，节点先释放；不公开 Yoga 指针 |
| LayoutNodeId | Document 局部解析表 | Document identity + slot/generation；不跨 Document 接受、不以裸地址识别 |
| 内容键与revision | 节点保存值，provider拥有真实内容 | 键不保活业务对象，测量期间内容可读 |
| 布局快照 | 显式拥有几何数组的结果值 | 下一次编辑/计算不覆盖；可在Document结束后作为纯值读取 |
| 字体/排版/glyph/图片 | 测量或文本 Adapter | 不由Yoga析构，不借布局root延寿 |
| 业务引用/事件入口 | UI绑定owner，位于核心外 | 复用Runtime根与执行契约，不藏在node context |
| 目标像素视图 | composition Adapter | 按target/DPI/policy和逻辑版本派生，不回写核心 |

第一版每个 Document 一棵有序树。根随 Document 存活，不能作为普通子树移除。普通节点只能有一个父，无环/共享子树；测量叶不能有孩子。创建指定父与位置；移除节点移除整棵子树，旧局部 ID 失效。重排/移动到合法父保留存活 ID，并使旧父、新父及相关祖先失效。

slot复用增加generation；代际或Document identity耗尽明确拒绝，不回绕旧身份。不是存档/网络ID，不借Runtime ObjectId，不新增业务类型表。

Yoga是唯一可变style/children/内部测量缓存权威；外层只保存保护句柄所需的owner/generation映射、内容revision与已发布几何。可以有临时批量校验结构，但没有第二份长期Widget/style/children/cache权威。

#### 3. 小而完整的 Interface

调用者只需理解五组行为：创建/结束Document；创建/移除/描述节点；提交有序父子关系与内容版本更新；对根约束同步layout；读取完整快照。无需知道上游默认配置和几十个setter，也不先做三后端工厂、一般属性反射或命令框架。

有效编辑推进input_revision；相同值的无操作编辑不推进。快照标识document、input_revision和根约束，并包含稳定顺序的节点键、父关系、未舍入box与明确的overflow信息。**box相对父外框左上角，父padding已体现在child位置里，消费方不再加一遍padding。** 根原点为(0,0)。绝对坐标从同一份未舍入父链推导，不逐级舍入；可有读取辅助函数，不维护另一份可变权威。树顺序不是z-order、绘制顺序或clip栈。

快照自身拥有值存储，不借Yoga layout内存或provider；旧快照不被新计算覆盖。节点已删除时，旧快照仍可读历史几何，但其中的旧键不能重新解析到新节点。成功布局才发布新快照，失败保留上一成功值及其原版本。

#### 4. 逻辑单位、约束与子集

输入是有限逻辑长度；auto/intrinsic/unbounded采用明确枚举或可选值，不能拿NaN/Infinity或0当哨兵，0是合法零长度。核心不绑定物理DPI；Windows Adapter可以约定DIP，但核心不查询屏幕。

根每次接受明确非负有限exact width/height。首版不承诺无限根容器。外框尺寸包含padding，不支持border/margin的CSS语义。

| 轴规则 | 行为 |
|---|---|
| fixed(length) | 外框固定，必须满足min/max及padding下限；固定项不为兄弟自动缩小 |
| intrinsic | 叶来自provider；容器来自受支持单行/单列内容、gap与padding，仍受父约束/min/max |
| fill_equal | 仅父主轴确定时，basis=0/grow=1等分剩余空间；min/max可以限制各孩子 |

暂不公开任意权重或flex简写：CSS grow总和小于1可保留余量，不等同一般权重分配。容器仅row/column、不换行；children顺序明确，不支持CSS order。四边padding非负，gap作用于主轴相邻孩子；空容器只有padding，单孩子没有gap。

交叉轴只有start/stretch；fixed优先于stretch；非fixed在stretch下采用父可用交叉轴并应用min/max，start保留测量/自然尺寸。不含baseline/center/end/space-between、RTL或writing-mode；初始为明确LTR物理坐标，不因Yoga已有枚举而全部开放。

min/max为非负有限可选外框限制，缺省无对应限制。min>max、fixed超出min/max、fixed/max小于该轴padding总和须拒绝。父空间不足以容纳孩子最小总需求时允许明确overflow：固定/最小需求保持，不产生负宽、不自动裁剪。填充外框不小于自身padding，溢出是几何结果而非layout失败，裁剪归composition。

未知父主轴下的fill_equal、测量叶带孩子、不支持枚举、非有限结果须有明确诊断。min/max饱和后的分配及intrinsic容器组合必须用真实golden场景固定为该pin的窄合同；若实际语义与本表冲突，在发布Interface前裁决，不暗改预期或宣称通用CSS兼容。

#### 5. 叶测量 seam

输入至少包括content key、content_revision、measure_revision、两轴约束模式(exact/at_most/unbounded)；输出有限非负内容width/height。padding由布局处理，provider不再加一次。首版不返回baseline/ink bounds；未来需独立定义坐标与版本。

- exact采用指定长度，at_most不得超过上限，unbounded返回自然尺寸。
- provider使用真实文本排版或图片尺寸；这些实现不进入核心，不用硬编码字宽冒充文本。
- 一次layout读取稳定内容快照；允许重复测量，不承诺一次。相同输入/版本/约束必须给相同结果。
- 明确尺寸可能不调用measure；最终box读layout输出，不能用回调发生与否驱动资源生命周期。
- 文本、字号/字体、locale、排版选项等影响尺寸的变化必须更新revision并经过节点更新触发dirty。单改void* context不通知Yoga。
- 回调期间不改Document、不调用Runtime、不释放内容/Document；借用只跨该同步调用，返回后再编辑。
- provider失败必须变成明确layout失败，不能将0尺寸或上游日志/钳制冒充成功。Yoga原始measure无错误返回通道，封装必须解决失败传递、阻止发布、清理受污染缓存和下一次合法重试；具体采用内部错误记录或受控中止方式留给实现前的小设计裁决，不能把该缺口漏掉。

若文本Adapter的hinting/度量确实依赖DPI，将其纳入measure_revision并重算；纯composition像素化不应让核心重做字体测量。异常与未来语言ABI隔离，OOM保证按仓库既有边界，不承诺完整恢复。

#### 6. 失效、修改与发布

style/children/content变更使受影响节点及祖先失效；父约束变化可影响未dirty孩子。外层不再用NodeId单键缓存尺寸盖住Yoga的约束缓存。HasNewLayout只是内部优化提示，不是快照事务证明。

一次layout对应同一输入版本，按有序children计算；只有全部成功才发布。调用者能区分“旧快照可读”和“当前输入成功”，不得将旧几何贴新revision。重复读取快照不访问可变Yoga树/provider。

语义非法的批量编辑在变更前拒绝，旧有效树与结果保持。合法编辑后旧快照仍是其原版本，直到新布局成功。内存耗尽不自动保证上游树事务：若无法证明内部失败后树仍一致，Document须显式进入不可继续计算状态，允许释放与读取已拥有旧快照，不能继续消费半损坏缓存。该错误路径必须在实现时确定并验收，不默认所有异常均可重试。

#### 7. 错误与线程

错误包含操作、节点(如能识别)及原因。确切枚举名尚未发布，但必须覆盖：invalid/stale/foreign_node、invalid_tree、invalid_constraints、unsupported_rule、measurement_failed、reentrant/busy、capacity_exceeded及不可用Document。语义错误不静默忽略、不返回全零几何、不交由上游fatal/assert处理。

首版Document单拥有线程、一个同步操作；测量回调重入编辑/layout明确拒绝。没有后台布局/线程安全承诺。不可变纯值快照可以由调用方正常同步后跨线程交付；不授权其他线程访问Document、provider、Runtime或GPU资源。

#### 8. composition / RHI / Runtime

核心默认Yoga pointScaleFactor=0保留逻辑精度。composition按target/DPI/policy选择是否snap；若snap，先累加未舍入绝对边界，再按同一政策舍入left/right/top/bottom，宽高由边界相减。不能每层round再相加，也不能各宽独立round。负坐标、half-pixel tie、clip覆盖/hit-test政策不属于首版核心，需由composition后续合同裁决。

文字ink/overhang不同于box advance，box snap不能暗改shaping或独自保证文字不裁。RHI float viewport与整数scissor分别遵后端接口，不把UI政策推广到所有GPU坐标。GPU在飞资源由渲染/RHI拥有，布局Node不成为GPU root。

业务状态由UI Adapter转为纯值，默认不保活对象。未来若工具绑定需要持有，ObjectRoot和稳定代码入口属于绑定owner，不属于Node context；失效/显式销毁/执行准入沿共享Runtime。当前acceptance中的Business Entry不是现成通用UI回调接口。

#### 9. 重复性与资源限制

同pin、构建、有效输入和真实测量输出产生一致几何，warm=fresh是实际标准。不承诺跨CPU/编译器/字体/库版本f32 bitwise一致。输出顺序依赖children，不依赖地址或哈希遍历。

上游含递归、逐节点及临时分配。首版实现前须选择并公开节点数/深度/数值范围上限，超限在求解前拒绝；限额来自目标和真实边界实验，不从本轮小fixture推断任意深度安全。不承诺零分配、O(变化节点数)、硬帧预算、并行或跨帧中断。

所有正常/失败/寿命/失效要求都须通过 [acceptance.md](acceptance.md)，不能因候选探针已PASS就跳过正式Interface验收。
