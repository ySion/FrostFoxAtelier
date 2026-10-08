# 视口编辑阶段验收 — 2026-10-07

状态：首轮验收通过；后续输入与模式栏调整已通过后台验收，原生窗口复验待执行，范围见文末。目录为 `E:/FrostFoxAtelier`，基础提交为 `402fab075bb2aa6605b51fdb767387ebd0e08869`，包含本地未提交改动。当前操作合同见[编辑器模块](../modules/editor/README.md#视口编辑)。

## 验收范围

| 范围 | 结果与证据 |
| --- | --- |
| Dev / Release 后台专项 | `atelier_editor_document_acceptance --viewport-edit` 均通过；使用真实 World、网格、Session、Runtime 和 DSL 布局 |
| Release 原生窗口 | `atelier_editor --acceptance-edit` 通过；正式 Win32 输入路径、D3D12 绘制及 GPU 读回截图 |
| 选择与历史 | 真实网格拾取、Shift 多选、框选、组删除/恢复、活动 Actor 和组件检查目标恢复 |
| 变换 | 通用手柄、拖动、G/R/S、轴约束、键入数值、一次提交一次撤销、重做、非法值拒绝、取消和捕获丢失回滚 |
| 参数 | Position/Rotation/Scale 同行元组、四元数与角度联动、连续输入到 120°、原值 Enter 不增加历史 |
| 资源检查 | 选择 Cube 显示 Mesh、`builtin/cube`、Engine 和 12 个三角形；切回 Scene Tree 恢复实例属性，已检查实际截图 |

同波次完整文档消费者、共享 UI（23/23）和 Project Scene 曾在 Dev / Release 通过，Release Project Integration 也通过。本次最后修正涉及变换数值计算及窗口断言，已重新执行双配置视口专项和 Release 原生窗口；没有重复运行无关模块。

## 验收中修正

- 资源行改用宿主统一分配的控件 key，消除与外壳 key 冲突导致的黑屏。
- 原生资源检查不再从仅包含编辑框的 `text_geometry` 查找静态标签；后台验证实际元数据控件，窗口验证切换、UI 提交并检查 GPU 截图。
- float 字段使用其自身精度格式化，颜色显示为 `0.19` 等最短可往返数值。
- 欧拉角连续输入使用事务起始姿态；转换采用 double 中间计算，45° 编辑及撤销显示为 `45`。
- 非法变换预览不能确认先前的有效值；Ctrl+Z/Y 先结束视口草稿再执行历史，组件检查目标与多选活动 Actor 随撤销正确恢复。

## 本机证据

证据位于忽略的 `out/` 构建目录，未作为源码提交：

- `out/takeover-dev-edit-final.log`
- `out/takeover-release-edit-final.log`
- `out/takeover-edit-release-accepted/acceptance.txt`
- 同目录 `multi-selection.png`、`box-selection.png`、`transform-parameters.png`、`resource-parameters.png`、`edit-mode.png`

独立验收窗口正常退出，原编辑器窗口保留。本次不包含 Dev 原生窗口复验、真实系统 IME、多显示器 DPI 迁移、表面吸附或跨区域资源拖放；不据此宣称这些能力已验收。

## 后续：游标、吸附与模式栏

原生观察用户当前的 Blender 4.5.14 LTS 设置：左键定位游标、右键选择，W 打开对象菜单，Snap 子菜单提供选择/游标到网格和相互定位。观察 Houdini 22.0.429 的 Move/Rotate 切换：左侧工具选择、上方工具选项和视口内模式名称同步变化，视图菜单位于画面右上。上述观察不代表其他键位预设的默认行为。

本轮已接入同一 DSL、命令和 Session 事务链：

- 左键游标、右键拾取/框选、W 操作菜单；中键环绕、Shift+中键平移。
- 红白圆环与十字游标；优先定位实际网格表面，可关闭表面定位使用构造平面。
- 移动与游标共用网格开关和步长；整组移动按活动 Actor 的世界网格对齐，保留组内偏移。
- 可折叠工具选项栏、操作模式名称、透视/三个正交视图，参考网格随视图选择平面。
- View 菜单按模式、枢轴、吸附、视图分组；相同菜单复用同一命令描述及勾选/互斥状态。

Dev / Release 构建和视口端到端专项均通过，覆盖新增的游标与选择分离、网格/临时反转/撤销、正交投影拾取、DSL 模式文本、折叠高度、菜单状态，以及被动标签和交互菜单的命中区分。完整编辑器文档链与共享 UI 23/23 也在本轮通过；随后菜单分组与新增断言通过视口专项复验。

本机日志：`out/viewport-input-{dev,release}-edit.log`、`out/viewport-input-{dev,release}-document.log`。新的 GPU 绘制与原生输入验收尚未执行，不沿用首轮截图作为本轮的视觉证据。独立验收驱动已包含新操作路径，等待可保持前台的桌面时段。
