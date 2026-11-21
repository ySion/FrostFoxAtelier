# 持久实例与稀疏属性更新

`Session`持有唯一控件状态和布局节点。初装或增删、重排、改父关系使用`set_controls(span<const Control>)`；属性绑定用`update_controls(span<const Control>)`提交发生变化的完整Control值。两入口使用同一校验、交互清理和描述提交实现。

持续生成有序描述的宿主可复用一个 `ControlList`。每次 `begin` 后按绘制顺序追加区段，再 `finish` 并交给 `Session::set_controls`。非零区段版本标识相同的控件内容；版本、位置和长度都匹配时沿用已收集的值。宿主附加提示等派生属性的外部输入改变时，须同时更新 `begin` 的装饰版本。Session 仅在自身描述版本与上次成功提交的基线一致时采用增量。调用者始终可以从列表读取完整描述。

描述生产者可在发布前调用 `validate_control_properties(Control)`，复用 Session 对文本、枚举、数值和样式的约束。该函数不检查父树引用或求解布局；整批结构关系、身份和交互状态仍由 Session 提交入口校验。

## 属性批次

- 每项必须对应当前存在的非零key；不接受重复key，输入顺序无意义。未出现的控件保持原值。
- 不接受改变`kind/parent/layout.kind`、`tree_row_count/tree_index/tree_depth/tree_branch/tree_expanded`、`popup_owner/selection_group`、`context_target/context_group/edit_command/menu_branch`。这些定义种类、结构或命令目标；结构更新仍使用完整描述，业务重绑使用新key。
- text、value、enabled、selected、modified、`tree_lines`、`tree_primary`、layout尺寸/对齐、classes、style和style_scope沿现有Control合同更新。
- 同值提交不推进描述版本、不计入更新次数。完整描述入口仍有全值比较的快速返回。
- 一批先验证全部控件和相关引用，再准备发生变化的描述与新节点。非法批次不会应用前面的有效项。文字服务准备失败时显式返回 Result，恢复描述、交互与树状态，不发布候选清理 effects；不执行宿主回调。宿主 CPU 分配失败按工程合同 fatal，不靠 catch/退栈恢复。
- 只有全部准备成功才推进`controls_revision`并返回有序Reply。宿主应在旧binding仍可查询时处理取消effects，再退役旧映射。

以下是已有描述的定点更新；`field`和对应业务绑定由调用者保存：

```cpp
field.value = model_value;
auto changed = ui.update_controls(std::span<const ui::Control>(&field, 1));
if (!changed) return report(changed.error());
apply_effects(*changed);
```

活动文字草稿不被新的权威值覆盖；未发生编辑的被动缓冲跟随权威值并保留合法选区。祖先禁用/折叠、移除或kind改变仍通过原路径结算focus、capture、IME、按键状态及owned popup。删除再出现使用新incarnation；纯绘制改变不更换身份。

`action_target`、按钮/菜单行为和 radio 组可以沿同一属性入口更新，但改变它们会取消旧按压/编辑，并阻止旧接受帧继续派发。`code` 换文档指针也走原失效清理；已确认源码仍属于 CodeDocument。稳定 key 只表示控件寿命，不能代替外部业务对象/字段身份。

表格的数据快照、`table_layout` 和 `table_selection` 分别表达模型、呈现与选择。新快照提交前建立并校验行/列索引；仅改变选择或呈现不重建数据索引。数据或呈现变化取消旧指针手势，选择变化也清理待键盘激活状态。接受帧保留对应数据和呈现，旧几何不能用于新布局的输入。完整表格及交互状态合同见[UI 模块](README.md)。

## 失效与实际成本

`prepare`按父先序检查状态与样式继承，只有脏控件或必要后代重新生成有效NodeInput、同步到LayoutDocument。

- 最终颜色改变只更新样式/绘制，复用文字Coverage，不同步布局输入、不重测或重新栅格化。父文字颜色改变传播继承样式，不伪造字体/provider版本。
- text/value变化通过原草稿显示规则和TextStore内容版本；只有被实际布局使用的度量依赖才触发布局重算。固定尺寸文字仍在绘制准备时生成当前内容的文字资源。
- 字体、文字对齐、single_line、有效padding、树缩进/行高、scrollbar gutter按实际变化同步。selected/modified/enabled可能通过SS改变字重，不能无条件当作纯绘制属性。
- 编辑/组合显示版本变化只要求对应字段重新同步；首基线上下文和父样式仍按树序传播。

稀疏批次只复制改变的Control，不复制整棵状态树。树过滤、树行层级/可见性改变仍会更新树元数据；移除默认值时会检查引用该目标的reset菜单。暂存描述、effects和需要保留的交互状态可能分配内存，不承诺零分配。

仍存在以下成本：

- `prepare` 检查状态依赖并构建完整绘制帧，整体成本不等于 O(改动数量)。
- 结构变化时，[`synchronize`](../../../modules/gui/ui/src/session.cpp) 生成父项在前的最终树，单次调用 `LayoutDocument::set_tree`；省略的布局节点随批次退役，保留节点不再逐项移出和重新挂接。祖先反转、保留孩子移出被删祖先及兄弟重排都按最终关系提交。布局核的拓扑处理为 O(节点池历史槽位数 + 控件数)，另计描述复制与比较；Session 仍有状态表查找和完整描述遍历。这是实现分析，整次准备的耗时仍待采样。
- 几何真正失效后，布局核仍求解参与布局的实例树，没有通用子树布局调度。普通 list/tree 的绘制裁剪也不减少已经实例化的控件数。

稀疏属性更新继续使用现有 `update` 路径。结构批次失败不会部分提交布局输入；Session 保持结构待同步，重试重新生成全部条目。控件描述提交与 `prepare` 仍是两个阶段，不因为布局失败撤销先前成功的 `set_controls`。批量合同见[布局核心](../ui-layout/contract.md#完整树批量提交)。

## 候选、失败和资源寿命

`UiFrame.controls_revision`记录本次prepare使用的完整描述版本。`accept`同时核验描述版本、SS版本、Session/Surface身份和绘制帧顺序。准备候选后再提交新描述，旧候选被拒绝，即使key和incarnation相同。

`prepare → render → accept`期间保持单线程更新阶段，不重入描述修改；renderer最终accepted后才接受对应UiFrame。`accept`返回false不能解释成GPU画面回滚，宿主必须处理该失败。

描述提交与完整帧发布是两个阶段。NodeInput规则、布局依赖、实际文字资源等仍在`prepare`解析。出帧失败保留已接受UiFrame及资源，但不会撤销已成功提交的描述或取消effects，也不会自动恢复已取消的草稿。Session此时暂停交互激活；只有失败之后准备的新候选被接受，才恢复输入。每次准备开始即预留现有单调Frame.id，失败时记录该边界，因此重交此前的帧也不能解除暂停，即使描述、主题和Surface均未变。失焦/捕获丢失和带token的平台结果继续沿原清理协议处理。

TextStore 的段落候选和 TextSnapshot 的 caret stops 在全部系统调用成功后才发布到缓存；失败不标记半份结果为有效。已接受帧继续独立拥有旧 DirectWrite 段落和 Coverage。输入失败保留 `Reply.error` 的原码，宿主停止后续普通动作，但仍执行已产生的取消编辑、释放 capture、IME 取消及键盘 latch 退休。

没有失败的普通候选准备仍保持原合同：同一活身份可以按已接受几何交互，候选坐标不会提前参与命中。移除/重建的key由incarnation检查拦截；业务对象或字段变化须由生产者换key，不能仅替换外部binding表。

## 观测

`Session::statistics()`提供以下描述与布局同步累计值；其余文字、资源和裁剪计数见公开头：

| 字段 | 口径 |
|---|---|
| control_batches | 成功且产生变化的描述批次 |
| controls_updated | 更新的已有Control数量，完整入口也只计不同值 |
| controls_created / controls_removed | 插入/移除的活Control数量 |
| layout_syncs | 脏控件生成并同步布局输入的尝试次数 |
| layout_nodes_created | 成功创建的显式布局节点数，不含隐式root |
| layout | 原LayoutDocument::Stats，包括compute、node_visits、测量缓存和内部缓冲统计 |

其中 `layout.topology_commits` 统计成功且产生变化的完整树批次，`layout.topology_links_written` 统计改变的最终孩子序列实际写入的节点数。批量失败或同值提交不增加这两项；计数不包含描述扫描、比较、普通逐项编辑或后续布局求解。

使用操作前后差值。布局、文字、绘制统计分别表达自己的工作；零node_visits不代表整次prepare没有扫描控件。

## 历史验收

本节是 2026-10-05 稀疏更新增量的固定证据，不是当前全部 UI 场景清单。起始基线`855433a840584b414073758d6587f5ce6ae66703`，交付提交`08b2fa2cbeee1033948603f97a9c9564abd5e93d`，Dev/Release各执行正式`ui_acceptance`，均20/20 PASS，exit 0。新增四组真实Session/LayoutDocument/DirectWrite端到端场景：

- `persistent_control_updates`：33个静态节点；单项颜色更新的controls_updated增量1，layout_syncs、layout.node_visits、measure_calls、rasterizations增量均0；文字更新仅同步1个控件；继承颜色/字体、同值no-op及旧帧身份/像素保持。
- `control_batch_validation`：全量/稀疏坏批次、缺失/重复key、种类/父关系/绑定引用拒绝；活动草稿保持、提交值正确、被动缓冲同步。
- `control_update_cleanup`：祖先禁用时真实编辑协议的edit_cancel、IME取消和capture release；全量移除复用清理；坏批次不关闭popup，owner禁用正常dismiss。
- `control_candidate_revision`：描述/主题版本拒绝旧候选；布局失败及几何已发布后的UI准备失败均保留旧UiFrame并暂停激活；暂停Surface恢复同一坐标映射后，旧帧仍不能绕过失败边界；恢复必须等待新帧accepted，重复接受同一恢复帧有效。

命令：正式开发环境中运行`cmake --build --preset dev --target ui_acceptance`和release对应入口。日志位于`out/review/ui-sparse-updates/{dev,release}-acceptance.log`。

该次增量未进行每个分配点的异常注入，也未运行GPU、窗口或真实系统IME/剪贴板；生产协议注入只证明UI侧边界，不替代平台验收。当前覆盖与其他集成证据从[UI 模块](README.md#限额与验收)进入。
