# 原生 UI 下一增量：研究与实施方案

状态：**S1 主持已收口（`ec6a118`）；S2 新增组合起点几何的 UI 双配置 13/13 通过，Window 位置应答与验收线程输入源选择双配置编译通过。新增代码的最终普通窗口及本机搜狗真实链尚待完成，按用户要求暂停窗口验收；S3 未启动。旧 `afb2286` 双配置各 709 帧仍是该旧版本的证据。**

日期：2026-09-28，更新于 2026-09-29。首版代码基线：`9d294866b035a40a09abecbd8540c04cdcd826b0`；S2 实施基线：`ec6a118130c6c96000e99d4d8296176e3d8c5c88`；唯一集成分支：`codex/ui-foundation`。
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

S1 已通过唯一 `Session::set_style(const Style&)` 校验并应用样式，统一解析 normal/hover/pressed/focus/selected/disabled/invalid；Workspace 只声明角色、语义状态及结构布局。当前合同见 [UI 模块](modules/ui/README.md)。
颜色变化复用 R8 覆盖数据；字体族、字重或度量变化更新文字键/内容版本及测量 provider revision，不能继续把 provider 写死为 1。最终删除 Workspace 与 `prepare` 的重复颜色、`Control::font_size/background` 临时覆盖入口；有实际需要的差异通过同一角色解析表达。
保留 LayoutDocument 与 TextStore。文字测量、视觉命中、选择区、caret 和 raster 使用同一 DirectWrite layout；扩展现有 caret 查询为点命中、范围矩形及必要的边界信息，不增加第二套文字 renderer。

### TextEdit 与 number 共用编辑过程

S2 的状态、事件、平台往返及输入验收统一由第 9 节共同合同定义。保留一个 TextEdit 和同一 TextStore；number 使用数值解析/范围/格式化策略，Workspace 参数搜索是实际文字消费者，不增加业务字段或第二套编辑缓冲。

### 剪贴板、IME 与错误回路

平台接线归 `Window`/`Application`，UI 不持 HWND/HIMC。产品沿唯一 IMM32 接线和系统候选窗推进，本机搜狗真实输入验收是完成条件；不放隐藏 EDIT，也不并存第二种文本后端。平台接口、失败与清理顺序见第 9 节。

## 6. 文件归属与替换清单

应用负责人拥有 Window/Application、Workspace/编辑事务、整体窗口验收和根接线；UI 负责人拥有 `src/ui/`、UI 模块 CMake/验收及唯一 CPU 绘制协议；RHI 负责人保持 renderer 消费合同并协助测量。Runtime 线仅在真实接口缺口出现时介入。本轮未发现必须先扩展 Runtime 或 RHI 的需求。

| 旧入口/真实消费者 | 同一入口的变化 | 删除时点 |
| --- | --- | --- |
| [Workspace::describe](../src/editor/workspace.cpp) 与 [Session::prepare](../src/ui/session.cpp) 的颜色/字体分支；`Control::font_size/background`；UI/编辑器验收描述符 | 统一 Style/角色/状态解析，消费者全部迁移 | S1，删除散落覆盖字段及硬编码状态分支 |
| [TextStore::set/measure](../src/ui/text.cpp)、Session 文字更新；固定字体与 `provider_revision != 1` | 字体键、样式度量版本与同一缓存失效链 | S1，删除固定 provider 合同 |
| `Session::prepare` 对屏外控件直接 raster/emit | 候选 bounds∩clip 可见性检查，保留布局/滚动/身份信息 | S1，替换原生成位置，无第二遍兜底绘制 |
| `Session::input/change_text/finish_edit` 的 ASCII 专用分支、单位置 caret；[Event/Action](../src/ui/session.hpp)；[Window::receive](../src/application/window.cpp)/`Application::native/action` 与验收消费者 | 共享 TextEdit、字符串/组合事件、typed value、同一 TextStore 查询和平台往返 | S2，删除旧 `character`/纯 double 处理及专用编辑缓冲，不留兼容入口 |
| `Reply.actions`、`Application::effects/action` 失败仍继续发动作、错误被后续状态覆盖 | 原地迁移到有序 `effects`、有结果的顺序派发和同一取消回路 | S2，所有消费者同时删除旧 actions 入口 |
| `Workspace::describe` 标题/字段/键全部取 selected；`Application::action` 的 selected-only guard；[Session::begin_position/replay](../src/editor/session.cpp)；[scene 验收](../acceptance/editor/scene.cpp) | 显式目标、follow/pin、默认值与单一位置回放 | S3，删除无参 begin、旧 selected guard 与位置回放的选择赋值 |
| `Session::hit` 先过滤当前存活性、`prepare` 统一继承父 clip；`Application::native` UI 前相机抢占 | 同帧 overlay/拦截，先拦截后活性校验，已有 capture 优先、未消费事件导航 | S3，替换原路由；普通子控件仍保留正常父裁剪 |

数值解析器、Layout、已有焦点/capture/incarnation、DrawQuad/R8、Runtime 根和历史都是继续使用的有效机制。无需为迁移新增别名、第二种主题、备用输入后端或独立演示产品。

## 7. 实施切片与端到端终点

主持已收口 **S1** 并正式派发 **S2**，授权必要生产修改、dev/release 端到端验收及本地提交/集成。双方从包含 `ec6a118` 和已通过共同合同的本次派发提交开工；UI 使用新分支 `codex/ui-foundation-s2-ui`，统一集成至 `codex/ui-foundation`，不重复 cherry-pick S1。S3 不启动；每片迁移完全部消费者并清除旧路径后交付。

| 切片/主责 | 正式应用中的可观察终点 | 必须一起完成的清除与验收 |
| --- | --- | --- |
| S1 样式与可见内容；UI 主责，应用迁移描述符 | 对象列表、按钮、数值字段、分隔和状态文字具有一致状态表达；改变统一字体后测量、caret、像素仍对齐；滚出视区不生成无用文字内容 | 清除表中样式/provider/屏外生成旧路径；dev/release 真实 HWND 覆盖焦点、非法值、hover/pressed/selected/disabled、滚动、resize/DPI 过渡与原有编辑链 |
| S2 同一文本/数值编辑；UI+应用平台接线 | 参数搜索接受中文 IME；文本/数字均可选择、移动光标、横滚、复制剪切粘贴；数字拖动、提交、Esc 和一次 undo 保持可用 | 删除 ASCII 专用缓冲和旧事件/载荷处理；dev/release 同应用验收真实输入法确认/取消/候选位置、不重复插入、非法粘贴、剪贴板失败/过期、控件移除/失焦、代理对/组合字符/ZWJ/双向文本；记录实际 IME/系统与字符覆盖限制 |
| S3 参数查看和上下文命令；应用主责，UI 提供 overlay/路由 | pin A/select B→编辑/重置/undo/redo A，B 保持选中；删除固定 A 显示无效态，undo 恢复可查看；unpin 跟随；菜单操作不驱动相机 | 删除 selected-only 参数绑定/无参 begin/位置回放选中副作用及相机抢占分支；dev/release 覆盖合法/非法草稿切目标、整向量一次 undo、无变化无历史、菜单外点击/键盘/失效/旧帧、相机捕获释放、清理事件和关闭 |

端到端都从 README 的同一 `atelier_editor` 入口及现有窗口验收扩展，不建立单元/mock 框架。真实 IME 体验不能用合成 WM_IME 消息冒充；必要的故障注入只补充正式路径的失败分支证据。报告区分实机、消息注入和静态检查。
每片交付更新对应模块的当前合同及 README 实际能力；除本页明确裁决的 S2 输入政策与 S3 位置历史选择语义外，保留原有增删、拖动、相机、尺寸过渡和关停行为。已知缺陷或遗留双路径阻止切片结项；延后的新功能不宣称完成。

## 8. 性能边界与验收证据

现有 renderer 每个 quad 一次 draw，使用不可变 R8 内容身份/版本缓存，帧末淘汰不用的 GPU 内容；滚出再滚回可能重新上传。普通 tint/位置/clip 变化无需新纹理；整段文字变化仍可能重栅格与上传。不预建 atlas、保留预算缓存或批处理框架。
S1 在 UI 生成候选内容时先检查 bounds∩clip，再 raster/emit；布局与输入身份仍保留，绘制、命中和菜单拦截随同一完整帧发布。renderer 的 accepted/skipped/终态合同不变。
集成后用真实场景对象列表（现有上限 256）滚动和连续参数编辑采样 release 的 prepare/render 时间分布、栅格次数、上传字节/draw 数及持有资源量；只加完成该测量所需的计数。根据结果决定是否存在下一项优化，报告推断与实测分开。
当前真实业务参数只有位置 XYZ，不能由此宣称已验证高密度参数面板；出现真实更密集参数消费者后再测量，不制造假字段或另建 benchmark 产品。本轮尚无性能改善测量结论。

## 9. S2 共同合同（应用与 UI，2026-09-29）

本节已纳入主持的交互裁决并获正式实施授权。S2 只交付共享单行编辑与参数搜索，不提前迁移 S3 的对象 pin、位置回放选择或菜单路由。

### 9.1 焦点、文字模式与事务

focus 表示控件拥有键盘位置；文字模式决定鼠标是在调数值还是选文字；Edit 事务由首次实际文字变化启动。三者分别记录。点击、移动 caret、改变选区或 Ctrl+A 不建立事务；`1` 改成 `1.0` 虽然数值相等，仍是可本地撤销的文字修改。
number 未进入文字模式时，保留原指针阈值和拖动调值；未拖动的 click 进入文字模式并可全选，随后 down/move 用同一 DWrite 命中拖选。拖调完成后回仅聚焦，没有文字事务或本地历史。text_edit 聚焦即可选字和输入；number 仅聚焦时收到有效单行文字事件也进入文字模式并统一插入，包括不能解析成数字的草稿，不恢复 ASCII 白名单。文字导航/选择/剪贴板按键必要时先进入文字模式；仅 Z/Y 按下表决定是否交业务，字段焦点下 Delete/Backspace 仍归文字处理，不得删除场景对象。

| 当前状态 | Ctrl+Z / Ctrl+Y（含 Ctrl+Shift+Z） | Enter | Esc |
| --- | --- | --- | --- |
| 仅聚焦；无事务、无本地历史、无组合 | `consumed=false`，交应用场景历史；不得先 lazy begin Edit | number 非文字模式只进入文字模式；clean 文字模式结束模式；text_edit 无修改不写业务 | 结束文字模式/待处理请求，无业务写入 |
| 已有未提交草稿；当前方向无本地撤销/重做可用 | 消费但不改变值，不穿透到场景 | 合法：提交；非法：保留 invalid 草稿、焦点和文字模式 | 取消整次 Edit，恢复 original |
| 本地 undo 或 redo 可用，含 undo 回原文但仍有 redo | 只移动本地历史；有效新草稿沿同一 preview 更新 | 同上 | 同上 |
| IME 组合中（覆盖上述状态） | 消费，不动本地或场景历史 | 只确认组合，结果作为一次普通文字修改；不再次提交字段 | 只取消本次组合，恢复组合前草稿/选区；不取消已存在的 Edit |

Enter 提交、clean Enter 或 Esc 结束字段时，清事务/本地 undo-redo/组合及 pending 请求、推进 generation，**保留控件 focus**；number 退出文字模式，text_edit 保留可输入焦点但无事务。保留 Tab 位置，下一次 Z/Y 可进入场景历史。实际换焦点则归新控件；OS 失焦或控件失效清焦点。模式转换与提交/取消只响应一次 Enter/Esc 按下序列，不能因 repeat 或对应 WM_CHAR 再走一次转换。
数字拖动之后或仅聚焦时，场景 undo/redo 改变权威值，下一次 `set_controls` 同步被动缓冲及合法选区，并更新 revision；不能让旧缓冲覆盖新权威值。`wants_keyboard()` 表示字段文字输入能力，Z/Y 归属只依 `Reply.consumed`。

**S2 有意改变 S1 的三项行为**：仅 number 焦点不再吞场景 Z/Y；非法 Enter 保留草稿而非取消退出；字段结束后保留 focus。迁移原窗口断言，不能靠绕开命令证明新行为。未提交的非法数字草稿不发业务 preview/commit；已有合法 preview 保持到后续合法修改或整次取消，取消恢复事务原值。

### 9.2 搜索、选择与本地历史

- Workspace.query 是真实查看投影，匹配参数名/别名（“位置”、Position、X/Y/Z）；查询控件始终存在。首次真实修改发 `edit_begin(original)` 和 `edit_preview(draft)`；后续非组合修改和本地 undo/redo 即时过滤，preedit 不过滤。查询不进入场景历史。
- Enter 或普通换焦点提交最后查询；Esc、OS 失焦、Surface 失效、控件移除/禁用/折叠取消并恢复 original。仅聚焦无修改不发 begin/cancel，不回写外部值。数字普通失焦则合法提交、非法取消；尚未确认的组合先取消，再处理已有草稿。
- 一个 TextEdit 共用 UTF-16 缓冲、anchor/caret（含 trailing affinity）、横向滚动、original 和本地历史。索引单位是 UTF-16 码元，编辑端点落共享字素边界；不允许孤立代理项。单行事件中的换行或超长插入整笔拒绝，不截断字符或向多个参数分发。
- 点击/拖选使用同一 DWrite layout 点命中；Left/Right 沿视觉顺序到合法字素端点，Shift 扩选，Home/End 到行端，Ctrl+A 全选；删除/替换使用逻辑范围。双向选区用 `HitTestTextRange` 多矩形，不能用两个 x 相减。至少验收非 BMP、组合重音、variation selector、明确 ZWJ 序列和双向混排；记录边界实现/Unicode 版本及实测限制，不将 shaping cluster 宣称为完整字素规则。
- 连续普通键入、连续 Backspace、连续 Delete 各可合组，须同类且 caret 连续；移动/选区、pointer、Ctrl 命令、焦点/模式、clipboard/IME 操作断组，无时间阈值。cut/paste/一次 IME result 各一组，preedit 不入历史；新修改清 redo。
- 单活跃会话 undo+redo 最多 64 组，保存的 UTF-16 历史文字合计最多 64 KiB（每码元 2 字节）；任一上限淘汰最老完整组。original 和当前缓冲另受 text_edit 4096、number 64 码元上限约束。可用简单文字/选区快照，不要求差量算法；淘汰不能改变快捷键归属或破坏 Esc 恢复 original。

### 9.3 唯一共享头与平台往返

UI 维护 `src/ui/session.hpp` 的唯一合同，以下形状在共享头提交时一次落定，不保留旧别名：

| 合同 | 最小形状与不变量 |
| --- | --- |
| Control / Action | `ControlKind::text_edit` 使用已有 `Control.text` 作权威投影；number 保留 value/min/max/step 策略。`Action.value = variant<monostate,double,wstring>`；activate/select 无值，number/splitter 编辑用 double，文字编辑用 wstring；保留现有 ActionKind |
| Event | 删除 `character`；文字事件拥有完整 `wstring`。Key 增加 c/x/v，Z/Y及Shift使用现有修饰位；平台结果、组合事件都是拥有的数据，不带 HWND/HIMC/临时指针 |
| EditToken | `{key, incarnation, generation, revision}`。generation 标识焦点编辑上下文，结束/重绑换代；普通草稿、caret、selection 改变推进 revision。preedit 的临时内容/cursor 不推进基础 revision |
| ClipboardRequest/Result | kind=read/copy/cut，完整 token、唯一 `operation_id`；请求可带选中文字，结果带 success/native_error/拥有的 text。UI 分配 id；kind+id+完整 token 匹配且尚未结算才应用一次。同 revision 的两个请求也不得误配 |
| ImeRequest/Result | kind=complete/cancel，完整 token、独立 `composition_id`；结果带 success/native_error。成功只表示原生调用成功，不能据此合成确认文字；确认/结束以 OS 事件为准，旧命令不得作用于新组合 |
| CompositionEvent | start/update/end/cancel、`composition_id` 和固定绑定；可选 result 字符串与可选拥有完整 text/cursor/属性的 preedit 快照，区分缺失与空。可在同条原生事件内先含 result 再含新 preedit，两者均处理；原生取消不伪装为空文字提交 |
| Reply | 原地删除 `actions`，唯一 `effects = vector<variant<Action,PlatformRequest>>`；PlatformRequest 为上述有限 clipboard/IME 请求，capture/cursor 沿用已有字段。无通用命令注册或第二请求队列入口 |
| TextCaret / 查询 | `UiFrame::caret = optional<{token,Rect bounds,Clip clip}>`，为客户区物理像素；frame/surface/DPI 复用 drawing。`Session::text_input()` 返回可选当前 token/composing/accepted_caret；只有与当前身份、焦点、token、Surface 相容的 presented 帧才提供位置 |

剪贴板使用 `CF_UNICODETEXT`，按 [SetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setclipboarddata) 管理成功时的所有权转移。cut 在匹配写入成功后才删除请求时选区；paste 读取成功后走同一插入和本地历史。过期、重复、失败结果都不改缓冲；普通剪贴板失败保留 Edit/history/selection 供重试并显示原生错误，不取消整字段。
业务 begin/preview/commit 失败则停止该组剩余普通 effects，执行唯一 UI cancel/业务 rollback 与必要 IME/capture 清理；保留首个错误并明确报告 rollback 失败。清理不得因停止队列而遗漏，后续成功 status 不得覆盖首错。

### 9.4 原生消息、组合与 accepted caret

1. 原地调整 `Window::poll`：逐条 Dispatch 后，将复制拥有的 NativeEvent 交 Application 依序处理及应用 effects，再接下一条消息。WndProc 的输入事件只复制/排队；平台调用引发的同步输入回调也排队，不能重入 Session/业务，不另建输入泵。同步几何查询只读已拥有的 accepted 快照作答。普通文字按当前合法上下文依序应用，不给整个 poll 盖同一旧 revision。
2. [WM_IME_COMPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-composition) 的 result/preedit/cursor/属性当场复制。start 分配 composition_id，绑定当前 token/base revision 和原替换选区；后续沿该组合流，不按最新 focus 重新盖标签。取消使流失效，迟到结果丢弃，真正新 start 才可重绑。
3. 组合 result 进入统一文字插入及本地历史；数值解析/搜索只消费确认文字。同条事件 result 后仍有新 preedit 时，后者基于刚更新的草稿/revision 开始下一段，不能因旧标签拒掉，也不能丢弃。普通选区/目标变化先取消当前组合；取消恢复组合前选区，保留之前已确认的 Edit 草稿。
4. 原生 IME 处理期间，终止组合的 Enter/Esc 的 keydown/repeat/keyup 均保持 IME 归属，不能结果先到后把 keyup/重复 down 当字段命令；Ctrl+Z/Y 也不落本地或业务历史。普通输入仅由拥有的文字事件插入，不能 keydown 与 WM_CHAR 各插一遍。消费 result 后按 [Microsoft 样例](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/winui/input/tsf/tsfapps/tsfpad-step1/TextInputCtrl.cpp) 避免默认处理再产生重复文字。
5. 候选位置只用匹配 accepted caret，经 [ImmSetCandidateWindow](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immsetcandidatewindow) 给系统客户区位置/排除矩形；没有匹配 caret 时等下一接受帧，不用未呈现的候选几何。字体、横滚、DPI 同时更新文字与 caret；失焦/失效立即撤销可用锚点，旧帧仍可持有但不能作为新目标的输入位置。
6. [IMR_QUERYCHARPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/imr-querycharposition) 同步查询只支持本轮实际使用的组合 UTF-16 偏移 0。UI 随帧发布单值 `TextInputAnchor{token,composition_id,composition_revision,bounds,clip}`；`TextInputState::accepted_anchor` 只有当前身份及组合显示版本相符时可用。无组合时取插入/替换选区起点，组合时取组合起点，同一 DWrite 布局提供字符左上位置与行高；不能把显示 caret 当作任意组合偏移。Window 在所有 IMM 调用前复制或清空快照，并在原生组合/失焦/Surface 变化时立即撤下；WndProc 用 ClientToScreen 将物理 bounds 左上及字段 text_clip 转成屏幕坐标，填写 IMECHARPOSITION，不再次乘 DPI。无有效快照、无焦点、结构尺寸错误、空指针或非零偏移返回 0，不保留系统输出指针。该快照是小型 optional 值，不增加每帧共享对象或字符位置数组。

### 9.5 本机环境与真实输入证据

用户已明确指定“微软拼音被我移除了，测搜狗就可以”。S2 的真实 IME 收口目标改为本机现有搜狗，不要求恢复微软拼音，不据此宣称其他输入法通过。
2026-09-29 实机系统 build **26200.9457 / 25H2**；用户输入列表为 zh-Hans-CN 搜狗拼音与 en-US 美式键盘，默认覆盖为搜狗。实际搜狗模块 `C:\Windows\System32\SogouTSF.ime` 的 FileVersion/ProductVersion 均为 **16.8.0.5415**。搜狗 TIP 为 `{E7EA138E-69F8-11D7-A6EA-00065B844310}`，profile 为 `{E7EA138F-69F8-11D7-A6EA-00065B844311}`，语言为 `0804`。

`atelier_editor --acceptance-ime` 检查并保存当前活动 profile 的完整 type/lang/CLSID/GUID/HKL，`GetActiveProfile` 必须返回 S_OK；S_FALSE 表示无活动 profile，不当作成功。若线程当前是美式键盘等其他 profile，仅在本验收线程选择已启用搜狗；成功或失败退出都恢复原 type1 文本服务或 type2 键盘布局。入口不启用输入法、不改用户默认或输入列表。产品只有 IMM32 接线；TSF profile API 只用于验收环境选择与核对。

此前按启用授权写入的 Microsoft Pinyin 用户 enable 位及追加可选项，在收到用户改用搜狗的指令后，已通过原受支持 API 精确撤回；未覆盖用户整份设置。`out/pinyin-profile-reverted.txt` 与 `out/pinyin-environment-reverted.json` 记录 enable=0、原列表/顺序及搜狗默认与操作前一致。这段记录不代表微软拼音验收通过。

真实搜狗验收要求：

- 同一 `atelier_editor`、同一消息泵与 TextEdit。发送物理键前核对完整搜狗 profile，结束恢复并核对原线程 profile；保持用户默认、列表和启用状态。
- 确认窗口前台、交互桌面与输入权限后，使用真实硬件输入或 [SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) 的物理键/扫描码键序列输入拼音并选候选。按系统实际候选文本定位“位置”，不假定它始终是第一个候选。失去目标前台即停止送键，记录实际前台窗口身份；不得把文字直接灌入替代组合。
- 搜索中实际拼出“位置”，检查候选窗、确认/取消、组合 Enter 不提交字段、结果只插入一次；再覆盖数字 invalid 草稿、焦点/Surface/目标变化、横滚与 caret 定位。原生候选窗在 OS 层，须原生窗口区域证据或人工观察记录，renderer readback 只证明应用绘制。
- 报告分别列出 **系统 IME 驱动**（实际版本/profile、键序列、系统组合流及可视候选）、**故障消息/结果注入**（过期/重复 token、剪贴板失败、迟到组合、业务失败回滚等）和 **静态环境检查**。SendMessage/合成 WM_IME 只算注入；搜狗结果不代表微软拼音或所有 IME 通过。真实链未通过时 S2 未完成。

### 9.6 文件归属、迁移顺序与终点

| 阶段 | 唯一负责文件/动作 | 交接与清除条件 |
| --- | --- | --- |
| 共同合同确认后：共享头提交 | UI：`src/ui/session.hpp`；TextEdit/TextStore 私有设计仍归 UI | 一次提交 ControlKind、拥有事件、Action.value、effects、tokens、caret/query；应用仅按明确 SHA 消费，不复制头、不留旧字段兼容 |
| UI 实现与平台接线并行 | UI：`src/ui/session.cpp`、`text.hpp/.cpp`、必要私有编辑实现、`acceptance/ui`、UI CMake/文档；应用：`Window::NativeEvent/receive/poll`、`Application::native/action/effects` 及转发、Workspace 查询和 `acceptance/editor`、根接线 | UI 删除 numeric-only Edit/ASCII 分支和单 x 选区查询；应用删除 char32 WM_CHAR 强转、旧 double 访问与 `.actions` 消费。业务位置入口/回放保持 S1，RHI 不扩展 |
| 唯一分支集成 | 应用只 cherry-pick UI 明确提交到 `codex/ui-foundation` | 同一 TextEdit、DWrite、Reply.effects 和 Win32 泵贯通全部消费者；平台失败与业务失败分别处理，清理无遗漏；中间接口提交可短暂未构建，不能带兼容入口交付 |
| dev/release 同窗口验收 | 应用统筹，UI 提供真实文字服务验收 | 四状态键盘矩阵、number 两种指针模式、权威值同步、搜索提交/回滚、分组和有界本地历史、Unicode 选区/删除、剪贴板成功/失败/过期、业务失败清理、真实搜狗及候选、原有场景/窗口链；全部通过才提交主持收口 |

主持已批准按上述顺序实施。用户已指定使用现有搜狗完成真实输入验收；通过前 S2 不结项，S3 不启动。

## 会审结论入口

主持已收口 S1：`ec6a118` 干净集成树、dev/release UI 8/8 与各 597 帧窗口链及原始截图通过，证据见[编辑器验收](modules/editor/README.md#验收入口)，不重复验收。第 9 节是应用/UI 的唯一 S2 实施交接依据。新增组合起点几何已按 `4de055a` → `7cd9c28` 集成，UI 双配置 13/13 与应用编译通过；新增 Window 接线仍待最终普通窗口及真实搜狗验收，旧 `afb2286` 的各 709 帧记录不覆盖新增产品代码。用户已要求稍后再测，当前不弹窗/送键/自动重试；[当前集成状态](modules/editor/README.md#s2-集成状态与输入法验收)分别记录已见候选、尚未确认以及最新前台中断的证据。S3 未启动，不推送或合并主线。
