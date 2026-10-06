# 参数标题栏与图标方案

日期：2026-10-06。依据：用户参考图、现场 Houdini Indie 22.0.429 的参数编辑器操作、当前源代码和下列官方资料。

## 参考与本轮落地

现场确认：路径栏、对象标题、内容页签是三个独立层次。对象标题包含类型图标、类型名称、黑色名称编辑器和紧凑图标按钮。点击放大镜，在对象标题与页签之间展开搜索行，输入框自动获得焦点；再次点击收起，页签向上恢复。名称编辑器与数值编辑器使用相同高度，图标按视觉中心对齐；空白处不堆放操作说明。SideFX 的[参数编辑器说明](https://www.sidefx.com/docs/houdini/network/parms.html#searching-and-filtering-parameters)同时明确了搜索入口与可选搜索模式。

本轮由 `ObjectHeader` / `Parameters` 和 `editor.ss` 实现紧凑标题、类型图标、放大镜与可收起搜索，沿用共享单行编辑器及现有参数行。真实 Actor 类型、名称、Position 与 Components 保留。收起时暂停筛选、保留查询文字，是本编辑器明确采用的行为；不将它描述为未经验证的 Houdini 隐藏状态语义。未实现的预设、节点帮助、动画及 gizmo 不添加空按钮。完整 TRS 和这些业务能力仍按编辑器能力清单演进，当前不宣称整张 Houdini 面板已逐像素复制。

## 当前图标从哪里来

- `src/ui/style.hpp` 的 `ControlIcon` 是控件图标标识。
- `src/ui/session.cpp` 将它映射到 `TextStore::Symbol`。
- `src/ui/text.cpp` 的 `TextStore::symbol` 用圆、线段和几何覆盖判断生成单色 R8 Coverage，采用 4×4 子像素采样，按图标和实际像素尺寸缓存；重复帧复用该缓存。
- 渲染沿现有 Coverage 上传和绘制路径。当前没有加载 SVG 文件，也没有使用第三方 PNG 图标包。文件夹、网格和搜索符号都是自有几何图案。

这个路径适合箭头、勾选和分隔条等简单符号；复杂彩色对象、资源和工具图标需要美术素材。仅向几何分支持续追加复杂造型会增加维护成本，也难以达到参考图的质感。

## 素材比较

| 图标库 | 官方提供与许可 | 对当前编辑器的判断 |
| --- | --- | --- |
| [Fugue](https://p.yusukekamiyamane.com/) | 作者提供 3570 个 16×16 PNG，另有扩展图标和 PSD 源文件；CC BY 3.0，作者另提供免署名商业许可 | 像素精修、彩色桌面工具风格，适合紧凑树和资源列表；仅使用原始 16px 图不能覆盖大尺寸工具栏和高 DPI |
| [Oxygen](https://develop.kde.org/frameworks/oxygen-icons/) | KDE 桌面图标，LGPL 3 或更高；官方另有[应用分发说明](https://techbase.kde.org/Projects/Oxygen/Licensing) | 立体、材质化的视觉更接近 Houdini。选用时应保留对应 SVG 源、许可证及署名，并按官方说明分发外部素材 |
| [Fluent System Icons](https://github.com/microsoft/fluentui-system-icons) | Microsoft 提供 SVG 等资源，[MIT 许可](https://github.com/microsoft/fluentui-system-icons/blob/main/LICENSE) | 适合统一搜索、信息、帮助和文件操作等通用符号；线性风格与 Houdini 彩色工具图标有明显差别 |

[CC BY 3.0 官方摘要](https://creativecommons.org/licenses/by/3.0/)允许包括商业用途在内的复制和改编，要求署名、许可链接及修改说明。最终纳入哪个包，就在资产目录保留该包的原始许可和所用文件清单；本轮仅研究，尚未引入这些素材或新增依赖。

## 建议

认可小尺寸 PNG 作为运行时格式，优先选择为目标尺寸绘制的图，而不是把大图机械缩小。目标是清晰的 16/20/24/32px 及对应 DPI 变体，按实际绘制尺寸选取，避免只提供一张 16px 位图再任意拉伸。有 SVG/PSD 源时保存源资产，构建阶段生成 PNG；运行时无需引入 SVG 解释器。

通用符号继续使用一套一致的图形语言；Actor、组件和资源类型可以使用彩色图标。接入 PNG 时扩展现有控件图标入口和渲染资源缓存，保留 ControlIcon/图标身份、控件状态、DPI、纹理所有权与缓存失效的一条路径，避免另建图标控件或每次绘制解码文件。色彩图标需要 RGBA，现有单通道 Coverage 不能直接保留彩色信息。

## 验收证据

- 开发与发布配置的 `editor_document_acceptance` 通过：默认收起、展开位置、隐藏时暂停筛选、重开查询保留、空选中状态、文档重置，以及原有 Source/JIT、项目和 Table 消费链。日志：`out/parameter-search-dev.log`、`out/parameter-search-release.log`。
- 开发版普通窗口完整验收通过，接受 2728 帧。原生鼠标点击、空格重开、自动焦点、名称与数字字段等高、名称与图标居中、查询不进入场景历史及停靠/菜单/编辑/项目原有链通过。证据：`out/parameter-search-20261006/dev-window-final/acceptance.txt`，截图 `parameters-search-closed.png` / `parameters-search-open.png`。
- 开发版项目与真实保存热更验收通过：展开的查询输入会话在布局/文字更新、编译失败与恢复后保留，Actor 换绑和各资源视图同时通过。证据：`out/parameter-search-20261006/dev-authoring/run-90880-431851359/acceptance.txt`，同目录 `ui-authoring.csv` / `asset-ui-authoring.csv`。
- 当前用户的发布版编辑器窗口保持运行；本轮编译了发布版应用库并通过后台消费者，未替换被该进程占用的发布版可执行文件。新 UI 的真实窗口验收使用独立开发版进程。
