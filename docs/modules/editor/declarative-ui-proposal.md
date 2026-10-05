# 引擎原生声明式 UI

Parameters 的查询、Position、组件列表，以及 Scene Tree 的树行和查询，已由 [editor.ui.source](../../../assets/editor.ui.source) 定义。它们使用同一份 Source/JIT、文档 Runtime、SS、Session、布局与绘制设施。停靠、资产浏览器、菜单和编辑器外壳继续使用现有 C++ 描述。

## 编辑与保存

Dev 和 Release 开发工作树的构建均优先读取 `assets/editor.ui.source`；该文件不存在时读取可执行文件旁部署的同名文件。在 VS Code 保存这份文件即可更新运行中的面板，无需重新编译 C++ 或重开工程。

- 每 100 ms 检查文件时间与大小，变化稳定 200 ms 后读取；相同内容不重复编译。单文件上限 1 MiB。
- 候选成功后在下一次控件提交中替换。状态栏显示版本和本次编译耗时。
- 语法、类型、实例构建或控件批次校验失败时，状态栏显示文件诊断，已有界面继续工作。修正并再次保存即可恢复。
- 同名、同类型 state 及兼容声明节点保留身份；正在编辑的字段继续使用原 Session 草稿。删除、禁用或业务对象换绑仍按原输入合同结算旧交互。
- 当前 Program 的全部操作限定 Runtime 线程。因此去抖后的编译在 UI 安全点同步执行，可能产生短暂停顿；不声称已实现后台 JIT。

该流程针对 `.ui.source`。SS 文件、C++、多文件 imports、语言服务与调试器尚未接入保存更新。

## 声明、输入与组合

一份文件可声明多个有名 `ui` 类型，可在声明前定义使用显式参数的普通 Source 辅助函数。每个类型使用一个生成的 `void __ui_dispatch(uint operation)`；实例共享 Program，不为每个属性或列表项编译代码。更新仍检查完整类型与组合关系；同一 Runtime 中，同名类型的生成代码完全相同时，只共享上一已接受模块的 Program。每版的源码映射、声明和子类型关系独立建立。共享 Program 发布后不再修改，不建立历史版本缓存。

```text
ui Query(in TextEditBinding edit) {
    Row row {
        single_line: true;
        align_center: true;
        gap: 8;
        Text label { text: "Filter"; width: 48; single_line: true; }
        TextField field { edit: input.edit; }
    }
}
ui Inspector(in TextEditBinding query, in bool visible) {
    Column inset {
        classes: "parameter-inset";
        Query query { edit: input.query; mounted: input.visible; }
    }
}
```

组件参数必须显式提供。布局由所属容器表达，组合节点本身不额外插入一圈内边距。节点名在组件内唯一；组件类型不能与内建节点重名。`input`、`state`、`event`、`node` 和 `__ui_` 前缀用于语言接线，组件输入不能叫 `mounted`。

| 输入 | 宿主值与用途 |
| --- | --- |
| `bool` / `double` | 有限标量，表达式由既有 Source 编译器检查 |
| `Text` | 拥有式文字快照，用于 text、classes 或 filter |
| `EditBinding` | 既有类型化数值字段、分组重置与事务路由 |
| `TextEditBinding` | 稳定编辑 key、权威文字和 enabled；走共享单行编辑器 |
| `Key` | 宿主分配的 UI 身份；例如树的逻辑父行 |
| `Items<RowType>` | 不可变项列表，每项有稳定宿主 key 和 RowType 的完整输入 |

`Text` 经 plain `TextReference` 与 typed sink 复制到控件；不是 Source String 返回值的新规则。文字属性接受字符串字面量、直接 `input.text` 或直接文字编辑输入；类名仍经过 SS 的公共校验。列表转发必须保持声明的元素类型。

### 动态内容

```text
ui Entry(in Text title) {
    Text label { text: input.title; single_line: true; }
}
ui Entries(in Items<Entry> entries, in bool visible) {
    Column root {
        Repeat rows { items: input.entries; mounted: input.visible; }
    }
}
```

Repeat 按宿主稳定项 key 匹配实例，排序保留状态，移除后再插入建立新生命周期。不能用列表索引或 Actor 地址充当 key。每个被重复的类型须有一个具体根节点，该根使用宿主项 key，使 Scene Tree 保持原选择路由。列表是不可变快照，发布后不得通过保留的可变别名修改。

`mounted: false` 在成功提交后关闭子 scope；`collapsed` 只退出布局并保留实例。Source 组合循环被拒绝。每个宿主根区域最多展开 10000 个声明节点，并最多发布 10000 个控件；剩余额度逐层传入，新建子实例前检查，因此没有输出的空参数行也不能无限扩张作用域。最终完整批次继续受 Session 的容量和树结构校验约束。尚无插槽、跨文件模块或实例虚拟化。

### 控件与状态

- `Column`、`Row`、`Text`、`Foldout`、`NumberRow`、`TextField`、`Tree`、`TreeItem` 复用已有控件。`single_line: true` 使用同一个单行高度原语；文字编辑器默认使用它。
- 同一具体节点的 `height`、`expand_height`、`single_line` 互斥，避免高度策略依赖绑定更新顺序。组件输入使用这些名字时仍按普通类型化参数处理。
- `input.name` 只读；`state bool`、`state double` 属于实例。绑定记录实际执行的 getter，每次成功求值替换依赖集。未读取的输入变化不求值该绑定；稳定输入不重建节点。
- `on activate` 用于 Foldout，`event.checked` 为 bool。事件允许修改局部 state，支持 Source 的普通语句、辅助函数与循环。表达式不能直接访问 native context、创建业务对象或启动异步任务。
- `NumberRow.edit` 使用公共 `append_number_row`，保留 XYZ、默认值、菜单、拖调、文字草稿和原编辑事务。`TextField.edit` 使用已有文字编辑路由，保留 Unicode、IME、选区和历史。
- `TreeItem.tree_parent` 表达逻辑关系，物理 parent 仍为 Tree。可用 `node.name` 引用声明拥有的具体节点 key。树行必须按父先子后的顺序提交；状态、过滤与可见范围绘制仍归 Session。
- 字体、颜色和边框继续使用 SS；UI 声明负责结构、数据绑定及布局组合。

## 单一发布链与生命周期

```text
宿主输入/命令完成 → 采样当前 World、选择与字段
  → LiveDocument 检查保存、准备候选 Module
  → Instance::publish / update → 可拒绝的控件快照与子树变更
  → Session::set_controls → 用旧 owner 消费取消 effects
  → commit_controls 替换候选、提交子树、释放移除的 scopes
  → prepare → Renderer / RHI → accepted 命中帧
```

这条链独立于 World Tick。业务字段的写入仍是 `Action → editor::Session::begin/preview/commit/cancel → World`；JIT 回调只复制 UI 值，不重入 JIT，不调用 World 命令，不构造或销毁 Runtime 对象。

原生注册和 Source 共用文档 Runtime。每个 Instance 创建一个 UI Context；不持有 Actor 根或业务地址。C++ 按当前 World 成员采样，即使撤销历史保活了已删除 Actor，也不会继续投影为可编辑目标。

一个 LiveDocument 管理一个作者文件及具名宿主区域。区域的 parent 和 key aliases 在其生命周期内固定，变化会明确拒绝。编译候选先独立建立实例，按兼容声明迁移状态/key；旧实例在完整 Session 批次被接受且取消 effects 消费后才释放。隐藏的宿主区域保留实例，以便重新显示时沿用状态。

普通输入更新也遵循同一事务：`update` 准备整棵子树，不移除已接受的 owner；`reject_controls` 恢复输入、局部状态和快照；`commit_controls` 在取消 effects 后一次提交成员关系。重复 commit/reject/close 安全。相同的失败输入暂缓重试，已有区域仍可响应局部事件；首次挂载被拒绝时保留空视图并释放候选实例，修正输入或源码后恢复。关闭 Instance 会释放代码、元数据和子实例，即使其 C++ 外壳仍由调用方保留。

控件 key 来自宿主同一个单调分配器。Runtime 和 key 分配器必须晚于实例销毁；Workspace 保持地址稳定。文档切换先取消输入、移除旧控件及结算 effects，再关闭 UI scopes、代码租约与文档 Runtime。

旧 UiFrame 自持文字、样式和图片，没有 JIT 回调或依赖 JIT 的资源 deleter。GPU 在途数据仍按 Renderer/RHI 完成条件退休，UI accepted 不代表 GPU 已完成。

编译、创建及 Session 描述拒绝保留旧版本。描述提交后的布局/渲染失败继续沿用 Session 失败恢复，不回滚已经取消的草稿；分配失败也不伪装成可恢复语言错误。

## 模块入口

| 入口 | 责任 |
| --- | --- |
| [declaration.cpp](../../../src/ui_language/declaration.cpp) | Source Lexer、UI 结构/组合检查、表达式降低及源码位置映射 |
| [component.cpp](../../../src/ui_language/component.cpp) | 共享代码、实例状态/依赖、动态子实例及控件快照 |
| [live_document.cpp](../../../src/ui_language/live_document.cpp) | 文件去抖、候选版本、宿主区域、提交与拒绝 |
| [editor.ui.source](../../../assets/editor.ui.source) | 真实面板结构、文案、查询组合、折叠和动态列表 |
| [workspace.cpp](../../../src/editor/workspace.cpp) | World 阶段采样、原业务 key、整体工作区描述 |
| [application.cpp](../../../src/application/application.cpp) | 原生输入、Session 提交/effects 顺序、Renderer 接受链 |

`atelier_ui_language` 链接 `atelier_ui` 与 `atelier_source`，不依赖 editor、World 或 RHI；基础 UI 仍不依赖 Runtime。结构和表达式没有第二个解释器。

## 验收与成本边界

正式入口为 `cmake --build --preset <dev|release> --target editor_document_acceptance ui_acceptance atelier_editor`，窗口使用 `atelier_editor --acceptance-project output [source-project]`。

[组合验收](../../../acceptance/editor/composed_ui.inl) 使用实际文件、JIT、Runtime 与 Session，覆盖文本输入、独立状态、列表重排/重挂、语法/属性/结构错误保留、恢复与草稿连续性。[事务验收](../../../acceptance/editor/ui_reload_transaction.inl) 覆盖失败卸载、首次挂载拒绝、容量、幂等及关闭后的资源释放；[代码复用验收](../../../acceptance/editor/ui_reload_profile.inl) 覆盖旧租约、当前源码位置、子类型更新和 Runtime 边界。[窗口验收](../../../acceptance/editor/ui_authoring_window.inl) 保存作者文件的独立副本，验证真实画面更新、错误恢复、输入 token、折叠状态及后续 World 字段编辑，并记录编译及保存到接受帧的耗时。

组件只求值失效绑定，输入只复制变化槽位；变化片段可通过 swap 提交或恢复，长度变化时准备新的连续快照。控件 key 序列不变时跳过 key 集合构造与差集统计；稳定帧不递归执行退休清理。源码按组件生成代码复用 JIT，不是一般的函数级增量编译。宿主仍逐帧采样并 describe/set_controls；这不等于整个编辑器只遍历变化节点，也不等于增量布局求解。

可选真实热更采样入口为 `atelier_editor_document_acceptance --profile-ui-reload output.csv`，在对应构建目录运行。它执行 120 轮编译、替换、失败和关闭，记录编译量、耗时、Runtime 存活数和进程内存。解释及证据见[热更审计](../../research/ui-hot-reload-audit-20261005.md)。

第一阶段的历史证据见[原生 UI 首片](../../research/native-ui-language-20261005.md)。当前第二阶段的结果见[组件与保存更新验收](../../research/native-ui-authoring-20261005.md)。后续按实际需求迁移资产浏览器和更多 Inspector 控件，再建设插槽/跨文件组织及独立 HUD；后台编译需先明确 Runtime 的并发准备合同。
