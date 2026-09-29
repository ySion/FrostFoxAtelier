# 首个场景编辑器

唯一产品入口为 `atelier_editor`；启动和构建命令见[根 README](../../../README.md)。
当前沿[对象模型合同](../../research/game-world-object-model.md)的 IV 将正式 Actor/Component/World 接入同一编辑器；源码迁移已完成，集成验收进行中。已收口的 S1/S2 输入与视觉合同保留，历史证据见下文，不能代替本轮 World 迁移验收。
模型与渲染消费完整 TRS；面板仍只暴露位置 XYZ。旋转/缩放控件、存档重开、JIT 单步、视口拾取、gizmo、节点画布、自由停靠及 S3 面板体系未启动。

## 正式执行链

```text
Win32 窗口消息 → Window 复制拥有的事件 → 每条消息后顺序排空
  → UI 已接受帧命中/焦点/捕获 → Reply.effects
  → Action → editor::Session → world::World → Runtime 的真实 Actor/Component 字段
           → Workspace.query → 参数查看投影
  → PlatformRequest → Window 剪贴板/IMM32 → UI 结果事件
  → Workspace 控件描述 + World RenderItem 矩阵值 → UI 帧 + renderer 方块实例
 → renderer 最终 accepted → UI 接受对应命中帧
```

WndProc 的输入事件只复制和排队，不重入 UI/业务；同步输入回调也在同一泵中依序消费。UTF-16 代理项在顺序投递时组装，组合流绑定独立 composition_id 与原编辑身份。候选位置只使用与当前 token 相匹配的 accepted caret。同步 IMR_QUERYCHARPOSITION 只读对应帧的插入/组合起点快照，将客户区物理坐标转为屏幕坐标；仅支持偏移 0，无有效快照或其他偏移明确拒绝。快照在原生组合/失焦/Surface 变化时立即撤下，不用旧显示 caret 冒充组合起点。IME 终止键的首次按下、重复和抬起保持同一归属，避免确认组合后又提交字段。
剪贴板失败保留草稿/选区/历史供重试；业务 Action 失败停止同组剩余普通 effects，仍执行 UI 取消、业务回滚、IME 与捕获清理，保留首错并报告清理失败。

[World](../../../src/world/world.hpp)和 Runtime 管理唯一 Actor/Component 身份、组成与成员资格。编辑 World 保持 stopped；Actor 的 AttachedInactive 状态仍可呈现和编辑，renderer 不按 Active 状态筛除编辑内容。
[Session](../../../src/editor/session.hpp)是文档唯一写入口，保存 Actor 选择和统一的 Actor、组件、字段历史。字段事务捕获具体组件 ObjectId、正式 FieldPath 和控制会话，不再从当前选择或类型顺序猜目标。一次拖调连续改真实字段，提交只记一次历史。Transform 字段可能使四元数其他分量规范化，所以记录完整 TRS 前后值，经 World 原子恢复；不是删除时的对象图快照。属性 undo/redo 继续选中记录的 owner Actor。
Actor 删除记录保存普通 Actor 根及 World 成员位置，组件独立移除记录保存组件根及 owner/顺序/依赖恢复资料。Undo 恢复同一 ObjectId 的当前组成与成员资格，重新生成 UI 绑定。裁剪历史/redo 与关闭文档只释放持有，不隐式 destroy。历史最多128项，编辑器最多256个 Actor；只有 World 成员可编辑，没有离线写入或多写者冲突机制。
World 操作的 Result 失败表示提交未完成；成功 Commit 中的 notification_error 表示关系已改变。Session 先记录变化并更新选择，应用再报告通知错误并停止后续动作，不把它当成未提交或重做一次。成员 span 只在结构变化前借用，UI/renderer 不持有它。
宿主同线程逐帧以32工作单位推进 GC；普通根保活不表示对象仍参与 World。文档关闭先结束活动编辑，再关闭 World、释放历史；World close 撤成员/参与和根，Runtime 生命周期随后由宿主管理。

## 参数和坐标

位置 X/Y/Z 默认0米，合法域为有限的 `[-10000, 10000]`。普通拖调每逻辑像素0.01米，细调0.001米。
范围、默认值、单位与拖调步长由[应用参数描述](../../../src/editor/session.hpp)绑定 Runtime 字段，布局与 Runtime 不承担编辑器政策。
控件 key 独立单调分配，绑定具体 ObjectId 和 FieldId 路径；稳定控件复用其键，失效后退役且不再分配。Actor/组件恢复保留业务身份，但不会恢复旧控件会话、IME 或剪贴板请求。

- 左手坐标系，+Y向上，单位米；方块各轴范围 `[-0.5, 0.5]`。World 提供完整 `S * R * T` 矩阵，renderer 不自行重算 TRS；首片没有空间父子操作。
- 默认相机 eye `(6,5,-8)`、target `(0,0,0)`、up `(0,1,0)`；使用 LookAtLH/PerspectiveFovLH。
- CPU 矩阵行主存储，行向量按 `p * world * view * projection` 计算，D3D 深度为 `[0,1]`。
- viewport 与 UI clip 使用窗口客户区左上为原点的物理像素。1280×720客户区内的 `{240,32,760,660}` 表示从 `(240,32)` 开始、宽760高660的视口。
- UI 根据实际 backbuffer 尺寸和 DPI 生成 draw/clip；renderer 不再次缩放。深度目标显式匹配实际 backbuffer。

## 首版操作

工具条提供空 Actor 新增、删除、撤销、重做、聚焦选择和重置相机；左侧列表负责 Actor 单选，右侧提供实际 Transform/Cube 组件增删和位置字段。首片 UI 每个 Actor 只添加一次 Transform/Cube。Actor 没有 Transform 时位置字段和 Focus 停用；移除 Cube 后 Actor 与 Transform 仍保留。右侧参数区仍可拖动分隔条调整宽度。
数值控件普通拖动用于调值，单击进入文字模式后拖动用于选字。首次实际文字修改才建立事务；合法 Enter 提交，非法 Enter 保留草稿，Esc 恢复整次编辑的原值。结束后保留字段焦点，数值退出文字模式。字段焦点下 Delete/Backspace 只编辑文字。
文本与数值共用 Unicode 字素边界、视觉光标、双向选区、横滚及本地撤销/重做。Ctrl+A/C/X/V 提供全选、复制、剪切、粘贴。存在本地事务时 Ctrl+Z/Y 只操作本地历史，即使历史耗尽也不穿透；仅聚焦无事务时进入场景历史，Ctrl+Shift+Z 同样重做。历史合组、容量和 Unicode 覆盖见 [UI 模块](../ui/README.md#共享单行编辑)。
参数搜索按 Position/“位置”/X/Y/Z 名称与别名过滤实际位置行。确认文字实时过滤，IME 预编辑不改变查询；Enter 或普通换焦点提交查询，Esc/OS 失焦/Surface 失效恢复原查询。查询属于 Workspace 查看状态，不写场景历史。
右键拖动视口旋转观察，滚轮缩放；Focus 或 F 聚焦选中对象，Reset camera 恢复初始相机。
调整窗口尺寸、DPI变化、失焦或外部捕获丢失都会取消当前未提交编辑。应用主动释放捕获产生的同步通知不会误取消刚进入的文本编辑。

## UI 样式与诊断

Workspace 只声明 `normal/panel/title/caption` 角色及 enabled/selected 等语义状态，样式值由唯一 [UI Style](../../../src/ui/style.hpp) 提供。工具条与参数标题共用 title 层级，说明和状态文字使用 caption；容器结构 padding 留在 NodeInput，按钮、列表项和数值文字的内容内距由 Style 提供。工具条按钮宽度按文字测量并保留原最小宽度，字号改变时不再靠固定宽度裁掉末字。
`Application::set_style` 直接进入 UI 的整体校验/应用路径，应用不保存另一份主题。颜色复用文字 Coverage，字体族、字重和字号经同一 TextStore 更新度量、caret 和栅格；已接受帧仍保有旧几何与不可变内容。具体样式、版本及状态合同见 [UI 模块](../ui/README.md)。
`Application::sample_frames` 显式开启 CPU prepare/render 计时，默认关闭；Capture 帧不计入。窗口验收从同一应用按钮添加真实场景成员至 256，再采样列表滚动与位置拖动。UI 累计计数按段求差，renderer 的 `FrameResult::stats` 按实际采样帧合计；CPU Coverage 和 renderer cache 的持有峰值分别记录。
render 计时包括 Present/等待，R8 上传字节只表示有效 payload，缓存 texel 字节不表示物理显存。验收使用硬件 D3D12 debug/GBV 与 vsync，含 release 配置；数据用于说明该验收负载，不能直接当作关闭验证的产品帧耗时，也没有同负载改前基线可用于百分比提升结论。

## 帧与关停

应用在 renderer 的最终结果成功且确认 accepted 后才切换命中帧，skip 保留旧帧。
accepted extent 固定为录制/呈现时尺寸，frame末尾resize后的新尺寸仅用于下一帧。
尺寸/DPI过渡期取消press/capture并暂停指针命中，释放/失焦/捕获丢失继续处理；匹配帧接受后恢复。
上传也是录制；录制开始后的错误没有回滚，终态错误停止新交互与后续帧。普通帧不调用finish，诊断截图才显式同步。
关闭停止输入并取消编辑，释放renderer拥有的资源后关闭RHI，最后销毁窗口；成员/历史根先于Runtime析构。

## 验收入口

`editor_document_acceptance`消费正式 World/Session，覆盖空 Actor、实际组件增删、单次字段历史/控制会话、完整 TRS undo、成员资格、删除跨 GC 的同身份恢复、历史裁剪普通根保组/最终收集及关闭释放。旧 Scene 库和位置专用入口已删除。
`atelier_editor --acceptance <输出目录>`运行同一正式窗口，用Win32消息和系统焦点/捕获操作驱动控件，再核对Runtime状态与GPU帧。
窗口链包含分隔、增删/历史、列表选择、实时拖调/一次undo/Esc、文字Delete、非法值、相机、失焦/捕获、resize、最小化和正常退出。
S1 覆盖正常/hover/pressed/focus/invalid/disabled、颜色改变的同Coverage/零raster/零上传、非法Style整体拒绝、字体family/weight/size及caret同步失效、旧帧几何/像素寿命。256真实成员由工具条加入，验证屏外控件几何仍在、文字不生成、滚回恢复，以及连续拖调历史。
S2 扩展覆盖本地/场景历史归属、非法 Enter/保留焦点、文本模式拖选、真实参数过滤、代理对/组合字符/ZWJ 删除、双向选区、CF_UNICODETEXT 读写。用真实剪贴板占用验证失败保留草稿；预先建立真实 Session 事务，使随后原生拖动的 begin 失败，验证后续 preview 停止、回滚/捕获释放和首错保留。
截图来自 renderer 实际颜色/深度 readback，输出 `m0.png`、`editor.png`、`s1-pressed/focused/invalid/disabled/color/font/list-256.png`、`s2-search-unicode/bidi-selection/number-selection/failed-edit.png` 与 `acceptance.txt`。首版和已收口 S1 证据分别保存在各配置的 `editor-evidence-v1/`、`editor-evidence-s1/`；被后续运行覆盖的工作目录不作为 S1 原始证据。
DPI 验收覆盖当前显示器上的 WM_DPICHANGED 过渡政策，不据此声称已验证跨不同 DPI 显示器迁移。Unicode WM_CHAR 和 UI 组合协议注入不代表真实系统输入法通过。

### S2 集成状态与输入法验收

本节是 World 迁移前的已验收记录。当前 `Window`/共享文本后端未修改，本轮不重复未受影响的真实 IME 验收；组件字段接入仍通过普通窗口的 Unicode、剪贴板和目标失效链检查。

2026-09-29：共享 UI 提交 `60472ed` 已集成为 `0e45c6a`；当时 dev/release 的 UI 12/12 通过，实际文字服务报告 ICU 72.1 / Unicode 15.1。`afb2286` 的应用平台接线与验收驱动在 dev/release 编译通过，顺序运行 `editor_acceptance` 均通过：Scene PASS、窗口各接受 709 帧，含原有场景/窗口链及 S2 非 IME 操作。该版本原始报告和截图另存于 `out/{dev,release}/editor-evidence-afb2286/`，与后续 IME 工作目录分开保留。

组合起点快照 `4de055a` 已集成为 `7cd9c28`，同一集成树 dev/release UI **13/13** 通过。最终 Window 包含 IMR_QUERYCHARPOSITION 单起点应答、原生边界立即失效及输入绑定更换的上下文生命周期。目标失效/更换时依序取消、解绑旧上下文，发布新绑定，再恢复原默认上下文；原生事件捕获时保存 token，取消/解绑的同步回调仍归旧目标。正常确认的 revision 推进不解绑。最终产品与验收驱动已完成双配置编译和以下独立实测，证据归档于 `out/{dev,release}/editor-evidence-s2/`，不复用旧版本报告。

| 最终验收 | dev | release |
| --- | --- | --- |
| 本机搜狗真实输入 | PASS；starts/results 各3，candidate changes 57 | PASS；starts/results 各3，candidate changes 57 |
| 原跨字段 reading 回归 | query=`位置 `，X=0，business editing=0 | query=`位置 `，X=0，business editing=0 |
| 原线程 profile 恢复 | verified=1 | verified=1 |
| 普通窗口全部链路 | PASS；709 accepted frames | PASS；709 accepted frames |

消息验收 helper 显式设置本次 Ctrl/Shift 并清除未请求修饰位，覆盖修饰位继承和意外失焦中断；生产失焦取消语义保留。最终双配置在稳定前台时段顺序完成，没有干预其他进程。

本机搜狗真实输入使用同一 `atelier_editor --acceptance-ime <输出目录>`，CMake 入口为：

```powershell
cmake --build --preset dev --target editor_ime_acceptance
cmake --build --preset release --target editor_ime_acceptance
```

这两个目标须依次运行，并保持编辑器前台且无物理修饰键按住。驱动保存原线程 profile，必要时仅在本线程选择已启用搜狗，退出时恢复并核对原文本服务或键盘布局；不修改用户默认、输入列表或启用状态。实际输入使用 SendInput 扫描码，系统生成的 IMM32 快照进入正式共享编辑路径。GPU 截图保存应用预编辑文字，`s2-ime-native-candidate.png` 保存编辑器所在桌面区域的原生候选窗；`ime-acceptance.txt` 记录系统组合/结果/候选通知及结果。

用户已指定使用现有搜狗实测，当前模块为 SogouTSF.ime 16.8.0.5415。精确系统/profile 信息、此前 Pinyin 设置撤回证据与完整收口要求见[共同合同](../../ui-foundation-plan.md#95-本机环境与真实输入证据)。本轮证据必须分别看待：

- 真实 dev 运行已通过位置应答的屏幕坐标/非法请求检查。原生截图 `out/dev/editor-evidence/s2-ime-native-candidate.png` 确认 WEIZHI 的首候选为“位置”；物理 Space 已通过真实 START → GCS_RESULTSTR → END 将“位置”插入一次，`s2-ime-confirmed.png` 与 `out/dev/sogou-real-result.log` 保存确认结果。候选阶段可只有 VK_PROCESSKEY、IMN_CHANGECANDIDATE 和起点查询，没有 GCS 组合；不以 Session::composing 判断搜狗 reading 是否存在。
- 加强生命周期检查后，在 X=0 的数值字段输入 WEIZHI，切到原查询为“位置”的搜索末端，再按 Space，实际变成“位置位置”。同一搜索字段的干净状态 Space 基准是“位置 ”。最新有界诊断存于 `out/dev/sogou-rebind-diagnostic-20260929/`：旧/新 token 转换正确、同一 HIMC/open=1，CPS_CANCEL 确实调用并返回成功；切换前后 starts/results 均为3，Space 后才变4。Space 前原生候选仍可见；最终 expected UTF16 `4f4d 7f6e 20`、actual `4f4d 7f6e 4f4d 7f6e`，X=0、business editing=0。该次证据排除取消分支未执行及取消期间新 start 排队错绑，表明 pre-START reading 仍存活。
- 加入解绑/恢复后的版本在 dev/release 均通过原真实回归：切字段/失焦/Surface/目标变化后，Space 与同一 IME 的干净状态基准一致。字段内容、旧对象值和业务 editing 分别断言并记录；不以非空候选缓存或结果消息计数增加判失败。候选文本枚举已移除，仅用候选数量观察准备阶段、按本机已观察首候选送 Space。最终 `ime-platform.txt` 的跨字段 expected/actual 均为 `4f4d 7f6e 20`；`ime-acceptance.txt` 与 `acceptance.txt` 分别记录真实搜狗和普通窗口 PASS。最终归档不包含旧失败截图；诊断原始证据单独保留。

驱动只请求一次前台并等待用户切换，随后失焦即停止，不反复抢焦点。临时生产 DEBUG、逐键跟踪、有界取消快照及诊断分支均已移除，保留必要原生计数、失败分类和正式回归。`out/sogou-environment-final.json` 与实测前记录核对，输入法列表/顺序和搜狗默认未变。系统 IME、协议注入、静态环境检查分别记录；此次仅证明本机搜狗 16.8.0.5415 及已观察候选顺序，不代表其他 IME。S2 主持已收口，S3 未启动。

### S1 真实负载采样

以下是已收口 S1 的同一次 release 窗口验收在 AMD Radeon RX 6800 XT、debug/GBV 与 vsync 开启时的结果。原始数据为 `out/release/editor-evidence-s1/s1-performance.csv` 和 `s1-resources.csv`；dev 归档目录保留相同格式。没有同负载改前数据，不声称性能提升比例，也不把这组数据当作 S2 测量结果。

| 256成员负载 | 样本帧 | prepare CPU 均值 / p95（ms） | render CPU 均值 / p95（ms） | 栅格次数 / R8字节 | 上传次数 / 有效R8字节 |
| --- | --- | --- | --- | --- | --- |
| 列表滚动 | 48 | 0.235 / 0.342 | 15.346 / 17.422 | 232 / 370890 | 490 / 780588 |
| 连续位置拖动 | 64 | 0.228 / 0.326 | 16.080 / 16.944 | 64 / 143427 | 64 / 143427 |

滚动/拖动分别累计 UI draw 4245/5632、场景 draw 48/64。采样时 UI CPU Coverage 持有峰值分别为277项/451706字节、277项/457032字节；renderer cache峰值分别为33项/65354 texel字节、32项/64488 texel字节。CPU像素统计不含DWrite对象等开销，cache统计不含gfx待退休资源或物理显存。
滚动中累计剪除11716个屏外文字控件生成，拖动中剪除15616个；滚回可能重新上传是现有帧末GPU缓存淘汰行为。真实业务参数仍只有XYZ，数据不代表高密度参数面板已经验证。
