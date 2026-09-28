# 协程的工业实现与 Runtime 首增量建议

日期：2026-09-28。状态：研究与候选契约，**不是已实现能力或验收记录**。

后续实施裁决见第 6 节；当前行为与限制由[Runtime 协程协议](../runtime-model.md#协程核心协议)维护。

范围：挂起状态、内存与所有权、原生调用边界、取消清理、调度线程、代码寿命。只使用规范、项目官方文档和实现者资料；没有外发本仓库源码，没有运行性能测量。业务例子用于判断核心契约，本研究不引入业务调度器。

## 结论

协程应当现在进入 Runtime 设计，因为挂起会把局部状态、引用和代码寿命延长到一次同步调用之外。工业实现没有一个在所有指标上占优：显式无栈状态便于控制保存内容；有栈上下文可以保留深层同步调用；VM 上下文便于检查脚本调用栈。它们分别把复杂度放在编译转换、栈资源或虚拟机里。

结合本项目当前单线程、机器码 JIT、共享 GC 和代码租约，建议先验证**编译器生成可恢复状态，Runtime 管理帧和生命期，宿主显式恢复**。这是按现有约束作出的工程判断，不是性能排名，也不把 LLVM 协程转换设为唯一实现方式。

## 1. 先分清四种责任

| 责任 | 应回答的问题 |
| --- | --- |
| 语言语义 | 哪些操作可挂起；局部值何时结束生命；完成、错误、取消分别意味着什么 |
| 编译器 | 哪些状态必须保存；怎样生成恢复、清理与调试信息 |
| 核心 Runtime | 谁拥有帧；引用如何进入对象图；如何拒绝非法恢复；代码何时可以退休 |
| 宿主/运行库 | 何时恢复、是否并发、等待什么、是否做超时和任务树 |

不能从一种语言的 `async` 语法直接推导线程模型、取消策略或堆分配次数。下面把语言与特定实现分开。

## 2. 工业方案比较

### C++ 协程语义与 LLVM 无栈转换

C++ 的协程状态保存跨挂起所需的参数、局部状态及 promise；恢复、最终挂起和销毁是不同操作。等待对象参与是否挂起及如何恢复的协议。它能表达不同运行库策略，但调用者必须遵守句柄和对象寿命，不能把任意普通 C++ 调用栈自动变成可恢复状态。[C++ 工作草案：协程定义](https://eel.is/c++draft/dcl.fct.def.coroutine)、[await 表达式](https://eel.is/c++draft/expr.await)

LLVM 提供帧和入口拆分；常见 switched-resume 形式生成初始、恢复、销毁入口，并允许部分分配消除。它是降低基础设施，不负责引擎 GC、调度、取消策略和代码版本。文档明确提示跨 LLVM 版本兼容性不保证。[LLVM Coroutines](https://llvm.org/docs/Coroutines.html)

- **收益**：帧内容由编译器掌握，正常恢复与清理可以共用类型和生命周期分析；能延续机器码执行路线。
- **代价**：借用地址可能越过原拥有者寿命；编译器、ABI 和代码退休必须一起管理。分配能否消除依赖具体形态与优化，不能预先承诺零分配。Clang 的协程 lambda 生命周期检查说明，语法支持本身不能消除悬空捕获。[Clang 生命周期检查](https://clang.llvm.org/extra/clang-tidy/checks/cppcoreguidelines/avoid-capturing-lambda-coroutines.html)

### Rust `Future`：状态值、`poll`、所有权

Rust 的 async 表达式产生 Future，实际布局不属于语言承诺；rustc 生成的形态可以理解为按 await 状态保存恢复所需数据。状态可以组合，但大小会随捕获和嵌套状态增长。[Rust Reference：async blocks](https://doc.rust-lang.org/reference/expressions/block-expr.html#async-blocks)

`Future::poll` 返回 Ready 或 Pending，执行器通过 Waker 得到再次推进的机会。Future 本身不会自动运行；返回 Pending 不等于系统已经为它安排线程。`poll` 应迅速返回，完成后再次 poll 的行为也没有统一可用保证。[Rust Future](https://doc.rust-lang.org/std/future/trait.Future.html)

Pin 的价值是为可能自引用的状态提供地址稳定契约，避免每一层异步组合都强制单独分配。代价是实现者必须理解哪些状态不可移动，不能把“装在堆上”等同于“具有不可移动保证”。[Rust Pin](https://doc.rust-lang.org/std/pin/index.html)

丢弃直接拥有的 Future 会销毁其状态，这适合确定性撤销等待；但它不会自动撤回已经发生的副作用，也不保证取消另一个独立任务。Tokio 的 `select!` 通过丢弃未选中的分支取消该 Future；**丢弃 Tokio JoinHandle 则分离任务，任务继续运行**。这两件事必须区分。[Tokio select](https://tokio.rs/tokio/tutorial/select)、[Tokio JoinHandle](https://docs.rs/tokio/latest/tokio/task/struct.JoinHandle.html)

### C# / .NET：状态机、Task 与合作取消

C# 的 await 在未完成时把后续代码交给 continuation；挂起本身不会退出词法作用域或执行 finally，也不会自动创建额外线程。线程选择还取决于等待实现和同步上下文。[Microsoft TAP 文档](https://learn.microsoft.com/en-us/dotnet/csharp/asynchronous-programming/task-asynchronous-programming-model)

Microsoft 的实现说明展示了生成状态机和首次异步完成时的堆承载；同步完成存在避免状态机堆分配的路径。这是所述 .NET builder 的实现策略，不能推广为所有 awaitable 或所有版本的固定分配次数。文章也明确：`async` 修饰符本身不是方法签名的一部分，Task 等结果协议才对调用方可见。[.NET 实现说明](https://devblogs.microsoft.com/dotnet/how-async-await-really-works/)

CancellationToken 表达请求，监听者决定如何响应；请求与停止不是同一时刻。收益是可以经过正常错误/清理路径退出；代价是未合作的代码不响应取消，已发出的外部操作也需要自己的协议。[.NET 合作取消](https://learn.microsoft.com/en-us/dotnet/standard/threading/cancellation-in-managed-threads)

### Lua 5.4 / Luau：可挂起的语言调用栈

Lua 协程可以在嵌套 Lua 调用中 yield，但 C 栈不会自动保留：跨 C API 的 yield 需要 `lua_yieldk/lua_callk/lua_pcallk` continuation。Lua 5.4 的 `coroutine.close` 会关闭待关闭变量；仅仅不再恢复协程，不保证这些变量得到关闭。[Lua 5.4：C yield](https://www.lua.org/manual/5.4/manual.html#4.5)、[协程关闭](https://www.lua.org/manual/5.4/manual.html#pdf-coroutine.close)

Luau 的 C API 同样要求原生函数返回 `lua_yield` 的结果，yield 调用本身不会像保存原生栈那样立即转移并在原位置继续。它的 close RFC 特别说明不支持 Lua 5.4 的待关闭变量。不能把两个项目的清理保证混用。[Luau C API](https://luau.org/api/)、[Luau close RFC](https://rfcs.luau.org/function-coroutine-close.html)

- **收益**：语言调用栈可由上下文管理，宿主驱动模型直观。
- **代价**：可达的挂起上下文仍占用栈和值，GC 回收内存与确定性释放外部资源不同；原生边界要额外 continuation。对本项目而言，借鉴上下文契约有价值，迁入解释执行体系没有本轮依据。

### AngelScript：上下文提供机制，宿主组织协程

AngelScript 的协程示例以每条执行路径的 `asIScriptContext` 保存调用栈，宿主在上下文之间推进；协程组织是应用/附加库提供的能力。[AngelScript Co-routines](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_adv_coroutine.html)

`Execute` 区分完成、挂起、失败；原生函数内调用 `Suspend` 或 `Abort`，效果在该原生函数返回后发生。`Unprepare` 用于释放上下文资源，但活动或挂起态会拒绝；终止执行与释放上下文不能混为一谈。[AngelScript Context API](https://www.angelcode.com/angelscript/sdk/docs/manual/classas_i_script_context.html)

其优势是把执行路径作为可检查的上下文；代价是每条存活执行路径需要保留调用栈，并维护宿主的上下文寿命。热更新也有明确限制：官方指出，若上下文栈上的函数已修改，序列化的程序位置不能可靠恢复。这支持“首增量固定旧帧代码版本”，不支持自动迁移挂起点。[AngelScript 动态编译与热更新](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_adv_dynamic_build.html)

### Boost.Context：有栈对照

Boost.Context 保存 ABI 要求的寄存器与栈指针，并管理独立栈。它可以保留深层原生调用中的局部状态，适合需要直接挂起同步调用链的场景。相应成本是栈空间、平台切换实现和展开规则；未完成 continuation 的销毁依赖展开，代码不能吞掉内部 `forced_unwind`。[Boost.Context continuation](https://www.boost.org/doc/libs/latest/libs/context/doc/html/context/cc.html)

受保护的固定栈可用保护页发现越界，但有额外虚拟内存操作成本；这是一种栈分配选择，不能推导所有有栈方案的统一开销。[Boost protected_fixedsize_stack](https://www.boost.org/doc/libs/latest/libs/context/doc/html/context/stack/protected_fixedsize.html)

**对本项目的判断**：有栈是有价值的备选，尤其在“普通深层原生函数必须挂起”成为明确需求时。当前没有这个需求，不值得仅为语法方便立即引入另一种栈与展开生命期。此判断不声称无栈在真实负载中更快。

## 3. 候选核心契约

以下是基于上述资料和本项目现有约束的设计建议，需要由本轮实施确认；不作为已生效 ABI。

| 主题 | 建议 |
| --- | --- |
| 一份状态 | 创建、可恢复、正在执行、完成、失败、已取消要能区分；运行中重入、终态恢复给出确定错误 |
| 所有权 | 首增量用单一拥有者管理帧；明确移动、关闭和销毁责任。Runtime 必须活到其帧清理完成 |
| 执行活动 | 只在实际运行/清理期间持有活动执行范围；挂起帧单独持有状态与代码，不能永久阻塞宿主安全点 |
| 对象根 | 挂起帧的持有引用加入现有 GC；观察引用保留身份但不保活。不建立协程专用的第二套对象管理 |
| 稳定存储 | `ExecutionFrame` 所借用的根槽必须属于可跨挂起的拥有者，不能借用已返回调用的栈槽；注册之后不得因扩容/移动失效 |
| 原生边界 | 首增量只允许生成代码的显式挂起点；原生函数完成返回后再挂起。普通原生借用和临时参数区不能跨点保留 |
| 恢复时检查 | 挂起期间对象可以被允许的显式销毁；持有根不等于不可销毁。恢复后的访问仍遵守代际及访问校验 |
| 清理 | 正常完成、失败、取消、未完成句柄销毁都只清理一次；按已经构造的局部状态清理，保留现有首因规则 |
| 取消 | 首增量可限定在非运行状态取消并同步清理；运行态拒绝或仅记录合作请求，必须选定一种，不假装抢占原生代码 |
| 结果 | 挂起状态与失败状态分离；只在成功完成时提交最终结果，不让取消/失败污染调用方结果 |
| 发布与退休 | 帧固定持有创建它的版本，恢复和销毁都用该版本；新调用可选新版本；旧帧未清理前不回收旧代码、相关元数据或展开信息 |
| 调度 | 首增量只暴露推进与清理机制，由同一 Runtime 线程显式驱动；不内建 Tick、计时器、任务池或业务事件 |

### 需要保留的弹性

- “可挂起”需要体现在调用/发布契约中，但不一定机械照搬一个 `async` 关键字或只加布尔字段。可以通过独立执行种类或返回的任务协议表达，需与当前函数签名规则共同确定。
- 帧保存内容既受机器码后续使用约束，也受语言的引用存活期约束。不能只做 LLVM 活跃变量优化就静默缩短持有引用的词法生命期。
- 取消当前执行状态不会回滚已经发生的业务修改；将来需要异步取消确认的外部操作，应拥有独立操作对象。帧销毁不等于 GPU、文件或网络操作完成。
- 单线程并不消除挂起期间状态改变的风险；恢复后必须重新建立短期借用和不变量。不能把挂起前验证无限延长。

## 4. 首增量的可观察验收终点

首增量应通过真实源码编译与 JIT 运行验证，不以手写 C++ 状态机展示替代语言能力：

1. 源码在至少两个显式挂起点之间执行，保存标量并在恢复后给出正确最终结果；循环/分支中的挂起位置保持正确。
2. 挂起后执行宿主 GC，帧持有的对象仍可用；观察目标失效可检测；词法退出或取消后根正确撤销。
3. 完成、错误、取消和丢弃未完成实例都能清理；重复恢复/清理及运行中重入有明确结果。
4. 同一程序的两个实例保持独立局部状态、参数、错误和结果缓冲。
5. 旧实例挂起后发布新兼容代码，新实例运行新代码，旧实例恢复/清理仍使用旧代码；两者释放后旧版本可以退休。
6. 原生调用错误、结果契约违约和不允许的挂起位置保持现有诊断及输出规则。

以上覆盖的组合比“yield 后能打印下一行”更能证明核心完整。按项目规则在 dev/release 执行；本研究没有运行这些验收。帧字节数和分配次数可在实现中解释清楚，但吞吐、尾延迟与内存优势留待真实测量。

## 5. 实施前仍需确定

1. 第一版的语言语法、初次调用是立即执行还是返回待启动实例，以及最终结果读取方式。
2. 帧布局由显式语言状态转换还是 LLVM Coroutines 产生；二者应履行相同的 Runtime 合同。
3. 取消运行态的 API 行为、关闭失败怎样报告、Runtime 与程序宿主的销毁顺序。
4. 当前单入口的有限实现怎样拒绝尚未支持的嵌套挂起调用；不能静默把可挂起函数当同步函数。

这些点明确后即可做一个完整的核心增量。任务树、等待组合、跨线程恢复、抢占/预算中断、调试器体验、挂起帧序列化与跨版本迁移均不必捆绑到第一版。MLIR 是编译表示选择，不替代上述运行时契约；是否引入应另按本项目编译转换的实际收益判断。

## 6. 本次实施裁决与 MLIR 取舍

用户要求优先考虑长期健壮性后，首增量选择显式无栈协程，同一 Runtime 线程由宿主恢复。源码使用 `async` 和完整语句 `yield`；创建仅取得待启动实例。运行中取消记录请求，执行错误优先，否则取消阻止结果提交；宿主须在 Runtime 关停前清理全部非终态帧。具体协议与语法分别归当前运行时文档和 [v0.0.3](../../LANGUAGE-v0.0.3.md)。

后端选择 LLVM switched-resume 转换。已有 LLVM 23.1.1 SDK 提供协程 intrinsics、CoroEarly/CoroSplit/CoroCleanup 和对应库；编译器显式组织挂起/清理 CFG，LLVM 负责跨挂起值、帧构造和恢复/销毁拆分。选择依据是减少语言扩展后自建状态转换的维护面，不是未经测量的性能结论。[LLVM 协程转换](https://llvm.org/docs/Coroutines.html)

| 方案 | 本项目的实际收益 | 成本与局限 | 本轮选择 |
| --- | --- | --- | --- |
| 自行发射状态机 | 帧与恢复状态直接可控，当前语句级挂起可以保守保存局部 | 以后表达式挂起、嵌套调用和复杂清理需要持续扩展自己的转换 | 保留为实现候选，本轮未采用 |
| LLVM 协程转换 | 利用既有 SDK 的帧构造、状态保存和代码拆分 | 前端仍负责清理语义、GC 根、错误及运行时状态；须显式接入转换并验证结果 | 采用 |
| MLIR | 可用 dialect 表达高层语言操作，再逐级转换和复用分析 | 当前 SDK 无 MLIR；还需供应/锁定依赖、定义操作及转换；引擎语义仍由自己负责 | 本轮不引入 |

MLIR 的多层表示可以保留语言特定操作，再降低到 LLVM；这对以后更复杂的值、引用或批量计算转换可能有收益。[MLIR Toy 教程](https://mlir.llvm.org/docs/Tutorials/Toy/)、[Lowering to LLVM](https://mlir.llvm.org/docs/Tutorials/Toy/Ch-6/)

MLIR `async` dialect 的标准运行库包含线程池执行与等待后恢复，其官方实现可能在线程池或完成通知路径执行续体，不能直接当作当前引擎固定线程的调度实现。自定义 dialect/lowering 可以不使用它，但需要相应实现成本。[Async dialect](https://mlir.llvm.org/docs/Dialects/AsyncDialect/)、[AsyncRuntime 源码](https://mlir.llvm.org/doxygen/AsyncRuntime_8cpp_source.html)

代码退休继续由实例固定持有编译版本。ORC 的资源移除不会自动识别 Runtime 中挂起帧的恢复和销毁地址；相关代码及展开信息必须保留到帧清理结束。[ORC 资源管理](https://llvm.org/docs/ORCv2.html#how-to-remove-code)

本轮没有比较各方案的吞吐、尾延迟、帧大小分布或编译耗时，不能从采用 LLVM 推导性能领先。
