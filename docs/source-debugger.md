# Runtime / JIT 源码调试

当前支持精确源码断点、进入/跳过/跳出单步、只读调用栈与局部值、错误现场和受控终止。公共入口为 [`source/debugger.hpp`](../src/source/debugger.hpp)，执行仍使用现有 `Entry/Lease.invoke`、`start` 和 `Coroutine.resume`。

这是已经落地的调试核心。编辑器调试面板、快捷键及外部指令的共同接入方向由 [Editor Control](modules/editor/control.md#源码调试接入待实现)维护，目前仍处于方案阶段；各调用方将共享同一宿主调试会话。

## 暂停与执行合同

停点在 Runtime 所在线程同步调用处理器。**处理器返回前，原来的 `invoke/resume` 不返回**；原生栈、参数借用、GC 根和代码版本保持有效。处理器接收独立拥有的 `DebugSnapshot`，返回带本次 `stop_id` 的命令。

调试暂停时协程仍为 `CoroutineState::running`；只有源码 `yield` 才返回语言层面的 `suspended`。宿主可安排 Runtime 执行线程，并在处理器中接入通信；核心不创建 UI、消息循环或命令队列。外线程只能处理拥有的快照与命令，不能访问当前 Runtime 或 Debugger。通信断开时，适配器应返回继续或终止命令，结束等待。

## 编译和使用

通过普通请求选择 `CompileOptions{.debugging = true}`，与 `diagnostics`、`artifacts` 独立。未开启调试的版本收到 `Debugger*`，在参数构造、协程分配与源码副作用前返回 `unsupported_operation`。

```cpp
source::Debugger debugger([](const source::DebugSnapshot& stop) {
    // 宿主可在此处理快照并等待命令；示例逐个跳过源码语句。
    return stop.command(source::DebugAction::step_over);
});
auto result = entry.invoke(receiver, arguments, {1}, nullptr, &debugger);
```

第四个参数仍是可选 `Diagnostics*`；调试器位于其后。`ValueOutput` 调用和异步 `start` 同样在原有参数后追加。宿主继续使用返回的 Coroutine 控制恢复与取消。

- 默认在首次进入真实源码函数和首个运行错误时暂停，可通过 `DebugOptions` 分别关闭。
- 普通编译不生成调试帧、变量表和停点调用。调试编译保留可观察存储和函数调用，继续使用 LLVM 管线，承担明确的调试开销。
- 调试版本可省略会话正常执行。没有处理器的会话自动继续，不生成暂停快照。
- Debugger 副本观察同一会话。同一会话归一次同步调用或整个协程生命周期所有，活动期间复用返回 `busy`；终态后可复用。
- 活动调用持有会话。处理器内释放外部 Debugger 外壳仍安全；状态和历史快照不保活 Runtime 或机器码。

## 断点与单步

`Lease::debug_sites()` 返回该固定版本可绑定的原始文件、文件版本、行列和函数位置。`set_breakpoints()` 接受 `DebugBreakpoint{path, source_version, line, column}`：

- 文件与版本精确匹配；真实版本 `0` 合法，不能充当通配。
- 行列从 1 开始。`column == 0` 绑定该行所有可执行位置；非零列精确匹配。
- 调用前完成绑定，任一断点未绑定即拒绝执行，错误中保留请求位置。不会自动搬到附近行或新版本。
- 当前仅非活动会话可以修改断点；协程 ready/suspended 仍属活动会话，该轮执行使用开始时的绑定。

语句执行前发停点，块括号不单独停车。声明停点位于初始化前；每次实际循环条件都可停，同一行循环也能连续单步。项目导出包装函数不显示为源码帧。

| 命令 | 行为 |
| --- | --- |
| `continue_execution` | 运行到显式断点、首错或结束 |
| `step_into` | 下一个源码语句或函数入口，允许进入 helper |
| `step_over` | 跳过更深调用，停在当前动态帧的下一语句；若当前帧返回，则在调用方继续观察 |
| `step_out` | 等当前动态帧离开，在调用方的返回位置或后续语句停止 |
| `abort` | 阻止后续源码语句，沿既有清理和取消协议退出 |

单步使用动态调用实例身份，显式断点仍可打断 over/out。原生调用作为一次操作，不进入 C++。helper 返回停点可能仍在调用表达式，调用方的初始化/赋值尚未完成，变量状态如实显示。step 跨 `yield` 保留在同一协程，下次宿主 resume 后继续；不自动推进任务或跟随另一协程。

命令必须携带当前暂停编号。编号跨同一 Debugger 的多次调用保持单调，迟到的命令不能恢复下一次暂停。

## 值快照与只读保护

帧按最内层到最外层排列，包含动态 `activation_id`、源码位置及变量。每个变量标记 `available`、`uninitialized`、`out_of_scope` 或 `unavailable`；尚未初始化、未进入/已离开作用域、不支持的表示都不会被读取成伪造数值。

支持 bool、float/double、整数/枚举、数学分量、直接偏移记录字段、String 和 Array 的有界预览。引用只显示身份、强弱性质及当时的存活状态；不新增根、不获取持有引用、不沿对象图递归。不透明原生值明确显示不可读取。

快照没有可写地址，不调用用户复制构造、getter 或格式化函数；容器使用已有可信序列投影。默认限制 64 帧、256 个值、32 个展开子项、4 层嵌套及 4096 字节文本。值、展开项和文本预算分别由整份快照共享，受限时标记 `truncated`，包括被截断的名称和位置文本。完整停点身份使用 `Lease::debug_sites()` 查询。

处理器期间持有 `ExecutionInspection`，复用 Runtime 的回调和只读遍历准入，拒绝同一 Runtime 的 JIT 重入、可写原生借用、受检对象/值/引用/序列修改、新增根、GC 推进和代码发布。只读查询及既有外部根的 RAII 释放继续遵循原合同。该保护不拦截任意 C++ 地址写入；宿主仍须遵守调用期间的借用和 Runtime 外部寿命要求。

## 错误、终止和资源释放

正常运行错误在显式清理前暂停，错误点不可恢复失败的操作。首错只报告一次；后续包装传播、处理器异常、abort 或错误命令均不覆盖它。既有 finish 策略在用户值清理后执行，因此该阶段失败时，已销毁变量显示为作用域外。

普通停点的处理器异常、非法动作、迟到命令或快照失败会终止执行，本次会话不再重复调用出错处理器。仅使用 Debugger 时，返回错误也补充原始 `Error.source`，不要求同时启用 Diagnostics。

同步 abort 返回 `execution_aborted`，调试状态为 `cancelled`。异步 abort 通过 `CoroutineFrame::request_cancel()` 交由核心裁定取消；业务失败优先。此前副作用保留，不回滚、不提交本次调用结果，也不以异常或 longjmp 穿越 JIT 栈。

正常返回、失败和取消均解除生成的调试帧，清理期间不再暂停。async 调试表在 `coro.begin` 后、首次挂起前初始化，局部地址与可用状态保存在 LLVM 协程帧中；取消 ready 帧不会进入源码或伪造停点。

Program.close、热更新和旧 Lease 遵循原有版本保活契约。源码映射在准备发布时按请求的快照建立，保存的快照在代码与 Runtime 销毁后仍可读取。调试与[调用诊断](source-diagnostics.md)可同时使用：调用计数单列 `debug_hook`，执行计时包含暂停处理器等待。最终性能测量应使用未启用调试插桩的版本。

## 范围与验收

当前不提供任意机器指令暂停、C++ 单步、变量写入、表达式求值或反向执行。运行中的暂停/终止请求尚无独立控制句柄，`abort` 只在已有停点处理器返回时生效；不能据此承诺终止未命中停点的无限循环或抢占长时间原生调用。工业依据见[研究记录](research/source-debugger-research.md)。

[`acceptance/source/debugger.cpp`](../acceptance/source/debugger.cpp) 通过正式编译、调用和 GC 路径覆盖单步、精确版本断点、变量初始化/作用域、只读保护、快照寿命、首错、中止，以及 async 跨 yield 和旧版本清理。它纳入 `source_acceptance`，也可单独运行：

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target atelier_source_minimal
./out/dev/atelier_source_minimal.exe --debugger
```

发布配置使用相同入口，将 `dev` 替换为 `release`。

验收还覆盖跨 `yield` 的参数和局部值读取，确认调试表位于协程帧内；未传入 Debugger 的调用不复制调试元数据的共享指针。
