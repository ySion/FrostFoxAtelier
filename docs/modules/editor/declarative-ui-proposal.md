# 引擎原生声明式 UI：实现和迁移

真实 Parameters 的 Position 区已接入声明与现有 Source/JIT，Session 支持稀疏属性更新。实际验证的版本与结果见[阶段验收记录](../../research/native-ui-language-20261005.md)。完整编辑器迁移、动态区域和游戏内 HUD 尚未完成，不以首片通过代表全部目标完成。

## 对专家建议的判断

采用引擎内强类型声明、持久节点、按依赖更新和共享对象体系。保留现有布局、控件、输入、SS、编辑事务和绘制，不另建 UI VM、反射注册表或 Actor 包装世界。

结合当前代码，调整四处前提：

- Source 已有同模块函数与 JIT，但 Program 只公开一个选定入口。首片生成一个 `void __ui_dispatch(uint operation)`，内部函数共享同一次编译；不为每个属性或实例创建 Program，也不提前扩展通用多导出。
- Runtime、World 没有覆盖原生直接写入的属性通知。首片使用明确的 UI 阶段快照发布，并在求值时记录 getter 实际读取；不把 World 生命周期回调冒充字段通知。
- LayoutDocument 保留节点、测量缓存和成功发布的几何；发生几何失效时仍求解参与布局的树。UI 属性定点更新与局部几何求解是两件事，不能宣称已有通用局部布局算法。
- 文字使用缓存的不可变 R8 Coverage，并无现成字体图集；视口直接绘制到主窗口，随后叠加 UI，并非已有视口纹理。首片不需要新增 GPU 协议或离屏视口体系。

此前“先用手写构造函数，再加入类 UXML 标记”的顺序由本次决定替代：直接提供对象块声明，表达式和函数仍使用现有语言编译器。

## 当前实现入口

| 入口 | 责任 |
| --- | --- |
| [declaration.hpp](../../../src/ui_language/declaration.hpp)、[declaration.cpp](../../../src/ui_language/declaration.cpp) | 使用 Source 的 Lexer/Tokens 读取 UI 结构，生成普通 Source 函数及源码位置映射；表达式类型检查交给原编译器 |
| [component.hpp](../../../src/ui_language/component.hpp)、[component.cpp](../../../src/ui_language/component.cpp) | 同 Runtime 的组件上下文、共享 Program/Lease、实例状态、实际读取依赖、失效合并和拥有式控件数据 |
| [position.ui.source](../../../assets/position.ui.source) | 真实 Position 区的唯一层级、文案、折叠状态和事件定义 |
| [workspace.cpp](../../../src/editor/workspace.cpp) | 从真实选中 Actor/root component 发布输入，提供既有字段及重置 key；消费组件输出 |
| [session.cpp](../../../src/editor/session.cpp) | 在文档 Runtime 首次创建对象前注册 UI 上下文；编辑与撤销仍归原 Session |

`atelier_ui_language` 链接 `atelier_ui` 与 `atelier_source`，不依赖 editor、World 或 RHI。它是 UI 的语言接线层；基础 `atelier_ui` 仍不依赖 Runtime。编辑器部署 `position.ui.source` 到可执行文件旁，与 `editor.ss` 一起读取。修改声明后重新打开文档或启动应用即可重新编译该组件；目前没有文件监视或运行期热换。

## 首片语义

作者使用 `ui Name(in bool ..., in double ..., in EditBinding ...) { ... }`，可在前面定义普通 Source 辅助函数。

UI 模块保留 `input`、`state`、`event` 以及 `__ui_` 前缀，辅助函数的参数和局部变量同样不能使用这些名字。表达式和控制流仍由 Source 检查；事件内的 state 赋值支持普通语句及 `for` 的初始化、步进位置。

- `Column`、`Row`、`Text`、`Foldout`、`NumberRow` 提供静态结构；节点有组件内唯一名字。新增无宿主固定别名的节点使用宿主同一单调 key 分配器。
- `input.name` 只读，`state.name` 属于实例，普通绑定更新不重新初始化状态。当前 state 支持 bool/double。
- 属性表达式编译成持续绑定。每次成功求值替换实际依赖集；条件分支和辅助函数中的读取都经过同一 getter。未读取的输入变化不运行该绑定。
- 常量属性首次求值后没有外部依赖。普通更新不会再次 JIT，也不会再次运行节点创建函数。
- `on activate` 当前用于 Foldout，`event.checked` 为 bool。只有事件可写 state；绑定与辅助函数不能写外部状态，不能直接访问生成上下文、创建对象或启动异步任务。
- `NumberRow.edit` 接收 `EditBinding`，其字段 key 对应真实对象和 FieldId 路径。它使用共享 `append_number_row`，保留标签对齐、XYZ、默认值、参数菜单、拖调、文本草稿和编辑事务。整个参数面板没有藏在一个原生函数里。
- 布局数值通过现有 Source 的显式数值转换进入 DIP 布局值；没有另建表达式解释器。颜色、字体和边框继续由现有 SS 负责。
- `collapsed` 保留实例状态并退出布局。条件销毁、key 列表、插槽和虚拟区域尚未开放，不能用 collapsed 替代这些语义。

字符串属性当前支持现有 Source 可以传给原生 sink 的 String 表达式。String/Array 返回值等通用语言缺口没有在 UI 层偷偷放宽；动态业务文字输入、更多控件与组件组合按真实用途继续扩展。

## 调用和所有权合同

```text
输入及宿主命令完成
  → 读取当前文档的成员资格、选择和已绑定属性
  → Instance::publish 比较本次阶段输入并合并失效
  → Instance::update 调用受影响的 JIT 绑定
  → 同一个 ui::Session / LayoutDocument
  → UiFrame → Renderer → RHI
  → Renderer accepted 后接受对应 UI 命中帧
```

这条阶段链独立于 World Tick。原生/反射/Source 对业务数据的写入，只要在该阶段采样前完成，就由真实值比较发现；没有接入该发布边界的模型不声称自动可观察。采样不保留成员 span 或 Actor 地址，也不额外持有业务 ObjectRoot。World 中移除但被撤销历史保活的 Actor，同样不应继续成为 Inspector 的有效目标。

一个 Component 在所属 Runtime 中编译一次，多个 Instance 共享其 Program 及固定代码租约；每个实例只创建自己的 UIContext 对象。输入、局部状态和依赖集在实例中持久保存，Source 调用栈不承担这些寿命。原生 sink 复制 String/值，不保存语言借用，不重入 JIT，不在执行回调中构造或销毁 Runtime 对象。

绑定写入真实属性继续走 `Action → editor::Session::begin/preview/commit/cancel → World`。不能在活动 JIT 内直接调用 World 编辑入口；它们的静止点准入仍有效。后续业务事件须先产生命令，退出生成代码后由宿主执行，游戏 UI 不因此获得编辑器撤销依赖。

文档切换先关闭菜单、取消输入并消费旧路由的清理 effects，移除旧控件，再卸载 Workspace 的实例和代码租约，最后关闭并释放旧文档 Runtime。应用正常析构也保证 Workspace 先于文档销毁。外部消费者必须遵守相同的 Runtime 晚于组件和租约销毁合同。

旧 UiFrame 自持有文本、样式和图片；不保存指向 JIT 实例的绘制回调或依赖 JIT 的资源 deleter。GPU 在途资源仍按 Renderer/RHI 完成条件退休。accepted 不代表 GPU 已全部完成。

## 当前成本与后续接点

实例暴露绑定/事件求值、控件创建/移除和发布计数。一次阶段内多次文档修改合并为一次输入发布；稳定输入不复制组件快照，也不求值绑定。属性更新只复制变化节点或 NumberRow 的控件片段；片段数量改变时才重排完整输出。业务目标变化时仍换字段 key。

Session 的 `update_controls` 已支持现有控件的稀疏属性批次，与 `set_controls` 共用校验、交互清理及发布逻辑；描述 revision 防止旧候选帧接收输入。33 个控件的真实 Session 场景中，单项颜色更新只更新 1 个控件，布局同步/节点访问、测量和栅格化增量均为 0；文字变化触发 1 次布局同步。合同见[属性更新](../ui/updates.md)。

周围的旧 Workspace 当前仍全量 describe/set_controls，尚未直接提交组件的变化批次。不能把“组件只求值一个绑定”写成“整个编辑器已完全增量更新”；帧生成也不承诺只遍历变更节点。

首片编译失败显示声明文件诊断；组件发布前与 Session 共用控件属性校验，非法文本、class、数值和参数行输入返回可恢复错误，保留上一次发布数据。修改输入后可重试；分配失败仍按底层异常合同传播。这里没有承诺任意实例、草稿、布局和 GPU 状态的全事务回滚。热重载在事件代际、模板兼容和旧代码退出规则明确后再实施。

## 模块职责

| 模块 | 责任 |
| --- | --- |
| UI 语言 | 声明降低、源码映射、实例状态、绑定与事件执行 |
| 编辑器与 World | 成员资格和选择、阶段采样、原字段编辑与撤销；不把对象存活等同于当前 World 成员 |
| Source 与 Runtime | 权威类型与对象身份、Program/Lease、编译与执行期准入 |
| UI 与布局 | 控件交互、稀疏描述提交、版本校验、SS、文本与几何求解 |
| Renderer 与 RHI | 拥有式帧消费、绘制顺序、GPU 提交和在途资源退休 |

## 验收与未完成范围

正式无窗口入口仍是 `cmake --build --preset <dev|release> --target editor_document_acceptance`。新增 [declarative_ui.inl](../../../acceptance/editor/declarative_ui.inl) 贯通实际文档、Source/JIT、控件、布局、输入和模型：稳定更新、属性批量写入、单绑定求值、撤销重做、折叠事件、选择换绑、删除、卸载、类型/副作用诊断及函数内动态依赖。

普通窗口使用正式 `atelier_editor --acceptance-project`，开发/发布两个配置均在 `E:/ffa_testproj` 的独立副本通过。覆盖位置编辑、撤销重做、保存重开、文档更换，以及真实 Renderer/RHI 提交；原项目的 Config/Content/Source 文件集合与哈希保持不变。命令、证据目录和覆盖边界见[阶段验收记录](../../research/native-ui-language-20261005.md)。

随后依次完成：更完整的组件输入和文本能力；条件/稳定 key 列表与虚拟化；场景树、Inspector、菜单和编辑器外壳的声明迁移；同核心且不链接 editor 的真实 HUD；最后依据测量补优化及有限热重载。现有停靠、资产浏览器和输入法不因迁移而删除或另造一套。系统文件对话框和间歇性 Unicode 剪贴板问题仍按实际验证范围保留。
