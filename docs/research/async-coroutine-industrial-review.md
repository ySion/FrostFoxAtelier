# async 协程落地：工业实现与文献复核

日期：2026-09-28。范围：v0.0.3 显式协程增量。性质：设计对照与后续建议，不是测试记录，也不代替专家团的代码审查。

本次重新查阅了官方文档、规范和官方源码。当前会话没有可调用的 Exa，实际使用 Web 搜索与一手页面；没有把仓库源码提交给搜索服务。在线文档可能随版本更新，其中 LLVM 页面当前描述开发版本，精确 intrinsic、pass 和 ABI 行为仍须以仓库锁定的 LLVM 版本及实际生成结果为准。

## 结论

**当前方向适合作为游戏引擎 Runtime 的首个协程核心增量。** 冷启动、显式挂起、单线程推进、确定性释放拥有的帧、挂起期间保留对象根与旧代码，共同形成了可解释的生命周期。工业资料没有给出必须改成有栈协程、自动线程池或立即引入 MLIR 的理由。这是针对项目约束的判断，不是性能排名。

需要明确三点：

1. `async` 是合适的语法选择，但当前提供的是宿主推进的协程，没有 C# `Task` 的等待与完成通知协议。普通 C# async 方法会在调用时推进到不能同步完成的等待点；本项目创建时不运行源码体。[Microsoft：TAP 控制流](https://learn.microsoft.com/en-us/dotnet/csharp/asynchronous-programming/task-asynchronous-programming-model)
2. 当前“运行中请求取消，恢复返回时取消压过成功结果”的规则是项目自己的确定性选择。Tokio 允许请求 abort 后任务先正常完成；不能把两者称为相同语义。[Tokio：任务取消](https://docs.rs/tokio/latest/tokio/task/index.html#cancellation)
3. 下一阶段最值得补的是等待关系和关停责任的明确表达；是否需要调度器、任务树或异步清理，应由真实调用链决定。当前文档已经写明 Runtime 必须比非终态帧活得更久，这项责任仍由宿主承担。[本项目核心接口](../../src/runtime/execution.hpp)

## 1. 本次对照所依据的当前契约

以下内容来自 [v0.0.3](../../LANGUAGE-v0.0.3.md) 和 [Coroutine 接口](../../src/runtime/execution.hpp)，没有把候选设计当作已实现能力。

| 维度 | 当前承诺 | 对后续设计的约束 |
| --- | --- | --- |
| 启动 | 创建复制标量参数并固定版本；首次 resume 执行源码 | 未来 await 需要说明谁负责启动被等待对象 |
| 挂起 | 仅源码显式 `yield;`，不产生中间值 | `yield` 当前不能代表计时器、事件、I/O 等待 |
| 结果 | 仅 completed 发布可选标量；失败、取消不发布 | 后续组合需继续区分完成值、执行错误和取消 |
| 线程 | 同一 Runtime 线程，宿主决定推进时机 | 外部完成通知不能直接假定可以执行 continuation |
| 清理 | 未启动或挂起实例取消时清理；运行中先记录请求 | 不抢占原生调用，也不保证无 yield 循环的取消时延 |
| 对象 | 持有引用保活，观察引用不保活；显式销毁仍可能发生 | 恢复后的对象访问必须重新遵守身份和代际校验 |
| 代码 | 帧恢复及销毁始终使用创建时版本 | 新版本发布与旧版本退休是不同事件 |
| 拥有者 | Runtime 活到所有非终态帧清理结束 | 无自动全局任务回收承诺；集成时要安排销毁顺序 |

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

本项目当前规则为：

| 取消发生时 | 当前结果 |
| --- | --- |
| ready / suspended | 同步清理并进入 cancelled |
| running，随后返回执行错误 | failed，保留执行错误 |
| running，随后正常挂起或完成 | cancelled，不提交完成结果 |
| 已经进入终态 | 不改变该终态 |

这套规则很明确，也比“请求可能来不及”的协议更便于宿主判断结果是否还会提交。其代价是：本次恢复可能已经产生副作用，最后仍报告 cancelled。因此 cancelled 的含义应继续保持为“没有提交协程完成结果”，不能解释成“工作从未执行”。依据是本项目 [v0.0.3 取消契约](../../LANGUAGE-v0.0.3.md#取消与代码版本)。

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

## 5. 可执行建议

### 现在值得做

1. **保持既有首增量边界。** 当前无栈、冷启动、单线程、固定版本的组合没有文献层面的根本冲突。本轮具体缺陷以代码专家复核为准，不为追齐各语言功能扩大范围。
2. **把关停顺序落到首个真实消费者。** 在同一线程停止新增实例，取消并释放现存实例，再销毁 Runtime 和所需后端。只需要覆盖这条真实端到端路径，当前不要求引入全局任务注册表。
3. **保留取消竞态的精确表达。** 现有错误优先、取消压过本次成功结果的规则应成为未来 await 的输入；不要直接套用 .NET/Tokio 的完成竞态直觉。
4. **把文献对照和证明分开。** 构建通过证明工具链可用，端到端验收证明覆盖的路径，源码审查验证不变量；工业实现只能支持设计取舍，不能替代这些证据。

### 出现下一项真实需求时做

| 需求 | 最小应补的核心契约 |
| --- | --- |
| 协程等待另一项操作 | 完成状态、错误/取消传播、唤醒注册与撤销、完成和注册竞态、恢复线程 |
| 一个拥有者管理多项并行工作 | 拥有关系、取消传播、等待所有清理结束；任务范围可表达这些责任，但不必进入游戏业务模块 |
| 跨线程 I/O 完成 | 通知回 Runtime 线程的边界；不要在完成线程直接恢复现有帧 |
| 用户可定义清理 | 取消和失败时执行顺序、能否挂起、清理错误与首因关系 |
| 大量长期挂起实例 | 先测帧分配、总保留字节、版本滞留和恢复延迟，再决定合并分配或其他优化 |
| 编辑器诊断困难 | 显示状态、创建版本和挂起源码位置；不必暴露 LLVM 帧布局 |

上述建议是后续入口条件，没有把 await、任务树或诊断工具记为本轮欠交付能力。

### 当前不建议做

- 为了贴近 C# 命名，连带改变启动时机或隐式引入线程池。
- 把任意深层原生暂停、有栈切换与当前无栈实现一起纳入首版。
- 把帧销毁当作外部 I/O、GPU 工作或副作用已经撤回的证明。
- 自动迁移旧帧到新代码，或在没有真实转换收益时接入 MLIR。
- 在没有测量的情况下宣称比 AngelScript、Lua、Rust、C# 更快、更省内存。

本次复核的结论是保留路线、明确语义、优先把拥有者关停责任接入真实消费者。长期弹性主要来自生命周期和调用契约能继续扩展，而不是首版包含尽可能多的协程机制。
