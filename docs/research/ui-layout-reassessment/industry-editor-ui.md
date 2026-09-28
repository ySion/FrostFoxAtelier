# 工业编辑器 UI：共享尺寸、虚拟化与可定制布局协议

> 历史来源研究，保留固定版本、主源与取舍依据；文中候选/下一步措辞仅指研究当时。当前实现采用原生LayoutDocument，现行合同和39项验收见[布局模块](../../modules/ui-layout/README.md)。本页不构成待实施计划。

## 1. 研究边界、身份与版本

本文把 Qt、Dear ImGui、Flutter 作为工业实践参照，**不是引入这些框架的建议，也不把完整框架能力算给布局核心**。标记含义：**事实**＝已获取的一手代码或文档；**上游自报**＝维护者判断或性能描述；**推断**＝对本项目的设计解释；**待验证**＝尚未执行的实验。本轮未写文件，未构建、测试、lint 或格式化，没有本机性能胜负。

| 对象 | 身份分类 | 精确研究样本与许可 |
|---|---|---|
| Qt Widgets | framework；QLayoutItem/QLayout 是布局 protocol，具体 layout 实现局部算法 | `qtbase/v6.8.0`；所读布局源文件 SPDX 为商业许可或 LGPL-3.0-only、GPL-2.0-only、GPL-3.0-only 选项 [Q1][Q2] |
| Qt Quick Layouts | framework 内布局组件，不是独立 solver | `qtdeclarative/v6.8.0`；所读布局源文件使用同类 SPDX 选项 [Q4] |
| Dear ImGui tables/docking | framework；即时调用接口＋内部布局、交互状态 | **`v1.91.9b-docking`，明确为 docking 分支发布 tag**；MIT；头文件定义 TABLE/DOCK/VIEWPORT 功能标记 [I1][I7] |
| Flutter RenderObject/BoxConstraints | framework 内 protocol；具体 RenderObject 实现算法 | `flutter/3.29.3`；BSD-3-Clause；不是可直接链接的 C++ solver [F1][F2][F9] |

版本用于建立可重复引用的机制样本，**并非最新版本或冻结依赖候选**；本次固定到 tag，未核实 commit SHA。Qt `qt-6.8` 文档实际页面标为 6.8.7/6.8.8，Flutter 在线 API 也会滚动更新，均不可冒充样本版本快照。[Q2][Q5][F10]

**较新资料交叉检查：**Dear ImGui 1.92.9 发布页注明 2026-07-25，表格列增删后的 ID/设置匹配已重写；1.92.9b 发布页注明 2026-07-31。另读 `v1.92.9b-docking` 表格源码，默认 clipper 的等高行限制仍在。Flutter 当前 `layout()` 仍保留约束相同且未脏时的短路、`parentUsesSize` 语义，但内部边界表示已变化，不能照抄旧私有字段。Qt 后续 6.8 文档仍禁止布局与外部绑定同时写子项几何。未核查所有后续发行版，也未把 WIP 或网页横幅当作正式发布事实。[I8][I9][I10][F10][Q10]

## 2. 先分清四个独立层次

**事实：**Dear ImGui 维护者把 immediate-mode 定义为接口方式，而非内部禁止缓存。业务值由应用提供；ID 又用于保存活动项、树展开等状态；table/docking 保有跨帧状态，输出的命令与顶点由渲染后端消费。“每帧声明”不等于“每帧分配全部对象”，更不等于“立即发 GPU 绘制”。[I3][I4][I5]

Qt Quick 的 QQuickItem 与 QSG 渲染树是两套结构；GUI polish、同步及渲染线程工作有不同归属。Flutter 的布局失效、绘制失效与 composited layer 更新也分开，未脏的 repaint boundary 可以复用图层。**推断：**应分别讨论声明更新、持久交互状态、几何缓存、渲染缓存；retained/immediate 标签不能替代成本模型。[Q8][F2]

## 3. 关键工业机制

### Qt：共享轨道不等于全局自动共享

**事实：**QFormLayout 在同一表单中组织标签/字段两列，并提供行换行策略；QLayoutItem 用最小/首选/最大尺寸、伸展方向、`heightForWidth(w)` 和最终 `setGeometry()` 协商。自定义 QLayout 可自行管理 item 集合，接收 item 所有权；这不等于接管全部 QWidget 生命周期。**推断：**多个独立表单要跨组共享标签宽度，仍需共同的轨道所有者，不能期待各自布局后自然一致。[Q1][Q2][Q3]

**事实：**Quick Layouts 监听隐式尺寸和结构等变化；`invalidate()` 标记尺寸提示与排列脏，`updatePolish()` 更新布局项、尺寸提示，再排列。宽度改变引起 Text 隐式高度改变，可以再次安排 polish。6.8.0 源码超过连续循环阈值时记日志，**仍继续安排 polish，不是“两次内必收敛”的硬上限**。布局管理的子项另设 anchors/几何绑定属于冲突。`AlignBaseline`、`baselineOffset` 参与布局，不能用矩形居中替代基线。[Q4]

**事实：**QTreeView 的 `uniformRowHeights` 只能用于真正等高的数据；`wordWrap=true` 本身不会扩高单元格，仍可能省略文本。Quick TableView 复用 delegate，隐式列宽仅取**当前已加载项**的最大值；整列离开视口后该宽度被丢弃，回来时可能不同。外部 provider 改变可见行列尺寸后须 `forceLayout()`。因此，“有 tree/table 控件”不是“全局自然列宽＋变高虚拟化”无条件成立的证明。[Q5][Q6]

### Dear ImGui：table 是共享轨道，clipper 不是变高树索引

**事实：**table 首次进入行时确定列宽、位置和裁剪；列可以固定、自动或按权重伸展。滚动表通过 child window 区分外部视口与内部宽度；横向滚动时使用 stretch，需要明确内部宽度。它适合 Inspector 共享列，但不是任意依赖图的通用约束协商器。[I2]

**事实：**默认 ImGuiListClipper 假设等高行。列因横向滚动被裁掉，仍可能决定该行高度；只有确认不影响行高，才可依据 `TableNextColumn()==false` 跳过内容。自动列宽与只提交可见行也存在信息缺失。[I2][I9] **推断：**变高树表需要展开索引、有效列宽下的测量、行高缓存和像素位置查找，或不同的专用控件；不能仅换上 clipper 就宣称覆盖。

**上游自报：**维护者推荐 docking 分支用于实际项目，但 DockBuilder 仍是内部 API，被认为需要重设计。dockspace 还有提交顺序、隐藏时 keep-alive 要求。**推断：**docking/splitter 是交互、约束更新与状态持久化的组合；popup 还涉及焦点、输入捕获与裁剪，均不能算成 solver 单项能力。[I4][I3]

### Flutter：显式尺寸依赖，视口使用另一种协议

**事实：**BoxConstraints 下传宽高 min/max，子节点返回满足约束的 Size，位置由父节点决定；固定宽度、放开高度即可表达宽度相关测量。父节点读取子布局结果时须声明 `parentUsesSize=true`。3.29.3 的 `markNeedsLayout()` 把依赖传播到 relayout boundary；`flushLayout()` 合并工作并按深度处理；不脏且约束相同时 `layout()` 可以短路。它是有条件复用，**不是整树或共享列永远 O(1)**。[F1][F2]

**事实：**RenderTable 用行优先数组保存单元格；IntrinsicColumnWidth 遍历该列全部 cell 的 intrinsic width，固定列宽则无需该遍历；baseline 是显式对齐模式。RenderSliverList 使用视口协议：屏外项未实例化时不知道其真实高度，通过相邻位置推算和 `scrollOffsetCorrection` 修正。**推断：**普通 table、虚拟列表、全局宽度索引是不同机制，不能把前两者叠起来就保证第三者。[F3][F4]

**事实：**MultiChildLayoutDelegate 必须每个 child 恰好布局一次，且自己的尺寸不能依赖孩子布局结果；内容决定父尺寸时应下降到自定义 RenderObject。IntrinsicHeight 会增加预探测阶段；深层嵌套最坏 O(depth²) 是上游警告，不是本机测量。嵌套滚动协同由 ScrollController/视口体系解决，不是 BoxConstraints 自动解决。[F5][F6][F7]

## 4. 所有权、失效、资源与失败

- **事实：**Qt 依赖对象/item 图；ImGui 依赖上下文、ID 与池化表格状态；Flutter 依赖 RenderObject/ParentData 图及内部数组。**推断：**三者均不是任意宿主存储的零拷贝接口，抽取协议与直接嵌入整库是两回事。[Q2][I2][I3][F2][F3]
- **事实：**ImGui 可替换 allocator，但编译配置须跨编译单元一致，不保证共享库 ABI 前后兼容。QWidget 只能在主线程使用，Quick 独立渲染线程不代表布局可并发；Flutter 通常在主 isolate 执行 UI。**待验证：**同一 ImGuiContext 的并行访问和异步测量回灌，本报告没有认证。[I1][I6][Q9][Q8][F8]
- **事实：**Quick 与 Flutter 存在递归子树路径；Flutter 脏节点排序、RenderTable 列数变更后的数组重建有真实成本。**待验证：**最大深度、栈峰值、分配次数、缓存占用没有实测。ImGui 应分别考察应用层递归、ID 栈和 docking 树，不能压成单个深度数值。[Q4][F2][F3]
- **上游自报：**Qt 文档警告 height-for-width 缺少缓存可能引发指数级重复工作。**事实：**Flutter 布局异常被报告后，仍有清除脏位/标记绘制路径，不承诺事务回滚；ImGui 对部分非法参数使用断言。**推断：**项目必须明确失败后保留旧结果、拒绝提交或终止本轮，不可静默转为零尺寸。[Q1][F2][I2]
- **推断：**测量键至少包含内容/字体版本、有效宽度、字号/缩放和相关样式；同键重复测量应幂等。字体 shaping、失败与重入不能由“支持文本布局”自动兜底。静态帧、叶更新、祖先宽度变化及结构编辑必须分别测量；共享列变化导致多行失效可能是必要工作。[Q1][Q4][F1][F2][F3]

## 5. S1—S9 场景映射

下表是依据上述机制作出的**推断性映射**，不是本机通过清单。

| 场景 | 从编辑器实践提炼的边界 |
|---|---|
| S1 HUD | 定位、覆盖与宿主安全区分层；不必引入完整浏览器规则。 |
| S2 格子/标签 | 共享轨道适合格子；标签自动换行仍需明确 flow 算法，table 不是标签流。 |
| S3 Inspector | QForm/Grid、ImGui table、RenderTable 启示共享列所有者；跨组及屏外信息需单独政策。 |
| S4 splitter/docking/popup | 交互状态产生约束；浮层另有坐标、裁剪、输入与生命周期。 |
| S5 树表/日志 | 区分逻辑项、展开项、实例化项、可见项；固定高度与可变高度不能共用未经验证的索引假设。 |
| S6 本地化/基线 | height-for-width、baseline 进入协议；字体变化使相关测量失效。 |
| S7 嵌套滚动 | viewport、content extent、scroll offset、裁剪及滚动链不可压成一个矩形。 |
| S8 更新模式 | 冷启动、静态帧、局部内容、祖先 resize、结构编辑分别计数。 |
| S9 失败边界 | 非法约束、不收敛、重入、深树、容量与旧结果必须有可观察策略。 |

## 6. 四个可执行 editor 反例 / probe

下述为下一阶段的**待验证执行配方**，本轮没有运行。数值只是实验输入，不是产品预算。统一记录几何、测量次数、访问项、分配、布局/绘制时间与输入命中；不能只看截图或 solver 时间。

### P1：屏外最长标签与跨组共享列（S3/S5/S8）

建立 2,000 行、两列 Inspector，第 1,500 行标签显著更长，视口 480×320。依次执行：初次显示、滚到该行、横向移出整列后返回、修改该标签、折叠其组。

Qt Quick 对照默认隐式宽度与外部 `columnWidthProvider`；ImGui 对照自动列宽＋clipper 与固定列宽；Flutter 对照完整 RenderTable intrinsic 列与外部宽度索引＋SliverList。

**判据：**先选定“全数据精确最大值”或“可见项估计”。前者若不读取屏外信息却声称精确即失败；折叠项是否参与最大值必须一致。默认可见项策略不能覆盖全局精确宽度；外部索引可以覆盖，但其维护成本必须记账。[Q5][I2][F3][F4]

### P2：横向裁剪不能改变变高树表行高（S5/S6/S7）

建立 10,000 行四列表格，第三列每第七行含六行文本，其余一行；两级展开树，视口 500×300。显示长文本列后，水平滚动使其离屏而同一行仍可见；随后缩窄列宽、折叠一个祖先。

ImGui 先故意采用等高 clipper/跳过离屏列作为反例，再用高度索引并分离测量与绘制裁剪。Qt Widgets 禁用 `uniformRowHeights`，delegate 按有效列宽返回高度，不能仅开 `wordWrap`。Flutter 使用变高 sliver，而非固定 extent。

**判据：**仅横向滚动时，同一行高度、后继位置、命中索引不变；宽度/展开变化后的内容总高、可见范围与滚动锚点一致。标准 ImGui clipper 不直接覆盖；其他框架也必须验证测量失效和滚动修正，不能据控件名称宣布成功。[I2][Q6][F4]

### P3：循环尺寸协商与自定义布局边界（S3/S7/S9）

Qt Quick 构造内容自适应 popup，在布局子项设置 `implicitWidth: width < 200 ? 220 : 180`，加入宽度相关换行文本；逐帧记录 polish 次数及宽度序列。对照组由外部明确 popup 可用宽度，仅允许高度随文本变化。Flutter 用 MultiChildLayoutDelegate 尝试让父高等于布局后孩子高度之和，再用自定义 RenderBox 对照。

**判据：**循环反例须明确拒绝或按已声明策略截断，不可把无限调度当“最终收敛”；对照组应稳定且满足约束。Quick 循环日志不是安全截止线；Flutter 多子项 delegate 不覆盖内容决定父尺寸，自定义 RenderBox 才是正确扩展点。该输入足以推翻“一个任意回调覆盖所有自定义布局”的窄设计。[Q4][F5]

### P4：splitter 拖动中的 popup、基线与状态（S4/S6/S8）

Inspector 同行放 12/20/28 逻辑像素文字、图标与三行错误信息；打开行末 popup，连续拖 splitter、改变窗口缩放、滚动父面板，最后隐藏并恢复 dockspace。Qt 使用 QSplitter/Quick Popup；ImGui 对照保留/省略隐藏 dockspace keep-alive；Flutter 由布局对象更新尺寸，浮层独立定位。

**判据：**按文本语义对齐基线；popup 的锚点、遮挡、点击区域一致；尺寸变化不丢编辑焦点，隐藏恢复不意外拆 dock。Qt Popup 的 Item/Window/Native 模式不可混算；ImGui keep-alive 反例暴露生命周期，而不是 solver 精度。混合字体、浮层和输入覆盖仍需真实后端验证。[Q4][Q7][Q11][I3][I4][F3]

## 7. 协议提炼、支持与反对理由

**推断：**FrostFoxAtelier 应允许普通 box/flow、共享轨道、虚拟视口、自定义布局并存；至少分离约束输入、尺寸/基线测量、最终排列、父对子尺寸的依赖、内容范围/视口、失效版本、结果提交。布局仅拥有自己负责的几何；popup、滚动、docking 各有明确所有者；尺寸协商不默认接受任意循环。这些是待交叉对抗的要求，不是冻结 Interface。[Q1][Q4][I2][F1][F2][F4]

| 参照 | 最强支持理由 | 最强反对理由 |
|---|---|---|
| Qt | 共享列、宽高互依、基线及控件交互展示成熟分工。 | 对象、样式、模型视图、事件循环不能一起算作轻量 solver；绑定反馈仍可能振荡。[Q1][Q4][Q5] |
| Dear ImGui | 业务声明、ID 持久状态与 table 共享轨道并存，反驳 immediate 必然无缓存。 | 默认变高虚拟化不足，docking 有内部 API 和生命周期约束，非通用测量协议。[I2][I4][I5] |
| Flutter | constraints-down/size-up、依赖声明、失效边界和 sliver 协议值得借鉴。 | intrinsic 全量测量可能抵消惰性实例化收益；Dart/framework 不是 C++23 裸布局依赖。[F1][F2][F3][F4] |

**未知/未证：**真实 shaping 与字体回退的统一测量接口；共享列对隐藏项的政策；变高树任意跳转的高度索引；测量取消、失败、重入及旧结果策略；深度/容量、分配峰值；Windows x64 自有渲染、输入、DPI、工具链/ABI 的实际接入成本。Qt/Flutter 的全部新版本变更未穷尽，不宣称后续版本完全不变。下一阶段应优先执行 P1—P4 推翻不充分协议，再比较求解器性能。

## 一手来源

源码链接以 tag 固定；浮动文档已在正文标明。关键符号：Qt 的 `QLayoutItem::heightForWidth`、`QQuickLayout::invalidate/updatePolish/ensureLayoutItemsUpdated`；ImGui 的 `TABLE SIZING`、`TABLES CLIPPING/CULLING`、`BeginTableEx`；Flutter 的 `RenderObject::layout/markNeedsLayout`、`PipelineOwner::flushLayout`、`IntrinsicColumnWidth`、`RenderSliverList::performLayout`。

[Q1]: https://raw.githubusercontent.com/qt/qtbase/v6.8.0/src/widgets/kernel/qlayoutitem.cpp
[Q2]: https://doc.qt.io/qt-6.8/qlayout.html
[Q3]: https://doc.qt.io/qt-6.8/qformlayout.html
[Q4]: https://raw.githubusercontent.com/qt/qtdeclarative/v6.8.0/src/quicklayouts/qquicklayout.cpp
[Q5]: https://doc.qt.io/qt-6.8/qml-qtquick-tableview.html
[Q6]: https://doc.qt.io/qt-6.8/qtreeview.html
[Q7]: https://doc.qt.io/qt-6.8/qsplitter.html
[Q8]: https://doc.qt.io/qt-6.8/qtquick-visualcanvas-scenegraph.html
[Q9]: https://doc.qt.io/qt-6.8/threads-qobject.html
[Q10]: https://doc.qt.io/qt-6.8/qtquicklayouts-overview.html
[Q11]: https://doc.qt.io/qt-6.8/qml-qtquick-controls-popup.html
[I1]: https://raw.githubusercontent.com/ocornut/imgui/v1.91.9b-docking/imgui.h
[I2]: https://raw.githubusercontent.com/ocornut/imgui/v1.91.9b-docking/imgui_tables.cpp
[I3]: https://raw.githubusercontent.com/ocornut/imgui/v1.91.9b-docking/docs/FAQ.md
[I4]: https://github.com/ocornut/imgui/wiki/Docking
[I5]: https://github.com/ocornut/imgui/wiki/About-the-IMGUI-paradigm
[I6]: https://raw.githubusercontent.com/ocornut/imgui/v1.91.9b-docking/imconfig.h
[I7]: https://raw.githubusercontent.com/ocornut/imgui/v1.91.9b-docking/LICENSE.txt
[I8]: https://github.com/ocornut/imgui/releases/tag/v1.92.9
[I9]: https://raw.githubusercontent.com/ocornut/imgui/v1.92.9b-docking/imgui_tables.cpp
[I10]: https://github.com/ocornut/imgui/releases/tag/v1.92.9b
[F1]: https://raw.githubusercontent.com/flutter/flutter/3.29.3/packages/flutter/lib/src/rendering/box.dart
[F2]: https://raw.githubusercontent.com/flutter/flutter/3.29.3/packages/flutter/lib/src/rendering/object.dart
[F3]: https://raw.githubusercontent.com/flutter/flutter/3.29.3/packages/flutter/lib/src/rendering/table.dart
[F4]: https://raw.githubusercontent.com/flutter/flutter/3.29.3/packages/flutter/lib/src/rendering/sliver_list.dart
[F5]: https://api.flutter.dev/flutter/rendering/MultiChildLayoutDelegate-class.html
[F6]: https://api.flutter.dev/flutter/widgets/IntrinsicHeight-class.html
[F7]: https://api.flutter.dev/flutter/widgets/NestedScrollView-class.html
[F8]: https://docs.flutter.dev/perf/isolates
[F9]: https://raw.githubusercontent.com/flutter/flutter/3.29.3/LICENSE
[F10]: https://api.flutter.dev/flutter/rendering/RenderObject/layout.html
