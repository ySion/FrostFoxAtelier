# 协程实现的工业比较与取舍依据

资料日期：2026-09-28。保留当时两轮官方文档/源码调研的机制比较和决策理由；本次合并没有重新联网核对或进行性能实验。原首增量建议、重复的当前协议表及已完成实施清单已移除。

当前已经采用 LLVM switched-resume、宿主同线程恢复、冷启动和固定代码版本；完整语义只由[语言规范](../../LANGUAGE.md#显式协程)及[Runtime 协议](../runtime-model.md#协程核心协议)维护。正式宿主关停和资源基线已在[阶段记录](runtime-stage-closeout.md)落实，以下工业对照不另开实现任务。

选择 LLVM 转换是为了复用已有编译后端的帧布局与恢复/销毁拆分，减少自建状态转换的维护工作。它不自动提供 GC、取消或代码版本寿命，也不证明性能优于有栈实现。上游网页可能更新，具体 intrinsic、pass 与 ABI 仍须核对工具链锁和实际生成结果。
## 2. 工业机制比较

下表比较机制，不把语言、运行库和执行器视为同一层。AngelScript 一项仅描述 SDK 上下文能力，不能据此推断 UnrealEngine-AngelScript 插件已经暴露同样的协程接口。

| 方案 | 启动与推进 | 完成、错误、取消 | 对当前方案的启示 |
| --- | --- | --- | --- |
| C# / .NET TAP | async 方法可以同步推进；未完成的 await 注册后续执行；关键字本身不创建线程 | Task 承载完成和错误；CancellationToken 是合作请求 | 借鉴结果与通知协议；不能从 `async` 名字推导冷启动或线程策略。[TAP](https://learn.microsoft.com/en-us/dotnet/csharp/asynchronous-programming/task-asynchronous-programming-model)、[取消](https://learn.microsoft.com/en-us/dotnet/standard/threading/cancellation-in-managed-threads) |
| Rust Future / Tokio | Future 通过 poll 推进；Waker 提供再次推进通知；spawn 后独立任务另有生命周期 | 直接拥有的 Future 可通过 drop 停止推进；drop JoinHandle 却会分离任务；abort 完成仍需观察 | 本项目拥有者析构取消的行为更接近直接拥有状态；未来不能把观察句柄和拥有者混为一类。[Future](https://doc.rust-lang.org/std/future/trait.Future.html)、[select](https://tokio.rs/tokio/tutorial/select)、[JoinHandle](https://docs.rs/tokio/latest/tokio/task/struct.JoinHandle.html) |
| Lua 5.4 | create 后首次 resume 启动，可保留 Lua 调用栈 | resume 返回成功标记和值或错误；close 处理待关闭变量；不再恢复不等于确定性关闭 | 当前冷启动与宿主推进很常见；GC 与资源关闭仍应分别设计。[Lua 手册](https://www.lua.org/manual/5.4/manual.html#2.6) |
| Luau | resume/yield 控制协程；部分 C 调用和元方法边界不能 yield | close 使协程不可恢复并清空栈；不能套用 Lua 5.4 待关闭变量的保证 | “能够暂停”不自动带来完整清理语义。[Luau 标准库](https://luau.org/library/#coroutine-library)、[close RFC](https://rfcs.luau.org/function-coroutine-close.html) |
| AngelScript | asIScriptContext 保存执行上下文，宿主调用 Execute | 原生函数内 Suspend 要等该原生函数返回；完成、暂停、错误由状态区分 | 游戏脚本系统也明确约束原生边界；当前限制有工程依据。[上下文 API](https://www.angelcode.com/angelscript/sdk/docs/manual/classas_i_script_context.html)、[协程组织](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_adv_coroutine.html) |
| C++ / LLVM 无栈协程 | 初始挂起由协议决定；LLVM 保存跨挂起状态并拆分入口 | 正常恢复与销毁入口不同；运行中错误使用句柄可能产生未定义行为 | 复用转换能减少自建状态机工作，Runtime 仍须负责准入、GC、取消和代码寿命。[C++ 协程规范](https://eel.is/c++draft/dcl.fct.def.coroutine)、[LLVM 协程](https://llvm.org/docs/Coroutines.html#switched-resume-lowering) |
| Boost.Context 有栈上下文 | 保存栈和寄存器上下文，可保留深层调用状态 | 未完成 continuation 销毁涉及栈展开，不能吞掉 forced_unwind | 当普通深层原生调用必须暂停时有价值；栈、展开和原生借用都会成为核心契约。[概览](https://www.boost.org/doc/libs/latest/libs/context/doc/html/context/overview.html)、[展开规则](https://www.boost.org/doc/libs/latest/libs/context/doc/html/context/cc.html) |

## 3. 本次核对中最重要的语义差异

### 3.1 冷启动合理，但调用不等于调度

Rust 的 Future 本身需要 poll，且可以只是另一个已运行任务的结果通道。因此，“创建一个等待对象”和“启动工作”必须分别定义。[Rust Future](https://doc.rust-lang.org/std/future/trait.Future.html#runtime-characteristics)

本项目冷启动让创建时的副作用边界清楚，宿主可以先取得拥有者，再决定推进。它也带来一个未来必须解决的问题：如果增加 `await child`，child 是由父协程直接推进，还是由宿主调度，或者两者都有明确的不同 API？在这个问题明确前，不宜把现有 `Coroutine` 直接宣称为通用 `Task<T>`。

目前不需要改关键字。继续让当前文档和例子清楚呈现“创建—首次恢复—挂起—完成”，即可避免使用者把熟悉的 C# 启动时机带入本语言。

### 3.2 取消请求、清理完成、外部操作结束是三件事

.NET 取消由监听者自行响应；Tokio abort 发出请求后，需要等待任务结束才能确认取消已经完成。Tokio 还明确说明，已运行的 spawn_blocking 任务不能靠 abort 停止。[.NET 取消](https://learn.microsoft.com/en-us/dotnet/standard/threading/cancellation-in-managed-threads)、[Tokio 取消](https://docs.rs/tokio/latest/tokio/task/index.html#cancellation)

本项目取消优先级由[协程核心协议](../runtime-model.md#协程核心协议)统一维护；取消不撤销已经发生的业务副作用。

未来若允许帧持有文件、GPU 操作或网络请求，帧清理只能证明释放了本地拥有关系。外部操作是否停止、缓冲区何时可复用，要由该操作协议确认。Tokio 的优雅关停示例也把取消通知与等待所有工作结束分开处理。[Tokio 优雅关停](https://tokio.rs/tokio/topics/shutdown)

### 3.3 确定性清理有价值，但当前不承诺执行用户清理代码

Lua 5.4 对挂起后不再恢复或错误终止的协程，并不自动保证所有待关闭变量得到关闭；Luau 则没有 Lua 5.4 的这套变量机制。[Lua 待关闭变量](https://www.lua.org/manual/5.4/manual.html#3.3.8)、[Luau close RFC](https://rfcs.luau.org/function-coroutine-close.html)

本项目 `CoroutineFrame` 析构明确只释放根、帧和代码，不恢复用户代码、不挂起、不抛错。这个限制使当前 cancel 的同步完成承诺成立。以后若加入 `defer`、`finally`、记录析构或异步资源释放，必须先确定：取消会运行哪些清理、清理能否失败、是否允许等待，以及首因错误如何保留。不能只在语法层增加这些能力，再沿用当前析构限制。

这不是要求现在实现一套异常展开系统。当前语言能力有限，这个边界是可接受的；需要把它作为新增资源语义时的入口条件。

### 3.4 对象存活和代码存活必须同时成立

LLVM switched-resume 帧保存恢复与销毁入口；ORC 的 ResourceTracker 提供代码移除机制。由这两点推导，宿主必须保证帧仍需要的入口未被移除，LLVM/ORC 本身不会替本项目建立 Runtime 拥有关系。[LLVM switched-resume](https://llvm.org/docs/Coroutines.html#switched-resume-lowering)、[ORC 移除代码](https://llvm.org/docs/ORCv2.html#how-to-remove-code)

本项目采用旧帧固定版本，而非迁移挂起点，是合理且保守的选择。AngelScript 官方也指出，若上下文调用栈上的函数发生变化，序列化的程序位置不能可靠恢复。[AngelScript 热重载](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_adv_dynamic_build.html)

需要同时区分四种寿命：

- 帧内强引用所保活的对象。
- Runtime 本身及其根注册机制。
- 帧对应的生成代码、销毁入口与展开信息。
- 代码所调用的原生回调、DLL 及回调用户数据。

保留旧生成代码只解决其中一项。当前 Runtime 必须活过非终态帧的声明已经覆盖了关键前提；接入真实宿主时，应让实际拥有者按顺序关停。此处是集成责任和验收重点，不据此声称现有实现发生了悬空访问。

### 3.5 长期健壮性不要求现在保留任意原生栈

Lua 的 C API 需要 continuation 才能处理跨指定 C 边界的 yield；AngelScript 的原生内 Suspend 也等原生函数返回后才生效。普通原生栈可直接暂停并不是这些脚本系统共有的默认保证。[Lua C continuation](https://www.lua.org/manual/5.4/manual.html#4.5)、[AngelScript Suspend](https://www.angelcode.com/angelscript/sdk/docs/manual/classas_i_script_context.html)

有栈方案能承载深层同步调用，但同时延长该调用链内锁、借用和线程相关状态的寿命。Boost.Context 的栈与展开规则说明，这需要独立的资源和异常处理约束。[Boost.Context](https://www.boost.org/doc/libs/latest/libs/context/doc/html/context/cc.html)

对本项目的建议是保持明确原生边界。先让真正需要异步完成的原生能力返回可等待的操作描述，由 Runtime 线程推进；只有实际需求证明这种边界不足，才评估有栈互操作。不宜同时维持两套普通协程入口来预防假设需求。

## 4. MLIR 的真实收益和引入时机

MLIR 支持保留高层操作，经过多个 dialect 逐步降低到 LLVM。它可以成为以后表达类型化对象操作、批量计算或复杂控制流的中间表示；它不会自动给出本项目的 GC、版本和取消规则。[MLIR Toy：降低到 LLVM](https://mlir.llvm.org/docs/Tutorials/Toy/Ch-6/)

还应区分 MLIR 基础设施和其标准 async 运行库：async dialect 描述 token、value、依赖与执行；当前公开 AsyncRuntime 源码中，普通 execute 提交到线程池，部分 await-and-execute 路径则直接调用 continuation，或把它登记到完成通知中。这不提供本项目所需的固定 Runtime 线程保证。[MLIR async dialect](https://mlir.llvm.org/docs/Dialects/AsyncDialect/)、[AsyncRuntime 源码](https://mlir.llvm.org/doxygen/AsyncRuntime_8cpp_source.html)

| 选择 | 适用条件 | 本次判断 |
| --- | --- | --- |
| 继续现有 LLVM 协程降低 | 当前显式挂起和少量类型已经能清楚表达 | 继续使用；升级 LLVM 时核对协程转换和生成结果 |
| 将来引入 MLIR，自定义语言操作及降低 | 多项真实编译转换需要共享高层类型和控制流信息 | 届时以具体转换收益评估，不因项目名字含 async 就接入 |
| 直接采用 MLIR 标准 async 运行库 | 接受其执行与完成通知模型，或明确改造调度适配 | 当前缺乏收益依据；会增加线程与生命周期适配工作 |

上表是设计判断，没有吞吐、延迟、帧大小或编译耗时测量支持。采用 LLVM 也不保证零分配；更换为 MLIR 更不能单独证明性能会提高。


## 补充原始来源

- [await 表达式](https://eel.is/c++draft/expr.await)
- [Clang 生命周期检查](https://clang.llvm.org/extra/clang-tidy/checks/cppcoreguidelines/avoid-capturing-lambda-coroutines.html)
- [Rust Reference：async blocks](https://doc.rust-lang.org/reference/expressions/block-expr.html#async-blocks)
- [Rust Pin](https://doc.rust-lang.org/std/pin/index.html)
- [.NET 实现说明](https://devblogs.microsoft.com/dotnet/how-async-await-really-works/)
- [协程关闭](https://www.lua.org/manual/5.4/manual.html#pdf-coroutine.close)
- [Luau C API](https://luau.org/api/)
- [Boost protected_fixedsize_stack](https://www.boost.org/doc/libs/latest/libs/context/doc/html/context/stack/protected_fixedsize.html)
- [UE5Coro 自身文档](https://github.com/landelare/ue5coro/blob/master/Docs/Coroutine.md)
- [锁定版本 CoroSplit 源码](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.1/llvm/lib/Transforms/Coroutines/CoroSplit.cpp#L904)
- [Microsoft x64 展开信息](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64?view=msvc-170)
- [Microsoft CRT 边界](https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries?view=msvc-170)
- [LLVM 转换说明](https://llvm.org/docs/Coroutines.html#coroutine-transformation-passes)
- [Godot 官方 GDScript](https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/gdscript_basics.html#awaiting-signals-or-coroutines)
- [Hazelight 官方介绍](https://angelscript.hazelight.se/)
