# Houdini 22 工作区参照

日期：2026-10-01。依据：SideFX 当前英语文档（页面标识 Houdini 22.0）。本文记录官方公开行为和适用于本项目的工程建议；不把文档推断写成 Houdini 实机观察。用户指定的 UI Scale 1.75 是实机校准条件，具体像素尺寸需以同机截图和测量为准。

## 官方对象模型

| 名称 | 责任和关系 | 官方依据 |
| --- | --- | --- |
| Desktop | 命名工作区布局；管理 Pane 和附属于它的浮动窗口。只查询附属对象的接口与查询所有可见对象的 `hou.ui` 接口有所区别。 | [hou.Desktop](https://www.sidefx.com/docs/houdini/hom/hou/Desktop.html) |
| Pane | 矩形布局区域，容纳一个或多个 PaneTab；递归分割产生更多区域，具有当前标签和最大化状态。 | [hou.Pane](https://www.sidefx.com/docs/houdini/hom/hou/Pane.html) |
| PaneTab | 编辑器/视图实例。内部名称与显示标题不同；当前标签是所在 Pane 的选择状态。`setType` 会替换实例，旧引用失效。 | [hou.PaneTab](https://www.sidefx.com/docs/houdini/hom/hou/PaneTab.html) |
| FloatingPanel | 独立窗口，通常容纳 Pane 与 PaneTab；可附属 Desktop，并随其保存和显隐。单标签简化浮动窗口也存在，它没有普通 Pane 容器。 | [hou.FloatingPanel](https://www.sidefx.com/docs/houdini/hom/hou/FloatingPanel.html) |

因此，业务编辑器实例、标签组、分割几何、原生窗口和命名工作区是不同责任。Houdini 的 Panel 通常指窗口容器，不能直接等同于本项目的 Scene、Actors、Parameters、Controls 四个业务面板。

## 操作与语义

| 操作 | 已公开行为 | 官方依据 |
| --- | --- | --- |
| 分割 | 菜单提供左右、上下分割。HOM 的 `splitHorizontally` 新区域在右，`splitVertically` 在下；新区域只有一个标签，类型与原当前标签相同。 | [hou.Pane](https://www.sidefx.com/docs/houdini/hom/hou/Pane.html) |
| 分隔线 | 拖动调整；中央点击交换两侧，Alt 点击改变方向；箭头收起/展开一侧。 | [Desktops and panes](https://www.sidefx.com/docs/houdini/basics/panes.html#pane-management) |
| 最大化 | 当前 Pane 临时填满布局，再次操作恢复；另有只扩展行/列的命令。 | [Pane menu](https://www.sidefx.com/docs/houdini/basics/panes.html#pane-menu) |
| 关闭 | Close Pane Tab 移除当前标签；Close Pane (All Tabs) 移除整个区域及标签；Undo Close Tab 恢复该 Pane 最近关闭的标签。 | [Pane menu](https://www.sidefx.com/docs/houdini/basics/panes.html#pane-menu) |
| 标签移动与浮动 | 标签可在窗口间拖放，包括浮动窗口与主窗口。Tear Off 移出当前标签；Tear Off Copy 复制标签到浮动窗口。 | [Floating panes / Pane menu](https://www.sidefx.com/docs/houdini/basics/panes.html#floating-panes) |
| 完整浮动窗口 | `createFloatingPanel` 可继续加标签与分割；`createFloatingPaneTab` 是限制为单标签的简化窗口。浮动窗口位置和尺寸以桌面像素表达，关闭窗口会关闭其所有标签。 | [hou.Desktop](https://www.sidefx.com/docs/houdini/hom/hou/Desktop.html)、[hou.FloatingPanel](https://www.sidefx.com/docs/houdini/hom/hou/FloatingPanel.html) |

公开文档没有明确规定拖标签所需的鼠标按钮/修饰键、同组重排的插入线、拖向区域边缘自动分割、热区阈值、拖出后立即创建窗口、空区域自动消除，以及最后一个标签关闭的处理。对 Ctrl、Shift 和中键的官方资料定向检索未找到 H22 手势说明。上述细节必须实机核对，或明确作为本项目自己的交互约定。不能仅凭“支持拖放”声称某种五向停靠预览与 Houdini 一致。

### 本轮主任务实机记录

主任务操作者回报：Pane 菜单 Split Top/Bottom 在新 Pane 生成当前标签的副本；普通左键把 Performance Monitor 标签拖到新 Pane 的标签栏未产生变化。前者与 HOM 规定的新标签同类型一致；后者只说明这次手势未成功，不能推导 H22 不支持移动，也不能推导必须按 Ctrl。具体拖动按钮/修饰键和边缘分割仍待核实。此段为本轮实机记录，来源不同于上表官方文档。

## 键盘、保存和视觉条件

- Ctrl+B 操作鼠标所在 Pane 的扩展；Ctrl+PgDn / Ctrl+PgUp 切换鼠标所在 Pane 的标签，首尾循环。[Interface intro](https://www.sidefx.com/docs/houdini/start/intro.html)、[Pane management](https://www.sidefx.com/docs/houdini/basics/panes.html#tips-and-tricks)
- 快捷键按上下文解析；20.5 起动作定义、上下文、默认绑定由不同 JSON 文件承担，绑定文件为 `.keymap2`。因此不从旧版快捷键海报推定 H22 全部默认键位；关闭、分割和撕出等键位以当前菜单或 Hotkey Manager 核对。[Configuring hotkeys](https://www.sidefx.com/docs/houdini/basics/hotkeys.html#transitioning-hotkey-systems)
- Desktop 支持保存、另存、重新载入。管理器提供复制、删除和 No Save，存储位置可属于安装、用户或当前场景目录；桌面文件使用 `.desk`，可用 `.nosave` 阻止持久修改。[Desktop Manager](https://www.sidefx.com/docs/houdini/ref/windows/desks.html)、[Configuring Houdini](https://www.sidefx.com/docs/houdini/basics/config.html#preventing-user-changes)
- 布局自动保存是可选项，文档默认关闭。Global UI Scale 缩放整个界面；标签可显示类型图标及类型/上下文颜色，颜色可仅为顶端条纹。[Houdini Preferences](https://www.sidefx.com/docs/houdini/ref/windows/mainprefs.html)

公开接口规定浮动窗口尺寸必须大于零，但未公布 Pane 的通用最小尺寸或 UI Scale 与 Windows DPI 的完整换算公式。[hou.FloatingPanel](https://www.sidefx.com/docs/houdini/hom/hou/FloatingPanel.html) 对本项目应在同机 Scale 1.75 条件下记录字体、标签栏、分隔线、菜单和业务内容边距；换算必须经过已有 DPI/SS 入口，不能把观察到的物理像素直接当作 DIP 常量。

## 本项目首个完整增量建议

以下是工程建议，未声称是 Houdini 内部实现，也不新增项目规范。

1. 用一棵递归分割树表达几何；叶子保存有序标签实例身份和活动身份，标签实例身份与 Scene、Actors、Parameters、Controls 四种内容类型分开。真实内容和状态继续属于现有 Workspace/Session。移动标签只改变位置，重排或最大化不重建业务编辑器；分割创建新标签时明确哪些视图状态复制、哪些业务状态共享。
2. 主任务决定第一阶段使用单个宿主窗口，围绕四种已实现内容完成独立 PaneTab、标签选择、组内重排、跨区域移动/拖放分割、左右/上下分割、分隔线调整、关闭/重新打开、最大化/恢复和恢复默认布局。拖放预览属于本项目约定；独立原生浮动窗口列为后续，需要窗口、输入和渲染生命周期闭环。菜单只提供真实能力。
3. 结构操作共用一个变更入口。命中、焦点、捕获和目标身份从接受帧进入；应用一次变更后归一化空叶子、单子分割、活动标签和最大化目标。继续复用现有 popup、SS、布局、文字输入和绘制链路，避免停靠另起一套输入世界。
4. 业务状态不挂在 Pane 几何上：参数已提交值、Controls 样本、选中对象、筛选和视口相机在移动/关闭后仍有明确所有者。正在进行的输入、IME、拖动和弹层按现有取消/提交合同退出，再迁移布局；避免隐藏编辑器留下捕获或旧帧命中。
5. 最小尺寸由真实内容需求和标签栏合成，并沿分割树自底向上计算。拖动比例限制在可行范围，极窄窗口采用明确的压缩/滚动政策。分割动作在空间不足时禁用；最大化只是显示选择，原分割比例保留。上述约束是项目决策，不能标注为 Houdini 数值。
6. 若本轮包含持久保存，保存版本、分割方向/比例、标签实例/类型描述、叶子顺序和活动标签即可；内容类型从现有四种业务类型解析，不序列化绘制矩形、指针或瞬时拖动状态。加载先验证完整布局再替换，损坏/未知类型回到默认布局；具体文件位置遵循项目现有配置入口。

端到端终点：编辑参数或 Controls 样本后，重排、跨区域移动、分割、最大化/恢复、关闭/重新打开仍可继续编辑并保持各自历史；窄窗口不越界，拖动取消恢复原布局；菜单和拖动期间视口不误接输入。持久保存则额外覆盖重启恢复和损坏文件恢复。视觉验收以同机 Houdini Scale 1.75 的截图为证据。

## 本机 1.75 缩放的补充观察

2026-10-01，使用用户调好的 Houdini 22.0.429。激活窗口后取得 1745×1125 的逻辑坐标截图；未激活时曾返回 2560×1392 的另一坐标捕获，后续测量只使用激活后的同一坐标系。Windows 物理像素与这里的逻辑坐标不可混算。

| 元素 | 激活后的截图测量 | 本项目对应目标 |
| --- | --- | --- |
| Pane 标签栏 | 约 24 高，未选 #292929，选中连接 #3d3d3d 内容面 | 24 DIP、内容驱动宽度，右侧最大化/菜单 |
| 数值输入框 | 约 24 高，输入内部 #131313 | 24 DIP 起，字体增大后随内容增长 |
| 参数行节奏 | 约 31，行间约 7 | 24＋7 DIP |
| 标签到字段的横向起点 | Pane 左侧至字段约 180 | 面板 padding 12、caption 160、gap 8 |
| 下拉和分组 | 约 26–28 高 | 26 DIP 的分组与紧凑选择 |
| 分隔线 | 约 6–8，带明暗边缘 | 6 DIP，可交互热区与显示线一致 |

现场验证：Split Top/Bottom 在原 Pane 下创建同内容的新 Pane；关闭这个新增标签后空间重新合并；最大化按钮隐藏其他 Pane，再点击恢复此前分割；Tear Off Pane Tab Copy 产生带相同标签栏和路径栏的原生浮动窗口，原 Pane 保留。临时浮动窗口已关闭。普通左键拖 Performance Monitor 到另一个标签栏没有移动，未据此推断 H22 的修饰键约定。截图保留在 `out/houdini-workspace-175/`。未保存 Houdini 场景。

实现分段：本轮建立单宿主停靠与视图移动，视图的已提交状态不属于布局树。项目现有四个视图每种只有一个实例；分割操作移动已有标签，不复制业务状态。原生多窗口、同类型多实例和布局文件持久化保留为后续明确合同后的增量。本轮真实搜狗验收按用户要求跳过。

## 实施结果

单宿主 `ui::DockSpace` 已接入正式 Workspace，移除了固定三栏及 Controls 布尔切换入口。四个视图通过稳定标签身份描述到同一 UI Session：支持分栏拖放、标签重排/合并、水平与垂直分隔、关闭重开、最后一页关闭后恢复、最大化/还原、组内快捷键及 Reset layout。关闭/最大化图标共用现有 Coverage 缓存；标签拖放复用原捕获与接受帧输入，不建立原生消息旁路。

字段绑定按 World 的实际成员身份保留，隐藏视图不会退役仍存续的绑定；对象/组件移除依旧退役，撤销恢复对象时取得新的绑定。参数面板及实验台使用真实滚动，防止缩矮面板后高频字段被固定说明挤出。当前四种视图各一实例、布局不跨进程保存，独立浮动窗口尚未接入；不将这些后续能力写成已经复刻。

最终验证：dev/release 编译通过；UI 公共消费者各 15/15，编辑器文档链通过；最终真实窗口各接受 1130 帧。新增场景覆盖边缘拖放、标签合并/重排、双向分隔、最大化还原、Esc 取消、快捷键、关闭重开、关闭最后一页后恢复、已提交数值和历史保留；既有参数、剪贴板、Unicode、对象生命周期与窄面板验收继续通过。实际搜狗按用户要求跳过，不引用之前阶段的 PASS 充当本轮证据。

报告、源码副本、原始与 LF 规范化哈希、构建产物哈希、工作区截图保留于 `out/ui-controls-evidence/houdini-docking-175-20261001/`。本阶段已完成验收收口；后续增量另行记录。
