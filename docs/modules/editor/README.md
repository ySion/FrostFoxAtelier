# 首个场景编辑器

唯一产品入口为 `atelier_editor`；启动和构建命令见[根 README](../../../README.md)。
启动日志和 Help → About 使用构建时生成的同一提交标识及配置；未提交改动标为 `local changes`。`atelier_editor --version` 可在不创建窗口的情况下读取该二进制的标识。工程打开成功后记录实际根目录，About 同时显示当前工程目录和场景路径。标识在构建时刷新，运行时不读取当前目录的 Git 状态。
当前消费者将正式 Actor/Component/World 接入同一编辑器，并使用自动反射、共享调用合同与基础值类型。已收口的 S1/S2 输入与视觉合同保留；当前基础控件增量及其验收入口见下文。World 使用 SceneComponent、StaticMeshComponent 和真实 StaticMeshActor；网格使用中立的不可变 `geometry::MeshRef`，遵循[World 合同](../world/README.md)。
模型与渲染消费完整 TRS；业务面板暴露位置 Vector3，已接入默认值恢复、修改状态和折叠分组。通用参数控件、独立菜单窗口、下拉框、中键数值阶梯及同宿主实验台已落地；旋转/缩放业务字段、JIT 单步、视口拾取、gizmo、节点画布、可浮动编辑器视图、布局文件保存、参数面板固定目标仍未实施。

UI 作者层的后续方向见[声明式 UI 方案](declarative-ui-proposal.md)：类 UXML 标记、现有 SS 与自有语言共享当前构造和事务链。该方案已归档，UI 构造接口及绑定接线尚未实施。

## 工程与文件

工程直接选择文件夹，包含 Config、Content、Source、Cache。已接入创建/打开、场景保存重开、资源目录、三种浏览视图和 OBJ 几何导入；Source 的跨文件编译仍是后续语言接点。格式、操作、资源身份和实际支持范围见[文件夹工程](projects.md)。

## 正式执行链

```text
Win32 窗口消息 → Window 复制拥有的事件 → 每条消息后顺序排空
  → UI 已接受帧命中/焦点/捕获 → Reply.effects
  → Action → editor::Session → world::World → Runtime 的真实 Actor/Component 字段
           → Workspace.query → 参数查看投影
  → PlatformRequest → Window 剪贴板/IMM32 → UI 结果事件
  → Workspace 控件描述 + World RenderItem 矩阵/资源/材质值 → UI 帧 + renderer 网格实例
 → renderer 最终 accepted → UI 接受对应命中帧
```

WndProc 的输入事件只复制和排队，不重入 UI/业务；同步输入回调也在同一泵中依序消费。UTF-16 代理项在顺序投递时组装，组合流绑定独立 composition_id 与原编辑身份。候选位置只使用与当前 token 相匹配的 accepted caret。同步 IMR_QUERYCHARPOSITION 只读对应帧的插入/组合起点快照，将客户区物理坐标转为屏幕坐标；仅支持偏移 0，无有效快照或其他偏移明确拒绝。快照在原生组合/失焦/Surface 变化时立即撤下，不用旧显示 caret 冒充组合起点。IME 终止键的首次按下、重复和抬起保持同一归属，避免确认组合后又提交字段。
剪贴板失败保留草稿/选区/历史供重试；业务 Action 失败停止同组剩余普通 effects，仍执行 UI 取消、业务回滚、IME 与捕获清理，保留首错并报告清理失败。

[World](../../../src/world/world.hpp)和 Runtime 管理唯一 Actor/Component 身份、组成与成员资格。编辑 World 保持 stopped；Actor 的 AttachedInactive 状态仍可呈现和编辑，renderer 不按 Active 状态筛除编辑内容。
World 的类型与字段元数据由[自动反射](../../reflection.md)生成，仍经 `world::register_types` 发布到 Session 拥有的 Runtime。编辑器使用该入口返回的类型和字段路径；生成头、World 库及正式编辑器均由当前工作树、当前配置的 CMake 依赖链构建。
[Session](../../../src/editor/session.hpp)是文档唯一写入口，保存 Actor 选择和统一的 Actor、组件、字段历史。字段事务捕获具体组件 ObjectId、正式 FieldPath 和控制会话，不再从当前选择或类型顺序猜目标。一次拖调连续改真实字段，提交只记一次历史。SceneComponent 字段可能使四元数其他分量规范化，所以记录完整 TRS 前后值，经 World 原子恢复；不是删除时的对象图快照。属性 undo/redo 继续选中记录的 owner Actor。
Actor 删除记录保存普通 Actor 根及 World 成员位置，组件独立移除记录保存组件根及 owner/顺序/空间父节点/根身份恢复资料。Undo 恢复同一 ObjectId 的当前组成与成员资格，重新生成 UI 绑定。裁剪历史/redo 与关闭文档只释放持有，不隐式 destroy。历史最多128项，编辑器最多256个 Actor；只有 World 成员可编辑，没有离线写入或多写者冲突机制。
World 操作的 Result 失败表示提交未完成；成功 Commit 中的 notification_error 表示关系已改变。Session 先记录变化并更新选择，应用再报告通知错误并停止后续动作，不把它当成未提交或重做一次。成员 span 只在结构变化前借用，UI/renderer 不持有它。
宿主同线程逐帧以32工作单位推进 GC；普通根保活不表示对象仍参与 World。文档关闭先结束活动编辑，再关闭 World、释放历史；World close 撤成员/参与和根，Runtime 生命周期随后由宿主管理。

## 参数和坐标

位置 X/Y/Z 默认0米，合法域为有限的 `[-10000, 10000]`。普通拖调每逻辑像素0.01米，细调0.001米。
范围、默认值、单位与拖调步长由[应用参数描述](../../../src/editor/session.hpp)绑定 Runtime 字段，布局与 Runtime 不承担编辑器政策。
控件 key 独立单调分配，绑定具体 ObjectId 和 FieldId 路径；稳定控件复用其键，失效后退役且不再分配。Actor/组件恢复保留业务身份，但不会恢复旧控件会话、IME 或剪贴板请求。

位置使用共享 `append_number_row` 组合三个 number；标题和字段随宽度自动换行，分量共用一行。修改状态逐分量比较 `Parameter.default_value`。数值菜单可恢复当前分量，标题或菜单的整组命令恢复完整位置（包括搜索过滤隐藏的分量）。标题绑定具体 SceneComponent 身份，`Session::reset_position` 经 World 一次应用完整 TRS，再记录一次现有 FieldChange。无变化不增加历史，旋转和缩放保持原值，undo/redo 沿用已有 owner 选择政策。

- 左手坐标系，+Y向上，单位米；方块各轴范围 `[-0.5, 0.5]`。World 组合局部 `S * R * T` 与父世界矩阵；renderer 不自行重算 TRS。空间换父 API 在 World 提供，当前面板编辑根位置。
- 默认相机 eye `(6,5,-8)`、target `(0,0,0)`、up `(0,1,0)`；使用 LookAtLH/PerspectiveFovLH。
- CPU 矩阵行主存储，行向量按 `p * world * view * projection` 计算，D3D 深度为 `[0,1]`。
- viewport 与 UI clip 使用窗口客户区左上为原点的物理像素。1280×720客户区内的 `{240,32,760,660}` 表示从 `(240,32)` 开始、宽760高660的视口。
- UI 根据实际 backbuffer 尺寸和 DPI 生成 draw/clip；renderer 不再次缩放。深度目标显式匹配实际 backbuffer。

## 工作区与停靠

默认布局参照 Houdini 的分区：Scene View 在左，Parameters/Controls 共用右上标签组，Actors 在右下。各视图只保留一份业务状态，可以移到其他标签组或分割区域。

固定视图标签统一使用正体，包括 Scene View；选中状态仅通过灰色底面区分，不使用斜体或焦点描边。标签只绘制右侧分隔边，首标签直接衔接分隔条的收边，不叠加左边框。

- 拖标签到另一个标签栏可合并/重排；拖到内容的左、右、上、下边缘可分栏，松开前有预览，Esc 取消。分隔条可拖动改变两侧比例。两侧箭头收起对应面板为 8 DIP 细条，重按恢复原比例；Tab 栏直接从标签开始。中央单排方点单击交换两侧，Alt+单击切换横竖方向，拖动仍调整比例。
- 分隔条的三个按钮沿条方向各占 36 DIP，连续居中排列，窄小宿主内等比例缩短。背景为 `#494949`，靠前边为黑色、靠后边为 `#333333`；横竖方向共用 SS 边框规则，悬停和按下反馈限制在按钮内部，保持外侧细边连续。
- “＋”紧跟最后一个标签，打开原生菜单，将已有视图移到当前组，或重新打开已关闭的视图；视图的值和历史保留。右端下拉菜单的 Show 则跳转到视图当前所在组。
- 标签的关闭按钮移除其显示；右侧菜单可以重新打开四种视图，并把当前标签移到右方/下方的新区域。空间不足或组中只有一个标签时，对应分割菜单不可用。
- 面板右上角切换最大化/还原；Ctrl+B 对最近操作的面板执行同一动作。Ctrl+Tab / Ctrl+Shift+Tab 切换组内标签，Ctrl+W 关闭当前标签；菜单打开期间由菜单独占键盘。
- **Window → Reset layout** 恢复默认位置及标签；场景、相机、筛选、样本值和历史不因恢复布局而重置。最后一个视图关闭后也可恢复。
- 样本与场景历史按照最近操作的视图选择，允许 Controls 与 Parameters 同时显示。视图隐藏/移动时，未提交交互仍通过 Session 的取消/失焦合同结束；业务字段 key 依对象实际成员生命周期退役，隐藏面板不退役对象绑定。
- Scene View 隐藏时提交空场景矩形，不生成场景实例；其他 UI 继续走原窗口、renderer 和接受帧。参数面板与实验台的标题和内容页签固定，下方内容滚动以适应较矮的区域；窄参数行仍使用公共 Flex 换行。

以 Houdini UI Scale 1.75 为同机参照：数值字段最低 23 DIP、标签列 160 DIP、行间 7 DIP。按 2026-10-02 用户截图复测，菜单栏为 32 DIP，Tab 栏为 22 DIP。菜单栏与工具架统一使用 8 DIP 左侧收起轨道、6 DIP 箭头和 6 DIP 恢复条；轨道完整高度均可点击，正文从同一条右边界开始。当前中英文正文与 Tab 使用 13 DIP Sarasa UI SC，菜单栏文字为 13.5 DIP。Tab 选中后使用灰色底面，键盘焦点不额外画框；文字字段保留输入焦点提示。F10/Alt 菜单快捷键会展开收起的菜单栏；恢复布局一并展开菜单栏和工具架。系统 DPI 仍只有 Window → Surface 的一个换算入口。原始观察见[工作区研究](../../research/houdini-workspace-reference-20261001.md)，栏结构与实现边界见[栏结构校准](../../research/houdini-chrome-20261002.md)。独立原生浮窗、多份同类视图和持久布局属于后续增量。

布局与参数的本轮参照、原生观察及验收记录见 [Pane 细节](../../research/houdini-pane-details-20261001.md)。

## 控件实验台

**Controls** 标签打开实验台；选择 **Parameters** 回到实际场景参数，也可将两者分栏同时显示。实验台使用同一个 `ui::Session`、SS、布局、输入法和 renderer，没有独立绘制循环。

- 可编辑 Position Vector3、Exposure、Unicode 名称、Enabled 勾选、Vector4 和 Projection 下拉样本。Position / Vector4 使用显式轴提示色线；Exposure 同时提供数值框和滑条，合法数值为 0–32、滑条显示 0–16，支持拖动、键盘细调、撤销及取消。数值与下拉共用右对齐标签列，名称位于固定的 Sample 标题行；数值字段从 23 DIP 起，共用黑色输入框、凹入边缘、轻渐变按钮；默认灰白文字、非默认数值加粗、灰色编辑边界和菜单定位；勾选使用勾号，折叠使用三角。
- 右键菜单提供分量/整组默认值、复制和粘贴；Ctrl+中键直接恢复默认值，中键按住调出数值阶梯。整组恢复仍只记一次撤销，全部样本保留统一 Reset。Projection 和 Vector4 均为独立样本，不向 World 添加虚构字段。
- Disable sample fields 检查禁用传播，Show long labels 检查换行，Transform and display 与 Color components 标题支持鼠标与 Enter/Space 折叠。
- 分隔条由视图最小尺寸和当前工作区空间共同约束。窄面板保持字段可达，超出可用高度的样本通过同一 list 滚动。
- 样本最多保留 64 次提交历史；UI 活动草稿仍优先使用共享文本历史。实验台打开时，Edit 菜单 Undo/Redo 与无本地事务的 Ctrl+Z/Y 进入样本历史，Delete 不穿透到场景删除。样本值不写入 World。

真实窗口验收在既有 `editor_acceptance` 中覆盖整向量单次撤销、搜索隐藏修改分量后的整组恢复、修改状态、折叠与稳定字段身份、勾选键鼠行为、最窄面板中的 Vector3/Vector4 与禁用、样本历史隔离及返回业务面板。新增验收覆盖菜单不穿透外部点击/视口滚轮/Delete、Menu/Shift+F10/Esc、下拉键盘选择、菜单剪贴板及非法粘贴取消、中键阶梯/细调/默认值/取消/缓存复用。截图为 `out/<配置>/editor-evidence/controls-*.png`；真实搜狗输入、Space 确认、折叠/切换面板取消组合由同一 `editor_ime_acceptance` 覆盖。

原生菜单的统一开启入口先取消主 UI 当前抓取手势，经原 effects 链回滚业务编辑后再转移鼠标捕获。窗口验收覆盖数值拖动和中键阶梯期间按 F10，随后释放鼠标、关闭菜单、再次移动，确认不恢复旧手势、不新增历史。

本阶段最终验收、review 修复和已知边界见 [2026-10-02 阶段收口](../../research/editor-ui-20261002-closeout.md)。

## 首版操作

顶部菜单栏提供 File / Edit / Create / View / Window。Create 快捷区创建 Actor 或 Static Mesh Actor，View 快捷区聚焦选择和重置相机；Window 打开视图、切换/关闭页签、最大化和恢复布局。应用启动为空场景，不再默认创建演示对象。Parameters 通过真实类型显示对象标题，位置字段编辑根 SceneComponent；Components 页展示实际组成。没有场景根时位置字段与 Focus 停用。

`commands.hpp` 是编辑器命令描述入口，`Workspace::command_enabled` 判定当前可用性，`Application::execute` 执行。菜单和顶部快捷区读取同一描述，快捷键与资源点击进入同一个执行函数。执行时再次检查可用性；UI 只产生通用 Action，不直接修改 World。输入字段和拖动仍通过 Session 事务入口。

菜单栏和工具架通过共享 `append_collapsible_bar` 描述轨道、箭头位置及内容隐藏；工作区高度预算使用该入口返回的实际占用高度。窗口验收比较两栏在展开、分别收起、同时收起、悬停和窗口变化后的物理边界与样式，避免分别固定两个互不一致的尺寸。

顶部工具架使用 18 DIP 分类行与紧凑图标区，总高 56 DIP；左侧窄轨可收起到 6 DIP，保留展开箭头和键盘激活。分类选择跨收起/展开保留，收起不会改变场景历史，Reset Layout 恢复展开。工具按钮统一宽 64 DIP、18 DIP 图标与 10.5 DIP 短标签，菜单保留完整指令名。图标和文字由同一按钮绘制/命中，`icon-placement: above` 由共享 SS 测量与绘制处理。

Asset Browser 是独立可停靠视图，浏览工程目录与引擎资源；单击选择，Add to Scene 执行共享创建命令，支持名称/类型过滤与 List/Details/Tiles，详见[工程与资源浏览](projects.md)。顶部快捷创建继续引用相同内置资源。World/renderer 不按 Cube 名字分支；拖放、吸附和模型缩略图尚未实施。
数值控件支持普通拖动和中键七档数值阶梯；上下移动选步长，左右移动调值，Shift 细调，释放提交一次、Esc 取消。右键或 Menu/Shift+F10 打开参数菜单；Ctrl+中键恢复分量或标题对应整组默认值。菜单期间视口输入暂停，外部点击只关闭菜单，Esc/Tab 返回原焦点。普通拖动用于调值，单击进入文字模式后拖动用于选字。首次实际文字修改才建立事务；合法 Enter 提交，非法 Enter 保留草稿，Esc 恢复整次编辑的原值。结束后保留字段焦点，数值退出文字模式。字段焦点下 Delete/Backspace 只编辑文字。
文本与数值共用 Unicode 字素边界、视觉光标、双向选区、横滚及本地撤销/重做。Ctrl+A/C/X/V 提供全选、复制、剪切、粘贴。存在本地事务时 Ctrl+Z/Y 只操作本地历史，即使历史耗尽也不穿透；仅聚焦无事务时进入场景历史，Ctrl+Shift+Z 同样重做。历史合组、容量和 Unicode 覆盖见 [UI 模块](../ui/README.md#共享单行编辑)。
参数搜索按 Position/“位置”/X/Y/Z 名称与别名过滤实际位置行。确认文字实时过滤，IME 预编辑不改变查询；Enter 或普通换焦点提交查询，Esc/OS 失焦/Surface 失效恢复原查询。查询属于 Workspace 查看状态，不写场景历史。
右键拖动视口旋转观察，滚轮缩放；Focus 或 F 聚焦选中对象，Reset camera 恢复初始相机。
调整窗口尺寸、DPI变化、失焦或外部捕获丢失都会取消当前未提交编辑。应用主动释放捕获产生的同步通知不会误取消刚进入的文本编辑。

### 独立菜单窗口

菜单栏、参数右键、Projection 下拉和 Pane 菜单全部通过同一个 PopupHost，使用独立 owned HWND，主窗口保留输入焦点。F10 或 Alt+F/E/C/V/W 打开顶部菜单；打开后左右键或悬停切换菜单，上下/Home/End 导航，Enter/Space 执行，Esc/Tab 关闭并返回原焦点。快捷键列与分隔线来自同一菜单模型。窗口外点击只关闭，不执行背后的编辑器命令；视口和场景快捷键在菜单期间暂停。

顶部菜单在未展开时不因 hover 改色；按下和展开的标题，以及展开菜单中当前指向的条目，共用 `#b36600` 橙色底面。菜单背景为 `#3a3a3a`，细框上/左为 `#949494`、下/右为 `#262626`。条目最低高 25⅓ DIP，快捷键为右对齐的灰色斜体，分隔线位于组间留白中央。PopupHost 用同一个 SS/文字测量入口计算标题与快捷键两列的宽度；短菜单不留空滚动槽，超出屏幕高度时预留实际滚动条空间。

菜单关闭后，标题恢复透明底面和普通文字颜色；保留的逻辑焦点不产生灰色 active 底面。标题的橙色状态仅由按下和实际展开驱动。

共享滚动范围计算抑制小数行高累计产生的浮点尾差，防止刚好容纳内容的菜单出现几乎不可滚动的滚动条；真实溢出仍保留。UI 端到端验收覆盖四项菜单的初始测量、像素尺寸窗口、滚轮和末项导航，以及 100%/125%/150%/175%/200% DPI。

弹出窗口基于锚点显示器的工作区定位，可超出编辑器边界；下拉空间不足时向上翻转，超宽/超高按可用空间限制并滚动。主窗口移动、尺寸/DPI 改变、失焦、系统取消或捕获丢失关闭菜单；关闭编辑器时先关闭弹出窗口的 GPU 资源，再销毁 HWND。它不创建第二套字段编辑或场景历史。

当前 renderer/RHI 的所有权仍是一窗口一上下文，菜单宿主首次使用时建立并在后续菜单之间复用，隐藏时不渲染，静止时不重新提交，主窗口负责帧节奏。跨窗口共享 GPU 设备和纹理缓存尚未实现；多显示器真实 DPI 迁移需要额外设备验证。现阶段只有单级菜单，未实现级联子菜单。定位依据见 [Microsoft 多显示器指南](https://learn.microsoft.com/en-us/windows/win32/gdi/positioning-objects-on-multiple-display-monitors)，焦点与捕获遵循 [SetCapture](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcapture) 的前台窗口限制。

主窗口统一消费线程消息队列，每次分发后处理自身事件记录；弹出窗口只处理已由该消息泵或同步回调排入的自身记录和回调错误，业务消费者始终在 WndProc 返回后执行。

## UI 样式与诊断

### 参数面板组合

路径栏显示 World 上下文；固定对象标题用类型＋名称输入框表达当前 Actor。名称是 editor::Session 的文档标签，通过对象身份索引，重命名、删除恢复和撤销重做共用场景历史；标签本身不持有运行时对象，GC 完成后清理失效索引。它尚无磁盘持久化，也不是新增的 World 反射字段。

路径栏的下拉通过同一原生菜单选择 World 或 Actor；前后箭头浏览最近的选择。导航历史属于 Workspace，最多保存 64 个不保活的身份，保留 World 入口并清理失效对象；导航调用现有 Session::select，不增加场景撤销条目。路径框、普通输入框、内容页签和滚动条的像素色值与共享样式扩展见[细节校准](../../research/houdini-chrome-pixels-20261002.md)。

Parameters 的 Transform / Components 是内容页签；对象标题与页签固定，仅下方参数区滚动。Controls 的 Parameters / Appearance 使用同一组合，Appearance 内的 Color / Channels 是独立嵌套组：RGB 元组、三条逐通道滑条和色块消费同一组样本值，支持折叠、禁用、默认值及样本撤销。所有行、组和伸展继续使用现有 row/column/Flex，未引入第二套布局器。

折叠标题以常规字重显示，鼠标悬停使用 `#4f4f4f` 灰底，按下使用 `#464646`；展开状态由箭头方向表示。焦点仅轻微提亮文字，不增加边框或黄色下划线，Enter/Space 仍可切换展开状态。

全局交互样式区分按下、选中与焦点：普通按钮和列表用灰色底面反馈，菜单栏展开标题及当前菜单条目用橙色底面反馈；普通控件获得焦点不增加外框。焦点通过文字或控件标记的亮度表达；文本和数值编辑器保留原宽度的细灰色边界，非法输入仍使用红色。鼠标点击保留键盘焦点，Tab、方向键、Enter/Space 和菜单返回继续使用同一焦点状态。

Tab 标签悬停不改变背景，统一使用正体。列表有带握柄和双步进按钮的滚动条，支持拖动、翻页和滚轮；字段不会伸到滚动条下方。数字调整、文本、分隔条、停靠拖动与视口观察使用对应鼠标指针。研究依据和未覆盖能力见[参数组合研究](../../research/houdini-parameter-composition-20261002.md)；完整 Ramp、色轮弹窗、动画/表达式编辑仍未实现。

Workspace 声明 `normal/panel/title/caption` 角色、enabled/selected 等语义状态及菜单栏、参数区域等类名。基础主题和 [editor.ss](../../../assets/editor.ss) 通过同一 UI 解析得到最终样式；应用启动时从可执行文件旁读取 SS，非法内容报告行列。对象类型使用 object-type 样式，说明和状态文字使用 caption；容器结构 padding 留在 NodeInput，文字内容内距由最终样式提供。菜单栏标题宽度按文字测量并保留最小宽度。样式覆盖、图片/圆角和相邻合批合同见 [SS 文档](../ui/style-sheets.md)。
主窗口和原生菜单通过同一 `editor_fonts()` 加载 Sarasa UI SC，数字使用 Source Code Pro；额外缺字及 emoji 沿系统回退，应用不修改系统字体设置。构建部署 `fonts/` 和两份 SIL OFL 许可证；选择依据、字重、体积与来源见[字体说明](../../../assets/fonts/README.md)。窄面板继续同一 Flex 换行。
`Application::set_style` 直接进入 UI 的整体校验/应用路径，应用不保存另一份主题。颜色复用文字 Coverage，字体族、字重和字号经同一 TextStore 更新度量、caret 和栅格；已接受帧仍保有旧几何与不可变内容。具体样式、版本及状态合同见 [UI 模块](../ui/README.md)。
`Application::sample_frames` 显式开启 CPU prepare/render 计时，默认关闭；Capture 帧不计入。窗口验收从同一 Create 菜单添加真实场景成员至 256，再采样列表滚动与位置拖动。主画布 UI 累计计数按段求差，主 renderer 的 `FrameResult::stats` 按实际采样帧合计；弹出窗口的独立上下文未计入这些既有计数。CPU Coverage 和 renderer cache 的持有峰值分别记录。
render 计时包括 Present/等待，R8 上传字节只表示有效 payload，缓存 texel 字节不表示物理显存。验收使用硬件 D3D12 debug/GBV 与 vsync，含 release 配置；数据用于说明该验收负载，不能直接当作关闭验证的产品帧耗时，也没有同负载改前基线可用于百分比提升结论。

2026-10-01 独立菜单增量：dev/release UI 15/15 通过；正式窗口完整链分别接受 2632 / 2631 帧并 PASS。新增断言真实 owned WS_POPUP（非子窗口）、跨编辑器边界点击、显示器工作区约束、F10/Alt 快捷入口、左右键/悬停切换、宿主复用、焦点/捕获清理，以及原参数菜单和下拉业务操作。报告及独立 GPU 截图归档于 `out/native-menu-evidence-20261001/{dev,release}/`。开发版通过后仅调整了发布验收中完整拖动的消息投递时序和接口注释；最终两种配置均重新编译。本轮未运行真实搜狗验收。

## 帧与关停

应用在 renderer 的最终结果成功且确认 accepted 后才切换命中帧，skip 保留旧帧。
accepted extent 固定为录制/呈现时尺寸，frame末尾resize后的新尺寸仅用于下一帧。
尺寸/DPI过渡期取消press/capture并暂停指针命中，释放/失焦/捕获丢失继续处理；匹配帧接受后恢复。
上传也是录制；录制开始后的错误没有回滚，终态错误停止新交互与后续帧。普通帧不调用finish，诊断截图才显式同步。
关闭停止输入并取消编辑，释放renderer拥有的资源后关闭RHI，最后销毁窗口；成员/历史根先于Runtime析构。

## 验收入口

`editor_document_acceptance`消费正式 World/Session，覆盖空 Actor、实际组件增删、单次字段历史/控制会话、完整 TRS undo、成员资格、删除跨 GC 的同身份恢复、历史裁剪普通根保组/最终收集及关闭释放。旧 Scene 库和位置专用入口已删除。
`atelier_editor --acceptance <输出目录>`运行同一正式窗口，用Win32消息和系统焦点/捕获操作驱动控件，再核对Runtime状态与GPU帧。
`atelier_editor --acceptance-project <输出目录> [源工程目录]`在同一个正式窗口内先执行共享视口模式／TRS 验收，再接续工程操作链，输出目录下每次新建独立的 `run-*` 证据目录。提供源工程时，仅复制 Config／Content／Source 到验收目录；跳过链接和 Cache，所有编辑、导入、保存和目录搬移发生在副本。输出目录必须位于源工程之外。省略源工程时创建空工程 fixture；CMake 的 `editor_project_acceptance` 使用这一形式。窗口输入仍要求编辑器保持前台，外部焦点丢失会停止输入。
窗口链包含分隔、增删/历史、列表选择、实时拖调/一次undo/Esc、文字Delete、非法值、相机、失焦/捕获、resize、最小化和正常退出。
S1 覆盖正常/hover/pressed/focus/invalid/disabled、颜色改变的同Coverage/零raster/零上传、非法Style整体拒绝、字体family/weight/size及caret同步失效、旧帧几何/像素寿命。256真实成员由 Create 菜单加入，验证屏外控件几何仍在、文字不生成、滚回恢复，以及连续拖调历史。
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

## Scene Tree

Scene Tree 使用共享 `tree/tree_item`，投影 World → Actor。行标签同时显示实例名称和 Runtime 发布的实际类型；World、Actor、StaticMeshActor 使用不同的原生矢量符号和颜色。组件仅在 Parameters 中显示，不进入 Scene Tree。当前 World 没有跨 Actor 父子合同，因此成员平铺在 World 下，不用组件空间关系伪造 Actor 层级。行绑定实际 ObjectId，视图状态不持有业务根，不增加场景撤销。

参照 Houdini 22.0.429 的 Tree View：20 DIP 等高行、18 DIP 逐层缩进、空白区连续交替底色、细层级连接线、圆形加减按钮、类型图标和底部 Filter。全行选择采用灰色，遵循当前编辑器无黄色焦点框的约定。树节点的展开状态跨停靠、隐藏及重开恢复；每份接受帧只保存一次，绑定退休时同步删除其展开记录，避免旧帧重放和逐条反查绑定。底部过滤保留祖先并临时展开结果，清空后恢复手动折叠。

上下/Home/End 导航并显露选中行，左/右键回父层、折叠和展开。复用当前路径栏、字号、滚动条、输入捕获与接受帧；当前 World 没有 Actor 父子关系，因此没有添加虚构父子、拖拽重挂或多选语义。

共享 UI 端到端包含四档 DPI、过滤、长树滚动、禁用祖先、退休和失效命中；真实窗口验收新增 `tree-component-selection.png`、`tree-filtered.png`、`tree-view.png`，随完整窗口链执行。2026-10-03 开发版与发布版完整窗口链均通过，最终清理候选报告分别为 `out/dev/ui-stage-evidence/acceptance.txt`、`out/release/ui-stage-evidence/acceptance.txt`；共享 UI 16/16 与文档消费者同样通过。普通窗口验收仅在启动时等待一次前台激活，开始输入后失焦仍立即中断。本轮未运行搜狗验收。

## 页签菜单与弹窗资源

页签下拉菜单提供 Set Pane Tab Type 子菜单、前后页签、关闭页签、恢复最近关闭、最大化、关闭整个 pane 和左右/上下分割。已注册视图保持单实例；更换类型交换已有视图的位置，关闭后重开保留该视图状态。加号负责向当前组加入视图。浮动窗口、复制视图尚未实现，菜单不提供无效动作。

菜单描述可嵌套。每层是保留的原生无激活窗口，主窗口保留键盘焦点与 capture；点击或 Right 打开子菜单，Left/Escape 回到父菜单，选择叶命令关闭整条菜单链。改变根来源、失焦、关闭和改变 surface 时清理子菜单路径。每层按实际文字、DPI、样式和屏幕工作区测量，短菜单不保留滚动槽。

应用初始化主菜单及首层子菜单宿主；Renderer 仅在实际网格绘制时创建场景管线和深度资源。隐藏菜单折叠已有控件，保留文字缓存；相同描述重开复用尺寸，不再先缩到 32×32。验收输出 `popup-open.csv` 记录当前首次打开和暖重开在更新/呈现路径上的 CPU 耗时，不包含启动期 Context 初始化，也不包含截图读回；不据此声称已经测量旧版本加速比。
