# 原生 UI 下一增量：研究与实施方案

状态：**S1 已实施并通过 dev/release 验收，待主持收口；S2/S3 待启动。**

日期：2026-09-28。代码基线：`9d294866b035a40a09abecbd8540c04cdcd826b0`；方案分支：`codex/ui-foundation`。
本页是编辑器当前排期的唯一入口。[首版记录](first-editor-plan.md)保留已交付的 M0/M1 与尚未完成的 M2 目标；保存重开、JIT 单步暂不进入本轮。
应用负责人汇总三席只读研究（参数、工作区、UI 结构）及 UI/RHI 负责人反馈；本轮未运行 Houdini，以下严格区分官方文档观察、仓库事实和本项目选择。

## 1. 本轮可观察结果

用户在同一正式编辑器中创建 A、B，固定参数面板查看 A，再选择 B；修改 A 的 XYZ、恢复默认值及撤销重做时，场景仍选中 B。取消固定后面板跟随 B。
参数区具有统一状态样式、通用单行文本与数值编辑、真实参数搜索、非默认值提示和一层参数菜单。搜索可用中文输入法输入“位置”并匹配位置参数；复制、剪切、粘贴、选择和光标沿同一文字路径工作。

| 范围 | 内容 |
| --- | --- |
| 已有并继续使用 | 一个 HWND/正式应用；LayoutDocument；DirectWrite 测量与 R8 覆盖图；已接受帧命中；焦点/捕获/incarnation；Runtime 对象、场景成员及位置事务；增删/历史、相机、分隔条 |
| 本轮实施 | 统一样式与控件状态；共享 TextEdit；参数搜索；Unicode 剪贴板与中文 IME 接线；XYZ 默认值/修改提示/重置；一个参数面板 follow/pin；一层菜单及完整输入优先级 |
| 延后 | 无真实字段的 checkbox/enum 参数；value ladder；表达式/参数引用；多行/富文本；完整国际化；动态面板/自由停靠/联动通道；节点图、gizmo、拾取；RGBA 图片、非矩形裁剪 |

Pin 按钮是查看状态，菜单项是命令，均不冒充 bool/enum 业务参数。搜索属于 Workspace 查看状态，不新增 Runtime 字符串字段。位置仍用现有专用事务；出现真实第二类业务参数后再评估通用化。

## 2. 官方观察与本项目取舍

| 官方材料及可确认行为 | 对本轮的启发与边界 |
| --- | --- |
| [SideFX 参数编辑器](https://www.sidefx.com/docs/houdini/network/parms.html)：非默认值强调、恢复默认值；参数复制涉及值或引用语义 | 实施默认值提示及重置。普通文本剪贴板采用 Windows 文字语义；不宣称已实现 Houdini 参数引用复制 |
| [参数定义](https://www.sidefx.com/docs/houdini/ref/windows/optype.html)：分量默认值；滑条范围与锁定输入边界有区别 | 复用 `position_parameters` 的默认值和真实合法边界；不把未来显示范围直接当业务限制 |
| [Value ladder](https://www.sidefx.com/docs/houdini/basics/ladder.html)：标签可调全部分量，字段可调单分量，纵向选量级、横向调值 | 先保留已有数值拖调。一次 undo、Esc 回滚与像素速度由本项目定义；文档不能证明 Houdini 原生事务细节 |
| [面板与固定](https://www.sidefx.com/docs/houdini/basics/panes.html#pinning)：固定查看目标，未固定时跟随；另有联动通道 | 只实施一个面板的 follow/pin；不将完整停靠、标签系统或通道框架作为前置 |
| [节点标志](https://www.sidefx.com/docs/houdini/network/flags.html#setting-flags)、[上下文菜单](https://www.sidefx.com/docs/houdini/basics/tabmenu.html)、[快捷键上下文](https://www.sidefx.com/docs/houdini/basics/hotkeys.html)：选择、显示输出与渲染标志不同，命令依赖上下文 | 分开场景选择、面板目标、菜单打开时的目标；不建立一个包办所有用途的全局对象变量 |

**本项目已获主持认可的产品选择：纯位置 undo/redo 保持当前场景选择；增删历史维持现有选择策略。** 其余具体交互合同如下，均为本项目方案，不以 Houdini 文档替代实机证据。

## 3. 对象目标、事务与失效

- `Workspace` 保存查看模式及固定的 `ObjectKey`；有效目标为固定键或 `Session::selected()`，每次使用都校验场景成员。Pin 不持有业务 root。Runtime、场景和现有编辑/历史根继续承担保活。
- A 固定、B 选中时，读取、标题、控件绑定、编辑和重置均指向 A。删除 A 后保留固定键，显示“固定对象已移除”，停用参数与菜单业务动作，绝不自动改绑 B。删除 undo 将同一稳定键恢复为成员后，面板自动重新显示 A；历史淘汰导致 A 无法恢复时持续显示空态，直到用户取消固定。
- follow 模式实际切换目标，或普通焦点离开当前字段时：合法草稿先提交，非法草稿取消，然后切换。Pin A 后选择 B 不改变查看目标；点击列表造成的普通字段失焦仍遵循上述规则。OS 失焦、Surface 变化和交互取消回滚未完成编辑。
- 正在捕获的数值/分隔拖动先完成或取消，菜单/目标切换不抢夺手势、不排队延后执行。取消必须到达已有 active 事务，不能被“现在的面板目标不同”挡住。
- 原地替换 `Session::begin_position()` 为 `begin_position(ObjectKey target)`；开始时校验成员并取得现有事务根，preview/commit/cancel 继续使用 active 中的目标。同步迁移全部调用者并删除无参入口。
- 默认值来自 `position_parameters`。重置 X 只改 X；重置位置以一个 before/after Vec3 事务写入 XYZ，只产生一条历史；写入失败沿现有取消路径恢复 before，无变化不记历史。modified 按实际值与默认值比较，编辑草稿与 invalid 状态另行表达。
- 打开重置菜单先处理普通草稿。合法草稿提交后再执行重置属于两次操作，undo 重置返回刚提交的值；非法草稿取消后重置基于原值。非法数值粘贴不能写 Runtime。

### 位置历史的准确迁移

基线 [Session::replay](../src/editor/session.cpp) 的 `Kind::position` 分支在写入成功后执行 `selected_ = change.member.key`。
本轮删除该分支的选择赋值，保留唯一位置回放路径；后面的 add/remove 分支不改选择政策。
前：编辑 A 后选 B，再撤销位置会选回 A。后：固定 A、选 B，编辑/重置/undo/redo A，B 始终保持选中。删除 A 的 undo 仍按已有规则选择恢复的 A，不把位置规则扩大到增删历史。

## 4. 输入顺序与菜单

在 `Application::native` 与唯一 `ui::Session::input` 链中调整顺序，不另建菜单输入循环：

1. **生命周期清理先处理**：关闭、OS 失焦、capture_lost、Surface/DPI 失效同时清理 UI、IME 和相机状态。菜单不可吞掉清理；尺寸过渡仍允许释放/取消，不允许旧几何激活新动作。
2. **已有手势拥有者**：UI 或相机已有 capture 时，后续 move/up/Esc/丢失捕获交还该拥有者直到结束。相机右键拖出视口也能结束，不以当前是否在 viewport 内判断归属。
3. **正在输入的 IME**：组合中的 Enter/Esc 先确认/取消组合，不同时提交字段、执行菜单或触发应用命令。普通编辑在焦点/上下文转换时按第 3 节收束。
4. **已接受帧中的 popup 拦截**：先按呈现帧判定拦截，再校验当前 incarnation、目标成员和 enabled。已显示但目标失效的菜单仍吞掉事件，不能执行或穿透；关闭请求发出后，直到无菜单帧被接受前仍保留该拦截责任。
5. **普通 UI 控件**：处理命中、滚动、焦点和文本键；只有未消费的事件才进入 viewport 导航，随后才是应用快捷键。移除当前相机在 UI 前抢占右键/滚轮的分支。

首例为锚定参数字段的一层菜单：“恢复此分量默认值”“恢复位置默认值”，支持箭头、Enter、Esc。菜单打开时固定对象/字段上下文；执行时不重新读取全局 selection。
菜单绘制、命中及拦截元数据属于同一个 UiFrame，在布局根的 overlay 层输出，避免继承参数面板裁剪；仍用同一 Control/Action/DrawQuad 路径。
外部点击关闭且消费该次点击；执行或 Esc 关闭后焦点回到仍存活的锚点，否则回面板，再否则清空。owner 消失、失焦、Surface 变化取消菜单，不退回另一个业务目标。Esc 按 IME→菜单→普通编辑逐层处理。

## 5. 样式、文本与平台边界

### 同一套样式和文字服务

UI 提供一个小型 `Style`（字体、颜色、间距、边框等 token）和语义角色，`Session::set_style(Style)` 是候选入口。统一解析 normal/hover/pressed/focus/selected/disabled/invalid，Workspace 只声明角色、语义状态及结构布局。
颜色变化复用 R8 覆盖数据；字体族、字重或度量变化更新文字键/内容版本及测量 provider revision，不能继续把 provider 写死为 1。最终删除 Workspace 与 `prepare` 的重复颜色、`Control::font_size/background` 临时覆盖入口；有实际需要的差异通过同一角色解析表达。
保留 LayoutDocument 与 TextStore。文字测量、视觉命中、选择区、caret 和 raster 使用同一 DirectWrite layout；扩展现有 caret 查询为点命中、范围矩形及必要的边界信息，不增加第二套文字 renderer。

### TextEdit 与 number 共用编辑过程

- 同一编辑状态管理草稿、选择、caret、水平滚动、插入/删除、剪贴板及组合文本；number 只增加解析、范围、格式化策略，沿现有位置事务发出操作。
- S2 开工前须裁决快捷键的焦点/活动编辑边界：首版及 S1 中，number 只有焦点、没有未提交草稿时仍消费 Ctrl+Z；Esc 退出后才进入场景历史。S1 保留并验收两种上下文。共享 TextEdit 不得把该现状直接当作成熟交互，需明确何时处理草稿撤销、何时交还场景历史。
- `Action` 的数值载荷原地迁移为初期需要的 `variant<monostate, double, wstring>`。不预建无消费者的 bool/enum 载荷。
- 参数搜索是真实文本消费者：Workspace 保存查询，匹配参数显示名/别名（包括“位置”、Position、XYZ），非组合草稿可即时过滤；Esc 恢复本次编辑前查询，不产生业务历史。搜索框自身不被过滤。
- Win32 UTF-16 文本先整理成拥有所有权的字符串事件，替换当前把单个 `WM_CHAR` 强转为 `char32_t` 的链路。完整替换旧 ASCII 特判，禁止 number 与 text 各保留一个缓冲。
- 编辑边界至少保持代理对、组合重音及本轮验收的 ZWJ 序列完整；caret/选择覆盖混合方向文本的视觉位置。共享边界函数承担删除/移动规则，DirectWrite 承担排版命中。DWrite shaping cluster 不等于完整 Unicode 字素规则；不凭 UTF-16 存储宣称全 Unicode 支持，具体实现和限制随 S2 实测记录。

### 剪贴板、IME 与错误回路

平台接线归 `Window`/`Application`，UI 通过同一 Reply/Event 合同输出请求、接收结果和给出已接受帧的 caret 矩形；UI 不直接持有 HWND/HIMC。
剪贴板采用 `CF_UNICODETEXT`，文本有界且由事件拥有。往返带 `{control_key, incarnation, edit_generation, revision}`，过期结果拒收；剪切在平台写入成功后才删选区，失败保留缓冲。按 [SetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setclipboarddata) 的成功所有权转移规则管理内存。
拟采用一个 IMM32 接线与系统候选窗，保留自绘 TextEdit；不另放隐藏 EDIT 文本模型，也不同时维护 TSF 替代路径。技术选择必须通过真实 Microsoft Pinyin 验收，失败则该切片未完成，由主持评估修正或替换。

- WndProc 当场复制 [WM_IME_COMPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-composition) 的组合/结果字符串与属性，不将临时指针或稍后读取的 HIMC 状态排入队列。组合只作临时显示，未确认内容不进搜索或数值解析。
- 组合开始固定 edit_generation/基础 revision；临时组合更新不推进已提交草稿 revision。结果进入统一插入路径，取消、目标替换或选择变化结束该绑定。普通焦点转换舍弃尚未提交的组合，再按合法/非法规则处理已有草稿；OS 失焦取消整个编辑。
- 一个 native poll 内依序应用消息；不把整批消息盖上同一旧 revision 后逐条拒收。消费 IME 结果时避免系统再生成一遍 WM_CHAR，依据 [Microsoft 官方样例](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/winui/input/tsf/tsfapps/tsfpad-step1/TextInputCtrl.cpp) 核对消息默认处理。
- 通过 [候选窗定位](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immsetcandidatewindow) 使用 HWND 客户区坐标中的 caret/排除矩形；随滚动、DPI 与已接受帧更新，遵守系统可调整位置的合同。
- `Application::effects/action` 按顺序处理编辑动作。begin/preview/commit 失败后停止该组后续动作，调用同一取消路径并同步 UI 状态，保留首个错误；回滚失败明确显示。替换当前 `status(...)` 后盲目继续、可能覆盖错误的行为，不另建事务管理器。

## 6. 文件归属与替换清单

应用负责人拥有 Window/Application、Workspace/编辑事务、整体窗口验收和根接线；UI 负责人拥有 `src/ui/`、UI 模块 CMake/验收及唯一 CPU 绘制协议；RHI 负责人保持 renderer 消费合同并协助测量。Runtime 线仅在真实接口缺口出现时介入。本轮未发现必须先扩展 Runtime 或 RHI 的需求。

| 旧入口/真实消费者 | 同一入口的变化 | 删除时点 |
| --- | --- | --- |
| [Workspace::describe](../src/editor/workspace.cpp) 与 [Session::prepare](../src/ui/session.cpp) 的颜色/字体分支；`Control::font_size/background`；UI/编辑器验收描述符 | 统一 Style/角色/状态解析，消费者全部迁移 | S1，删除散落覆盖字段及硬编码状态分支 |
| [TextStore::set/measure](../src/ui/text.cpp)、Session 文字更新；固定字体与 `provider_revision != 1` | 字体键、样式度量版本与同一缓存失效链 | S1，删除固定 provider 合同 |
| `Session::prepare` 对屏外控件直接 raster/emit | 候选 bounds∩clip 可见性检查，保留布局/滚动/身份信息 | S1，替换原生成位置，无第二遍兜底绘制 |
| `Session::input/change_text/finish_edit` 的 ASCII 专用分支、单位置 caret；[Event/Action](../src/ui/session.hpp)；[Window::receive](../src/application/window.cpp)/`Application::native/action` 与验收消费者 | 共享 TextEdit、字符串/组合事件、typed value、同一 TextStore 查询和平台往返 | S2，删除旧 `character`/纯 double 处理及专用编辑缓冲，不留兼容入口 |
| `Application::effects/action` 失败仍继续发动作、错误被后续状态覆盖 | 有结果的顺序派发、同一取消回路 | S2，与文本/数值迁移一起完成 |
| `Workspace::describe` 标题/字段/键全部取 selected；`Application::action` 的 selected-only guard；[Session::begin_position/replay](../src/editor/session.cpp)；[scene 验收](../acceptance/editor/scene.cpp) | 显式目标、follow/pin、默认值与单一位置回放 | S3，删除无参 begin、旧 selected guard 与位置回放的选择赋值 |
| `Session::hit` 先过滤当前存活性、`prepare` 统一继承父 clip；`Application::native` UI 前相机抢占 | 同帧 overlay/拦截，先拦截后活性校验，已有 capture 优先、未消费事件导航 | S3，替换原路由；普通子控件仍保留正常父裁剪 |

数值解析器、Layout、已有焦点/capture/incarnation、DrawQuad/R8、Runtime 根和历史都是继续使用的有效机制。无需为迁移新增别名、第二种主题、备用输入后端或独立演示产品。

## 7. 实施切片与端到端终点

主持已批准路线并启动 **S1**；S2/S3 待 S1 验收收口后另行启动。每个切片完整迁移并清除对应旧路径后再交付；UI 与应用协议变化在同一集成点一起更新消费者。各负责人从文档提交 `fcb9ddf` 开新工作分支，不复用首版遗留开发分支，统一集成至 `codex/ui-foundation`。

| 切片/主责 | 正式应用中的可观察终点 | 必须一起完成的清除与验收 |
| --- | --- | --- |
| S1 样式与可见内容；UI 主责，应用迁移描述符 | 对象列表、按钮、数值字段、分隔和状态文字具有一致状态表达；改变统一字体后测量、caret、像素仍对齐；滚出视区不生成无用文字内容 | 清除表中样式/provider/屏外生成旧路径；dev/release 真实 HWND 覆盖焦点、非法值、hover/pressed/selected/disabled、滚动、resize/DPI 过渡与原有编辑链 |
| S2 同一文本/数值编辑；UI+应用平台接线 | 参数搜索接受中文 IME；文本/数字均可选择、移动光标、横滚、复制剪切粘贴；数字拖动、提交、Esc 和一次 undo 保持可用 | 删除 ASCII 专用缓冲和旧事件/载荷处理；dev/release 同应用验收真实输入法确认/取消/候选位置、不重复插入、非法粘贴、剪贴板失败/过期、控件移除/失焦、代理对/组合字符/ZWJ/双向文本；记录实际 IME/系统与字符覆盖限制 |
| S3 参数查看和上下文命令；应用主责，UI 提供 overlay/路由 | pin A/select B→编辑/重置/undo/redo A，B 保持选中；删除固定 A 显示无效态，undo 恢复可查看；unpin 跟随；菜单操作不驱动相机 | 删除 selected-only 参数绑定/无参 begin/位置回放选中副作用及相机抢占分支；dev/release 覆盖合法/非法草稿切目标、整向量一次 undo、无变化无历史、菜单外点击/键盘/失效/旧帧、相机捕获释放、清理事件和关闭 |

端到端都从 README 的同一 `atelier_editor` 入口及现有窗口验收扩展，不建立单元/mock 框架。真实 IME 体验不能用合成 WM_IME 消息冒充；必要的故障注入只补充正式路径的失败分支证据。报告区分实机、消息注入和静态检查。
每片交付更新对应模块的当前合同及 README 实际能力；保留原有增删、拖动/文本撤销、相机、尺寸过渡和关停行为，除本页明确裁决的位置历史选择语义。已知缺陷或遗留双路径阻止切片结项；延后的新功能不宣称完成。

## 8. 性能边界与验收证据

现有 renderer 每个 quad 一次 draw，使用不可变 R8 内容身份/版本缓存，帧末淘汰不用的 GPU 内容；滚出再滚回可能重新上传。普通 tint/位置/clip 变化无需新纹理；整段文字变化仍可能重栅格与上传。不预建 atlas、保留预算缓存或批处理框架。
S1 在 UI 生成候选内容时先检查 bounds∩clip，再 raster/emit；布局与输入身份仍保留，绘制、命中和菜单拦截随同一完整帧发布。renderer 的 accepted/skipped/终态合同不变。
集成后用真实场景对象列表（现有上限 256）滚动和连续参数编辑采样 release 的 prepare/render 时间分布、栅格次数、上传字节/draw 数及持有资源量；只加完成该测量所需的计数。根据结果决定是否存在下一项优化，报告推断与实测分开。
当前真实业务参数只有位置 XYZ，不能由此宣称已验证高密度参数面板；出现真实更密集参数消费者后再测量，不制造假字段或另建 benchmark 产品。本轮尚无性能改善测量结论。

## 会审结论入口

主持已关闭研究会审，批准三片顺序并授权 S1 生产修改、双配置真实窗口验收及本地提交/集成。S1 已完成统一样式、真实字体度量失效、屏外内容剪除及全部消费者迁移；dev/release UI 8/8、场景与各 597 帧窗口链通过，实测和证据位置见[编辑器验收](modules/editor/README.md#验收入口)。当前待主持收口；S2/S3 仍待启动，不推送或合并主线。
