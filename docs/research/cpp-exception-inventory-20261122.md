# C++ 显式异常位置清单

2026-11-23，分支 `codex/stl-library`，统计对应 Control 与文档验收迁移后的源码。基于 Git 跟踪的自有 C/C++ 源码、头文件和内联文件，用仓库锁定的 Clang 读取原始词法记号。注释和字符串不计入。各列按源码出现次数计数，`throw` 含原样重抛的 `throw;`。同一位置可能服务多条调用链。宏定义及条件编译分支按源码计入。

产品包含 `modules/` 和 `apps/`，工具、验收及研究样例分别统计。仅统计自有 C/C++，隐式可能抛错的库调用需另行审计。

异常状态 API 统计 `exception_ptr`、`current_exception`、`rethrow_exception`、`make_exception_ptr`、`uncaught_exception`、`uncaught_exceptions`、`nested_exception`、`throw_with_nested` 和 `rethrow_if_nested`。SEH 栏统计 `__try`、`__except`、`__finally`、`__leave` 和 `RaiseException`。

## 分层统计

| 范围 | 扫描文件 | 命中文件 | throw | try | catch | 异常状态 API | SEH |
|---|---:|---:|---:|---:|---:|---:|---:|
| 产品 | 343 | 0 | 0 | 0 | 0 | 0 | 0 |
| 工具 | 2 | 0 | 0 | 0 | 0 | 0 | 0 |
| 验收 | 69 | 0 | 0 | 0 | 0 | 0 | 0 |
| 研究样例 | 7 | 0 | 0 | 0 | 0 | 0 | 0 |
| 合计 | 421 | 0 | 0 | 0 | 0 | 0 | 0 |

## 当前状态

Control 验收的异步请求通过 Result 回到调用线程。子进程先请求正常退出，关闭超时则强制终止并等待回收。主操作和清理都失败时，验收入口返回原始失败，另行报告清理诊断。文档验收和 CPU 采样的提交、生命周期通知及文件系统错误也沿 Result 返回入口。

## W5 构建与终止边界

2026-11-23，dev 与 release 的有效编译命令各覆盖 49 个自有 C++ 目标、226 条翻译单元命令（含生成代码和同一源文件在不同目标中的编译）。逐条核对禁异常编译选项与 STL 宏，并按实际选项组合检查 Clang 前端参数。CMake 的 Windows 默认参数含 `/EHsc`，自有目标随后设置的 `/EHs-c-` 生效；前端关闭 C++ EH，MSVC STL 使用 `_HAS_EXCEPTIONS=0`。

- Logging 消费方同时取得 Quill 和 bundled fmt 的禁异常宏。Foundation 使用 `TBB_USE_EXCEPTIONS=0`；oneTBB DLL 保持上游 EH 和与宿主匹配的动态 CRT。gfx 保持上游编译器 EH 和显式禁异常 STL，Jolt 的编译器 EH 关闭。LLVM 消费方使用独立 `/MT` 后端。
- 每种配置用独立子进程实际触发不可满足的分配请求、受进程内存配额限制的线程启动失败、预编译 STL 的越界错误、工作线程 terminate 和 oneTBB 非法步长。五条路径均输出对应的 Atelier fatal 诊断，以 `0xC0000409` 终止。
- LLVM 使用正式后端对象和依赖重链接临时探针 DLL，通过原有材质编译入口安装处理器。静态 CRT terminate 和 `llvm::report_fatal_error` 分别输出 `Atelier fatal: abort`、`Atelier fatal: llvm fatal error`；dev 与 release 的退出码均为 `0xC0000409`。

双配置 Full 验收通过。每种配置运行全部 15 个无窗口和 5 个窗口验收项，其中 RHI 和三个 Renderer 入口开启 GPU-based validation。门禁返回 0，所有窗口验收结束时进程树为空。复验入口为 PowerShell 的 `& ./tools/acceptance-gate.ps1 -Full -Configs dev,release`。诊断覆盖范围及第三方边界见[工程指南](../engineering.md#唯一-fatal-出口)。
