# Houdini 参数面板组合参考

日期：2026-10-02。依据：用户提供的三张截图，以及 SideFX、Qt、Microsoft 官方资料。本文记录设计依据与本次实现建议，不宣称截图能证明 Houdini 的内部实现。

## 截图可观察到的结构

图 1、图 2 的工作区页签、位置栏、对象标题、参数内容形成四个紧凑层级。位置栏表达所在上下文；下一行通过图标、类型标签（Geometry / Null）和名称字段（box1 / null1）表达当前对象。图 3 的 `/ World` 与 `Actor 1` 缺少这套层级和字段边界，容易显得只是两条孤立文字。

图 1 的 Transform / Render / Misc 与 Shading / Sampling / Dicing / Geometry 是内容内的页签组。图 2 的折叠标题、普通分组标题、两组页签都可以包含参数行。参数行统一左侧标签、右侧控件；向量分量横排并分摊空间，分组沿纵向组织。右侧滚动条占据固定轨道，内容区域与其边界清楚。

静态截图不能验证 hover 的变化、拖动行为或鼠标指针集合。取消 Tab hover 高亮来自用户明确要求。图中的工具图标也不能直接当作鼠标指针素材或指针语义证据。

## 官方资料能确认的机制

SideFX 明确区分参数编辑器标题中的节点类型与名称文本框，名称文本框实际修改节点名称。位置栏的各路径段支持跳转，并有历史导航。这解释了类型、身份与位置各自承担的职责。[Parameter editor](https://www.sidefx.com/docs/houdini/network/parms.html)、[Desktops and panes](https://www.sidefx.com/docs/houdini/basics/panes.html)。

SideFX 的参数树将页签、分组框、折叠区域统一表达为 folder；folder 的展示方式可变，相邻页签能形成组。它支持在同一参数界面嵌套组织控件，并非只能在工作区层面切换编辑器。[Operator Type Properties](https://www.sidefx.com/docs/houdini/ref/windows/optype.html)、[hou.FolderParmTemplate](https://www.sidefx.com/docs/houdini/hom/hou/FolderParmTemplate.html)。

Qt 的 QBoxLayout 支持横向或纵向组织子控件及子布局；伸展比例分配剩余空间，margin 与 spacing 分别控制边缘和相邻间距。这是可参考的组合机制。截图呈现出的对齐与嵌套结构不构成 Houdini 内部使用 QVBoxLayout/QHBoxLayout 的证据。[QBoxLayout](https://doc.qt.io/qt-6/qboxlayout.html)。

Qt 滚动条区分范围、当前值、单步、页步；thumb 的长度通常对应可见内容比例。内容高度为 C、视口高度为 V 时，常见范围为 `[0, max(0, C−V)]`，页步为 V。范围独立于轨道像素长度，拖动需要在两种坐标间转换。[QScrollBar](https://doc.qt.io/qt-6/qscrollbar.html)。

Win32 的 WM_SETCURSOR 允许窗口根据命中区域设置指针；处理后返回 TRUE 阻止默认处理覆盖结果。Windows 提供文本、横/纵缩放、移动、不可用等标准指针。捕获鼠标期间也应维持当前操作的指针。[WM_SETCURSOR](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-setcursor)、[About Cursors](https://learn.microsoft.com/en-us/windows/win32/menurc/about-cursors)、[SetCursor](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursor)。

## 本次最小统一机制建议

以下基于主任务提供的现状：NodeInput 已有 row/column 与 Flex；DockSpace 管工作区页签；ui::Session 负责受控输入；UiFrame 是已接受的布局；滚动条目前只展示；窗口尚未接入 WM_SETCURSOR；Actor 尚无业务名称字段。

1. 在现有节点树中组合参数页签和折叠分组，复用 row/column/Flex、样式及命中机制。内容页签只决定某组子节点的可见性；DockSpace 继续负责工作区面板。
2. 页签的选中态保留明确对比，hover 不改变背景。标签列共享宽度规则；分量控件横向伸展；分组标题与字段使用相同尺寸、边界及间距规则，避免逐行手算坐标。
3. 路径行显示真实上下文；对象标题用类型标签和字段形态表达身份。若当前仅有对象 ID，则显示只读身份；可编辑名称必须接入真实模型、提交与 Session 历史，不能新增仅存在于绘制代码中的假名称。
4. Session 从已接受的 UiFrame 命中控件或活动拖动，导出单一指针意图，由 Windows 入口映射到系统指针。文本编辑用 I-beam，分隔条用对应 resize，拖动过程中优先保留操作指针；非客户端区域继续采用系统行为。
5. 滚动条的 thumb、轨道、步进按钮都修改已有滚动 offset。拖动保留抓取点，轨道点击按页移动；内容或视口尺寸变化时约束范围。绘制与命中必须使用同一轨道/thumb 几何，内容无需滚动时避免除零及无意义捕获。

验收聚焦真实编辑器：窄宽面板下字段对齐；两组内容页签独立切换；折叠后内容高度和滚动范围同步；滚轮、thumb 拖动、轨道点击及步进按钮都到达正确位置；文本区、分隔条及拖动状态的指针一致；Tab hover 背景保持稳定。无需扩展完整节点编辑器、动画或复杂 Ramp 系统。

## 实施与验收状态

已接入类型＋可编辑名称标题、固定路径/页签与滚动内容、嵌套参数页签、RGB 元组/通道滑条/色块、滚动条的拖动/翻页/步进，以及原生指针意图。Actor 名称作为文档标签接入同一场景历史，运行时类型未新增名称字段。当前行为见[编辑器模块](../modules/editor/README.md#参数面板组合)。完整 Ramp、色轮选择弹窗、动画和表达式不在此次实现中。

- dev/release 编译完成，UI 消费者 15/15 通过，文档消费者通过；文档链新增真实重命名、撤销、删除跨 GC 恢复和失效标签清理。
- 2026-10-02 用户恢复前台验收后，dev/release 普通窗口完整链均 PASS，各接受 2808 帧。新增对象名称提交/撤销/重做、工作区 Tab hover 稳定、原生调值指针与 WM_SETCURSOR、内容页签与独立嵌套选择、颜色数值/色块/历史，以及滚动条步进、按页、拖至末项、Esc 释放和内容避让检查均通过；原菜单、停靠、文本编辑、组件和场景链同时通过。
- 复验修正了验收驱动的跨帧手势：捕获开始前将系统鼠标与接受帧目标对齐，释放后恢复停放位置。此前系统鼠标停在标题栏，捕获产生的真实 WM_MOUSEMOVE 会被正确当作一次大幅拖动，干扰数值阶梯与滑条断言。原生指针专项也在真实字段位置验证。没有更改产品拖动语义，未运行搜狗验收。
- 本增量的最终报告及真实 GPU 截图归档于 `out/parameter-composition-20261002/{dev,release}/`，分别含 `acceptance.txt`、`controls-color-channels.png`、`controls-color-tuple.png`、`controls-workbench-narrow.png`、`scrollbar-thumb-end.png`。日志为上一级的 `dev.log` / `release.log`。
