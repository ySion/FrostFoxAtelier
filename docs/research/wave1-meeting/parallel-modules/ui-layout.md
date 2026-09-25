# UI 布局核心候选：无窗口几何计算，而非第二套业务世界

作者：WaveUi。日期：2026-09-25。状态：独立研究及两方质询完成；Main 已统一运行原始与精简链接探针，均真实 PASS，并裁决 UI 布局核心入选。已补齐 docs/modules/ui-layout/ 六份下一 session 启动文档；生产 Module 尚未实现。生产代码只读，本作者未安装依赖、构建、运行或测试；第11节区分 Main 实测与未测边界。

## 1. 问题、意图与结论候选

研究对象是把有序布局输入、父级约束和叶节点测量转换成几何结果的窄 Module，不是皮肤、编辑器、DOM、Web 框架或渲染器。建议进入独立候选：**C++ 窄 Interface + Yoga 3.2.1 内部实现**，先证实单轴 row/column、固定尺寸/剩余空间、padding/gap、受约束叶测量、稳定缓存失效和几何快照；不承诺完整 CSS/Flex/Grid。Yoga 只是内部求解器，不能把全部 style setter 原样变成引擎 Interface。[Y1–Y6]

这不是最终入选决定。自写窄算法与 Taffy 是真实反方：若业务只需固定框和无伸缩 stack，自写可能依赖更少；若必须接已有自有树且需要 Grid，Taffy 低层算法 Interface 更匹配。当前没有实测证据声称任一方案更快。[设计取舍]

## 2. 当前证据与 Spec / Standards

- **源码证实**：README 首段说编辑器及渲染尚未实现；CMakeLists.txt 当前只有 runtime、分配/JIT 验收相关目标。src 现有 runtime/language/jit/memory，没有布局入口。采用会议 baseline.json 固定的当前实现，不编造不存在的 Git diff。
- **Spec**：docs/language-contract.md「统一类型体系」及 CONTEXT.md「值/对象/对象句柄」要求业务对象共用 Runtime。布局用纯值数组与局部缓存句柄是计算表示，不是业务类身份。src/runtime/runtime.hpp:45–56 的 ObjectId 包含 context、slot、generation；布局 NodeId 不能冒充它或成为跨会话存档 ID。
- **Spec**：runtime.hpp:113–155 区分不自动建根的 ObjectReference 与 ObjectRoot。业务绑定在布局外。叶 context 不得藏永久业务裸指针/JIT 函数地址；默认只复制不可变内容/尺寸快照，不持有业务对象。事件 Adapter 在事件实际发生时按现有 Runtime 规则取得、校验和执行。ObjectRoot 也不能阻止显式销毁。
- **Standards**：docs/engineering.md 要求小而完整 Interface、明确拥有者/失效条件、允许复用成熟实现、CMake 单一权威。当前 C++23/clang-cl 已固定；Rust 不是语义错误，但有新增工具链、FFI 与锁定成本。不得为了布局建设第二套 GC、反射和通用事件对象系统。
- **已证实缺陷**：当前没有布局生产代码，本切片不报告布局生产缺陷。以下分别为设计取舍、可证伪风险、性能假设；不把建议冒充已发现缺陷。

## 3. 候选比较（固定版本，不宣称最新）

| 项 | 自写窄布局 | Yoga 3.2.1 | Taffy 0.9.1 |
|---|---|---|---|
| 实现/接入 | 本仓 C++，只做所选规则 | C++20 核心、C 函数 Interface、CMake 静态 yogacore | Rust 2021；manifest MSRV=1.65，strict_provenance 特性另需 1.84 |
| 语义范围 | 可只定义 row/column、固定/填充、约束测量，不假装 CSS | 成熟 Flexbox；只公开命名子集，Web defaults/errata 显式配置 | Flex/Grid/Block；可关 default features，仅选择所需算法 |
| 树/身份 | 自己维护局部存储/代际/无环/父唯一性 | Yoga 自有 owner/children 树；作为唯一几何存储，而不是再复制 Widget 树 | 高层 TaffyTree 管树/缓存；低层 traits 可接自有树，但调用者承担缓存/dispatch |
| 缓存/测量 | 所有失效边和重复约束自己维护 | style/children 向祖先 dirty；外部内容需 MarkDirty；父约束变仍可能重排 | Cache 以 known_dimensions、available_space、run_mode 复用；9 个测量槽；外部内容仍须使缓存失效 |
| 舍入 | 可定义绝对边界一次舍入；须维护逻辑输入和像素输出 | pointScaleFactor 可配置/禁用；PixelGrid 有 text 特殊规则，非 round(width) | round_layout 明确用绝对累积边界，读取未舍入值、写最终结果 |
| 当前成本 | 算法、兼容、错误边界与验收自负；极小需求可能最省 | 源码依赖 + 小封装；需防 CSS 渗入，并验证递归深度/原生分配 | 新增 Rust/C ABI、panic、分配归属、Cargo 锁与 CMake 协同 |
| 许可 | 本仓自有实现；移植上游算法仍须追踪来源/许可 | MIT，保留 LICENSE | MIT；LICENSE.md 保留原作者及 stretch/Visly 来源 [T6] |

来源：[Y1–Y6、T1–T4]。不按流行度决定：Yoga 本轮优势是与当前实现语言/构建相近、有真实入口及缓存完整机制。Taffy 优势是自有树接入和算法选择，不是“Rust 一定快”。自写只有在需求足够窄且正确性维护成本更低时才胜出。禁止先做可互换三后端抽象；当前选一实现，保留不暴露第三方类型的窄 Interface 即可。

## 4. 最小完整 Module 与 Interface 方向

### 4.1 输入、树和输出

LayoutDocument 原生拥有节点几何与局部缓存，单写者、同步计算。LayoutNodeId 仅在所属 Document/代际有效，删除后旧 ID 拒绝；不作为业务对象身份，不被 GC 扫描。父唯一、有序 children、无环；修改失败不交付半改树的几何。节点只承载布局 style、leaf content key、计算状态，不承载业务方法或类型注册。

首版规则：有限非负逻辑尺寸；每轴支持 fixed、intrinsic、fill_equal，fill_equal 在父主轴有确定尺寸时使用 basis=0/grow=1，并在 min/max 约束下分配。**暂不公开任意 grow 权重**：CSS 的 grow 总和小于 1 时可能不占满剩余空间，不能把它未经说明包装成一般权重。row/column、不换行、gap/padding、cross start/stretch，box-size 明确为包含 padding 的外框。固定项不 shrink；空间不足时可显式 overflow，填充项最小为自身 padding 占用，不产生负宽。拒绝 min>max、固定/最大外框小于 padding 总和、负数/NaN/Infinity、未知节点/多父/循环。禁止百分比、auto margin、Grid、绝对定位、cascade/order、transform、scroll、动画和并发修改。不支持的值须拒绝而非忽略。[Y8、Y10]

接受地图方的双状态反例后，存储进一步收紧：Yoga 是唯一 style/children/测量缓存权威；外层只留 Document owner、generation→私有 node 的解析表、内容 revision 和已发布几何，不再保存完整 Widget 树或并行 style/cache 表。几何快照是输出值，非另一份可变布局权威。是否需要局部稳定 ID 由增量树编辑这个实际用途支持，不因为业务已有 ObjectId 就借用其身份槽。

Interface 聚合生命周期、批量编辑、显式布局及带输入版本的几何快照。第三方配置/句柄不得泄漏给调用者；不能只给 Yoga setter 改名。成功才发布新结果；失败保留上一份完整结果并报告失败，不将旧几何标成新版本。前置验证挡住失效 ID、环、非法 float/约束组合，不能把上游 assert 当业务错误协议。

### 4.2 约束、叶测量和文本

约束区分“确切轴长”“至多可用”“未限长/自然尺寸”，不能拿 0/NaN 混用。叶测量输入包括内容 revision、排版 revision、两轴约束及模式；返回内容尺寸，padding 由布局处理，避免算两次。文本 shaping、换行、字体 fallback、glyph cache 属于文本 Module，不在布局里复制。真实文本排版 Adapter 接在此 seam；真实图片 intrinsic size 也是合法叶，不要用硬编码字符宽度假装文本。[Y2]

Yoga 确定尺寸节点可能不调用 measure，因此最终 box 必须从 layout output 读取；不能用“本次 measure 调用”驱动资源寿命，也不能把 measure 临时结果当最终 box。首里程碑不支持 baseline 对齐；将来若加，须定义 baseline 所属坐标空间，不能默认为像素坐标。

测量回调只读稳定内容快照，同步期间不改树、不调用业务代码、不释放 Document；异常在 Adapter 内捕获并以显式失败退出本次快照发布。未支持完整 OOM 恢复就明说，不能承诺第三方任意内部失败可恢复。

### 4.3 失效、顺序、增量与重复性

style/children 变更使节点与祖先失效；父约束变化能影响未 dirty 孩子。字体、文本、locale/shaping 参数变必须使测量 revision 更新并失效叶/相关祖先。缓存键不能只有 node pointer。DPI 若只影响最后舍入无需重做 shaping；若文本使用 device-dependent hinting，则 DPI 属于 measurement key，需要重新测量。[Y2、Y3、T3]

在一个不可变输入版本上按 children 稳定顺序计算。先有完整冷布局，再证明 warm/no-op 与局部修改等价冷重建；“增量”是缓存复用，不是时间切片，更不保证只访问变更叶。首版不并行、不引入任务系统。重复性只承诺相同 pin/构建/输入/测量结果产生相同几何；不宣称所有 CPU/编译器 f32 bitwise 一致。[设计取舍]

### 4.4 DPI、RHI、平台输入和 Runtime

**采纳 WaveRhi 的同树双 DPI/离屏反例后修正**：布局核心只发布未舍入逻辑快照，默认 `YGConfigSetPointScaleFactor(config, 0)`。按目标 surface、DPI、像素化政策派生的快照属于 **UI composition Adapter**，不是布局求解器和 RHI 的共同全局状态。同一逻辑快照可用于多个目标，无需改变树；RHI 不反向规定逻辑单位，不各自 round(child.width)。

UI 若选择像素对齐，先累加未舍入的绝对边界，再按同一政策舍入 left/right/top/bottom，宽高由边界相减，避免 101 分三份后各自 round(width) 得到 102。原始 float 几何仍保留；不需要像素对齐的动画/离屏目标可不 snap。RHI 的 float viewport 与整数 scissor 遵其真实后端要求，不能把 UI 最近像素政策推广到所有 GPU 坐标。半开 box、clip/hit-test 的坐标版本由 composition 同步；scissor 的覆盖裁剪、文本 ink/overhang 与 box 布局不等同，也不允许单次 box rounding 改写 glyph shaping。

RHI 消费渲染命令与 GPU 资源，不拥有布局节点、字体度量、ObjectRoot 或布局缓存。布局不等 GPU fence、不建窗口。文本与 renderer 使用同一布局版本；平台层报告 viewport/DPI/指针，事件绑定和焦点不属于布局。GPU 在飞资源有独立存活责任，不该靠延长布局树解决。

未来自有语言只经值/受控句柄进入原生 Adapter，不观察 Yoga pointer、C++ STL 或 Rust enum/Vec 布局。本轮不承诺当前最小语言已能表达通用 UI Interface，不为此扩张语言语法或第二套类型表。[Spec]

## 5. 版本、许可和依赖路径

- Yoga 3.2.1：官方 ref commit `042f5013152eb81c1552dec945b88f7b95ca350f`；facebook/yoga 已重定向 react/yoga。MIT。固定版本只用于本轮比较，不是“最新”或安全审计结论。[Y1、Y7]
- Taffy 0.9.1：annotated tag `32bf7a96d6aa0726005e77c469579f8b2394de09` 指向 commit `fa5930c84567545f55e6f848ca655aa9eec9bb75`。Cargo manifest 声明 MIT。低层树不会自动消除 FFI 成本。[T1、T5]
- Yoga 最小获取使用官方固定 commit tarball，仅构建 `yoga/` 子目录 `yogacore`，不需要 JS binding、Yarn、Chrome fixture generator 或上游 tests。选中后由 Main 在现有唯一 `cmake/dependencies-lock.json` 锁 URL/版本/包 SHA-256，不在研究笔记建永久第二份版本表。[Y5、Y6]
- 改选 Taffy 时精确 crate `=0.9.1`、`default-features=false`，首选 `std/taffy_tree/flexbox`；传递依赖与 Rust compiler 分别锁定。Cargo 被 CMake 集成调用而非另起权威构建系统。C ABI 约定谁分配谁释放、panic/异常不跨界；跨 ABI 不递交 Rust 容器或 trait object。[T1]

## 6. 第一里程碑与 Main 最小路径（候选已执行，见第11节）

第一里程碑应是可无窗口/无 GPU 独立验收的完整几何库：声明的规则子集、约束/非法输入验证、局部节点生命周期、带版本快照、真实叶测量 Adapter、dirty/cold 等价、DPI round-trip。不是只能画固定屏幕的 helpers。明确非目标：完整 UI/widget、文字引擎、输入焦点、绘制、GPU 和一般语言绑定。

最多三个优先动作：

1. **真实 Yoga 几何探针**：root=300×100、row、padding=10、gap=5；A width=50、B width=100、C flex-basis=0/grow=1，均不 shrink，cross stretch。预期逻辑矩形 A=(10,10,50,80)，B=(65,10,100,80)，C=(170,10,120,80)。root width=320 时 C.width=140；按 A,C,B 顺序后 A=(10,10,50,80)，C=(65,10,140,80)，B=(210,10,100,80)。warm/no-op、宽度变更和重排都与 fresh tree 比较。必须调用真正 `YGNodeCalculateLayout`，不复制一套算法。
2. **舍入/缓存反例**：root width=101，三个 basis=0/grow=1，无 gap/padding。scale=0 的逻辑宽约 33.666667；scale=1 的理想共边为 0,34,67,101，宽 34,33,34；scale=1.5 的物理共边为 0,51,101,152。换 DPI 后与 fresh tree 对照。具体 Yoga 局部坐标组合需要继续查源码；若违反唯一绝对边界舍入策略，则禁用库舍入，在快照输出层舍入一次，不让 RHI 打补丁。
3. **真实测量/失效**：已编写 `out/review/wave1-meeting/ui-layout-path-probe.cpp`，直接调用现有 SDK 的 DWriteCreateFactory→CreateTextLayout→GetMetrics，作为 Yoga 真实叶测量，不创建窗口、GPU、业务对象或伪字体库。按本机 Segoe UI/16/en-US 测 `FrostFox`；改 `FrostFoxAtelier layout` 不 dirty 的负对照应保留旧几何，MarkDirty 后应重新测量并与新树/直接 DWrite 结果一致；字体变 20 后显式 dirty，父约束从 400×120 变 100×120 后与实际排版度量比较。不规定首次测量恰好一次。探针要求 Segoe UI 存在，不默默换字体；但系统字体文件和 dwrite.dll 并非 SDK 版本锁的一部分，因此不硬编码跨机器字宽，不把此路径当 Unicode shaping 全验收。[D1–D3]

首版先收集 cold/warm 耗时、测量次数和访问数；没有测量不宣称性能收益，也不由 node 数推导帧时间。探针只验证候选，不把上游成熟度当引擎 Interface 验收。

## 7. 支线质询、回应与修正

本席先独立完成初稿，已分别 hub send 给 WaveModuleMap / WaveRhi；随后完整阅读各自独立初稿并完成以下证据往返。对方已直接确认回应，不用“三方同意”替代实验。

### 7.1 与 WaveModuleMap：成熟求解器是否只是多一层状态

- **本方起始质询**：若只因依赖少就先自写，会漏计受约束测量、祖先失效及缓存一致性成本；局部 NodeId 不等于业务对象身份。
- **反方主张**：地图并未默认自写；真正反例是长期只有固定盒、无伸缩 stack。若 Yoga 已有树/cache，外层再留 style/children/cache 会变成双状态。另质询无窗口时如何真实测量，以及长期工具回调的 Root/Entry 属谁。
- **来源与回应**：工程指南允许成熟机制复用，不为一致命名套层；Y2/Y3 和 pin 的 YGNode.cpp 表明外部 content 不自动 dirty，调用者必须通知；Taffy 低层也不自动免除此义务。runtime.hpp:113–155 区分引用/根，Node context 的 void* 不构成持有声明。DirectWrite CreateTextLayout/GetMetrics 可无窗口真实排版，DIP 几何由现有 Windows SDK 提供，而非 mock 字宽。[D1–D3]
- **已修正**：4.1 明定 Yoga 唯一可变 style/树/cache；外层只有 owner、generation 解析、内容 revision、已发布几何。DirectWrite 只在本次 probe/叶 Adapter，布局核心不链接 DWrite/Windows。首版无需业务绑定；未来需长期事件时 Root 和稳定入口归 UI 绑定 owner，默认弱绑定在触发时按 Runtime 取得/校验；显式强持有由该 owner 管理、解绑时撤根。节点不保存 JIT 裸函数；当前 acceptance 私有 Business Entry 不是现成通用 UI Interface。
- **反方确认**：撤回“无文本库会阻断独立布局”的泛疑虑，接受单一几何权威及 owner 分工。
- **未决/证伪条件**：若落地时出现第二份可变 style/children/cache、测量回调访问 Runtime、或本轮只接受固定框无 intrinsic/fill，则当前独立性/选 Yoga 理由分别失效，重新评估而非扩大包装。

### 7.2 与 WaveRhi：谁拥有 DPI 舍入政策

- **本方起始质询**：RHI 不可再次 round(child.width)，GPU fence 不应持有布局节点/业务 root。
- **反方反例**：同一逻辑树输出到两个 DPI 或离屏目标；如果布局快照只有一次全局像素舍入，就丢精度/绑死 surface。D3D12 viewport 字段为 FLOAT，scissor 使用整数 RECT，不是每个 GPU 坐标都须像素对齐。[R1、R2]
- **回应与修正**：接受反例。4.4 改为未舍入逻辑快照属于布局核心，按 target/DPI/policy 生成像素视图属于 composition Adapter；RHI不持 Node/Root，不制定 UI 普遍舍入政策。默认 Yoga scale=0，Text node 的特殊舍入不触发。文本 ink/overhang、clip覆盖及 hit-test 与 box 几何不可混同。
- **源码反例**：Yoga PixelGrid.cpp 以相对 nodeLeft 设置节点 position，但 width 使用绝对左右边界；因而先对多个局部 0.4 舍入再相加，与对逻辑绝对 0.8 一次舍入不是同一政策。这是消费方式的可证伪集成风险，不标成上游 CSS 缺陷。探针只输出 OBS，Main 观察真实值；Taffy round_layout 明说读未舍入数据/绝对边界，可作为政策参考而非需要移植其代码。[Y4、T4]
- **反方确认**：接受修正，未提出 RHI 必须反向持节点的反例；地图方也明确接受同一 seam。
- **未决/证伪条件**：最终 pixel snap、clip、hit-test 由 composition 后续场景裁决；若逻辑核心必须获知每个 surface 才能求同一布局，或 GPU 在飞必须保留 Yoga 树，则此次独立模块结论失败。不得把当前三人赞同当 GPU 或字体集成已验收。

## 8. 来源和证据等级

访问日期均为 2026-09-25。版本化源码优先，Yoga 网站是滚动文档，最终以 pin 源码为准。

- [Y1] [Yoga 3.2.1 README](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/README.md)、[官方 ref](https://api.github.com/repos/react/yoga/git/ref/tags/v3.2.1)。
- [Y2] [外部测量官方文档](https://www.yogalayout.dev/docs/advanced/external-layout-systems)。
- [Y3] [增量布局官方文档](https://www.yogalayout.dev/docs/advanced/incremental-layout)。
- [Y4] [YGConfig.h](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/YGConfig.h)、[PixelGrid.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/algorithm/PixelGrid.cpp)。
- [Y5] [yoga/CMakeLists.txt](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/CMakeLists.txt)。
- [Y6] [project-defaults.cmake](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/cmake/project-defaults.cmake)。
- [Y7] [LICENSE](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/LICENSE)。
- [T1] [Taffy Cargo.toml](https://raw.githubusercontent.com/DioxusLabs/taffy/v0.9.1/Cargo.toml)。
- [T2] [Taffy 0.9.1 Interface](https://docs.rs/taffy/0.9.1/taffy/)。
- [T3] [cache.rs](https://raw.githubusercontent.com/DioxusLabs/taffy/v0.9.1/src/tree/cache.rs)。
- [T4] [round_layout](https://docs.rs/taffy/0.9.1/taffy/fn.round_layout.html)。
- [T5] [官方 annotated tag](https://api.github.com/repos/DioxusLabs/taffy/git/tags/32bf7a96d6aa0726005e77c469579f8b2394de09)。

路径状态：现状与业务身份约束是本仓源码证实；候选能力有外部机制支持；第11节所列几何、dirty与DPI路径现已由 Main 本机证实。性能收益、深树/大规模适配、正式LayoutDocument/NodeId/错误快照仍未验证。当前无已确认布局生产缺陷；不得从候选PASS外推正式Module已完成。

### 8.1 补核的一手源码/文档

同一访问日期；未安装任何外部库。以下是已知一手 URL 的直接读取，不依赖搜索摘要。

- [Y8] [Yoga 3.2.1 YGNode.h](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/YGNode.h)、[YGNode.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/YGNode.cpp)：真正新建/计算/释放入口、MarkDirty 只允许测量叶、插入和孩子列表修改路径。
- [Y9] [node/Node.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/node/Node.cpp)：setMeasureFunc 自动赋 Text 节点类型；非法测量尺寸会被库记录/钳制，不能作为引擎显式失败保证。
- [Y10] [algorithm/BoundAxis.h](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/algorithm/BoundAxis.h)、[FlexLine.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/algorithm/FlexLine.cpp)：padding 下限、min/max、grow 总量处理；不要把原始 CSS 数值自动称一般权重。
- [Y11] [config/Config.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/config/Config.cpp)：pointScaleFactor 变化增加 config version；这支持 DPI 热/冷实验，但不是本机通过证据。
- [T6] [Taffy 0.9.1 LICENSE.md](https://raw.githubusercontent.com/DioxusLabs/taffy/v0.9.1/LICENSE.md)：MIT，保留原作者以及从 stretch/Visly fork 的来源。
- [T7] [TaffyTree 0.9.1](https://docs.rs/taffy/0.9.1/taffy/struct.TaffyTree.html)：mark_dirty 向祖先传播、compute_layout_with_measure、unrounded_layout、enable/disable_rounding。
- [D1] [CreateTextLayout](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritefactory-createtextlayout)。
- [D2] [GetMetrics](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextlayout-getmetrics)。
- [D3] [DWRITE_TEXT_METRICS](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/ns-dwrite-dwrite_text_metrics)：DIP 单位，content width 与 layoutWidth 区分，width 不含 glyph overhang；本探针用 widthIncludingTrailingWhitespace 和 height，不声称 ink bounds。
- [R1] [D3D12_VIEWPORT](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_viewport)：FLOAT 字段。
- [R2] [RSSetScissorRects](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-rssetscissorrects)、[RECT](https://learn.microsoft.com/en-us/windows/win32/api/windef/ns-windef-rect)：整数边界。Microsoft 为持续文档，这里只援引基础接口类型，不推定设备支持情况。

## 9. Main 可执行的最小候选路径

### 9.1 文件、入口和依赖

实验源码：`out/review/wave1-meeting/ui-layout-path-probe.cpp`，独占 WaveUi；未改 src/acceptance/cmake/语言规范。程序直接使用上游 Yoga 的 YGNodeNewWithConfig / YGNodeCalculateLayout / YGNodeSetChildren / YGNodeRemoveChild / YGNodeMarkDirty / YGNodeFreeRecursive 和系统 DirectWrite；没有 mock solver、字体度量替身或复制上游布局算法。

只需固定 Yoga 官方源压缩包、已锁定原生工具链和 Windows SDK 的 DWrite。probe 不是正式生产 target；其临时构建全部放 out 下，**本轮不更新正式 dependencies-lock.json**。归档 SHA-256 由 Main 下载后记入此次原始证据；这里不捏造尚未取包的摘要。正式接入若获批，才在已有唯一锁文件中记录相应依赖，不把本研究表变成永久锁源。

### 9.2 获取和编译（交 Main 决定执行，不是本作者执行记录）

在根目录 PowerShell、已有 `. ./tools/enter-dev.ps1` 环境下，可按以下入口执行；每步失败即停止，不运行陈旧 exe。官方 archive 只解压到 out，不安装系统包。临时 core 的上游 CMake 只编译 yoga/ 下生产 C++ 源，不进入上游 tests/JS/Java/gentest。

```powershell
$base = Join-Path $PWD 'out/review/wave1-meeting'
$pin = '042f5013152eb81c1552dec945b88f7b95ca350f'
$archive = Join-Path $base 'ui-yoga-3.2.1.tar.gz'
Invoke-WebRequest "https://codeload.github.com/react/yoga/tar.gz/$pin" -OutFile $archive
Get-FileHash -Algorithm SHA256 $archive
& tar -xf $archive -C $base
if ($LASTEXITCODE -ne 0) { throw 'Yoga extraction failed' }
$yoga = Join-Path $base "yoga-$pin"
$build = Join-Path $base 'ui-yoga-build'
& cmake -S "$yoga/yoga" -B $build -G Ninja `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_AR=llvm-lib -DCMAKE_LINKER=lld-link `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
if ($LASTEXITCODE -ne 0) { throw 'Yoga configure failed' }
& cmake --build $build --target yogacore
if ($LASTEXITCODE -ne 0) { throw 'Yoga build failed' }
& clang-cl /nologo /std:c++23 /EHsc /MD /W4 /WX /O2 `
  /DWIN32_LEAN_AND_MEAN /DNOMINMAX "/I$yoga" `
  "$base/ui-layout-path-probe.cpp" "/Fo$base/ui-layout-path-probe.obj" `
  /link "$build/yogacore.lib" dwrite.lib "/OUT:$base/ui-layout-path-probe.exe"
if ($LASTEXITCODE -ne 0) { throw 'UI probe link failed' }
& "$base/ui-layout-path-probe.exe"
if ($LASTEXITCODE -ne 0) { throw 'UI probe behavior failed' }
```

上游 yogacore 固定 C++20，probe 为 C++23、MSVC-compatible `/MD`。上面的仅 yogacore+dwrite 命令是研究阶段提供的可选复现方式，**不是本次实际执行命令**。Main 实际复用正式 release host 编译参数和当前 global new/mimalloc 注入；原始链接含 Runtime/JIT 库，随后另做精简链接去掉这些库并再次全 PASS。真实命令分别在 ui-layout-path-probe-build.json 和 ui-layout-minimal-build.json，第11节列出证据；没有因此证明正式新模块target已存在，也不把进程用时当布局性能。作者没有运行这些命令。

### 9.3 几何与失效判据

| 输入/动作 | 预期观察 | 能证明 / 不能证明 |
|---|---|---|
| 300×100 row，padding10/gap5，fixed50、fixed100、fill | A=(10,10,50,80)，B=(65,10,100,80)，C=(170,10,120,80) | 实际求解入口与指定子集；非完整Flex兼容 |
| root width320 | C=(170,10,140,80)，与 fresh tree 一致 | 父约束改变的重算 |
| 顺序 A,C,B | C=(65,10,140,80)，B=(210,10,100,80) | 有序 children 生效 |
| 删除/释放 B | C=(65,10,245,80) | 上游树移除及存活项重排；不证明尚未实现的引擎 generation 句柄拒绝 |
| 101 三等份，scale1 | 物理共享边界 0,34,67,101 | 共边与总宽；非“每宽独立取整” |
| scale1.5，再回1 | 1.5物理边界0,51,101,152；每阶段热树=fresh树 | config/缓存切换路径；生产仍默认禁内部舍入 |
| 嵌套局部偏移0.4+0.4 | 未舍入绝对0.8；单次绝对最近像素=1；记录 native rounded-local sum | 输出 OBS，展示消费政策区别，不将数值差异判作上游缺陷 |
| 同约束连续文字布局 | 第二次测量计数不增加 | 此 fixture 的 cache 命中；不承诺所有树零工作 |
| 改真实文本但不MarkDirty | 几何和次数仍旧（刻意负对照） | 外部内容未隐式接入 dirty；非引擎已实现bug |
| MarkDirty / 字号20 / 父宽100 | dirty后真实回调发生、content与新树一致、字体/父约束后与DWrite直接度量一致 | 真实无窗口排版 seam 与失效责任；非所有脚本/字体/语言正确 |

几何比较 epsilon=0.001 DIP；对于此小坐标 fixture 是明确容差，不推为所有画布规模的统一规则。代码成功输出三条 CASE PASS 与最终 `UI layout path probe: PASS`，返回0；嵌套舍入另输出一条 OBS，不用预写输出冒充运行日志。

记录机器字体环境才能解释文本结果。Main 可附：

```powershell
Get-FileHash -Algorithm SHA256 "$env:WINDIR/Fonts/segoeui.ttf"
(Get-Item "$env:WINDIR/System32/dwrite.dll").VersionInfo.FileVersion
```

这不是将微软系统字体纳入仓库或重新分发。需要跨机器固定文本几何时，应由后续文本 Module 选择可再分发字体文件、版本/哈希、fallback与shaping实现，另行验收。当前 probe 检查字体族存在，并与本机同入口度量对照，因此不依赖任意估算字符宽度。

### 9.4 探针失败与仍未证实的门槛

未运行不是通过。若 upstream core 在当前 clang-cl/CMake 组合编译失败，保留真实诊断，不换未固定源码或偷偷关闭错误；Main 决定是否局部修补或改选。若热/冷几何不等价、dirty不生效、DWrite路径失败，先保存输入/版本/输出再判根因，不能把上述预期数值改成“实际值”掩盖问题。

此 probe 是候选机制可行性，不是完整库交付：Document 句柄代际拒绝、批量校验/失败快照、所有约束边界、深树上限和正式CMake目标仍是**选中模块的第一里程碑**，没有在此伪实现。Yoga pin 源码含逐节点 new、FlexLine 局部 vector/reserve 与递归释放/rounding；故不宣称零分配、任意树深或增量硬帧预算。真实负载未测量之前保留成本假设，不新建池/调度框架。[Y8、Y10、Y4]

## 10. 下一 session 启动入口和裁决门槛

从本报告、会议最终 decisions/evidence、engineering.md、现有依赖锁和这份真实 probe 开始；只推进 Main 实际批准的布局候选。若获批，建议布局核心私有目录/target 与 acceptance 单独划定，根CMake/锁/许可证由 Main 串行整合。布局 core 不包含 Runtime/LLVM/Windows/RHI 头，测量 Adapter 单独编译；不能因为当前根CMake无条件配置JIT而误宣称现有顶层构建已经LLVM-free。

第一里程碑完整验收集：声明子集的 fixed/intrinsic/fill、row/column、padding/gap/min/max/overflow；错误输入和深度限制；创建/移除/重排/Document结束与旧局部ID拒绝；内容/字体/约束revision正确失效；warm=fresh；失败不发布半结果；无窗口真实测量；未舍入逻辑结果重复且可被两个composition DPI视图消费。每一项属于正式模块合同，不凭本轮probe覆盖其中一部分就宣布模块完整。

选型可被推翻：真实需求仅固定盒无intrinsic/fill时重新比较自写；需要Grid/自有树且愿意锁Rust/FFI时重审Taffy；Yoga局部状态/分配/深度在真实目标不能达标且维护代价不可接受时重评。现在不创建三种后端，不安排Web框架/DOM/CSS全语义迁移。

最终范围：本切片交付报告与真实探针源码，完成对地图/RHI各一轮以上质询；执行与最终是否入选由 Main 统一裁决。无已确认生产缺陷、无已测量性能收益，未修改生产代码或正式依赖。

## 11. Main 实测、入选与文档交接（2026-09-25）

### 11.1 已读取的真实证据

Main 已独立构建固定 Yoga 3.2.1 yogacore 并执行本席冻结的探针，本作者未构建/运行；没有因结果改变 probe。读取的原始材料为：

- `out/review/wave1-meeting/yoga-provenance.json`：commit `042f5013152eb81c1552dec945b88f7b95ca350f`；官方包 2,176,227 bytes；下载 SHA-256 `4742f41722a16f181e3da37abf943390db1e928f00f26402cb154662ae7f110f`；MIT；研究源码只在out，未改正式依赖锁。
- `yoga-core-build.log`：当前锁定原生工具链完成核心静态库构建。
- `ui-layout-path-probe-build.json` / `ui-layout-path-probe-headless-layout.json`：原始 release host 链接/运行；exitCode=0、signal=null、stderr空。
- `ui-layout-minimal-build.json` / `ui-layout-minimal-headless-layout.json`：Main 进一步删除 Runtime/JIT 静态及导入库，保留当前 global new/mimalloc、yogacore、dwrite、Windows系统库，同源码再次exitCode=0、signal=null、stderr空。

两次实际stdout相同：

```text
CASE row/constraints/order/removal: PASS
CASE DPI/shared-edges/cold-equivalence: PASS
OBS nested native local-sum=0.000000; unrounded absolute=0.800000; desired single absolute-edge snap=1
CASE DirectWrite/dirty/font/constraint: PASS; measure_calls=5; final=(97.548828,53.203125); font=Segoe UI/20/en-US
UI layout path probe: PASS
```

这关闭了三个候选优先动作的真实可行路径：指定树/约束/顺序/移除、DPI热冷一致、实际文字内容/字号/父约束与dirty。精简链接进一步证明此源路径无需Runtime/JIT库。嵌套观察支持逻辑结果不舍入、由composition选择目标政策；**不是Yoga错误结论**。

原始elapsed约47.78ms、精简约48.21ms均为整个进程运行，含启动和真实文字工作，不能用于比较性能或制定布局帧预算。measure_calls=5也是此程序总数，不是求解器通用上限。Segoe UI二进制与dwrite.dll未因SDK pin自动固定，实际文字宽高不是跨机器golden。

### 11.2 已证 / 未证分栏

| 已证 | 仍未证 |
|---|---|
| 官方pin/hash/许可来源、当前工具链可编译核心 | 正式生产依赖锁与新模块target接入 |
| 特定row/resize/reorder/remove几何 | LayoutDocument、公开NodeId/代际、跨Document拒绝 |
| 该fixture的content/font/constraint失效与负对照 | 完整约束组合、失败快照、测量错误桥接/重入 |
| scale切换的warm=fresh、嵌套舍入观察 | composition的最终clip/hit-test/负坐标政策 |
| 无窗口真实DirectWrite路径、移除Runtime/JIT库仍PASS | Windows-free、全字体脚本、未来业务语言绑定 |
| 三组真实CASE与退出0 | 性能收益、深树/大规模和完整生产dev/release验收 |

### 11.3 支线回应闭合与入选

WaveModuleMap 已在其报告9.1/9.3记录单一几何权威、真实测量Adapter、业务Root外置和多surface输出；其后读取同一Main结果，仅更新地图，不编辑本席模块文档。WaveRhi 已在其报告9/13记录双DPI反例、composition修正及float viewport/整数scissor证据，确认没有RHI反向持Node/Root的必要。两模块无交叉前置，各自独立推进。

Main 在真实probe结果基础上裁决**UI布局核心入选**，并授权本席独占完成 `docs/modules/ui-layout/` 六份文档：

| 文件 | 内容 |
|---|---|
| README.md | 目的/非目标、依赖方向、首里程碑、阅读入口与当前状态 |
| contract.md | 局部身份/所有权、规则子集、测量与失效、快照/错误/线程/重复性 |
| decisions.md | 自写/Yoga/Taffy取舍、双状态与双DPI反例修正、重开条件、实现前局部决定 |
| acceptance.md | 原始/精简真实输出、几何判据、已测/未测、正式合同验收与完成定义 |
| research.md | 固定来源/许可/hash、原始命令、一级证据及适用边界 |
| session-handoff.md | 下一session启动顺序、建议生产文件归属、共享CMake/锁的唯一集成者、阶段完成标准 |

六文件均已写入完整内容，不是占位目录；生产实现路径只作为下次获授权后的规划，不表示已创建。正式核心target必须独立于Runtime/LLVM/RHI/Windows测量库；不能拿本轮out二进制当正式Module。

### 11.4 本席终点

交付研究报告、冻结真实探针、六份可独立启动的模块文档；两方质询闭合，Main真实候选与精简链接全PASS。本席没有改生产源码/acceptance/cmake/语言规范，没有修改正式依赖，没有重跑验证。下一阶段剩余的是正式Module实施与完整合同验收，不是本轮未完成的研究或文档；不能宣称库已经实现。
