# 同线程源码调试核心研究

研究日期：2026-10-10。范围：同步源码编译为原生栈 JIT、async 使用 LLVM 协程、同 Runtime 单线程且可直接借用 C++ 对象。本文只判断源码 hook、局部变量与暂停契约，不提出另一套运行时或完整调试前端。

## 结论

**项目判断：语句 hook + 有类型的局部变量帧 + 同线程暂停回调，可以作为长期调试核心。** 关键条件是把暂停定义成“执行仍停留在 hook 回调内”，保持原生栈、既有根和执行准入；宿主在该回调中等命令或接通信，回调返回后继续。这个入口能够承载断点、单步和只读变量检查，不需要核心承担 UI、网络协议或线程调度。

这不是仅凭类比得出的做法：AngelScript 官方明确提供“在 line callback 内运行消息循环、返回回调恢复”的嵌入方式，官方 debugger addon 也在 `LineCallback` 内调用 `TakeCommands`。不过它还提供 VM 专属的 `Suspend → Execute 返回 → 再 Execute` 路径；**同步原生 JIT 不能因此承诺在任意语句暂停后返回原来的宿主入口，再无代价恢复原生栈。** [AngelScript 调试接口](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_debug.html)、[官方 debugger 源码](http://svn.code.sf.net/p/angelscript/code/trunk/sdk/add_on/debugger/debugger.cpp)

## 一手证据及其边界

| 官方事实 | 对项目的含义（推断） |
| --- | --- |
| AngelScript 的 line callback 按执行的脚本语句调用；addon 在回调中等待命令，并以调用栈深度实现 over/out，同时仍检查断点。[接口](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_debug.html)、[源码](http://svn.code.sf.net/p/angelscript/code/trunk/sdk/add_on/debugger/debugger.cpp) | 语句事件、执行动作、调用帧和局部变量足以形成嵌入式调试核心。长期实现应记录调用实例身份，避免只按深度判断递归、同深度另一调用或 async 恢复。 |
| AngelScript 暴露类型、变量地址和 `IsVarInScope`；addon 查找同名变量时逆序遍历并判断作用域。[Context API](https://www.angelcode.com/angelscript/sdk/docs/manual/classas_i_script_context.html)、[源码](http://svn.code.sf.net/p/angelscript/code/trunk/sdk/add_on/debugger/debugger.cpp) | 局部变量不能只是“函数全部栈槽”。需要稳定变量身份、词法作用域和初始化状态；尚未初始化及已析构的槽不能读取。 |
| Lua 的 line hook 在执行新行或向后跳转时触发，向后跳转可落在同一行；hook 执行期间禁止再次触发 hook。tail call 没有配对的 return 事件，且不保留普通调用栈层级。[Lua 5.4 手册：lua_sethook/lua_Hook/lua_getstack](https://www.lua.org/manual/5.4/manual.html#lua_sethook) | 停止点身份必须独立于显示行号；循环再次命中同一语句必须可停。纯深度计数需要明确优化、尾调用和重入边界。 |
| Lua hook yield 只允许 line/count 事件、零返回值，并有 continuation 限制；`lua_getlocal` 需要有效 activation record，读取当前活跃变量。[hook](https://www.lua.org/manual/5.4/manual.html#lua_Hook)、[getlocal](https://www.lua.org/manual/5.4/manual.html#lua_getlocal)、[官方实现](https://www.lua.org/source/5.4/ldebug.c.html) | 调试 hook 不自动获得任意位置保存与恢复原生栈的能力；回调内暂停是清楚、有限的契约。 |
| LLVM debug info 与优化共同变化。变量位置丢失时应报告 `optimized out`，不能沿用可能已过期的值；优化还可能合并或丢弃源码位置。[源码级调试](https://llvm.org/docs/SourceLevelDebugging.html)、[debug info 更新规则](https://llvm.org/docs/HowToUpdateDebugInfo.html) | LLVM debug metadata 不会自行变成进程内的有类型变量检查 API。可靠的调试编译模式需要显式可观察槽和语句 hook；优化构建只能承诺实际可观察的值与停止点。 |
| LLVM coroutine 仅在显式 suspend 点返回 caller；跨 suspend 存活的状态放入 coroutine frame，执行中的原生栈帧与该 frame 有区别；恢复尚未 suspend 的协程是未定义行为。[LLVM Coroutines](https://llvm.org/docs/Coroutines.html) | 当前运行中的 async 同样可在 hook 回调内暂停。不得在暂停回调中再次 resume/destroy 当前任务；所有挂起任务的局部变量及历史 await 调用链需要额外的权威布局和任务关系，不能从原生栈深度推导。 |

LLVM 网页为检索当日官方文档；项目锁文件使用 LLVM 23.1.1。这里引用的是语义边界，不声称其他版本的 debug-record API 与锁定版本逐项相同。

## 必须固定的核心不变量（项目建议）

1. **停止点语义。** 编译器为可执行语句分配稳定 source/statement 身份，定义为执行该语句之前停止。声明语句前变量尚未初始化；最后一条 return 语句前仍能查看活跃变量。空白行没有停止点；断点解析应明确返回实际绑定的位置或未绑定状态。同一行多个语句和循环重复执行不能靠行号去重。
2. **单一执行准入。** 暂停仍属于当前执行上下文。允许读取快照和管理调试命令；禁止对同 Runtime 发起新的业务执行、tick、当前 task resume/destroy、热卸载或破坏活跃帧的操作。仅禁止 hook 递归不够：Lua 允许回调再执行 Lua 的事实并不适合本项目的直接 C++ 借用契约。通信处理必须遵守同一执行准入。
3. **原生栈与既有根一起存活。** hook 到返回期间不弹栈、不释放根，不通过 `longjmp` 等路径绕过析构。局部变量调试登记扩展现有类型/值/根机制，不能创建另一套对象身份或 GC 世界。异常退出、正常返回和取消均解除登记。
4. **活跃局部变量按需物化。** 有类型 frame 描述变量身份、权威类型、作用域和 `available / uninitialized / out_of_scope / unavailable`。调试模式的 codegen 保证 hook 能读取这些槽，不能把 LLVM 元数据当作保证。调用者帧也只展示其当前位置仍有效的值。
5. **借用与快照分开。** 帧槽地址只在暂停期间借用，外部拿到的是 owning 数据：标量/字符串等按现有值语义复制，对象用既有安全身份或受管理引用保持生命周期。对象引用的 owning 副本不等于其整个对象图被冻结。不能逃逸的 C++ 借用只在回调中读取并转为可持有的数据；没有安全拷贝或身份机制时报告不可持有，不能把裸地址包装成 owning。对象展开要有限额并处理环，不调用用户 getter/to-string 或执行任意表达式。
6. **帧有调用实例身份。** 每次源码函数调用有 activation 身份和明确 enter/leave；单步记录起点 activation 与执行上下文。async resume/suspend 对活跃调试帧的安装与卸载遵守既有 task 生命周期，不能把已经挂起的栈上 frame 指针留给后续 resume。
7. **动作沿既有退出链完成。** continue/step 只改变后续停止策略；abort 是合作取消，在当前 hook 后阻止后续业务语句并沿既有清理路径传播，直到授权执行边界返回取消结果。它不回滚已有副作用，不能抢占尚未返回的 C++ 调用。如果现有生成代码没有完整取消传播路径，应先补齐该路径，再承诺 abort。回调异常、宿主断连和销毁也必须有确定的退出策略及同样的清理保证。

## 需要公开声明的功能边界（项目建议）

- **step into：** 下一个符合条件的源码停止点。直接调用 C++ 时，停到其返回后的下一个源码点；不进入机器指令或没有 hook 的函数。C++ 若按正常契约回调带 hook 的源码函数，该调用有自己的 activation。
- **step over：** 起点调用实例中的下一个语句；起点退出后在其调用者可见的下一点停止。期间更深调用的断点仍可打断单步。不能只比较行号，也不能把同深度的另一个调用当成原来的 frame。
- **step out：** 起点 activation 退出后，在调用者的下一可见点停止。执行入口自身返回时以完成/取消结果结束；没有 caller hook 时不保证还能产生暂停回调。
- **async：** 初期单步覆盖当前实际运行的源码调用链；step 绑定 task/activation，不能停到无关 task。跨 await 后是否继续跟踪同一 task 必须明确。挂起 task 的变量和逻辑 await 祖先只有在存在可安全保留的布局、根与关系时提供。
- **优化：** 调试编译模式可保证约定的语句点与局部变量。发布优化模式不能承诺所有源码行、被消除的调用或每个局部值；明确报告不可用，避免展示陈旧值。未来添加 LLVM 本机 debug info 可复用同一源码映射，但不改变上述 hook 协议。
- **暂停：** 只在 hook 安全点生效；长时间 C++ 调用或无 hook 的执行区间没有即时暂停保证。宿主若在回调内等待远程命令，会占用该执行线程，这是契约本身。

建议最小长期形态：一个调试会话入口、一个编译器发出的停止点 hook、复用权威值/类型机制的 frame 与 snapshot、一个动作枚举。上述不变量应在这条链上实施；通信与 UI 作为该暂停回调的消费者接入。

检索记录：4 次聚焦 Exa 搜索，返回 15 条候选结果；引用仅采用 AngelScript、Lua、LLVM 的官方文档和源码。事实与项目建议已分开标注。
