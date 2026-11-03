# Houdini Pane 与参数细节参照

资料查阅日期：2026-10-01。范围：收起/展开、分隔条三段操作、参数颜色与滑条。所查英语手册标识 Houdini 22.0；历史官方教学图仅用于补充箭头方向，不能证明 H22 的具体绘制尺寸。官方文档与原生 UI 观察分别标明，本项目现状见[UI 模块](../modules/ui/README.md)。

## Pane 收起与分隔条

| 位置/手势 | 官方明确语义 | 来源 |
| --- | --- | --- |
| 分隔条拖动 | 调整相邻区域尺寸。 | [Desktops and panes](https://www.sidefx.com/docs/houdini/basics/panes.html#pane-management) |
| 中央附近箭头 | 收起分隔条一侧的区域；再次点击可展开。 | [Desktops and panes](https://www.sidefx.com/docs/houdini/basics/panes.html#pane-management) |
| 分隔条中央单击 | 交换分隔条两侧的 Pane。 | [Setting up a new pane layout](https://www.sidefx.com/docs/houdini/basics/panes.html#setting-up-a-new-pane-layout) |
| 分隔条中央 Alt+单击 | 将竖直分隔变为水平分隔，或反向转换。 | [Setting up a new pane layout](https://www.sidefx.com/docs/houdini/basics/panes.html#setting-up-a-new-pane-layout) |
| 历史教学图的上/下箭头 | 官方示意标注上箭头 collapse up，下箭头 collapse down；中央明确标注左键交换、Alt+左键变向。 | [SideFX Foundations 官方图](https://media.sidefx.com/uploads/tutorial/foundations/houdini_foundation_overview_01.pdf) |
| Stow / Stow Pane Tabs / Stow Panebar | 分别显隐 Pane、标签栏、路径/顶端操作栏，属于不同动作。 | [Pane actions](https://www.sidefx.com/docs/houdini/basics/panes.html#actions) |

文档支持“收起某一侧再恢复”，没有规定收起后细条宽度、保留哪块边框、恢复比例、嵌套分割的传播规则。H22 的左右箭头对应哪侧、收起后剩余恢复手柄的位置与点击范围，应以同机 Scale 1.75 实测记录。三段手柄可与拖动条共存，但点击与拖动阈值需要项目自己明确。

## 参数颜色不能只按 RGB 判断

官方把参数框**底色**解释为通道/动画状态：无色为未动画；绿为当前帧有关键帧；黄为未提交变化；蓝为关键帧之间；紫为 Python 表达式；灰蓝为静音；橙为 CHOP 覆盖；深绿为空通道。[Parameter editor: Background color](https://www.sidefx.com/docs/houdini/network/parms.html#background-color)

另有独立的**通道列表标记**：加入 Channel List 的参数在右下角显示小点；标记可使用动画编辑器中的曲线颜色，也可显示为单色。偏好项名称为 Show Dot on Parameter in Parameter Editor 和 Color Parameter Dot with Channel Color。[Channel color dot](https://www.sidefx.com/docs/houdini/network/parms.html#channel-color-dot)、[Animation preferences](https://www.sidefx.com/docs/houdini/ref/windows/mainprefs.html#animation)

当前官方文字没有把截图中的 XYZ 彩色底线定义为永久轴色，也没有说明 H22 新 UI 是否将上述小点绘制成底线。**将 RGB 底线当作通道标记是待验证推断**。可在同一参数上移入/移出 Channel List、改变曲线颜色及切换相关偏好，观察底线是否相应变化；再在未动画与已关键帧状态之间比较底色。

非默认参数以粗体显示，也与上述颜色语义独立。[Parameter editor: Defaults](https://www.sidefx.com/docs/houdini/network/parms.html#defaults) 本项目若仅增加 XYZ 识别线，应明确这是项目轴提示，不能伪装成尚未实现的动画通道状态。

## 数值编辑与滑条

- 文本框可直接输入数值；Ctrl+中键恢复默认值。[Parameter editor: Editing](https://www.sidefx.com/docs/houdini/network/parms.html#editing)
- 数值滑条范围由参数接口 Range 定义；低/高端锁定才约束手工输入不能越界。因此“滑条范围”和“业务合法值范围”不能无条件合并。[Operator Type Properties: Range](https://www.sidefx.com/docs/houdini/ref/windows/optype.html)
- `hou.Parm.rangeMin/rangeMax/setRange` 操作实例的编辑器滑条范围；恢复范围优先使用参数设置的默认范围，否则使用参数模板范围。[hou.Parm: Range](https://www.sidefx.com/docs/houdini/hom/hou/Parm.html#range)
- 整数/浮点参数可用 `sidefx::slider=none` 隐藏数值框旁的滑条；官方将仅用于表达式的参数列为用途。[Operator Type Properties: Common tags](https://www.sidefx.com/docs/houdini/ref/windows/optype.html#common-tags)
- 中键按住参数名称/数值可用数值阶梯：上下选改变量级、左右改值；名称作用于整组，字段作用于单分量；Alt+中键改为乘法。阶梯没有滑条的默认范围。[Using the ladder](https://www.sidefx.com/docs/houdini/basics/ladder.html)

普通参数滑条的轨道点击是否跳转、拖动超出范围时是否自动扩展、Shift/Ctrl 精调方式、键盘步长、连续拖动的历史合并，以及向量是否显示每分量滑条，当前查到的官方手册未明确。不能拿 Animation Toolbar 的百分比滑条或 HUD 浮动滑条规则替代参数编辑器规则。

建议实测最少覆盖：单浮点与 XYZ、未锁定/锁定范围、手工输入越界、滑条左键拖动/轨道点击、Ctrl/Shift 拖动、Esc 取消、一次拖动后撤销。项目接入应复用同一数值字段与已有历史入口，滑条只提供另一种改值手势；本段为工程建议。

## 原生观察的边界

Houdini Indie 22.0.429、UI Scale 1.75 下的激活窗口截图逻辑尺寸为 1745×1125。右侧横分隔位于 y=617，三个操作区中心约为 x=1271 / 1304 / 1343：上箭头收起参数区，剩余条位于 y=132；重按恢复到 y=617；中央点阵单击交换参数区和网络区，再次单击还原，分割位置保持不变。这是固定环境的外部观察，不是本项目 DIP 尺寸。

Alt+中央点阵的依据仅为官方文档，没有原生 Alt 点击实测。用户提供的 Tab 栏截图没有前置收拢按钮。Atelier 的轨道点击、Shift 细调、键盘、取消及撤销行为由[UI 合同](../modules/ui/README.md#可组合参数控件)维护，不作为 Houdini 已观察行为。
