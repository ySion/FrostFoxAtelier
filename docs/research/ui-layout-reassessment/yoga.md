# Yoga：固定源码审查与真实路径验证提案

## 1. 身份与版本

**Yoga 是持有布局树及缓存的 Flexbox 求解器，不是通用布局协议、控件框架或完整 UI 系统。** 它值得进入游戏 UI 的 flex 子域短名单，但现有证据不足以把共享轨道、虚拟化和任意自定义容器统一交给它。[事实/推断：4–8]

本轮通过 Exa 获取 GitHub `releases/latest` 原文，返回 **v3.2.1**，`published_at=2024-12-13T01:58:00Z`；tag API 将其解析为 **`042f5013152eb81c1552dec945b88f7b95ca350f`**。本文机制判断固定在该提交，许可为 **MIT**。[事实：1–3] 版本号虽与旧选择相同，但这是重新核查的结果，不是沿用旧结论。本轮没有取得更新正式 release 的一手证据；不得把 main 开发状态、抓取日期或未来时间戳当发布事实。3.2.1 发布说明只有 Android 16KB page size 支持，不能据此声称布局能力升级。[事实：1]

未运行本机程序、构建、测试、lint 或格式化；所有 probes 均为待执行方案。**没有本机测量，不给性能胜负。**

## 2. 表达能力与具体边界

### Flex、百分比、gap、min/max

`YGNodeStyleSetFlexWrap` 支持 nowrap/wrap/wrap-reverse；basis 有点值、百分比和 auto，grow/shrink 独立设置。`calculateFlexLine` 依据受 min/max 约束的 basis，加 margin/gap 分行；absolute 不参与分行，shrink 权重还乘以 basis，不能理解为各项扣除相同宽度。[事实：5、7]

`resolveFlexibleLength` 采用两遍 min/max 冻结与空间分配，源码承认不能覆盖规范的所有迭代情形。实现也不自动计算文本最宽单词对应的 flex 自动最小主轴尺寸，而按默认最小值零处理。[事实：8] **这不是因“不完整 CSS”而否决 Yoga；本项目不要求完整浏览器。** 只有具体输入违反产品所需的空间分配或最小尺寸合同，才构成淘汰证据；第七节提供可推翻假设的输入。[推断]

width/height、min/max、margin/padding、inset 有百分比 API，gap 自 3.1 支持百分比；上游明确警告容器尺寸不确定时，百分比可能不同于浏览器。[事实：5；上游自报：9] 普通节点百分比参照父内容盒；height/top/bottom 依赖高度，margin/padding 包括纵向值也依赖宽度。**min/max width/height 不是 Grid `minmax()` 轨道算法。** 固定版 display 只有 flex/none/contents，没有共享轨道接口。[事实：6、10]

默认 column、shrink=0、relative 不同于网页常见默认；`UseWebDefaults` 不改变 relative 和 border-box。3.2 支持显式 content-box、display:contents，后者让包装节点的子项参与外层布局，不会建立跨行列宽约束。[事实：6、11]

### Absolute 与 baseline

absolute 可表达角落定位和覆盖盒；其 containing block 通常是最近非 static 祖先的 padding box，或 `YGNodeSetAlwaysFormsContainingBlock` 标记的祖先，否则使用根。该标记只是承接宿主 transform 等建立 containing block 的事实，不执行 transform、绘制层级或命中测试。双侧 inset 可以确定未指定尺寸；absolute 不贡献普通 flex 流尺寸。[事实：4、10、12]

`YGBaselineFunc(node,width,height)` 返回基线偏移；无回调时寻找首行 baseline/reference 子项并递归，无可用子项则回退自身高度。列向容器不走行向 baseline 布局路径。文字 shaping、字体指标、语言换行由宿主提供。[事实：4、13]

## 3. 所有权、测量与 custom layout seam

`Node` 自己持有 Style、LayoutResults、owner 和 `std::vector<Node*> children`。`YGNodeRef` 是不透明句柄；`YGNodeSetContext` 的 void* 只是关联业务对象，不是外部 children/style accessor 协议。自有 ECS/arena 通常仍需业务节点到 Yoga 节点的映射及同步；可以让 Yoga 成为唯一布局样式存储，减少重复，但不能消除业务树和布局树生命周期协调。[事实：4、14；推断]

`YGMeasureFunc` 每轴提供 Undefined/Exactly/AtMost，分别承接自然最大尺寸、精确尺寸、受上界限制的自然尺寸；输入已扣除 padding/border。支持宽度影响高度，同次求解可能多次测量，两轴尺寸确定时也可能完全不调用回调。最终尺寸必须从布局结果读取，不能采用最后一次 measure 返回值。[事实：4、8、15]

**measure 节点必须是 Yoga 叶子。** 带 children 时注册 measure、或给 measure 节点插入孩子，均有 fatal 检查。[事实：4、14] 因此可以把自定义 Grid/表格作为不透明叶，但不能在这个叶下同时保留 Yoga 子树。可行组合是 measure 回答外部子系统的尺寸需求，Yoga 决定最终盒，宿主再用最终盒 arrange 内部子项；不能把 arrange 副作用藏在不保证执行的 measure 中。[推断：8、15]

### 共享轨道与虚拟化：不必双权威，但必须划界

- **共享轨道：[推断]** 宿主统一计算列宽后单向投影到各行 Yoga width/basis，可只有一个可变轨道权威。若每行再 grow/shrink 并回写列宽，就形成双向求解与失效环。另一方案是整张表作为不透明叶，内部几何完全由外部布局拥有。
- **虚拟化：[推断]** 逻辑项、估计/实测行高、前缀和、滚动锚点由单一虚拟化控制器拥有；Yoga 只布局已实现行和必要占位，无需为每个逻辑项建节点，但它不提供这些控制机制。祖先宽度变化后，仅 dirty 可见文本不能证明离屏行高和总 extent 已正确。
- **两者组合：[推断]** 若列宽由所有逻辑行自然宽度决定，虚拟化不能消除离屏测量需求；预扫描、渐进估计还是稳定列宽，应成为产品合同。轨道宽度与 Yoga 几何可作为单向派生状态，不应各自成为可编辑权威。

## 4. Dirty、cache、rounding 与运行时机制

### 失效和缓存

样式 setter 在值变化时传播 dirty，普通插入/删除也传播；`markDirtyAndPropagate` 沿 owner 递归，到已 dirty 节点停止。外部文本/字体变化必须对 measure 叶调用 `YGNodeMarkDirty`，该 API 不是普通容器的通用 invalidate。[事实：4、5、14]

节点保存一个 layout cache 和 **8 个 measurement cache 槽**。dirty、generation、config version、owner direction 决定重访；测量复用比较约束模式、尺寸与兼容条件，部分比较经过像素取整。缓存没有业务文本/字体版本键。“98% 布局少于八槽”只是上游源码自报，不是本项目数据。[事实/上游自报：8、16]

不能声称所有 setter 自动 dirty：固定版 `setMeasureFunc`、`setBaselineFunc`、`setAlwaysFormsContainingBlock` 直接赋值；`YGNodeSwapChild` 也没有普通 InsertChild 的传播路径。因此运行期替换 provider、containing block 或 swap 必须单独验证失效，而不是套用统一 setter 假设。[事实：4、14]

**增量成本须分路径：** `calculateLayout` 仅在 `calculateLayoutInternal` 返回 true 时调用 `roundLayoutResultsToPixelGrid`；完全 clean 且根缓存命中时跳过这段遍历。反之，一旦进入该阶段，PixelGrid 在 `pointScaleFactor != 0` 判断之外递归所有 children，**scale=0 只跳过取整运算，不跳过递归**。所以不能说每次 compute 都遍历全树，也不能从 dirty 传播近似 O(depth) 推出单叶更新端到端 O(depth)。[事实：8、17；推断]

`HasNewLayout` 可以剪枝宿主应用结果的遍历，但只是“可能变化”，不是精确 diff 队列。[事实：4、15] 非零 scale 下文字节点有避免截断的特殊取整；逻辑坐标、DPI、最终像素和命中矩形应统一策略。[事实/推断：17]

### 分配、递归、线程

节点创建使用 new，children vector、样式池溢出和 clone-on-write 可分配。`calculateFlexLine` 每次建立本地 `itemsInFlow` vector，并 reserve 父节点 child count：**这限定于实际执行分行的路径，不能外推为完全缓存命中也分配**。布局结构未变也不能保证热重排零分配。样式池有四个内联槽，之后可转堆；公开配置没有宿主 arena/allocator 注入协议。[事实：4、7、14、18]

布局、baseline、dirty、rounding、递归释放存在递归路径；没有测得安全最大深度。全局 generation 使用 atomic，不代表 Node/Config 有线程同步。一棵树应单写者，回调内不改树、不重入；独立树并行仍须验证共享配置、字体缓存和日志回调。[事实：4、8、13、14、17；推断/待验证]

### 失败通道

布局入口返回 void、measure 返回 YGSize，没有结构化失败或取消返回值。fatal 先日志，启用异常时抛 `std::logic_error`，否则 terminate；logger 不是恢复处理器。measure 的负值/NaN 会警告并回退非负尺寸，baseline NaN 则 fatal，不能把这两种政策混为“统一报错”。[事实：14、19]

求解直接改写节点结果，未见事务回滚。若宿主要保留最后成功画面，应在整次成功后发布派生快照，并明确测量失败状态；不能让失败回退几何自动成为成功结果。OOM、Infinity、重入、回调抛异常及失败后缓存状态待独立验证。[推断/待验证]

## 5. S1–S9 场景映射

| 场景 | 能力与边界 |
|---|---|
| S1 HUD | absolute/containing block 可锚定；安全区、DPI、transform、叠层由宿主负责。[事实/推断：10、12、17] |
| S2 物品格/标签 | 等尺寸项加 wrap/gap 可做固定格与标签流；不等于共享自适应轨道。[事实：5–7] |
| S3 Inspector | flex 行、min/max、baseline、文本 measure 可组合；跨行列宽需要外部统一权威。[事实/推断：4–8、13] |
| S4 docking | splitter 改约束再布局；拖拽、停靠树、popup 策略不是 solver 能力。[推断：4、12] |
| S5 大列表 | 仅实例化可见行可组合；行高索引、复用、滚动锚点外置。[推断：4、14] |
| S6 本地化 | 可宽度依赖高度与 baseline；字体/内容版本失效属于必需集成责任。[事实：13–16] |
| S7 嵌套滚动 | overflow 影响测量约束，不提供滚动偏移、裁剪和虚拟总 extent；纯滚动可由宿主平移。[事实/推断：6、8] |
| S8 增量 | 有 dirty/cache；祖先 resize 可重测干净后代，实际重算后要计 rounding 遍历。[事实：8、16、17] |
| S9 失败边界 | fatal/异常、警告回退、递归、重入与非事务输出必须验收。[事实/待验证：14、19] |

## 6. Windows x64 / C++23 接入

固定版 CMake 要求从 3.13 起，构建静态 `yogacore`，默认 C++20；MSVC 路径有 `/EHsc /W4 /WX` 和关闭 RTTI。C++23 宿主可把它作为独立 C++20 target，使用 C API；本项目工具链组合未实际验证。原生 core 不要求 Java/JS 绑定运行时。[事实：20；推断]

应固定源码 SHA、编译器、CRT 和异常边界，优先使用 YGNodeRef，不把内部 C++ 类暴露为跨 DLL ABI。Node 持有 Config 指针，节点存续期间 Config 和 callback context 必须有效。`extern "C"` 不代表不抛异常或跨编译器二进制兼容。[事实：4、14、19–21；推断]

## 7. 最强论据与可执行 probes

**最强支持：[事实/推断]** C/C++ core 可嵌入且机制可审计；Flex、文本测量、baseline、absolute 和缓存可覆盖 HUD、工具栏、弹窗、表单局部布局，避免从头编写常用分配器。此理由不依赖旧选择和上游跑分。

**最强反证：[事实/推断]** 编辑器关键的共享轨道和虚拟化仍需外部协议，而 custom measure seam 不接受 Yoga 子树；这可能让大部分复杂性落到第二套布局层。两遍分配与全树后处理只是需要具体验证的风险，不能因未完整实现 CSS 直接否决。

以下全部未运行，数字结果注明推演，不是实测：

1. **多级 max 饱和。** root=300×10，row，无 padding/gap，scale=0；依次插入 B/C/A，均 basis=0、grow=1、height=10，B maxWidth=120、C 无上限、A maxWidth=30。若产品合同是“未达上限者继续分配余宽”，应得 B/C/A=120/150/30。固定算法推演可能为120/135/30，余15；改为 A/B/C 排列对照。断言浮点宽度总和及各项结果。**只有产品要求充分分配时，才构成反例。** [推断：8]
2. **内容和 provider 热替换。** measure 叶先返回40×10，清 HasNewLayout；修改同一 context 为80×20，分别不 dirty 与 MarkDirty 后计算，记录调用和最终尺寸；再仅替换 measure/baseline 函数。验收字体/内容/provider 版本变化不会陈旧，不要求每帧回调。[事实导出的 probe：4、14、16]
3. **共享轨道与 seam。** 两行 Inspector，第二行标签更长，要求统一标签列。先验证带 children 节点注册 measure 的 fatal；再以不透明叶+外部 tracks arrange。缩窗、加错误提示、隐藏最长标签，断言只有单一列宽写入权威，子矩形来自最终盒而非最后一次 measure。[事实导出的 probe：4、14、15]
4. **增量/分配。** 以实际布局节点1k/10k构造宽树及多行wrap树，分别测完全clean、单叶dirty、祖先resize、增删；统计 solver/rounding 访问、measure 次数、临时分配、宿主同步及结果应用。scale=0/1/1.25分组。分别验证“clean缓存路径”“实际重算后全树后处理”“分行临时分配”，不得混成每次compute都全树。[事实导出的 probe：7、8、17]
5. **containing block 与百分比。** root200×200/padding10，static中间盒100×100，绝对后代width50%/bottom10；切换中间盒relative或always-forms-containing-block，核对参照盒。另父宽auto、gap10%，记录自然宽及换行点。errata 固定，避免配置差异伪装成引擎差异。[事实导出的 probe：9、10、12]
6. **虚拟化与失败隔离。** 十万逻辑行只实现约百行，缩宽使离屏文本换行；记录逻辑、已实现、可见、重测数量，核对extent和锚点政策。另用独立进程逐档增加树深，注入负数/NaN/Infinity measure、NaN baseline及抛异常，观察退出方式和是否误发布部分结果。档位不是产品上限。[待验证]

## 8. 未知项与本阶段判断

真实字体提供者成本、缓存约束分布、MSVC Release 深度边界、shared tracks 的测量轮数、独立树并行的宿主依赖，以及失败快照成本均未知。[待验证]

**保留为 Flex 子域候选，不冻结为全局唯一 solver。** 下一阶段按上述反例验证产品语义，并统一计入业务同步、测量、solver、rounding、结果应用。本文不列上游性能排名；创建/布局/释放与layout-only计时不能横比。

## 9. 直接主源

源码正文经 Exa 获取；tag 已解析到固定 SHA。滚动文档若与源码不同，以固定源码为准。

1. [latest release API](https://api.github.com/repos/react/yoga/releases/latest)；[v3.2.1 发布页](https://github.com/react/yoga/releases/tag/v3.2.1)
2. [tag 到 commit](https://api.github.com/repos/react/yoga/git/ref/tags/v3.2.1)
3. [MIT LICENSE](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/LICENSE)
4. [YGNode.h](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/YGNode.h)；[YGNode.cpp](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/YGNode.cpp)
5. [YGNodeStyle.cpp](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/YGNodeStyle.cpp)
6. [YGEnums.h](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/YGEnums.h)
7. [FlexLine.cpp：calculateFlexLine、itemsInFlow.reserve](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/algorithm/FlexLine.cpp)
8. [CalculateLayout.cpp：resolveFlexibleLength、calculateLayoutInternal、calculateLayout](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/algorithm/CalculateLayout.cpp)
9. [Yoga 3.1：百分比 gap 警告](https://www.yogalayout.dev/blog/announcing-yoga-3.1)
10. [Containing block](https://www.yogalayout.dev/docs/advanced/containing-block)
11. [Yoga 3.2](https://www.yogalayout.dev/blog/announcing-yoga-3.2)；[默认样式](https://www.yogalayout.dev/docs/styling)
12. [AbsoluteLayout.cpp](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/algorithm/AbsoluteLayout.cpp)
13. [Baseline.cpp](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/algorithm/Baseline.cpp)
14. [Node.h](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/node/Node.h)；[Node.cpp](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/node/Node.cpp)
15. [外部测量集成](https://www.yogalayout.dev/docs/advanced/external-layout-systems)；[增量布局](https://www.yogalayout.dev/docs/advanced/incremental-layout)
16. [LayoutResults.h](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/node/LayoutResults.h)；[Cache.cpp](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/algorithm/Cache.cpp)
17. [PixelGrid.cpp：roundLayoutResultsToPixelGrid](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/algorithm/PixelGrid.cpp)
18. [StyleValuePool.h](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/style/StyleValuePool.h)；[SmallValueBuffer.h](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/style/SmallValueBuffer.h)
19. [AssertFatal.cpp](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/debug/AssertFatal.cpp)；[YGConfig.h](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/YGConfig.h)
20. [yoga/CMakeLists.txt](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/CMakeLists.txt)；[project-defaults.cmake](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/cmake/project-defaults.cmake)
21. [YGMacros.h](https://github.com/react/yoga/blob/042f5013152eb81c1552dec945b88f7b95ca350f/yoga/YGMacros.h)
