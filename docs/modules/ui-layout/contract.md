# 第一里程碑行为合同

状态：选中模块的**实现要求**，不是已存在的 C++ Interface。具体签名/数据成员布局尚未发布；按本文行为实现小而完整的 Interface，再用真实消费者验证，不把研究示意冻结成语言 ABI。共同规则沿用 [engineering.md](../../engineering.md)。

## 1. Module 与依赖

LayoutDocument 将有序树、逻辑约束和叶测量转换为完整未舍入几何快照。第一实现使用私有 Yoga；核心仅依赖标准 C++ 与该实现，没有 Runtime、LLVM、RHI、Windows/DWrite、字体库和任务系统依赖。

内容生产、文本排版、DPI、业务事件与渲染由调用侧 Adapter 负责。核心接受明确内容键/版本和同步测量 Interface，不查询全局窗口、字体或业务对象。

## 2. 所有权与身份

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

## 3. 小而完整的 Interface

调用者只需理解五组行为：创建/结束Document；创建/移除/描述节点；提交有序父子关系与内容版本更新；对根约束同步layout；读取完整快照。无需知道上游默认配置和几十个setter，也不先做三后端工厂、一般属性反射或命令框架。

有效编辑推进input_revision；相同值的无操作编辑不推进。快照标识document、input_revision和根约束，并包含稳定顺序的节点键、父关系、未舍入box与明确的overflow信息。**box相对父外框左上角，父padding已体现在child位置里，消费方不再加一遍padding。** 根原点为(0,0)。绝对坐标从同一份未舍入父链推导，不逐级舍入；可有读取辅助函数，不维护另一份可变权威。树顺序不是z-order、绘制顺序或clip栈。

快照自身拥有值存储，不借Yoga layout内存或provider；旧快照不被新计算覆盖。节点已删除时，旧快照仍可读历史几何，但其中的旧键不能重新解析到新节点。成功布局才发布新快照，失败保留上一成功值及其原版本。

## 4. 逻辑单位、约束与子集

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

## 5. 叶测量 seam

输入至少包括content key、content_revision、measure_revision、两轴约束模式(exact/at_most/unbounded)；输出有限非负内容width/height。padding由布局处理，provider不再加一次。首版不返回baseline/ink bounds；未来需独立定义坐标与版本。

- exact采用指定长度，at_most不得超过上限，unbounded返回自然尺寸。
- provider使用真实文本排版或图片尺寸；这些实现不进入核心，不用硬编码字宽冒充文本。
- 一次layout读取稳定内容快照；允许重复测量，不承诺一次。相同输入/版本/约束必须给相同结果。
- 明确尺寸可能不调用measure；最终box读layout输出，不能用回调发生与否驱动资源生命周期。
- 文本、字号/字体、locale、排版选项等影响尺寸的变化必须更新revision并经过节点更新触发dirty。单改void* context不通知Yoga。
- 回调期间不改Document、不调用Runtime、不释放内容/Document；借用只跨该同步调用，返回后再编辑。
- provider失败必须变成明确layout失败，不能将0尺寸或上游日志/钳制冒充成功。Yoga原始measure无错误返回通道，封装必须解决失败传递、阻止发布、清理受污染缓存和下一次合法重试；具体采用内部错误记录或受控中止方式留给实现前的小设计裁决，不能把该缺口漏掉。

若文本Adapter的hinting/度量确实依赖DPI，将其纳入measure_revision并重算；纯composition像素化不应让核心重做字体测量。异常与未来语言ABI隔离，OOM保证按仓库既有边界，不承诺完整恢复。

## 6. 失效、修改与发布

style/children/content变更使受影响节点及祖先失效；父约束变化可影响未dirty孩子。外层不再用NodeId单键缓存尺寸盖住Yoga的约束缓存。HasNewLayout只是内部优化提示，不是快照事务证明。

一次layout对应同一输入版本，按有序children计算；只有全部成功才发布。调用者能区分“旧快照可读”和“当前输入成功”，不得将旧几何贴新revision。重复读取快照不访问可变Yoga树/provider。

语义非法的批量编辑在变更前拒绝，旧有效树与结果保持。合法编辑后旧快照仍是其原版本，直到新布局成功。内存耗尽不自动保证上游树事务：若无法证明内部失败后树仍一致，Document须显式进入不可继续计算状态，允许释放与读取已拥有旧快照，不能继续消费半损坏缓存。该错误路径必须在实现时确定并验收，不默认所有异常均可重试。

## 7. 错误与线程

错误包含操作、节点(如能识别)及原因。确切枚举名尚未发布，但必须覆盖：invalid/stale/foreign_node、invalid_tree、invalid_constraints、unsupported_rule、measurement_failed、reentrant/busy、capacity_exceeded及不可用Document。语义错误不静默忽略、不返回全零几何、不交由上游fatal/assert处理。

首版Document单拥有线程、一个同步操作；测量回调重入编辑/layout明确拒绝。没有后台布局/线程安全承诺。不可变纯值快照可以由调用方正常同步后跨线程交付；不授权其他线程访问Document、provider、Runtime或GPU资源。

## 8. composition / RHI / Runtime

核心默认Yoga pointScaleFactor=0保留逻辑精度。composition按target/DPI/policy选择是否snap；若snap，先累加未舍入绝对边界，再按同一政策舍入left/right/top/bottom，宽高由边界相减。不能每层round再相加，也不能各宽独立round。负坐标、half-pixel tie、clip覆盖/hit-test政策不属于首版核心，需由composition后续合同裁决。

文字ink/overhang不同于box advance，box snap不能暗改shaping或独自保证文字不裁。RHI float viewport与整数scissor分别遵后端接口，不把UI政策推广到所有GPU坐标。GPU在飞资源由渲染/RHI拥有，布局Node不成为GPU root。

业务状态由UI Adapter转为纯值，默认不保活对象。未来若工具绑定需要持有，ObjectRoot和稳定代码入口属于绑定owner，不属于Node context；失效/显式销毁/执行准入沿共享Runtime。当前acceptance中的Business Entry不是现成通用UI回调接口。

## 9. 重复性与资源限制

同pin、构建、有效输入和真实测量输出产生一致几何，warm=fresh是实际标准。不承诺跨CPU/编译器/字体/库版本f32 bitwise一致。输出顺序依赖children，不依赖地址或哈希遍历。

上游含递归、逐节点及临时分配。首版实现前须选择并公开节点数/深度/数值范围上限，超限在求解前拒绝；限额来自目标和真实边界实验，不从本轮小fixture推断任意深度安全。不承诺零分配、O(变化节点数)、硬帧预算、并行或跨帧中断。

所有正常/失败/寿命/失效要求都须通过 [acceptance.md](acceptance.md)，不能因候选探针已PASS就跳过正式Interface验收。
