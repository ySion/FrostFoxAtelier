# Carbon 优化提交的模块复审

原生反射审查基线为 `f21f9099`，声明式 UI 审查基线为 `a3de0c0c`。本记录保留 `e25747ac`、`85c6127f`、`ba89fcc5` 涉及模块的审查发现和处理状态。以下状态截至 2026-11-19。

## 待处理

| 模块 | 证据与业务影响 | 处理时机 |
| --- | --- | --- |
| UI 辅助函数热更 | 每个组件都包含全部文件级辅助函数。只把工具架的 `shelf_expanded_height` 从 56 改为 57，本机同步重编 44/44 个组件，耗时 1.16 秒。[生成器](../../modules/gui/ui_language/src/declaration.cpp) · [Program 复用](../../modules/gui/ui_language/src/component.cpp) | 单独评估辅助函数的重编范围，保留类型检查、传递调用与源码诊断。 |
| P3 · UI 事件嵌套预算 | 给同一处理器增加兄弟绑定，会减少其可编译的正文深度。当前编辑器的八个本地事件均只有一条赋值。实测边界与替代分派方式见下文。[UI 分派生成](../../modules/gui/ui_language/src/declaration.cpp) · [Source 嵌套检查](../../modules/execution/language/src/source.cpp) | 调整生成结构时解决；无需因现有界面调高 Source 的全局嵌套上限。 |

## 已修复

| 模块 | 审查基线上的复现 | 当前行为 |
| --- | --- | --- |
| UI 拒绝输入的重试 | 相同 NaN 连续送达时每帧重报。 | 已修复。相同失败快照只拒绝一次，改回有限值后恢复；重新分配的 Repeat 项和数值编辑字段已验证，相同 NaN 位模式被抑制，载荷改变后重新尝试。[失败快照](../../modules/gui/ui_language/src/live_document.cpp) |
| UI 事件错误定位 | 事件第二次写入非有限 state 时回滚成功，但丢失源码位置。 | 已修复。保留底层错误码、消息和失败点，通过执行时的 UI 声明映射回作者位置与内容版本；复用代码、重新编译和拒绝热更候选均已验收。[运行时诊断](../../modules/gui/ui_language/src/component.cpp) · [端到端验收](../../acceptance/ui/hot_reload.cpp) |
| UI 热更重复扫描 | 每解析一个组件，`lower_module` 都重新从文件开头读取 token，直到到达该组件正文。 | 已修复。收集声明头时保存正文首个 token 和词法位置，解析器从这里继续读取。保留整份文件的前向组件引用检查和每版源码映射。[生成器](../../modules/gui/ui_language/src/declaration.cpp) |
| UI 的 Repeat 局部更新 | 只折叠一个 PropertyGroup，旧实现仍遍历整张列表、排序 key 并准备每个子实例。 | 已修复。已命中的子项按显示顺序准备，变化区间沿原事务提交与回滚。稳定 key 重排、按压期间换绑和拒绝后继续交互均已验收。[实现](../../modules/gui/ui_language/src/component.cpp) · [端到端验收](../../acceptance/ui/main.cpp) |
| 场景加载与自定义校验器 | 临时游戏组件 `SpawnWindow` 声明 `minimum <= maximum`。从默认 `(1,5)` 将下限改成 8，编辑器拒绝并恢复 1。同样的 `(8,5)` 写入场景后，严格和容错加载都成功、诊断数均为 0，重新调用 `Runtime::validate` 才报告失败。场景加载按内建组件类别调用校验器。该分支早于 Carbon，`ba89fcc5` 又向业务类型开放了声明式校验。[场景恢复](../../modules/editor/core/src/scene.cpp) · [World 编辑](../../modules/engine/world/src/world.cpp) | 已修复。严格加载拒绝，容错加载按失败路径恢复实例默认值。保存写入修复值，随后可严格重开。 |
| 原生反射校验器签名 | Release 生成注册和手写注册都接受 `static Result<void> check(Self copy)`。用记录的复制构造函数计数，两种入口均为校验通过时复制 1 次，失败时复制 2 次。失败会重走一次以生成字段路径。现有物理、灯光和相机类型使用无参 `const` 成员校验器。[生成器](../../tools/reflection/main.cpp) · [手写注册](../../modules/execution/runtime/include/runtime/native.hpp) · [签名约定](../reflection.md) | 已修复。生成器与手写模板均在接入时拒绝按值校验。 |
| 紧凑派生类的基类偏移 | `ba89fcc5` 的生成器为对齐为 1 的派生类和对齐为 8 的非虚基类也输出 `.base<Base>(0u)`。Runtime 按直接偏移的对齐契约拒绝整类注册。省略该可选偏移后，同一绑定可以注册；另在已确认基址按 8 字节对齐的堆对象上，C++、Runtime 与 JIT 读写一致。仓内业务类型未发现采用该紧凑布局。[偏移生成](../../tools/reflection/main.cpp) · [发布检查](../../modules/execution/runtime/src/runtime.cpp) | 已修复。生成时按实际基类对齐和偏移诊断，Runtime 对手写注册的回调与直接偏移路径也检查基类对齐。 |
| UI 与 CPU 材质的冗余状态 | UI 编译器内部的 `Evaluation::operation` 和 CPU 材质的 `MaterialProgram::State::resources` 均只写无读。UI 运行时使用的操作编号、材质使用的资源值仍有读者。[UI 编译器](../../modules/gui/ui_language/src/declaration.cpp) · [CPU 材质](../../modules/execution/source/src/material_program.cpp) | 已删除两个只写成员及其初始化，事件与绑定仍保留实际分派所需的操作编号。 |
| JIT 写标记文档 | 文档称多次写入各自盖章。同步 JIT 会合并同一执行段内对同一 `self` 的重复标记。执行期间 `Runtime::change_cursor()` 返回 `busy`，消费者无法在两次写入之间推进游标，观察到的变更集合一致。[标记生成](../../modules/execution/jit/src/source_backend.cpp) · [游标实现](../../modules/execution/runtime/src/runtime.cpp) · [写代号契约](../runtime-model.md) | 已按执行段内可观察的写代号更新运行时文档。 |

### 紧凑继承的取舍

在 Ryzen 9 3950X 上用项目锁定的 Clang 23.1.5、Release 优化和 mimalloc 对照了同一组字段（先 `double` 后单字节标记）。自然布局为 16 字节、对齐 8，紧凑布局为 9 字节、对齐 1。419 万条连续记录顺序读取 `double` 时，占用分别为 64 和 36 MiB，15 轮交错测量的中位数为 0.689 和 0.476 ns/条。用 Runtime 对象所用的分配器分别独立申请 104 万个数据块，两者的平均可用块大小同为 16 字节，打乱指针顺序后的读取中位数为 3.604 和 3.507 ns/条，四分位区间重叠。测量覆盖数据布局与分配器，不包含反射调用和 GC。[对象分配](../../modules/execution/runtime/src/runtime.cpp) · [分配器接入](../../modules/core/memory/src/global_new_delete.cpp)

活的反射对象及其基类按各自 C++ 类型要求的对齐访问。当前 JIT 会按字段类型的自然对齐生成读写，原生引用和方法也会消费实际地址。若今后引入紧凑外部格式，可在格式边界按字节解码。如果连续数据确有可测收益，再针对其访问方式决定是否支持直接计算。[JIT 字段读写](../../modules/execution/jit/src/source_backend.cpp) · [原生借用](../native-value-borrows.md)

## 等待业务信号

- **Mask 材质的 CPU 拾取。** 每个候选交点调用材质求值时，会为每个已声明纹理槽扫描全部绑定。StandardPBR 的六槽对应 36 次槽号比较，旧实现同样是平方复杂度。目前只有静态工作量，若拾取耗时成为问题，再用有遮罩的场景采样。[拾取入口](../../modules/editor/core/src/viewport_tool.cpp) · [纹理绑定匹配](../../modules/execution/source/src/material_program.cpp)
- **Flags 操作符参数。** 默认组合值 `A | B` 可以编码为整数 `3`，参数描述的 `enum` 却只列出成员名。场景字段的 Flags 掩码已另外处理。首个实际使用 Flags 参数的操作符接入时，再统一参数描述和解码器接受的值。[操作符参数描述](../../modules/editor/application/src/editor/operators.cpp)

## 首轮深审结果

**原生反射的生成与注册**，从 `ACLASS`、`ASTRUCT`、`AFUNCTION` 声明，经 `atelier_reflect`、`NativeRegistration`，追到 Runtime 发布类型后第一次按元数据读字段、校验对象或调用方法。

| 业务样本 | Release 公开入口验证 |
| --- | --- |
| `world.BodySettings` | 默认值通过。`material.restitution = 2` 在值入口报告 `material.restitution`，在派生的 `CollisionComponent` 上报告 `body.material.restitution`。碰撞忽略位和重叠位相交时报告整条 `body` 规则。两类错误同时存在时先报告字段硬边界。[业务校验器](../../modules/engine/world/src/objects.cpp) |
| `CameraComponent` | 两段真实基类偏移均为 0，`vertical_fov` 偏移为 32。Runtime 写 60，JIT 增至 67，C++ 和 Runtime 均读回 67。另用非虚继承、实际基类偏移为 8 的类型验证直接偏移和回调寻址，两条路径均读写了正确字段。[相机声明](../../modules/engine/presentation/include/presentation/presentation.hpp) |
| `world.Component::translate_owner` | 在 12 次 World tick 中分别走 C++、Runtime 标量调用、Runtime 字段实参和 Source JIT。传 1、2、3 个实参，四条路径提交后的位移分别为 `(3,0,0)`、`(3,4,0)`、`(3,4,5)`。方法是私有的，直接 C++ 对照仅在临时调用方关闭访问检查。另以生成类型核对 `const float&`、`const enum&` 默认实参，四条路径的省略值和显式覆盖结果一致。[方法声明](../../modules/engine/world/include/world/world.hpp) |

审查使用临时目录和工程的 Release 产物复现上述问题。修复后的 dev 与 release 均通过 Runtime、World、Source、JIT、Physics、工程场景、编辑器文档、工程集成、材质资源及 UI 端到端验收。自定义实体、嵌套组件记录和内建相机均已验证容错恢复后的保存与严格重开。两种配置也分别验证了生成器与手写入口拒绝按值校验器、紧凑基类拒绝、自然对齐为 16 的基类可生成和编译，以及 UI 事件分派和绑定状态更新。[反射契约](../reflection.md) · [运行时优化的历史记录](runtime-and-reflection-audit.md)

## 第二轮深审 · 声明式 UI 事件与属性绑定

以公开入口 `Module::compile → Instance::create → event/update → commit_controls` 检查编辑器源码中的 44 个组件，共 344 个声明节点、1,408 个属性绑定、8 个本地事件。最大的 SceneViewport 有 231 个绑定、零个本地事件。WorldPanel 有 123 个绑定、6 个本地事件，其他两个事件分别属于 Position 和 PropertyGroup。八个处理器均为单条赋值。工程初始 HUD 模板也没有本地事件。[编辑器界面](../../assets/editor.ui.source) · [工程模板](../../modules/editor/core/src/project_code.cpp)

**生成器对照。** Ryzen 9 3950X，锁定的 Clang 23.1.5，Release。只更换声明生成器，各版本链接同一份 Runtime、Source、JIT、UI 库，定型调用入口相同。生成器用临时目标构建，固定同一个逻辑处理器，打乱版本顺序取中位数。全量编译指当前进程首次编译这份 Module，已经预读文件并运行过一次 lowering。点击计时截至控件描述提交，不含设备输入、布局与绘制。界面类型和处理器来自编辑器原文件，输入与条目数量由测量程序给定。

| 工作负载 | 旧版逐项函数 + 顺序判断 | 当前内联正文 + 平衡树 | 内联正文 + 平铺判断 |
| --- | ---: | ---: | ---: |
| 全量编译 44 个组件（9 轮） | 2,163 ms | 1,164 ms | 1,092 ms |
| 合计生成 Source 字节数 | 204,062 | 159,293 | 159,262 |
| Position 折叠并提交，2 个绑定（9 轮） | 1.606 µs | 1.606 µs | 1.612 µs |
| WorldPanel 切换 Lighting / Performance 并提交，14 个绑定（9 轮） | 7.754 µs | 7.615 µs | 7.722 µs |
| SceneViewport 只刷新 FPS 文字，1 个绑定（7 轮） | 1.737 µs | 1.700 µs | 1.755 µs |
| SceneViewport 切换 compact，5 个绑定（7 轮） | 3.981 µs | 3.892 µs | 3.981 µs |

单独重复调用事件、保持状态相同时，Position 的旧版/当前入口耗时 131/140 ns；WorldPanel 后段标签为 133/136 ns。合成组件含 256 个兄弟绑定，在首、中、末三个事件间按固定伪随机序列选择并写 state，7 轮分别为旧版 230 ns、当前 255 ns、平铺 233 ns。大组件的这组计时包含事件查找、Runtime 调用与 state 写入，尚不能从中推算单条 CPU 分支的成本。旧版到当前的主要可见收益是编译工作量；平铺对照同样保留了这项收益。

**业务正文与生成树的边界。** 给末尾事件加入显式嵌套代码块，通过公开编译入口二分找到可接受的最大块数。0/63/255 个兄弟绑定时，旧版均为 63 层，当前分别为 62/56/54 层，平铺内联分别为 62/62/62 层。额外尝试只把事件正文提成函数，三组均恢复到 63 层。在平衡树中提取事件后，全量编译为 1,177 ms；固定一次正确分派并不要求给每个属性都生成函数。调整此处时应保留真实事件、绑定和构造操作共用一个对外入口。[编译器嵌套检查](../../modules/execution/language/src/source.cpp)

**Repeat 局部更新复测。** 对同一份真实 WorldPanel 的 lighting 输入发布不可变 PropertyGroup 列表，每组的属性列表为空，反复折叠、展开末组。沿用审查时的 Release 测量程序和原始基线可执行文件，与修复版在同一逻辑处理器上交错运行 9 轮。每轮的计时从事件路由到控件描述提交。每次只求值两个绑定，均未创建实例。下表为各轮均值的中位数。

| 属性组数 | 修复前 | 修复后 | 加速比 |
| ---: | ---: | ---: | ---: |
| 1 | 3.506 µs | 2.996 µs | 1.17× |
| 32 | 53.307 µs | 3.779 µs | 14.11× |
| 128 | 207.482 µs | 6.601 µs | 31.43× |
| 256 | 443.294 µs | 10.959 µs | 40.45× |

在独立的单绑定 Checkbox 计数样本中，1/64/256 行的实例准备调用从 2/65/257 次降为 2/2/2 次。首次事件查找仍分别访问 2/65/257 个实例。256 组用于观察规模增长，计时不含 Session 布局与绘制。dev、release 的 `ui_acceptance` 经真实 Source/JIT 与嵌套 Repeat 验证同批多行编辑、重排保留状态、操作目标与控件别名换绑、拒绝后的控件与状态恢复，以及删除再插入的生命周期。Release 编辑器窗口验收通过启动场景、编辑/保存/撤销、接受帧和 Play 关闭。

**热更解析复测。** 基线为 `328b63f3`。两版在同一 Ryzen 9 3950X 逻辑处理器上使用相同 Release 依赖与 [editor.ui.source](../../assets/editor.ui.source)，打乱前后版本顺序，交错运行 9 轮。计时范围为公开的 `Module::compile`，每次都检查实际编译和复用数。全部复用时每轮交替移动最后一个组件的位置 30 次，单组件修改时交替更换其标签文字 8 次。下表取各轮均值的中位数。

| 场景 | 编译 / 复用 | 修复前 | 修复后 |
| --- | ---: | ---: | ---: |
| 只改注释与空白 | 0 / 44 | 6.16 ms | 1.91 ms |
| 修改最后的 ProjectOutput | 1 / 43 | 60.46 ms | 56.23 ms |
| 首次编译整份文件 | 44 / 0 | 1,166.79 ms | 1,155.51 ms |

两种热更各省约 4.2 ms。首次全编译两组的四分位区间重叠。dev 和 release 均将原文件、移动正文后的版本、修改标签后的版本与基线逐组件对照；三份版本各自的 44 份生成 Source 和全部源码映射均逐字节相同。原文件共 159,293 字节生成代码、24,495 条映射。通过 `LiveDocument::poll` 热更磁盘副本，将最后一个组件挂入 UI Session。未知 Source 标识符先报第 959 行第 64 列，移动后报第 960 行第 14 列；同组件的未知 UI 属性报第 960 行第 9 列，均与作者文件吻合。错误版本保留上一接受帧，修正后原实例显示新标签。

**其他原型与交互边界。** 临时组合采用游标、平铺判断和独立事件函数，7 轮全量编译 1,088 ms、全复用 1.92 ms，同批原实现为 1,156 ms、6.24 ms；在 256 个兄弟绑定的混合事件样本，组合从 255 ns 增至 266 ns。真实 Checkbox 在同一接受帧处理两组指针按下/释放时，会两次发出 `true`，各提交一帧则发出 `true, false`；两次动作均送达，符合当前按接受帧选中值生成请求的设计。是否需要积累帧间意图属于输入语义取舍。[Session 激活](../../modules/gui/ui/src/detail/session_popup.inl) · [UI 契约](../modules/editor/declarative-ui.md)

## 声明式 UI 与编译器的分批定向分析

| 顺序与责任 | 已确认的事实 | 下一步分析的验收点 |
| --- | --- | --- |
| 1 · 声明式 UI 的重编范围 | 同一 Runtime、同一已接受的 44 组件模块为基线。仅移动位置重编 0 个，改 ProjectOutput 的一个标签重编 1 个；改工具架专用 helper、改 rail 布局函数、或新增完全未使用的合法 helper，三者均重编 44 个。dev 与 release 的结果一致。UI 为每个组件附加整段文件级函数，并用整份生成 Source 判断复用。[UI 生成](../../modules/gui/ui_language/src/declaration.cpp) · [复用判断](../../modules/gui/ui_language/src/component.cpp) | 已从 44 个分派入口的 Source 调试位置核对实际可达函数，结果见下。下一轮用这些组件作为依赖失效范围的独立对照。 |
| 2 · 通用 Source 前端的验证与依赖 | 单入口 Source 编译会解析函数体，再由 `analyze_calls` 沿已解析的 `source_call` 计算入口可达集。JIT 用该集合筛选要生成的函数。实测未使用 helper 的函数体写错标识符，仍在作者文件第 1 行第 31 列拒绝。UI 的复用决定早于这一轮 Source 分析。[Source 前端](../../modules/execution/language/src/source.cpp) · [JIT 消费](../../modules/execution/jit/src/source_backend.cpp) | 分别测声明收集、函数体检查和代码生成成本；验证修改签名、删除和间接调用的失效范围。先确定现有 Source 依赖信息能否在复用判断前消费。 |
| 3 · UI 接入 Runtime/JIT 的执行边界 | 向 `Module::compile` 提供新源码名并移动 helper 后，同一份定型代码继续被复用。helper 内整数除零从旧源码 2:15 正确移到新源码 4:15，错误保留两版各自的内容哈希。磁盘热更验收另验证原生 state 写入失败定位在写语句，并回滚同事件之前的写入。[定型调用](../../modules/execution/source/src/program.cpp) · [UI 消费](../../modules/gui/ui_language/src/component.cpp) | 前两批的候选方案都须经过旧版本保活、新映射生效、拒绝候选及关闭回收的公开调用链。只有复现定位到执行边界的问题，才扩展到 ABI 或 LLVM 生成策略。 |

首批取证使用真实编辑器的各份生成 Source，以 `__ui_dispatch` 为入口编译，按固定版本的 `Lease::debug_sites()` 归集辅助函数。下表包含间接调用，仅用于核对依赖；调试构建的耗时未计入性能结果。

| 文件级函数 | 从哪些组件的分派入口可达 |
| --- | --- |
| `shelf_expanded_height` | ToolShelf |
| `rail_button_length`、`rail_button_thickness`、`rail_button_width`、`rail_button_height` | PaneDivider |
| `choose` | EditorShell、PaneDivider、PopupEntry、SceneViewport、ToolShelf |

其余 39 个组件没有调用上述辅助函数。这为下一批提出了具体目标：只改 `shelf_expanded_height` 的函数体，保留整文件类型检查与当版源码映射，同时让其余 43 个组件复用已有代码。新增、删除及签名变更须另行验证，已有版本的可达集不足以判定所有编辑的影响。

按现有公共入口对这次工具架函数体修改做 Release 分段采样。每轮先降低真实文件，再为 44 个组件分别请求、准备和发布定型入口。7 轮累计时间的中位数为 UI 降低 2.51 ms、`Program::request` 1.32 ms、`Program::prepare` 1,735 ms、发布并绑定 0.28 ms。准备阶段各轮为 1,295–2,005 ms，包含 Source 类型检查和 LLVM 编译；这次采样无法把其中的时间单独归给 LLVM。优先验证缩小进入准备阶段的组件集合，同时保留未使用函数的错误诊断。
