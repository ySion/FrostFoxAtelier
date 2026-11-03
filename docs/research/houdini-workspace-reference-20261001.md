# Houdini 22 工作区参照

资料查阅日期：2026-10-01。依据：SideFX 英语文档（页面标识 Houdini 22.0）。本文记录官方公开行为和固定环境的观察，不把文档推断写成实机结论。本项目的现状与未实施方向见[停靠合同](../modules/ui/README.md#停靠与标签)和[编辑器](../modules/editor/README.md#未实现的交互)。

## 官方对象模型

| 名称 | 责任和关系 | 官方依据 |
| --- | --- | --- |
| Desktop | 命名工作区布局；管理 Pane 和附属于它的浮动窗口。只查询附属对象的接口与查询所有可见对象的 `hou.ui` 接口有所区别。 | [hou.Desktop](https://www.sidefx.com/docs/houdini/hom/hou/Desktop.html) |
| Pane | 矩形布局区域，容纳一个或多个 PaneTab；递归分割产生更多区域，具有当前标签和最大化状态。 | [hou.Pane](https://www.sidefx.com/docs/houdini/hom/hou/Pane.html) |
| PaneTab | 编辑器/视图实例。内部名称与显示标题不同；当前标签是所在 Pane 的选择状态。`setType` 会替换实例，旧引用失效。 | [hou.PaneTab](https://www.sidefx.com/docs/houdini/hom/hou/PaneTab.html) |
| FloatingPanel | 独立窗口，通常容纳 Pane 与 PaneTab；可附属 Desktop，并随其保存和显隐。单标签简化浮动窗口也存在，它没有普通 Pane 容器。 | [hou.FloatingPanel](https://www.sidefx.com/docs/houdini/hom/hou/FloatingPanel.html) |

因此，业务编辑器实例、标签组、分割几何、原生窗口和命名工作区是不同责任。Houdini 的 Panel 通常指窗口容器，不能直接等同于本项目的业务面板。

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

### 原生手势观察

Houdini 22.0.429 下，Pane 菜单 Split Top/Bottom 在新 Pane 生成当前标签的副本；普通左键把 Performance Monitor 标签拖到新 Pane 的标签栏未产生变化。前者与 HOM 规定的新标签同类型一致；后者只说明这次手势未成功，不能推导 H22 不支持移动，也不能推导必须按 Ctrl。具体拖动按钮/修饰键和边缘分割仍待核实；该观察来源不同于上表官方文档。

## 键盘、保存和视觉条件

- Ctrl+B 操作鼠标所在 Pane 的扩展；Ctrl+PgDn / Ctrl+PgUp 切换鼠标所在 Pane 的标签，首尾循环。[Interface intro](https://www.sidefx.com/docs/houdini/start/intro.html)、[Pane management](https://www.sidefx.com/docs/houdini/basics/panes.html#tips-and-tricks)
- 快捷键按上下文解析；20.5 起动作定义、上下文、默认绑定由不同 JSON 文件承担，绑定文件为 `.keymap2`。因此不从旧版快捷键海报推定 H22 全部默认键位；关闭、分割和撕出等键位以当前菜单或 Hotkey Manager 核对。[Configuring hotkeys](https://www.sidefx.com/docs/houdini/basics/hotkeys.html#transitioning-hotkey-systems)
- Desktop 支持保存、另存、重新载入。管理器提供复制、删除和 No Save，存储位置可属于安装、用户或当前场景目录；桌面文件使用 `.desk`，可用 `.nosave` 阻止持久修改。[Desktop Manager](https://www.sidefx.com/docs/houdini/ref/windows/desks.html)、[Configuring Houdini](https://www.sidefx.com/docs/houdini/basics/config.html#preventing-user-changes)
- 布局自动保存是可选项，文档默认关闭。Global UI Scale 缩放整个界面；标签可显示类型图标及类型/上下文颜色，颜色可仅为顶端条纹。[Houdini Preferences](https://www.sidefx.com/docs/houdini/ref/windows/mainprefs.html)

公开接口规定浮动窗口尺寸必须大于零，但未公布 Pane 的通用最小尺寸或 UI Scale 与 Windows DPI 的完整换算公式。[hou.FloatingPanel](https://www.sidefx.com/docs/houdini/hom/hou/FloatingPanel.html) 对本项目应在同机 Scale 1.75 条件下记录字体、标签栏、分隔线、菜单和业务内容边距；换算必须经过已有 DPI/SS 入口，不能把观察到的物理像素直接当作 DIP 常量。

## 本机 1.75 缩放的补充观察

2026-10-01，使用用户调好的 Houdini 22.0.429。激活窗口后取得 1745×1125 的逻辑坐标截图；未激活时曾返回 2560×1392 的另一坐标捕获，后续测量只使用激活后的同一坐标系。Windows 物理像素与这里的逻辑坐标不可混算。

| 元素 | 激活后的截图测量 |
| --- | --- |
| Pane 标签栏 | 约 24 高，未选 #292929，选中连接 #3d3d3d 内容面 |
| 数值输入框 | 约 24 高，输入内部 #131313 |
| 参数行节奏 | 约 31，行间约 7 |
| 标签到字段的横向起点 | Pane 左侧至字段约 180 |
| 下拉和分组 | 约 26–28 高 |
| 分隔线 | 约 6–8，带明暗边缘 |

现场观察：Split Top/Bottom 在原 Pane 下创建同内容的新 Pane；关闭新增标签后空间重新合并；最大化按钮隐藏其他 Pane，再点击恢复此前分割；Tear Off Pane Tab Copy 产生带相同标签栏和路径栏的原生浮动窗口，原 Pane 保留。以上没有证明任意缩放与 DPI 下的尺寸，也不替代本项目的产品验收。
