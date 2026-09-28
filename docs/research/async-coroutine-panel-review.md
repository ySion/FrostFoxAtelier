# async 协程落地：专家团审查

日期：2026-09-28。审查对象：基于 `c644ba5692fe2d67d1dda93953e227a50ba64f9f` 的审查时未提交工作区，包括未跟踪的 v0.0.3 规范与协程验收。此前的原生结果 ABI 诊断修复也在差异中，单独辨识其影响。下方保留原始发现，后续修复状态集中记录于“落地结果”。

后续源码执行链已[迁入正式模块](source-module-migration.md)。本文行号、结构范围和探针属于审查时快照；部分文件链接已指向迁移后的对应实现，现行模块归属见[结构说明](../runtime-structure.md)。

## 结论

**当前设计路线可以继续推进；审查确认的一个 P2 绑定缺口已完成修复和双配置验收。** 审查没有确认 P0/P1 问题。核心生命周期、GC 根、取消顺序、LLVM 帧释放及旧代码保活的链路成立；证据范围限于当前同线程、显式挂起、标量入口契约。

目前交付的是可运行的协程核心首增量。完整源码启动链仍在验收适配器内；`await`、完成通知、任务组合、统一关停和源码调试尚未交付。它们影响后续引擎可用性，但不违反本次明确的范围。

原审查阶段只新增报告与 `out/` 下的复现证据；用户随后授权了下述修复。两个阶段均未提交、推送或改变分支。

### 落地结果

- 同一个 `bind<R, Args...>(expected_execution)` 现在接受宿主预期执行种类，默认仍为同步。协程消费者显式传入 `FunctionExecution::coroutine`，沿既有 Runtime 签名解析与完整比较链校验，再通过返回的 `Entry::start()` 创建实例。
- 正式端到端验收通过绑定后的入口执行真实 async 源码，覆盖挂起、恢复、参数副本与完成结果；同时拒绝同步期望绑定协程、协程期望绑定同步函数，以及协程参数或结果类型不匹配。
- 移除每次恢复时诊断数组的 512 字节整块清零；DLL 仍负责在返回前初始化错误字符串。既有协程错误首因与 ABI 诊断场景一并通过，未测量性能收益。
- `cmake --build --preset dev --target jit_acceptance` 与 release 对应命令均退出 0，Business/JIT 验收通过；本次修改没有改变核心 Runtime 或构建链，未重复运行 RHI/UI 验收。

修复证据：[verification.json](../../out/review/async-bind-fix/verification.json)、[dev 日志](../../out/review/async-bind-fix/dev-jit-acceptance.log)、[release 日志](../../out/review/async-bind-fix/release-jit-acceptance.log)。原审查快照和失败探针保持为历史证据。省略执行种类的 `bind()` 现在仍按同步期望拒绝协程，这是保留的防误用行为；正向协程绑定由新增的显式参数表达。

### 审查分工与方法

六位独立子代理分别负责：规范与架构、Spec 与状态机、LLVM/Windows ABI、GC/代码版本及验收、性能与资源、工业实现与文献。主代理核对快照和历史证据，运行真实源码端到端探针，并对绑定发现安排跨专家复核。

使用 `code-review` 与 `research` 工作流；Standards、Spec 分别报告。当前工具环境未提供 Exa，实际使用网页搜索，关键依据来自官方文档、规范与项目自身源码。没有把第三方搜索摘要当作实现契约。

源文件快照：[source-manifest.json](../../out/review/async-coroutine-review/source-manifest.json)；本轮证据汇总：[review-evidence.json](../../out/review/async-coroutine-review/review-evidence.json)。交付前复核，20 个已审文件均未发生漂移。工业研究详见[独立研究报告](async-coroutine-industrial-review.md)。

## Standards

**硬性违反：0 项。** 依据根 AGENTS 与工程指南，当前模块划分具有实际职责：

- `Coroutine` 管理状态、执行准入、取消和结果。
- `CoroutineFrame` 隐藏编译帧恢复与资源释放。
- 既有 `BusinessProgram/Contract/Version` 继续管理源码声明、兼容发布和代码所有权。
- 对象身份、强根、弱引用与 GC 沿用同一 Runtime。
- DLL 的不透明句柄承担 CRT/LLVM 隔离；没有为协程增加另一个对象世界或业务调度器。

两项非阻断设计判断：

1. **复用边界。** `BusinessProgram` 仍定义于 `acceptance/jit/business.cpp:601`，只编入验收程序。首次正式引擎消费者接入时，应把现有实现迁入正式模块，并让验收调用同一路径；直接复制适配器会产生第二套入口。现阶段文档已经声明该范围，不要求本轮提前拆模块。
2. **资源说明需要完整。** 实例创建具有五个固定分配调用点，见下方成本表。两层恢复期间的共享保活分别保护核心拥有者和原始 DLL 句柄，具有真实职责，不应仅为了减少层数机械合并。

这两项是带前提的工程判断，不计为规范违反或实现缺陷。

## Spec

**审查时确认缺陷：1 项 P2；P0/P1：0 项。该 P2 已修复，原始分析保留如下。**

### P2（已修复）：匹配的 async 声明无法通过现有宿主 bind

位置：[business.cpp](../../acceptance/jit/business.cpp) 第 921–929 行。

`bind<R, Args...>()` 构造 `SignatureDefinition expected`，填写参数、结果和接收者，但没有填写新加入的执行种类。其默认值为 `synchronous`；第 928 行比较完整签名时，所有 `coroutine` 声明均被拒绝。

规范依据：[Runtime 发布协议](../runtime-model.md#单入口兼容方法体发布) 定义 `bind` 用于核对宿主期望并返回原 `Entry`；[v0.0.3](../../LANGUAGE-v0.0.3.md) 将执行种类纳入函数契约。返回的同一个 `Entry` 已提供 `start()`，但宿主无法向 `bind` 表达协程期望。

最小复现：

```text
async uint identity(uint value) { yield; return value; }
```

成功编译、发布后执行 `program.bind<std::uint32_t, std::uint32_t>()`，得到：

```text
Matching async bind rejected: Host binding does not match the published source signature.
Same declaration via entry().start(): PASS (result 7)
```

dev 与 release 均得到上述结果。探针直接包含当前真实业务适配器，链接现有 Runtime/JIT DLL，完成源码解析、LLVM 编译、发布、挂起、恢复和结果检查；没有使用 mock。[探针](../../out/review/async-coroutine-review/bind_probe.cpp)、[运行脚本](../../out/review/async-coroutine-review/run-bind-probe.ps1)、[dev 日志](../../out/review/async-coroutine-review/dev/run.log)、[release 日志](../../out/review/async-coroutine-review/release/run.log)。探针退出码 2 明确表示复现绑定缺陷。

影响：目前只能通过 `entry().start()` 绕过该冷端签名绑定。该路径自身仍有运行时参数检查，所以这里没有证实类型混淆或内存破坏。正式协程验收均使用 `entry().start()` 或 `Lease::start()`，因此遗漏了这一组合。

建议在同一接口增加可表达的期望执行种类，例如：

```cpp
template<class R, class... Args>
Result<Entry> bind(FunctionExecution expected_execution = FunctionExecution::synchronous) const;
```

将期望写入 `expected.execution` 后复用现有解析和完整比较。保留同步默认值；不要直接复制已发布声明的执行种类，否则宿主失去核验自身期望的机会。修复时增加一条真实 async 绑定并启动的端到端场景，并保留错误执行种类的拒绝行为。

## 生命周期、GC 与热更新

| 审查点 | 当前证据与判断 |
|---|---|
| 冷启动 | 创建时复制参数并固定版本，源码体到首次恢复才执行；LLVM 初始挂起位于参数读取和业务回调前 |
| 拒绝重入 | `Coroutine::resume` 先检查状态，再进入 `ExecutionScope`；借用/遍历禁入保留原状态，允许退出后重试 |
| 活动拥有者销毁 | 核心 Control 与 DLL State 在回调前各取得局部保活引用；回调之后不依赖可能已被销毁的外壳 |
| 取消优先级 | 运行中只记录取消请求；返回时先处理执行错误，再处理取消，最后处理正常挂起或完成；取消不提交结果 |
| 挂起强根 | 稳定 Frame 中的 Invocation 持有原 Runtime 的 ObjectRoot；`yield` 不提前结束词法生命期 |
| 弱引用与显式销毁 | 观察槽不保活；代际身份在实际访问时校验。已验收挂起后 receiver 被显式销毁 |
| 旧代码保活 | Frame → Version 与 DLL State → CompiledBusiness 覆盖恢复及 destroy；先销毁生成帧，再释放 Invocation，最后释放版本 |
| 兼容发布 | 暂停帧不占活动执行计数；新版本影响以后创建的实例。旧实例继续使用旧代码、绑定及清理函数 |
| 结果寿命 | 终态释放执行帧，保留标量结果或错误；结果查询不再依赖 Runtime |

关键实现：[execution.cpp](../../src/runtime/execution.cpp) 第 105–180 行、[business.cpp](../../acceptance/jit/business.cpp) 第 752–811 行、[business_backend.cpp](../../src/jit/source_backend.cpp) 第 745–756、940–966 行。

**需要保留的前提：** Runtime、宿主回调与 DLL 必须活到所有非终态帧清理完毕。当前没有自动关停登记，也没有跨线程保护。这与既有单线程 Runtime 契约一致；现有证据不能推广为任意销毁顺序均安全。

工业上的对象关联取消值得后续借鉴。UE5Coro 的 latent 模式借助引擎 latent action manager 管理目标 UObject 和协程寿命；它的普通 TCoroutine 又可以只是观察句柄。我们的 Coroutine 是拥有者，照搬句柄行为会改变析构语义。适合借鉴的是“谁负责取消、谁负责清理”的明确责任。[UE5Coro 自身文档](https://github.com/landelare/ue5coro/blob/master/Docs/Coroutine.md)

## LLVM 与 Windows ABI

LLVM 专家没有确认 P0–P2 实现缺陷，以下关键点与官方契约一致：

- 初始、普通、最终挂起的分支职责明确；最终挂起不能继续恢复。当前入口在终态提前拒绝恢复。
- `coro.destroy` 要求帧已挂起。运行中取消延期，实际销毁发生在生成恢复函数返回后。
- 分配与释放均采用 `max(coro.align, 2*sizeof(void*))`，满足 `coro.id(0)` 的对齐约定；分配为空在 `coro.begin` 前退出。
- LLVM 帧在 DLL 内配对分配与释放，成功前不覆盖输出句柄；生成代码不承接跨原生栈的 C++ 异常。
- CoroSplit 为拆分函数复制原函数属性；现有 Windows 插件登记整个 `.pdata` 并在释放内存前撤表。ORC 负责资源移除，何时允许移除由本项目的 Version/帧所有权决定。

依据：[LLVM Coroutines](https://llvm.org/docs/Coroutines.html)、[锁定版本 CoroSplit 源码](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.1/llvm/lib/Transforms/Coroutines/CoroSplit.cpp#L904)、[ORC 资源移除](https://llvm.org/docs/ORCv2.html#how-to-remove-code)、[Microsoft x64 展开信息](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64?view=msvc-170)、[Microsoft CRT 边界](https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries?view=msvc-170)。

一项有价值的证据补强：现有协程版本验收查询 ramp 地址的展开信息与代码页；尚未直接从首次 `yield` 后的原生回调展开 split resume 栈，同时销毁 raw DLL 外壳。以后修改这条 ABI/销毁链时，可复用现有抓栈工具，以一个组合端到端场景同时覆盖这两点。静态检查未发现错误，不把覆盖不足升级成已证实缺陷。

## 性能与资源

以下是源码成本分析，尚无协程吞吐、帧尺寸或尾延迟测量。

| 阶段 | 已知成本 | 判断 |
|---|---|---|
| 创建实例 | 五个固定分配调用点：宿主 Frame、Runtime Control、DLL State、DLL handle、LLVM frame | 有明确所有权理由；不具备“单次分配协程”的证据 |
| 条件存储 | 非空参数副本、超过内联容量的强根槽、弱槽、大参数调用缓冲，以及根表扩容 | 成本随源码布局与 Runtime 状态变化 |
| 首次恢复 | 两层局部 shared_ptr 保活、准入、虚调用/DLL/JIT 分派、参数进入生成局部状态 | 源码自身分配和 GC 成本另计 |
| 后续恢复 | 保活与准入、状态分派、恢复安全点 | 管理路径没有必需的新堆分配、JIT 符号查询或版本重解析 |
| 终结 | 显式清理强弱槽、释放帧；验收宿主额外检查根为空，RAII 兜底 | 清理成本与编译槽数相关 |
| 最后一个旧帧结束 | 可能同时承担旧 LLJIT 资源释放与 Windows 撤表 | 值得在实际热更场景观察尾延迟，目前未证实卡顿 |
| GC 根扫描 | 现有根表按历史槽位高水位遍历 | 大量任务退潮后可能增加整轮工作量，属于已有 GC 规模限制 |

审查发现的小冗余已处理：原 `business.cpp:762` 每次恢复先清零 512 字节诊断数组，而 DLL 入口 `business_backend.cpp:941` 已在所有返回路径之前清空首字节。现已省去宿主整块清零，保持错误字符串边界保证。没有测量收益，不计为严重性能问题。

参数 vector 在首次恢复后仍保留到实例销毁。当前参数只含标量，暂不为此改变捕获时点或引入内存池。若真实负载出现高频短任务，先测创建率、存活量、分配字节和终止尾延迟，再优化同一边界内的存储。

同步入口没有新增协程帧分配，也不经过 CoroEarly/CoroSplit/CoroCleanup。新增执行种类分支属于必要校验；同步 ABI 的 EntryFailure 参数来自此前诊断修复，应单独归因。

LLVM 的帧拆分和分配消除是不同能力。CoroElide 有调用与寿命前提，不能据“加一个 pass”推断跨 DLL/JIT 的长期拥有帧可以消除分配。[LLVM 转换说明](https://llvm.org/docs/Coroutines.html#coroutine-transformation-passes)

## 工业比较对本项目意味着什么

完整出处与比较表见[独立工业研究](async-coroutine-industrial-review.md)。本次综合判断：

1. **保留 `async` 关键词。** 需要明确本项目目前是冷启动、宿主恢复和显式 `yield`；关键词相近不要求复制普通 C# async Task 的启动行为。后续 `await` 应接入已有帧拥有者与错误规则。[C# TAP](https://learn.microsoft.com/en-us/dotnet/csharp/asynchronous-programming/task-asynchronous-programming-model)
2. **继续采用 LLVM 无栈降低。** 它与显式挂起、精确根槽、可控制代码版本相适配。原生深栈挂起若成为真实需求，需要另外评估借用、栈扫描与展开约束；Boost.Context 的控制转移能力不会自动解决本项目的对象和热更契约。[LLVM](https://llvm.org/docs/Coroutines.html)、[Boost.Context](https://www.boost.org/doc/libs/latest/libs/context/doc/html/context/overview.html)
3. **从引擎用法反推下一个核心原语。** Godot 的 `await` 可以等待信号或协程；对我们有价值的是通用完成通知、等待注册/撤销和恢复准入。计时器、动画和场景业务可以稍后作为消费者接入。[Godot 官方 GDScript](https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/gdscript_basics.html#awaiting-signals-or-coroutines)
4. **关停与任务归属比换后端更接近当前健壮性需求。** 首个长期宿主需要集中持有任务、在 Runtime 退出前取消并确认清理。是否把通用任务作用域加入核心，应由真实消费者决定；目前不必添加一个空调度框架。
5. **暂不引入 MLIR。** 当前待解决的绑定、任务归属和等待协议不是更换 IR 后端就能解决的问题。MLIR 在多级降低、领域优化等需求出现时再评估；其 Async dialect/runtime 的调度约定也不能直接等同本项目的同线程 Runtime。[MLIR Async dialect](https://mlir.llvm.org/docs/Dialects/AsyncDialect/)
6. **准确理解 UE-AngelScript 参照物。** Hazelight 的项目提供引擎整合、编辑器热更和脚本工作流；这些能力比当前协程增量广。AngelScript SDK 的协程 API、UE-AngelScript 集成和第三方 UE5Coro 是不同实现，不能相互代替能力证据。当前已核实资料不足以断言 UE-AngelScript 插件采用与我们相同的协程机制。[Hazelight 官方介绍](https://angelscript.hazelight.se/)

## 验收证据与建议顺序

### 本次采用的证据

- 历史 dev/release 总 `acceptance` 均通过，日志包含 Runtime、Business、JIT、RHI 和 UI 的预期 PASS；发生在 `async` 改名前。
- 改名后 dev/release `jit_acceptance` 均通过，包含四组协程场景；不能表述为当前快照重新执行过全部验收。
- 原 `verification.json` 的 20 个哈希中有 6 个已变化，均与关键词改名相关。当前审查另行固定快照，保留历史记录不重写。
- 本轮只运行了上述绑定端到端探针，两种配置均复现 P2。没有重新运行无关全套验收。

历史证据：[verification.json](../../out/review/coroutine-runtime/verification.json)、[dev 最新 JIT 日志](../../out/review/coroutine-runtime/dev-async-keyword.log)、[release 最新 JIT 日志](../../out/review/coroutine-runtime/release-async-keyword.log)。

### 实施优先级

1. **已完成：** 贯通同一 `bind` 的执行种类期望，扩展真实源码端到端验收并通过双配置验证；同时移除诊断数组冗余清零。
2. **第一个正式宿主接入前：** 迁移现有适配器到可复用模块，确定集中拥有/关停顺序；验收使用正式入口。
3. **需要等待外部完成时：** 设计通用等待、撤销注册、错误/取消传播和同线程恢复协议，再实现 `await`。避免每类引擎服务自建一套任务状态。
4. **有真实规模数据后：** 评估帧分配、根表高水位与旧代码退休尾延迟。当前不引入对象池、跨线程调度、原生深栈挂起或 MLIR。

Standards：0 项硬性违反；Spec：原有 1 项 P2（async 宿主绑定缺口）已修复。其他条目均已分别标明设计边界、性能推断或验收补强。
