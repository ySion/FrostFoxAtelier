# Runtime / JIT 按调用诊断

入口为 [`source/diagnostics.hpp`](../src/source/diagnostics.hpp) 和现有 [`Program`](../src/source/program.hpp)。诊断覆盖同一源码执行链，包括同步借用、拥有值、内部函数、原生服务与协程。记录由调用方持有；运行时不维护全局日志或报告历史。

## 启用与读取

编译能力和每次采集分开选择：

```cpp
using namespace atelier;
source::Program program(runtime);
auto request = program.request("math.source", "float scale(float x) { return x * 2.0; }",
    {}, {.diagnostics = true, .artifacts = true});
if (!request) return std::unexpected(request.error());
auto candidate = source::Program::prepare(*request);
if (!candidate) return std::unexpected(candidate.error());
auto published = program.try_publish(*candidate);
if (!published) return published;

source::Diagnostics capture({.timing = true});
std::array arguments{Scalar{3.0f}};
auto result = program.entry().invoke({}, arguments, {1}, &capture);
auto report = capture.snapshot(); // owning copy; also available after failure
```

`SourceSnapshot` 同样通过 `request(snapshot, options)` 选择编译能力。参数、输出存储、准入、发布和版本保活继续使用原有入口和契约。

- `CompileOptions::diagnostics` 默认为 false，普通生成代码不包含诊断计数、位置写入或空指针检查。启用后，调用方仍可省略 `Diagnostics*`。
- 未插桩版本收到采集请求返回 `unsupported_operation`，在参数构造、协程帧分配与源码副作用之前拒绝。通过普通编译与兼容发布切换诊断能力，业务签名不变。
- `DiagnosticOptions::timing` 独立控制时钟采样。关闭时只计数，`active_time` 为零。
- 所有操作遵循现有 Runtime 单线程约定。一个 capture 同时归一个调用所有；活动期间再次使用返回 `busy`，原记录不被覆盖。终态后可以复用，下一次调用重置记录。

## 报告含义

| 数据 | 含义与边界 |
| --- | --- |
| `program_id`、`code_version`、`entry` | 进程内程序身份、发布所用请求序号、入口名称。版本号不要求连续；原生地址不作为身份 |
| `source_name`、`location` | 编译输入名称；错误/挂起位置包含函数、原始文件路径、文件版本、字节偏移及从 1 开始的行列。单文本请求以请求序号表示源码版本 |
| `status`、`error` | 执行状态与首个错误；清理不会覆盖首错。错误文字有界保存，最长 511 字节 |
| `calls` | 生成代码实际经过的调用边界次数；统计口径见下文 |
| `before`、`after` | 开始及最近暂停/结束时，该 Runtime 的对象数、根数、待回收数与 GC 阶段。它们是上下文快照，不能直接当作本函数的分配量 |
| `gc_work`、`gc_reclaimed` | 本次调用经已有安全点推进 GC 的工作量与回收对象数；宿主在协程挂起期间推进的 GC 不计入 |
| `peak_owned_values`、`remaining_owned_values` | 本次源码 Invocation 管理的拥有值峰值及当前余额，包含内部函数；不表示字节数或全进程分配次数 |
| `resources_cleared` | 执行槽与拥有值是否已由显式清理归零；在 Invocation 的 RAII 兜底之前核对 |
| `resumes`、`active_time` | 实际进入源码帧的恢复次数；可选时间包含参数准备、执行及清理，排除 ready/suspended 的等待时间 |

计数和时间累加采用饱和处理，不因统计溢出改变业务结果。`snapshot()` 复制文本与数据，不调用用户观察回调，也不改变执行状态。活动时读取的资源快照对应最近一次边界，计数对应读取时已经执行的操作。

### 调用计数口径

- `field_read` / `field_write`、`native_call`、`poll`、`object_create`、引用操作及 `root_clear`：现有 Runtime 桥接服务。
- `value_construct` / `value_assign` / `value_destroy`、`sequence_mutation`：源码拥有值和容器生命周期服务。
- `sequence_view`：直接调用原生容器视图函数，包括绕过普通服务包装的路径。借用参数的视图在函数入口缓存；索引与内联数学不产生逐元素服务调用。
- `coroutine_allocate` / `coroutine_free`：LLVM 协程生成帧的分配/释放调用，不包含宿主控制块与其它 C++ 分配。
- `begin` / `finish`：现有执行策略服务；受保护值路径按原合同省略它们和 GC 轮询。
- `source_call`：语言内部函数调用，与 JIT→C++ 边界分开统计。即使 O2 内联了函数，仍表示源码调用次数。
- `debug_hook`：启用调试编译时的帧与停点服务。若同时启用执行计时，暂停处理器的等待时间也包含在内。

例如 `N` 次 `self.value = self.value + 1` 循环产生 `N` 次字段读、`N` 次字段写，以及入口和实际循环回边上的 `N + 1` 次 poll。借用 Array 的直接数学循环可只有一次 `sequence_view`。实际脚本应以报告为准，不能用这一例子推断所有调用路径。

## 协程、错误与寿命

`Entry::start` 和 `Lease::start` 使用相同的可选 capture。创建后为 `ready`；每次恢复进入 `running`，源码 `yield` 记录 `suspended` 和源码位置。Runtime 确定最终 `completed` / `failed` / `cancelled` 后，帧销毁执行生成清理，报告只终结一次。运行中取消仍按既有规则延迟处理，失败优先级保持不变。

协程帧拥有采集状态，所以调用方释放 `Diagnostics` 外壳不影响执行或清理；复制的句柄观察同一记录。报告与位置元数据不保活 Runtime、帧或机器码。热更新、Program.close 和旧 Lease 的调用分别记录实际使用的版本；Runtime 的原有外部寿命要求仍适用。

嵌套函数先记录失败位置，外层传播与清理保留该位置。有采集时，同步调用结果和协程结果中的 `Error.source` 也补充原始源码位置。需要断点、单步、只读变量和调用栈时，使用同一执行链的[源码调试接口](source-debugger.md)。

## IR、对象代码与反汇编

编译时设置 `artifacts = true`，随后通过固定版本的 `Lease::artifacts()` 获取：

- `original_ir`：优化前 LLVM IR；`optimized_ir`：协程转换、Scalarizer 和 O2 后的 LLVM IR。
- `entry_symbol`：所选公开入口；`target_triple` 和 `cpu_features`：编译目标信息。
- `object_code`：该次 JIT 编译实际产生的 x64 COFF 对象；`assembly`：Intel 语法反汇编，包含代码节、原始字节、符号和重定位。

**COFF 字节是链接前对象代码。** 反汇编地址是代码节内偏移，重定位操作数还未解析为运行地址，不能当作已链接入口的内存转储。多入口共享编译模块时，产物包含该模块；通过 `entry_symbol` 定位所选入口。

编译产物按需保留，`artifacts` 与 `diagnostics` 独立；可只保留产物检查无插桩版本。未启用时查询返回错误。查询返回拥有的拷贝，可由宿主保存到文件，释放 Lease 后仍可查看。

## 验收入口

[`acceptance/source/diagnostics.cpp`](../acceptance/source/diagnostics.cpp) 使用正式 Runtime 和 JIT，覆盖调用计数、借用/拥有值、GC、跨文件故障位置、协程终态与旧版本、报告及产物寿命。它纳入现有 `source_acceptance`，也可聚焦执行：

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target atelier_source_minimal
./out/dev/atelier_source_minimal.exe --diagnostics
# release 使用相同入口，替换配置名。
```

这些验收证明行为与计数契约；不提供诊断版本的性能提升结论。插桩会增加指令、分支和存储，也可能影响 LLVM 优化，应使用普通版本做最终性能测量。

最近验收：2026-10-09，dev / release 的 Source 公共消费者（含本页诊断场景与连续会话）、JIT 贯通和 Runtime 端到端验收均通过。本机完整日志为 `out/diagnostics-{source,jit,runtime}-{dev,release}.log`。
