# 原生UI消费与交互

`atelier_ui`提供正式文字服务和基础控件，使用现有`atelier_ui_layout`；不依赖Runtime、editor或RHI。`atelier_ui_protocol`只导出纯CPU绘制协议，renderer可以单独依赖它，不链接控件或DirectWrite。两个目标均由`cmake/ui.cmake`提供，应用在根构建include该文件后调用`atelier_add_ui()`。

## 悬浮说明

`Control::tooltip` 持有不可变 `Tooltip`（title、identity、description）。同一参数行的 label、数值输入、滑条等子控件继承最近祖先的说明；禁用控件仍可阅读。说明不持有 Runtime 对象、字段借用或回调。标题与身份为单行，正文允许换行；编码和字符沿用 UI 文字校验，不给合法反射描述另设长度上限。

指针停留 500ms 后，Session 使用已接受的几何与说明快照，在同一 `UiFrame` 绘制提示。提示使用现有字体、主题与 TextStore 缓存，按 client 边界换行、避让和裁剪；不参与布局、输入命中或 viewport RT 尺寸。`UiFrame::tooltip` 可供验收观察。

离开窗口、滚动、输入、拖动、菜单、失焦、控件退休或成功的 DSL 热更新都会取消提示。宿主提前消费输入时调用 `dismiss_tooltip()`；这不会取消业务编辑。候选提示绑定悬浮 generation，取消前准备的候选不能再次被接受。祖先身份、父链或说明尚未接受时，也不能利用旧几何显示新说明。

## 唯一入口

[session.hpp](../../../src/ui/session.hpp)的`ui::Session`管理同一窗口UI的布局节点、内容、焦点、按压、编辑缓冲和滚动。应用按稳定key提供父节点在前的平坦`Control`描述；控件布局直接复用`ui_layout::NodeInput`。零key表示隐式overlay根。普通容器可使用现有row/column/Flex/Grid；label/button/item/number/text_edit/checkbox/foldout/menu_item/combo/tab/tree_item 是文字叶，list、tree 和 popup 是带裁剪和滚动的 column，table 是持有共享数据快照的虚拟表格，code_editor 是持有共享源码文档的多行编辑叶，splitter/slider/viewport是盒。viewport不自动填色。

应用拥有业务值、撤销和重做，控件状态不持有业务对象。数字编辑只保存这次操作的起始值、文本缓冲与预览值；每次新场景投影不会覆盖尚未提交的文字。参数及菜单项 key 绑定具体业务目标，切换绑定时更换或退役旧 key。控件删除再出现会获得新 incarnation，旧帧命中不能误派给重用 key。

普通 button 可提供 `action_target`，把宿主动作身份与控件实例 key 分开。激活 Action 保留来源 `key`，另外携带 `target`（零表示沿用 key）；宿主在现有分发入口解析一次，继续检查业务可用性。该绑定与其他属性同批提交；改变 target 会取消旧鼠标/键盘按下状态，接受帧也保存 target，尚未绘制的新目标不能被旧画面触发。多个按钮可以指向同一命令，UI 不持有命令回调或业务对象。

`button_behavior` 显式区分 `command`（默认、空激活负载）与 `toggle`（请求接受帧 selected 的反值）。两者共用按钮外观；toggle 不绘制 Checkbox 指示器。行为改变与动作换绑一样取消在途鼠标/键盘按压，并使旧接受帧失效。选项组继续使用 `selection_group` 的导航合同，布尔开关值、组选中项与暂时按压不混为一个状态。

`MenuItem::behavior` / `Control::menu_behavior` 区分普通命令、checkbox 和 radio。`selected` 是唯一权威勾选/单选值；checkbox 请求其反值，radio 请求 true，同一 `radio_group` 至多选中一个条目。空方框/空心圆与选中标记始终可观察，hover/focus 只高亮当前菜单行。普通命令不预留勾选/单选标记列；checkbox 与 radio 才预留对应空间。图标拥有独立槽，不替代勾选标记。菜单行为、组或目标变化会取消旧按压；关闭菜单清理打开和悬浮状态，不更改业务值。只含图标的 Combo 在右下保留下拉角标。

调用顺序：

1. `set_surface({backbuffer_extent,dpi_scale,revision})`，应用处理返回的取消/释放捕获动作。
2. 初装/结构改变使用`set_controls(controls)`；同身份属性改变使用`update_controls(changed_controls)`。两入口共用校验与交互清理，应用处理返回的取消动作。完整合同见[持久实例与稀疏更新](updates.md)。
3. `prepare()`得到拥有式`shared_ptr<const UiFrame>`；失败仍可保有上一完整帧。
4. renderer同步消费`*frame->drawing`，应用持有整个帧至返回。
5. 仅renderer最终accepted后调用`accept(frame)`；skipped不调用。失败进入渲染终态时停止交互，不能声称画面回滚。

`UiFrame::find(key)`返回同次帧的物理像素bounds/clip，供场景viewport和真实窗口验收使用。候选prepare不会改变命中几何。输入始终使用已接受帧，依绘制逆序命中并检查当前控件身份与祖先启用状态。

`UiFrame.controls_revision`标识候选使用的描述批次；描述或SS版本改变后，`accept`拒绝旧候选。颜色改变保留控件incarnation。`prepare`失败仍保有旧帧，但暂停交互激活，直到修复后的帧被接受；已提交描述及已返回的取消effects不会因出帧失败自动撤回。

## 接入导航

- 尺寸、Flex/Grid、测量和几何寿命：[布局核心](../ui-layout/README.md)。布局核心只返回几何，DirectWrite 排版与控件输入由本模块负责。
- 描述提交、局部属性更新和失败恢复：[持久实例与稀疏更新](updates.md)。
- 主题、作用域、继承及最终样式：[SS 合同](style-sheets.md)。
- 组件组合、稳定 key、源码保存更新：[声明式 UI](../editor/declarative-ui.md)。生成的 Control 仍进入本 Session。
- 外部工具的业务指令与 busy 策略：[Editor Control](../editor/control.md)。

## 焦点与待结束交互

`Session::interaction_state()` 返回只读 `InteractionState`，分别记录 `pointer`、`keyboard`、`text_draft`、`composition`、`clipboard` 和 `popup`；`active()` 是这些状态的并集。宿主用它判断是否需要等待交互结束，业务命令仍由宿主自己的入口校验。

仅有 focus、被动文字绑定或可输入状态不表示活动草稿。字段提交/取消后可以保留焦点，同时不再有待提交事务；无效数字草稿仍属于 `text_draft`。`composition` 包含在途 IME 请求，`popup` 包含尚在已接受帧中显示的菜单。`wants_keyboard()` 继续表达键盘归属，`text_input()` 继续提供平台 token/位置，两者不替代待结束交互查询。

实际焦点转移统一取消旧控件的键盘按压，包括 Tab、显式 `focus()` 和过滤隐藏树行。旧键的重复与释放只结束旧手势，不激活新焦点；同一焦点的正常按下/释放仍激活一次。匹配的 key-up 在帧准入和控件分发前收尾，因此布局准备失败、文字字段接管或焦点清空都不会留下 `keyboard` busy 状态。

`CodeDocument` 的已确认文字与历史属于源码文档，不计入字段 `text_draft`；源码保存/替换是否允许由编辑器的文档规则决定。源码编辑中的按压、组合与剪贴板请求仍通过上述状态保护。

## 多行源码编辑

`ControlKind::code_editor` 使用 `Control::code` 指向宿主持有的 `CodeDocument`。文档跨控件挂载保持文本和增量历史；Session 共用原有焦点、捕获、Clipboard/IME token 与平台请求。`UiFrame::find_code` 返回可见行、文档版本和接受帧几何。单行字段继续使用其原有事务，源码已确认输入不会因失焦或隐藏回滚。作者与文件层契约见[源码工作流](../editor/source-editor.md)。

## 可组合参数控件

DSL 的 `PanePath`、`ObjectHeader` 和内容页签 用普通 row/column、label、text_edit、button 组合路径、类型＋名称与内容页签。对象字段仍由宿主绑定实际身份和历史。内容页签使用 `selection_group`：左右键/Home/End 切换，Tab 只访问组内选中项；宿主保存选择并折叠对应内容容器。它与可拖动的工作区 `tab` 职责分开，多层页签和折叠组可直接嵌套在同一布局树中。

布局缩进由父容器负责，编辑器所有参数类页面（Parameters、World、Material、Resource、Controls）只遵循一条规则：

- 页面级列表（`parameter-list`）从父区域顶格开始，没有页面级内边距；列表、页签行或标题行之间不插入额外空隙。
- 折叠标题（`foldout.section`，24 DIP）铺满父区域全宽，组之间以 1 DIP 间隔区分。
- 只有折叠标题下的行容器（`section-body`）缩进：左 8、上 5、下 7 DIP，行距 5 DIP。
- 页签行（`section-tabs`）固定 26.667 DIP，含底部 0.667 DIP 分隔线；页签本身没有左、右或顶部缩进。
- 页头行（`object-header`：参数头、Entity Structure 工具栏、Material/Resource 头、Controls 头）固定 32 DIP，左 6、右 4 DIP，24 DIP 控件垂直居中。
- 路径栏（`PanePath`、视口头、Asset Browser 工具栏）同为 32 DIP、左右 4 DIP，导航按钮 22×24、路径框与工具按钮 24 DIP 高，使相邻面板的路径与图标在同一水平线上。
- 底部过滤行与状态行左缩进 6 DIP（与页头一致），状态文字行高 20 DIP。
- 占位文字（`empty-state`）与页头使用相同的 6 DIP 起始缩进。

新增页面复用这些类，不在 DSL 节点上另写 padding_top/padding_left 来调整同类区域，避免同一角色出现不同边距。

`PathBar` 提供宿主拥有的路径 combo、后退/前进按钮身份及可用状态；路径图标与文字使用同一测量入口。`Control.icon` 可与文字并存，前置图标保留 indicator-size 加 6 DIP 的空间；没有文字时默认居中，可通过 `icon-align` 左/中/右对齐。下拉的方向标记限制在右侧预留区域内，较大的前置图标不会放大右侧箭头。路径菜单与普通下拉继续调用原菜单入口。

[controls.hpp](../../../src/ui/controls.hpp) 的 `append_parameter_row` 建立统一标题列和字段容器；文本、下拉与数值行共用此组合。`append_number_row` 在该容器中加入 1–4 个普通 number，传入各自的 key、值、默认值、范围和步长。标题宽 160 DIP，字段最小高 23 DIP；字号增大时数值行可随内容增高；分量紧邻排列，名称保留在描述中，不占用单独标签列。窄宽度通过现有 Flex wrap 将标题和字段分行，分量仍保持一行；重新排布不更换字段身份。主体最低宽度为每字段 24 DIP 加 2 DIP 间距，四字段为 102 DIP。组合层不创建焦点、草稿、业务对象或历史。

`Control.modified` 表示权威值与声明默认值不同，基础主题使用 `Palette.modified`，SS 可通过 `:modified` 覆盖颜色和字重。修改状态可通过字重影响文字测量，不代替非法输入状态。标题使用稳定的 `group_key` 作为身份，右键可请求整组菜单；Ctrl+中键发出 `reset(group_key)`，整组写入与一次撤销由宿主负责。宿主只展示字段子集时，通过 `ParameterRow.group_modified` 提供完整组状态；未提供时数值组合从传入字段推导。各字段始终独立比较自己的默认值。

名称、搜索与数值字段统一使用 `single_line_layout`，默认最低 23 DIP；采用相同字号和内容内距，字号增大时可随测量增长。标签可通过 `text-vertical-align: center` 对齐编辑框的内容中心，说明段落仍保持顶部对齐。

数值中键按住显示七档 value ladder：100、10、1、.1、.01、.001、.0001。指针位于阶梯横向范围内时，上下移动选择实际绘制的档位；离开该范围后保持选档。水平累计每 4 DIP 改变一个所选增量，保留不足一步的位移；换档或切换修饰键清除余量而不改值。Shift 将增量乘 .1，Alt 将每步改为乘以 `1+增量`（左移取倒数）；选中格第二行显示灵敏度或本次乘法累计倍率。`drag_step/fine_step` 继续服务普通左键拖调。释放提交一次，Esc/失焦/捕获丢失取消，Ctrl+中键恢复默认值。

阶梯几何在按下时冻结，绘制和命中共用 `ValueLadderGeometry`，并随接受帧发布；尺寸受当前 Surface 限制。它通过普通 popup 的 `value-ladder` 类解析同一 SS 作用域。七个档位的文字 Coverage 在按下时生成并在手势内复用，静止帧不重新排版或栅格化；手势结束释放。倍率使用文字服务的保留槽零。

`NumberField.axis` 显式选择 X/Y/Z/W 提示线，过滤后的字段保留原轴身份；`none` 不加提示。SS 的 `accent-color` / `accent-width` 绘制底边细线，项目将它用于轴识别，不表示动画或表达式状态。

单标量 `NumberField.slider` 可提供另一个稳定 key 和显示范围；`append_number_row` 仍是唯一组合入口，生成 72 DIP 数值框与可伸缩滑条。显示范围必须位于数值合法范围内；手工输入可超出显示范围，滑块停在相应端点，不改权威值。宿主将两个 key 映射到同一值和历史。

`slider` 复用 Session 捕获与 `edit_begin/preview/commit/cancel`：轨道点击预览，拖动释放提交一次，Esc/失焦/捕获丢失取消并恢复原值，包括显示范围之外的原值。拖动映射冻结已接受轨道宽度，Shift 降低十倍速度；方向键使用 drag_step，Shift 使用 fine_step，Home/End 到显示端点。禁用、隐藏和 Surface 变化沿同一失效清理。滑条不创建文字缓冲或另一个平台输入入口。

`checkbox` 和 `foldout` 是共享焦点、按压、禁用及 Tab 导航的文字叶。`selected` 分别表示勾选、展开；鼠标释放或 Enter/Space 抬起产生 `activate`，其 bool 值为已接受画面中 selected 的反值。宿主接受请求后重新描述控件，UI 不私自修改权威值。文字测量和绘制共同预留 indicator-size 加 6 DIP 的指示区域。勾号和三角形使用按形状/像素尺寸缓存的抗锯齿 R8 Coverage，沿同一 DrawQuad、上传和资源记账路径绘制。

DSL 的 EditorShell 和 ToolShelf 用普通容器、按钮和 `collapsed` 组成可收起栏：共享 12 DIP 轨道宽度、顶部箭头区域和恢复条高度，声明本次占用高度供宿主分配剩余空间。恢复条由同一个按钮横向展开，整行绘制和命中使用同一接受几何，图标通过 `icon-align: left` 留在左端。宿主提供稳定的栏/按钮/内容 key、展开高度及内容布局，仍通过原激活入口管理展开状态；没有独立控件生命周期或第二套输入逻辑。编辑器工具架已迁到 DSL，消费同一组尺寸、共享样式和普通按钮能力。

折叠标题与内容容器是两个控件；宿主将展开状态映射到内容容器的 `layout.collapsed`。现有祖先有效性检查统一取消隐藏/禁用字段的草稿、组合和捕获；展开后复用仍存续的 key。宿主替换目标时仍须退役旧绑定。

正式编辑器的 Controls 实验台与位置参数行均使用上述接口，真实窗口验收覆盖两者，见[编辑器模块](../editor/README.md#控件实验台)。

组合依据：[SideFX 参数编辑器](https://www.sidefx.com/docs/houdini/network/parms.html)区分类型、名称、默认值与编辑值；[Operator Type Properties](https://www.sidefx.com/docs/houdini/ref/windows/optype.html)与 [FolderParmTemplate](https://www.sidefx.com/docs/houdini/hom/hou/FolderParmTemplate.html)说明嵌套页签和分组。[数值阶梯](https://www.sidefx.com/docs/houdini/basics/ladder.html)支持垂直选档、水平改值及 Alt 乘法；本项目的具体步距与 Shift 政策以上文为准，不将自己的验收当作 Houdini 实机证明。[QBoxLayout](https://doc.qt.io/qt-6/qboxlayout.html)是方向、伸展、margin/spacing 的组合参考，不是截图能证明的 Houdini 内部实现。

### 树视图

`tree` 是共享滚动容器，`tree_item` 是它的直接布局子项。Tree 需要宿主提供展平后的行：宿主持有层级、展开状态和过滤，按深度优先先序把可见节点展平成 `tree_row_count` 行，只描述其中一部分。每个 `tree_item` 给出 `tree_index`（行号）、`tree_depth`（< 128）、`tree_branch`、`tree_expanded` 和 `tree_lines`（第 k 位表示第 k 层祖先或本行之后还有同级行，绘制该层连接线）。Session 校验描述行号递增、不超出行数、相邻行至多深一层且只在展开的分支下，然后只排布描述的行：每行按 `tree_index × 行高` 定位，树的滚动范围覆盖全部 `tree_row_count` 行。逻辑层级只表达展示关系，不创建业务对象或修改 World。

`ui/controls.hpp` 的 `flatten_tree` 把宿主节点展平成行并计算连接线，`tree_window` 按 `Session::tree_viewport`（当前滚动、上次排布的视口高度和行高）选出要描述的行：视口及上下各一屏、首末行，以及钉住的行（主选、焦点、内联重命名）及其相邻行和祖先。视口高度取上次排布的值；停靠变化或缩放超过一屏余量的那一帧可能缺行，下一帧按新高度补齐，输入只命中已接受帧中存在的行。

展开按钮、左右方向键只发出 `expand` 动作（bool 为请求的展开值），由宿主改变状态后重新描述；折叠焦点所在的子树时焦点移到折叠行。点击行或 Enter/Space 发出 `select`，`SelectionModifiers` 携带 Shift/Ctrl，业务选择集合由宿主维护。上下/Home/End 在已描述的行之间移动，越出视口会滚动显露；左方向键折叠或返回父行，右方向键展开或进入首个子项。Tab 将整个树作为一个停靠点。宿主把某行 `tree_primary` 从 false 改为 true（或换成新的主行）时，Session 滚动显露该行一次；稳定的主行不会反复拉回滚动。行离开描述窗口后再出现使用新 incarnation，禁用或移除行取消旧捕获。`UiFrame::tree_rows` 发布行号、深度、展开值和展开按钮命中区，鼠标与绘制共用这份已接受几何。

`flatten_tree` 的过滤是名称子串匹配（ASCII 大小写折叠，其他字符原样匹配），匹配节点保留祖先，匹配父节点保留其子树，过滤结果全部展开且不改变宿主保存的展开状态；空结果不产生虚假滚动条。固定行高、层级间距、交替底色和连接线通过 SS 表达；交替行覆盖空白区，复用现有文字与图标 Coverage 缓存、裁剪、滚动条及绘制协议。

### 表格

`ControlKind::table` 使用 `Control.table` 引用不可变 `TableData`：列提供稳定 key、标题、DIP 列宽/最小宽度、对齐和是否可排序；行提供稳定 key、启用状态及按列排列的单元格，每格有独立 key、文字和可选图标。宿主负责数据顺序、业务值、排序状态和已提交列宽。`Control.table_selection` 单独保存当前行与列，更新选择复用同一数据快照，不复制全表。

`Control.table_layout` 独立保存 details/list/tiles 呈现及行高、表头高度、卡片宽高和间距。List/Tiles 投影每行首列，Details 显示各列；三者共用一个控件、数据校验、选择、动作和可见文字缓存。卡片列数由当前接受帧的可用宽度决定，窄视图至少一列，首尾部分可见行参与裁剪，末行无额外滚动间距。TableGeometry 记录实际列数和行距供键盘导航使用；呈现改变取消旧按压、列宽拖动与键盘待激活状态，旧帧在接受新几何前不能继续输入。空模型使用 `Control.text` 绘制提示，不伪造资源行。

Session 在同一输入入口管理滚动、焦点和在途列宽预览。固定表头、单元格、分隔线和命中区域共用列几何；`UiFrame::find_table(key)` 发布本次接受帧的数据快照、选择、表头及可见单元格。虚拟行/格 key 是数据身份，不是假造的 ControlKey；整表只有一个布局节点，文字复用现有 TextStore，可见范围以外的缓存及时释放。

单击单元格产生 `select` + `TableSelection{row,column}`，选中行和当前格分别可见。整表是一个 Tab 停靠点；上下、Home/End、PageUp/PageDown 选行，左右选列，越界时显露当前格。双击或 Enter 产生 `activate`，可通过原 `action_target` 路由；平台用 `Event.clicks` 保留原生双击信息。单击与激活不混用，资源创建仍由宿主明确命令执行。

表头释放产生 `table_sort` + `TableSort`，宿主按实际类型排序，再提交新数据快照。列边界拖动使用捕获和 resize 指针，预览只在 Session 中，释放一次产生 `table_column_resize` + `TableColumnResize`；Esc、失焦、捕获丢失、隐藏或目标失效取消预览，不要求宿主回滚列宽。窄表保留列宽，横向滚动使用同一 ScrollbarGeometry，Shift+滚轮可横移；纵向滚动不移动表头。

行、列和手势均核对当前身份及接受帧数据。新数据的校验和行索引建立仅发生在数据快照变化时，选格不重新扫描整表。右键先发选择，再用真实 Table Control 作为 `ContextMenuRequest.source`，`table` 载荷记录原行/列；宿主菜单在原行消失、禁用或选择改变后关闭，不对后来的选择执行旧菜单动作。表格没有私有业务对象、命令入口或第二套文字编辑会话。

Asset Browser 的 List、Details、Tiles 共用 DSL `Table` / `TableBinding`、数据快照和原生虚拟绘制；[声明式表格合同](../editor/declarative-ui.md#表格与宿主)说明作者语法和宿主路由。当前不提供单元格编辑、范围多选或冻结列，这些能力需由实际业务和既有编辑事务继续驱动。

### 滚动与指针

list/tree/popup 默认使用 `scrollbar-gutter: stable`，布局时保留 `scrollbar-width + 2 * scrollbar-inset` 的右侧空间，子内容裁剪到该区域之前，避免滚动条盖住字段；保留空间不依赖当前是否溢出，防止出现滚动条时反复换行。`overlay` 不预留或裁剪滚动槽，溢出时滚动条覆盖内容右侧。正式菜单首次测量使用 overlay，宿主确认溢出后使用 stable，为实际滚动条留出空间。溢出时发布 `UiFrame::scrollbars`，轨道、滑块和底部双步进按钮的绘制与命中消费同一份已接受几何。滑块按可见高度/总高度计算比例，拖动保留抓取点，轨道按页、按钮按 24 DIP、滚轮按 48 DIP 滚动。范围随内容变化夹取，失焦、Esc、捕获丢失、隐藏及 Surface 变化释放滚动捕获，保留当前滚动位置，不生成业务编辑或撤销。

`Reply.cursor` 未提供时保留当前指针。指针意图由已接受命中或活动手势决定，数值调整、文本、分隔、停靠移动及视口观察各用相应形态。Window 缓存原生指针并处理客户区 `WM_SETCURSOR`；数值调整使用项目自己的双向箭头，文字和分隔条复用 Windows 指针。视口平移、环绕、移动、旋转和缩放使用独立语义指针，浅色笔画带暗描边，按意图与 DPI 缓存，销毁窗口时回收。捕获期间离开原控件仍保持手势指针。

视口工具图标与语义指针共用精选 [Tabler Outline](https://github.com/tabler/tabler-icons) 图形（MIT）。原始 SVG、透明 PNG、版本与许可保存在 [assets/icons](../../../assets/icons/README.md)。构建使用离线生成的压缩 alpha，继续经过 `ControlIcon → TextStore::Symbol → Coverage` 的既有缓存与主题着色；不引入运行时 SVG/PNG 解码器或另一套图标资源管理。

范围与轨道像素分开的参照见 [QScrollBar](https://doc.qt.io/qt-6/qscrollbar.html)。原生指针的处理依据为 [WM_SETCURSOR](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-setcursor)、[About Cursors](https://learn.microsoft.com/en-us/windows/win32/menurc/about-cursors)和 [SetCursor](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursor)；静态截图不能证明捕获期间的指针行为。

### 菜单与下拉

DSL 的 PopupMenu / PopupEntry 组合根级 `popup`、普通 `menu_item`、快捷键提示和分隔线。每个 Session 只允许一个 popup；同一 surface 内的非零 owner 必须先出现，浮层子树放在描述末尾。owner=0 表示独立菜单 surface，外部目标的身份与生命周期由宿主管理。Session 只负责本 surface 的布局、滚动、绘制和接受帧，不假定它是主窗口。

`context_target` 开启右键和 Menu/Shift+F10 请求；`combo` 在主按钮按下时请求菜单，对应释放交给菜单宿主，不再等待按钮释放才展开。Enter/Space 或上下键也走同一菜单请求。`context_menu` 携带 source、可选 group 和已接受帧的客户区物理坐标。Session 管理上下/Home/End、Enter/Space、Esc/Tab；外部点击只关闭，滚轮及键盘不穿透。`dismiss` 要求宿主撤下菜单，隐藏、禁用、失焦、Surface 失效与捕获丢失沿同一取消路径处理。

正式编辑器的列表菜单使用 [PopupHost](../../../src/application/popup.hpp)：一个复用的 owned `WS_POPUP` 窗口，具有独立的 Session、renderer 和接受帧，使用同一份 SS。主窗口保持键盘焦点与鼠标捕获；输入经屏幕坐标转换到菜单的本地接受帧。主窗口画布没有列表菜单副本，菜单可越过其边缘；菜单尺寸、翻转和夹取基于锚点显示器的工作区。视口饼菜单在主 surface 中通过 DSL 组合，复用同一菜单项模型与命令入口。

`MenuBehavior::checkbox` 的前置方框与普通 checkbox 共用框体和独立抗锯齿勾号绘制，使用 `indicator-size/background/border-color/color`。鼠标按下、松开均位于接受帧的方框列时，Session 发出 `activate` 并设置 `Action.keep_menu_open`，不发 dismiss；标签点击与键盘激活仍关闭菜单。方框列包含左内距、方框和其后 3 DIP 间隔，并受控件裁剪约束。宿主只对仍有效的 checkbox 尊重该标志，接受动作后重新发布权威 selected 值；错误、失效与取消沿原关闭路径。PopupHost 更新后复用原窗口，并在新 GPU 帧接受后恢复原条目的焦点。

同 surface 的 `menu_item.edit_command` 仍由 Session 直接处理；独立菜单宿主关闭菜单后调用原 Session 的 `execute(command, incarnation)`。该入口检查原控件的已接受身份和当前有效性，之后进入原 TextEdit、原生剪贴板与业务事务。有效数字粘贴立即提交一次，非法数字保留可取消草稿。控件被替换后不能把旧菜单命令派给新绑定。`focus(0)` 清除焦点，非零目标须为当前可见、已接受控件；切换焦点结束前一草稿，同目标保留草稿、选区和组合，文本字段立即绑定输入，数值字段保留被动焦点。取消交互同时清除键盘按下与终止键状态。

`Session(font_files)` 可在构造时将应用字体与系统字体合成一个私有 DirectWrite 集合，不安装系统字体。字体文件在 Session 生命周期内保持可读；已接受段落持有其原生布局，继续遵守旧帧寿命。默认构造仍只使用系统字体；未知字体名与中文缺字保持 DirectWrite 的系统回退。当前编辑器随程序部署 Sarasa UI SC 与 Source Code Pro，来源和许可证在 [字体资源](../../../assets/fonts/README.md)。

## 停靠与标签

[docking.hpp](../../../src/ui/docking.hpp) 的 `DockSpace` 拥有单宿主的分割树、标签顺序、当前标签、最大化目标和拖放预览。宿主注册稳定的 `DockTab.key/content`、标题与最低尺寸，通过 `presentation` 提供分割尺寸、页签与内容槽数据，由编辑器 DSL 描述全部界面结构。它不拥有业务对象、相机、草稿或历史；同类型多实例由宿主注册不同身份才能存在，不能通过复制 key 创建副本。

- 分割是横/纵方向、比例和两个孩子；叶子是标签组。移动后清理空组与单子分割；最后一组可为空，并仍能从菜单打开内容。最大化不改动原树。
- `tab` 复用 Session 的焦点、键盘激活、命中和捕获。超过 4 DIP 拖动阈值后产生 `drag_begin/preview/commit/cancel`，`DragPoint` 是已接受客户区的物理坐标。`DockSpace::apply` 使用同一接受帧决定目标，拖放预览不修改布局，松开才提交；Esc、捕获/焦点/Surface 丢失取消。
- 标签栏落点改变顺序或合并，内容四边落点分割；空间不足时不接受边缘分割，菜单也禁用对应项。分隔线复用普通 splitter 的数值预览/取消链路。每叶最低尺寸取已注册视图的最大需求，并沿树合成；窗口不足时按最低需求比例压缩，由内容自己的滚动/换行保持可达。
- `focused_control()` 供宿主依据已接受父链选择业务历史上下文；它不把键盘焦点等同于业务对象选择。`visible(tab)` 与 `focused_tab()` 分别表示实际显示和最近操作的视图。
- 每个分割节点保留展开比例及独立的收起侧。分隔条两端箭头收起对应侧，留下 8 DIP 细条，再点恢复。隐藏子树停止描述，业务视图身份、值和历史仍归宿主所有；菜单 Show 会展开目标的全部隐藏祖先。Tab 栏没有前置收拢按钮；菜单栏与工具架在 DSL 中共享收起尺度和按钮语义。
- Tab 栏的“＋”是带图标的 combo，紧跟最后一个标签；复用 context_menu 和宿主原生弹窗。选择已有视图将它移到当前组，关闭视图重新打开；右侧 Pane 菜单的 Show 保持跳转到原组的语义。`DockTab.classes` 进入原标签控件的 SS 匹配。
- 中央点阵握柄单击交换两侧，Alt+单击旋转分割方向；比例保持不变。握柄和其余分隔条都可拖动，普通 splitter 的 `click_action` 将未拖动释放转为 activate（bool 为 Alt）；超过拖动阈值只走一次编辑手势。收起期间拖动不覆盖展开比例。
- `Control.icon` 支持关闭、最大化、恢复、加号及四向箭头，可用于纯图标 button、splitter、combo。符号尺寸取 `indicator-size`，在 padding 后的内容区居中并限制尺寸；颜色取前景，最大化对角底色取 `indicator-background`，使用缓存 R8 Coverage。点阵握柄用可相邻合批的实色小方块绘制。

所有结构仍描述为普通容器、tab、button、splitter 和内容控件，经同一布局、SS、接受帧与 renderer 绘制。调用者保留当前 `DockSpace` 实例；原生浮动窗口与布局文件持久化不在该首片中。

## Style与失效

[style.hpp](../../../src/ui/style.hpp)的`Style`提供基础主题，由`Session::set_style`完整校验、复制后应用；非法值返回`invalid_style`且不修改任何配置。根/局部`StyleSheet`、类名及逐属性覆盖扩展同一解析入口，最终`ResolvedStyle`供测量、绘制与接受帧持有。语法、优先级和资源合同见 [SS 样式表](style-sheets.md)。更换配置后仍通过`prepare → renderer accepted → accept`发布。候选失败保留已接受帧。`UiFrame`记录本帧的`style_revision`和`measure_provider_revision`，旧帧颜色、几何与文字像素不会跟随当前Style改变。

控件选择`Role::normal/panel/title/caption`与类名；局部属性也通过定型`Control.style`进入同一规则系统。唯一状态解析处理normal、hover、pressed、focus、selected、disabled、invalid、modified。字体配置包括family、locale、weight和DIP字号；基础主题的title/caption通过比例表达层级，title另有字重。未知系统字体采用DirectWrite的字体匹配行为。

label 默认从内容顶部绘制，普通 button/item/number/text_edit/checkbox/foldout/menu_item/combo 默认在内容框内垂直居中。`text-vertical-align` 可显式选择 top、center 或 bottom；文字、选区、caret 与输入法锚点共用最后的文字原点。控件显式或经 Flex 父项参与首基线对齐时，其 UI 文字上下文沿子树继承，文字统一从内容顶部绘制，包括容器内贡献首基线的后代，避免额外偏移破坏布局基线。这只决定文字内部放置，不改变 NodeInput 对齐和脱流规则。退出基线上下文后恢复最终样式指定的对齐。

label 默认按可用宽度换行；`Control.single_line=true` 使用单行排版，超过分配的内容宽度时绘制尾部省略号。此属性通过同一文字版本、测量和接受帧快照生效，不修改权威 `Control.text`。可编辑字段仍使用完整原文、横向滚动和原有选择/caret 路径。

`Style.control_padding` 为 button/item/number/text_edit/checkbox/foldout/menu_item/combo 增加内容边距，与显式`NodeInput.padding`相加；label/container/viewport没有隐式内容边距。消费者不能重复添加相同控件内边距。测量、栅格化、选区和caret使用同一个有效内容框。border/caret/scrollbar尺寸也以DIP配置，绘制时统一转换物理像素。

- family、locale、weight、字号或角色字体比例改变：推进文字provider revision，清理对应格式/段落缓存，再通过正常布局入口测量。
- 内容或角色改变：由内容revision识别实际解析后的文字与字体；旧帧保持自己的Coverage。
- padding改变：作为同一NodeInput几何属性失效，更新文字约束；不伪造字体provider变化。
- 颜色、边框和caret外观改变：复用原有R8覆盖率，不触发字体重新测量或栅格化。

字体名称和locale要求非空、最多128个UTF-16码元且无内嵌NUL；解析后的所有角色字号为4–256 DIP，字重1–999；四边控件内边距0–128 DIP。有效Style仍可能与显式固定盒尺寸冲突，此时prepare返回布局失败，调用方可恢复Style或调整几何后重试。

## 应用与OS交接

`Event`区分物理像素指针、wheel、key命令、拥有的UTF-16文字、剪贴板结果、IME命令结果和组合快照。`Event.data`只承载本次事件的数据；不把keydown当成字符，也不把孤立的WM_CHAR代理项直接传给UI。`Reply.effects`是唯一有序`variant<Action,PlatformRequest>`列表；应用执行effects并处理capture/cursor/redraw。`Reply.error`报告可恢复的输入/平台错误。

Surface的revision在客户区坐标映射或DPI变化时递增。尺寸/DPI变化取消手势、草稿和组合、释放捕获，并暂停命中直到匹配的新帧accepted；零extent暂停。控件移除、禁用、collapsed或改变kind取消旧编辑；旧帧不能把旧控件类型解释成新类型。失焦/捕获丢失在暂停期间也可清理状态。正常ReleaseCapture产生的通知不取消刚进入的文字编辑。

## 共享单行编辑

`text_edit`以`Control.text`作为权威投影，number保留value/min/max及拖调策略；两者共用[TextEdit](../../../src/ui/text_edit.hpp)的唯一草稿、original、caret/anchor、选择和有界历史。number只增加完整有限数值解析、范围和格式化：有限数值超出 min/max 时夹到端点，与 Runtime 对有界字段的写入一致。输入文字本身不使用ASCII白名单：非数字或非有限的文字保留为invalid草稿，不写业务preview。

focus、文字模式和编辑事务分开。number普通拖动仍在超过原指针阈值后调值，Shift细调；单击进入文字模式后，拖动用于选择文字。仅聚焦时Enter或普通文字输入可进入文字模式。text_edit聚焦即可输入。移动caret、选择文字和进入模式均不建立事务。

- 首次实际文字变化产生`edit_begin(original)`，随后有效草稿产生`edit_preview`；普通文字每次修改均可预览，IME预编辑不发预览。
- 无事务时Ctrl+Z/Y（含Ctrl+Shift+Z）返回未消费，由应用历史处理。存在本地事务时只操作本地历史，即使undo回原文或当前方向没有历史也不穿透。
- 合法Enter提交并结束本地事务；非法Enter保留草稿、焦点和文字模式。Esc取消整次字段编辑。结束后保留focus、清历史和待处理平台请求并换generation；number退出文字模式。按住Enter/Esc的repeat及其WM_CHAR不能重复提交或切模式。
- 普通换焦点合法提交、非法取消；OS失焦、Surface失效、控件失效取消。数值拖调提交后回仅聚焦，下一次Ctrl+Z交业务。仅聚焦时新的权威投影更新被动缓冲，不能被旧文字覆盖。
- 字段聚焦时Delete/Backspace由共享文字编辑消费，不能删除场景对象。Tab按可见呈现控件顺序导航，Ctrl+A全选，Left/Right按视觉方向移动或折叠选择，Shift扩选，Home/End到视觉行端。

`Action.value` 使用公开头中的 `ActionValue`：普通命令激活无值；checkbox、foldout、toggle 按钮及 checkbox/radio 菜单项携带请求的 bool；number/splitter 编辑为 double，text_edit 为 wstring。上下文菜单、拖动、表格选择/排序/列宽及选择修饰键各有定型载荷，宿主按 ActionKind 和实际载荷解释。begin/preview/commit/cancel 继续沿应用的同一事务消费路径；cancel 携带原值。业务派发失败停止同组剩余普通 effects，仍须调用统一取消和 IME/capture 清理，保留首错。剪贴板失败则保留草稿、选择和历史供重试。

本地历史保存插入/删除片段及前后选区，避免每次编辑复制整份文字；连续同类插入、连续Backspace或连续Delete可合组，导航/选择/pointer/快捷键/平台操作断组，无计时阈值。cut、paste和每次IME确认各占一组。undo+redo总计最多64组、历史UTF-16文字最多64KiB；淘汰最老完整组，original独立保留保证Esc可恢复。text_edit为4096码元、number为64码元；超长插入、孤立代理项、控制字符或换行整笔拒绝。

索引为UTF-16码元，编辑端点来自系统ICU的extended grapheme边界。DirectWrite同一layout负责点命中、带视觉affinity的caret和`HitTestTextRange`多矩形选择，不能用两个x表示双向选区。Windows SDK的`icu.h/icu.lib`仅为UI私有依赖，不新建文字渲染器或系统控件镜像。历史验收环境报告ICU 72.1、Unicode 15.1，曾覆盖非BMP代理对、组合重音、variation selector、明确ZWJ序列和双向混排；当前消费者不再保留这些矩阵，也不据历史结果宣称所有版本与字体均已验收。一手依据：[Windows ICU](https://learn.microsoft.com/en-us/windows/win32/intl/international-components-for-unicode--icu-)、[ICU边界分析](https://unicode-org.github.io/icu/userguide/boundaryanalysis/)。

## 剪贴板、组合与平台位置

平台由Window/Application持有，UI不持HWND/HIMC。剪贴板请求含kind、唯一operation_id及`EditToken{key,incarnation,generation,revision}`；只结算匹配的当前请求一次。文字或普通caret/selection变化推进revision；结束/重绑上下文换generation。cut仅在匹配成功结果后删除请求时选区；paste成功沿统一插入。失败、重复、过期结果不改草稿，同revision不同operation也不能误配。

IME使用独立composition_id和固定起始token/替换范围；`CompositionEvent`的start/update/end/cancel以及可选result/preedit区分缺失、空和取消。Preedit拥有text/cursor/逐码元属性，只作暂态文字和选区/下划线显示；确认result进入普通编辑与历史。同事件可先确认一个片段，再开始新preedit；后段绑定更新后的token。UI请求complete/cancel的成功回报只代表平台调用成功，不能凭此合成文字。IME期间Enter/Esc先确认/取消组合，不同时提交/取消字段，终止键重复及keyup保持归属；普通焦点或选择变化取消未确认组合。结束/取消使旧绑定失效，迟到事件不能复活原组合。

`UiFrame.caret`保存该帧的token、客户区物理Rect和clip。`Session::text_input()`给出当前输入token/是否组合，只有当前身份、焦点、token、Surface及组合编号/显示版本匹配时才附带已接受帧的caret；候选prepare不改变平台位置。应用据此更新系统候选位置，窗口消息逐条复制、顺序投递和执行effects，不重入UI，也不把整个poll盖上同一旧revision。

`UiFrame.input_anchor`在同一次prepare中记录`TextInputAnchor{token,composition_id,composition_revision,bounds,clip}`。无组合时起点为选区的逻辑最小端点，空选区保留当前插入affinity；组合中为`c.first`，不取显示光标的`c.first + preedit.cursor`。同一DirectWrite布局提供字符左上与行高，RTL使用字符矩形左上，不将其右侧leading caret当左上。bounds和有效text_clip随绘制采用相同父变换、横滚和DPI，均为客户区物理像素。

`Session::text_input().accepted_anchor`只提供当前匹配的已接受快照，起点不可见、目标或Surface失效、组合更换、临时preedit/cursor已变化但尚未接受新帧时为空。临时组合显示版本独立于基础EditToken revision；旧UiFrame仍可被持有，但不能作为当前位置。平台在原生消息改变组合或坐标映射时先撤下缓存，再依UI顺序处理与accepted发布更新；WndProc只读取拥有的这个小值，不回调UI，也不创建第二文字模型。

本轮只支持[IMR_QUERYCHARPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/imr-querycharposition)实测需要的`dwCharPos=0`；Window把bounds左上和clip转换成屏幕坐标，填[IMECHARPOSITION](https://learn.microsoft.com/en-us/windows/win32/api/imm/ns-imm-imecharposition)的pt、cLineHeight和rcDocument。其他offset或无有效快照明确返回0，不夹到offset0，也不以显示caret冒充起点；没有预建逐字符坐标数组。此合同修复本身不证明真实输入法的组合链已经通过。

真实 CF_UNICODETEXT 由应用窗口链覆盖；本机搜狗的系统确认/取消/候选位置有独立的[输入法验收](../editor/README.md#输入法验收)。本模块的合成协议事件不能替代系统输入，旧版本通过记录也不能证明当前环境或其他输入法。

## CPU绘制与文字寿命

[frame.hpp](../../../src/ui/frame.hpp)定义唯一renderer协议：有序DrawQuad、物理像素bounds、半开clip、linear straight颜色、独立圆角和向内边框。可选资源为R8 Coverage或预乘RGBA ColorImage，二者互斥；无纹理时为实色盒。shader输出和blend必须配对，避免重复预乘。资源identity/revision不可重新解释成另一份像素；renderer自行管理GPU缓存、上传和在飞退休，并只合并相邻兼容项。图片字节编码、合成与统计见 [SS 绘制合同](style-sheets.md#图片圆角与批处理)。

文字服务位于生产`text.cpp`。同一个DirectWrite布局对象提供尺寸、首基线、caret位置和栅格字形；使用Factory2灰度分析合成为整段文字R8覆盖率，不建立通用图集。格式缓存按family/locale/weight/解析后字号索引。相同内容/字体/宽度/DPI复用像素；文字/约束改变时生成新版本，已发布帧持有旧像素，Session析构不使旧帧失效。固定尺寸、未参与布局测量的可见文字也在prepare时栅格化。

可编辑字段还由`UiFrame`持有不可变段落和最终文字原点、宽度、DPI与横滚。指针命中始终查询接受帧的文字快照，未接受的字体/布局改变不会重解释旧画面的坐标。内容已不同则请求重绘；连续键盘导航用接受帧的字体与宽度策略排布当前草稿。详细发布行为见 [文字一致性](style-sheets.md#发布与文字一致性)。

所有未collapsed且成功布局的控件保留`ControlBox`、身份、父子变换及滚动位置。只有最终物理bounds与祖先clip相交时才栅格化文字、输出绘制；子树仍按正常顺序处理，不以屏外父项替代语义遍历。滚回屏内会正常生成或复用文字。普通控件的裁剪减少栅格化和绘制成本，描述、布局及输入仍遍历实例；大型列表消费者使用 VirtualList 控制实例数。Table 内部按可见行列产生绘制与命中几何，不为每个数据格建立控件。

`Session::statistics()`提供常数时间观测：`prepares/measure_calls/text_layouts/rasterizations/raster_bytes/coverage_cache_hits/culled_text_controls`为Session创建以来累计值，窗口采样使用差值。prepare和rasterization计尝试次数，text_layouts计成功创建的原生段落，raster_bytes计完成的R8像素字节。符号 Coverage 同样计入 rasterizations/raster_bytes 和 live_coverages，text_layouts 仍只统计 DirectWrite 段落。`provider_revision`是版本标识，不是数量。

`live_coverages/live_coverage_bytes`是读取时仍被文字缓存、当前/旧帧或renderer持有的唯一CPU覆盖率数量和R8像素字节，同一像素的多个引用不重复计数，不包含DirectWrite对象、临时栅格数据、容器容量或GPU内存。已访问的屏外行可能继续被CPU缓存持有；内容移除或变化清理缓存后，旧帧仍可延长像素寿命。独立共享记账对象随最后Coverage释放，不回指Session，也不为计数保活像素。Session仍要求单拥有者访问；外部持有者释放采用原子记账。

可编辑字段保持完整文字排版，灰度纹理只生成当前内容窗口内的像素。横滚偏移和可见裁剪进入该字段的raster缓存键；4096个宽字符在100 DIP字段中也不会分配整条屏外位图。选择、字素导航和caret仍针对完整原文。边界由[CreateAlphaTexture](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwriteglyphrunanalysis-createalphatexture)的指定纹理矩形实现，不在生成整张大图后裁剪。

一手接口依据：[自定义文字绘制](https://learn.microsoft.com/en-us/windows/win32/directwrite/how-to-implement-a-custom-text-renderer)、[Factory2字形分析](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_2/nf-dwrite_2-idwritefactory2-createglyphrunanalysis)。核心layout保持无平台依赖；Windows/DirectWrite仅位于UI文字实现。

## 限额与验收

正式入口为 `cmake --build --preset dev --target ui_acceptance`，Release 使用同名目标。先按[根 README](../../../README.md)准备工具链和配置。[消费者的 cases 列表](../../../acceptance/ui/main.cpp)包含 4 组场景，承载以下 5 个代表，不创建原生窗口或 GPU 设备：

- K6：移除并重用控件 key，Session 析构后旧帧仍拥有文字 Coverage；释放最后一帧后弱引用过期。
- K6：真实 Session/TextStore 析构后，旧帧的 DirectWrite 段落仍能查询输入文字 `ABCDEFGHIJKLMN` 的第三个字符位置。
- K4：一张半透明红到透明的渐变图；输入 alpha 为 128/255，独立 sRGB 编码公式推出首端红色 188、alpha 128，末端 alpha 0。
- K3：高 100 DIP 的列表，底部布局边距 3 与样式边距 20，滚至末尾后尾行底部为 `(100−3−20)×1.5=115.5` 物理像素。
- K3：固定 640×480、DPI 1.5 的右下角 Tooltip，显示边界位于输入客户区以内；等待只用于到达显示态，不检验延迟合同。

单Session最多10,000个控件、布局深度128；Surface每轴最多32,768物理像素，DPI倍率0.25–8；普通 Control.text 最多4,096个UTF-16码元，单张CPU覆盖率最多16MiB。单行text_edit缓冲最多4096个UTF-16码元，number最多64个；CodeDocument 的文档限制由其[源码工作流](../editor/source-editor.md)和公开接口定义，不套用单行字段上限。字素边界和完整数值解析分别负责编辑与业务有效性。可恢复错误通过Result返回，内存分配失败正常传播。Session单拥有者线程；发布后不修改UiFrame/Coverage，也不保留可变别名。

正式消费者使用真实 Session、LayoutDocument 和 DirectWrite，检查在 Dev/Release 中均有效。撤销重做由 Editor 验收负责；基础层不重复保留历史、普通交互、错误/拒绝、计数、规模与参数矩阵。菜单形状指纹和产品输出互比不作为独立真值；产品协议与功能不因验收缩减而改变。

历史 S1 的 256 行/96 DIP 窗口首屏栅格化 4 行、裁剪 252 行，滚尾再返回累计栅格化 8 行；S2 的 4096 宽字符/100 DIP 字段保持完整编辑，覆盖率宽度不超过 100 像素。这些是固定历史证据，不是当前保留用例。正式窗口、搜索业务及本机搜狗真实输入的范围见[编辑器端到端验收](../editor/README.md)，不从 UI 协议场景推断其他系统输入法已通过。

模块、renderer及正式编辑器使用同一源码树和项目固定工具链构建；阶段验收范围与证据见[SS文档](style-sheets.md#验收)。

### 原生宿主的菜单链

`MenuItem::children` 描述子菜单，PopupEntry 将分支投影为带箭头的 `menu_item`。`Control::menu_branch` 只允许用于无 EditCommand 的菜单条目：点击或 Right 发出 activate 并保留父菜单，宿主按条目 key 打开子窗口；叶条目沿既有 activate/dismiss 路径执行。通用 UI Session 不持有平台窗口。

将 popup 根的 layout.collapsed 设为 true 会退出其 modal 状态，再展开会初始化菜单焦点。此路径保留控件/文字缓存，供原生 PopupHost 隐藏与暖重开使用；隐藏期间不能接受旧显示帧的输入。窗口链与来源失效策略由 Application 负责。
