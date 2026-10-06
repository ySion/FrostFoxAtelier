# UI DSL 更新粒度：QML、Unity UI Toolkit、Flutter 与当前实现

日期：2026-10-05。性质：官方资料研究与设计输入；同日按用户授权落实源码 patch 后，仓库能力部分已同步。外部框架的事实与项目的实现选择分开陈述。

Qt 以 6.8 文档为准，Unity 以 6.0 的 UIDocument 路径为主；Flutter 使用本次查阅的官方文档。旧版 Unity UI Builder 资料单独标明，避免跨版本推定内部实现。本仓库事实采用本轮调用链核对结果；本文未运行这些框架，也不提供性能排名。

## 1. 先分清五个问题

| 层次 | 要确认的问题 | 不能据此推出的结论 |
| --- | --- | --- |
| 普通数据更新 | 已有输入变化后，哪些绑定重新求值？ | 能按依赖求值，不等于能局部替换源码。 |
| 源码与编译 | 保存后重读、解析、降低、编译的范围是什么？ | 复用编译结果，不等于复用运行中的 UI 实例。 |
| 实例与状态 | 原实例保留，还是创建新实例并迁移状态？ | 数值和 key 保留，不足以证明对象身份保留。 |
| UI 树更新 | 更新属性、协调子树，还是重建整个文档/region？ | 树被重建，不等于所有下层控件都被销毁重建。 |
| 布局与绘制 | 哪些变化进入布局、绘制数据和提交阶段？ | 最终只改少量控件，不等于前面只做了少量工作。 |

这些是分析维度，不要求实现五套入口。代码版本、类型定义版本与对象身份也应继续遵循仓库 [CONTEXT.md](../../CONTEXT.md) 的区分。

## 2. 官方资料确认的边界

### Qt/QML：依赖更新与组件重新加载是不同能力

**官方明确：**属性绑定监视表达式读取的属性依赖，变化时重新求值并赋值。命令式写入静态值会移除原绑定；显式安装新绑定可用 `Qt.binding()`。[Property Binding][Q1]

`QQmlEngine::clearComponentCache()` 清理引擎内部未被引用的组件及其属性元数据；被引用的组件不会因此被丢弃。清理后，创建新对象前需要加载组件。旧对象（包括单例）保留旧类型；清理后从同一 QML 源码创建的新对象可能无法赋给旧对象的对应声明类型属性。官方建议清缓存时不要保留 QML 创建的存活对象。[QQmlEngine][Q2]

编译缓存还有另一层：Qt 可预编译函数/绑定，或在加载时生成并缓存字节码；使用缓存时会检查源码和 Qt 版本等条件。这描述的是代码的加载与编译复用。[The QML Disk Cache][Q3]

**本文判断：**以上契约不能支持“保存 QML 就把新表达式原位补进所有旧对象”的结论。属性依赖求值、显式替换绑定、清缓存后重新实例化，应分别描述；本文没有据此评价 Qt Creator 或其他预览工具的全部能力。

### Unity UI Toolkit：资产刷新、UI 重建与重新接线

**官方明确，Unity 6.0：**Live Reload 接收 UI Builder 或文本编辑器中的 UXML/USS 变化。Editor 窗口默认关闭，启用后作用于同类型窗口；Game view 默认开启，开关作用于所有 Game view。[UI Toolkit live reload][U1]

该页在 Editor windows 部分明确说明会重新创建整个 UI，需要保存并恢复希望保留的状态。UIDocument 所在 GameObject 的 companion MonoBehaviour 会在重载前停用、重载后重新启用；交互初始化和清理应放在 `OnEnable` / `OnDisable`。[UI Toolkit live reload][U1]

**绑定限制：**普通 runtime binding 可选择每次更新、源变化检测或显式 dirty；无法检测源变化时可能退回每次更新。数据源的版本/属性变化通知需要显式接入。因此不能概括成“UI Toolkit 的所有绑定天然只计算实际变化的依赖”。[更新触发条件][U2]、[数据源与变化跟踪][U3]

**USS 的证据边界：**旧版 UI Builder 1.0.0-preview.18 官方文档明确说，保存 USS 会刷新使用它的地方，无需重开 Editor 窗口；同页要求 UXML 重载后控制组件重新绑定。[Testing outside the UI Builder][U4] 这支持区分样式刷新和结构重载，但不是 Unity 6.0 对“所有 USS 修改保留全部元素身份”的保证。

**本文判断：**这里可确认的是编辑期的资产更新体验，以及重建后的状态恢复/接线责任；不能把它推广为任意 C# 代码热替换或已发布客户端的源码监控能力。官方 profiler 也把资产检查、绑定、样式、布局、绘制数据分开记录，资产检查列在 Editor-only 阶段。[UI Toolkit profiler markers][U5]

### Flutter：更新代码后保留兼容状态，再重建描述树

**官方明确：**原生开发路径由 Dart VM 的 JIT 和增量重编译支持；Web 使用自己的增量编译路径。[Dart overview][F1] Flutter hot reload 仅支持 debug 模式。更新库代码后，框架触发 widget 重建及后续布局/绘制；已有状态保留，`main()` 和已有 State 的 `initState()` 不重新执行。[Hot reload][F2]

原生流程的编译范围包括变化库、主库及主库到变化库之间的库；编译成 kernel 后送入 VM。库加载阶段本身不重新执行代码，随后由 Flutter 触发 UI 更新。这直接体现了编译更新和树更新的不同粒度。[Hot reload][F2]

状态保留也有树匹配条件：`Widget.canUpdate` 要求 `runtimeType` 与 `key` 都相等；符合条件时已有 Element 可以接纳新 Widget 配置。State 生命周期文档说明更换配置会调用 `didUpdateWidget`，hot reload 还会调用 `reassemble`；移除或不兼容的子树仍会经历停用和释放。[Widget.canUpdate][F3]、[State][F4]

**官方限制：**编译错误会拒绝 reload；枚举与普通类互换、泛型声明变化不受支持；原生代码修改需要 full restart。全局/静态字段的已有状态不会重新初始化，旧状态配新代码可能与重新启动得到不同结果。[Hot reload][F2]

**本文判断：**Flutter 的 stateful hot reload 依赖代码更新、树协调和生命周期共同成立。保留 State 不代表保留每个 Widget 对象，也不意味着只重建被改源码直接对应的一个节点。现有 State 不重跑 `initState()`，不妨碍更新后新增的 State 正常初始化。[State][F4]

## 3. 同一口径的比较

| 系统/路径 | 普通数据变化 | 保存源码后的主要动作 | 身份与状态边界 |
| --- | --- | --- | --- |
| QML 属性绑定 | 重算相关绑定。[Q1] | 绑定契约本身不规定文件保存流程。 | 更新既有属性；不能推导旧实例已取得新源码。 |
| QML 清组件缓存 | 不属于普通数据求值。 | 清缓存，再加载组件、创建对象。[Q2] | 旧对象仍属旧类型；不是自动迁移旧实例。 |
| Unity 6.0 Live Reload | runtime binding 有独立触发条件。[U2] | 接收 UXML/USS 更新；Editor UI 重建及 UIDocument 重新接线见上述契约。[U1] | 状态恢复由使用方处理；USS 不能直接套用无条件身份保证。 |
| Flutter hot reload | State 可请求子树 rebuild。[F4] | 更新代码后 rebuild；并非只刷新源文件对应节点。[F2] | 兼容 Element/State 可延续；Widget 配置可替换。[F3] |
| 当前 FrostFoxAtelier | 实际依赖的 dirty 绑定求值；Repeat 按 item key 保留 child。 | 全文件 lower；部分 Program 可复用；原 Instance 准备新定义与子树变更。 | 兼容 Context / state / key 保留；新增、移除或换型子组件改变生命周期；一次提交完整源码版本。 |

比较结论：**“保存即见结果”是交互体验；编译复用、状态保留、实例保留和局部树更新是四项独立的实现承诺。**这属于本文对上述官方契约的归纳。

## 4. 本仓库目前做到哪一层

以下是当前实现事实，不是对外部框架的推断。研究起点 `404b2e6` 曾在保存后创建所有区域的候选 Instance 树；同日后续实现已经移除此路径，采用稳定实例上的定义与子树事务。

1. `LiveDocument` 每次有效源码变化仍然 lower 全文件。
2. 同一 `Runtime` 中，同名组件的 `generated_source` 未变化时可复用 `Program`；Module 仍拥有本版组件集合和新的关系元数据。
3. `poll` 对 accepted 宿主 region 的原 Instance 调用 `update(inputs, candidate_type)`。同名兼容组件保留 Context；代码或结构变化只重建定义相关的数据，并协调原子实例。未变组件不重算绑定，但保存时仍会遍历已挂载实例以更新元数据、检查子定义。
4. 当前 `Application` 仍生成完整工作区快照，并调用 `ui::Session::set_controls`；Session 内部按 key 和值差分应用。
5. 基础 `Session::update_controls(changed_controls)` 只接收已有 key 的完整属性值，不能新增、删除或改变 kind、parent、结构引用。源码 patch 已在实例层落实，Session 层继续使用支持结构变化的完整快照入口。
6. 普通输入更新时，`Instance` 只求值实际依赖的 dirty 绑定；`Repeat` 的 child 复用按 item key 进行。这条已有运行期增量能力应与源码保存路径分开陈述。

上述结论的本地依据是 [LiveDocument::poll/commit_controls](../../src/ui_language/live_document.cpp)、[Module::compile 与 Instance](../../src/ui_language/component.cpp)、[Application::step](../../src/application/application.cpp) 及 [Session 控件差分](../../src/ui/session_update.inl)。当前用法与限制统一放在[DSL 文档](../modules/editor/declarative-ui.md)，实际身份、状态及拒绝行为由[源码 patch 端到端验收](../../acceptance/editor/ui_source_patch.inl)覆盖。

当前源码保存链路可以压缩为：

```text
源码变化 → 全文件 lower → Module/Program 复用或编译
         → accepted regions 在原 Instance 上准备定义与子树变更
         → 完整工作区快照 → Session 按 key/值差分应用
```

当前已具备编译产物复用、原实例保留和子树协调。组件自身代码/结构变化后会重算该组件全部绑定，不是逐表达式 patch；也不代表全文件前端、区域遍历或工作区快照提交变成与改动量成正比。

## 5. 实际采用的设计与后续边界

本项目采用稳定作用域与可变定义分开的结构，不引入外部框架或另一套解释器：

1. **身份可观察。**实际 Context 身份和子树创建/回收计数用于验证原对象存续；稳定 key 和最终存活数不足以证明这一点。
2. **结构与属性同一事务。**`PreparedUpdate` 同时覆盖定义、绑定缓存、代码租约和子成员。Session 接受并结算旧 effects 后统一提交，不留全区域克隆备用入口。
3. **兼容性明确。**state 按名称/类型保留；具名子组件按节点名称/种类/组件类型保留；Repeat 按 Item.key 保留。移除属性恢复默认值，新子实例正常初始化。
4. **优化边界继续按测量推进。**未变子树跳过、稀疏 Session 提交和后台 JIT 尚未接入。后台 JIT 尤其依赖 Runtime 的并发准备与关停契约；实例 patch 不自动提供线程安全。

本轮具体变更与本机采样见[源码 patch 验收记录](ui-source-patch-20261005.md)。后续已确定长期支持同步更新，后台化按真实负载评估，详见[默认更新策略](../modules/editor/declarative-ui.md#默认更新策略)。本文不以这些结果推断对 Qt、Unity 或 Flutter 的性能优势。

## 官方来源

链接均指向官方具体页；正文已将官方契约、本文判断和本仓库事实分开。

- Qt 6.8：[属性绑定][Q1]、[QQmlEngine 缓存与旧实例][Q2]、[磁盘与预编译代码缓存][Q3]。
- Unity 6.0：[Live Reload][U1]、[绑定更新触发条件][U2]、[数据源变化跟踪][U3]。
- Unity 补充：[旧版 UI Builder 的 USS/UXML 区别][U4]、[Unity 6.2 profiler 分层][U5]；不把这些版本的细节泛化为 6.0 的额外保证。
- Flutter/Dart：[JIT 与平台编译路径][F1]、[hot reload 流程及限制][F2]、[Widget 匹配条件][F3]、[State 生命周期][F4]。

[Q1]: https://doc.qt.io/qt-6.8/qtqml-syntax-propertybinding.html
[Q2]: https://doc.qt.io/qt-6.8/qqmlengine.html#clearComponentCache
[Q3]: https://doc.qt.io/qt-6.8/qmldiskcache.html
[U1]: https://docs.unity.com/en-us/engine/6000.0/manual/uitoolkits/uielements/uie-test-ui/ui-toolkit-live-reload
[U2]: https://docs.unity.com/en-us/engine/6000.0/manual/uitoolkits/uielements/uie-data-binding/uie-runtime-binding/mode-update
[U3]: https://docs.unity.com/en-us/engine/6000.0/manual/uitoolkits/uielements/uie-data-binding/uie-runtime-binding/define-data-source
[U4]: https://docs.unity3d.com/Packages/com.unity.ui.builder@1.0/manual/uib-testing-ui-testing-outside.html
[U5]: https://docs.unity.cn/6000.2/Documentation/Manual/UIE-profiler-markers.html
[F1]: https://dart.dev/overview#dart-native-machine-code-jit-and-aot
[F2]: https://docs.flutter.dev/tools/hot-reload
[F3]: https://api.flutter.dev/flutter/widgets/Widget/canUpdate.html
[F4]: https://api.flutter.dev/flutter/widgets/State-class.html
