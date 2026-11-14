# 日志体系评估：自有语义与成熟后端

状态：历史选型评估与隔离 CPU 验证记录（2026-10-29，基于 Windows x64 基线 `25aa632a28f5edd95427bfcf522fb5af717653b6`）；正式落地的进程日志合同见 [Logging 模块文档](../modules/logging/README.md)。[Renderer 验收未覆盖项](../modules/renderer/acceptance.md#未覆盖)仍独立跟踪，不能用日志方案代替原因验证。

## 结论

项目拥有自有的领域诊断和编辑器输出，但还没有贯穿引擎、编辑器、独立运行进程的统一日志服务。不能说“没有日志”，也不能把当前 Project Output 等同于 UE 的日志体系。

建议采用**自有诊断与判错合同 + 成熟日志后端**。Quill 已通过锁定工具链的隔离 CPU 验证，可以进入最小接入设计；spdlog 保留为对照方案，不据此宣称 Quill 性能更优，也不全仓替换。Quill 的后台正文格式化、每线程队列和等待式 flush 有实际设计优势，但本机反例确认默认正文输出、错误发现和丢弃策略不能直接承担项目判错合同。

当前最紧要的不是让更多日志更快写出去，而是让失败在检测点被保留、必要证据丢失可见、请求与实际采用结果能对应起来。这些职责不能外包给 Quill 或 spdlog；文件轮转、通用格式化、输出锁与队列则不值得继续纯自研。后端类型不进入 World、Source 或语言的公共合同。

## 已有能力和边界

| 现有机制 | 已实现 | 不等同于什么 |
| --- | --- | --- |
| [World Diagnostic](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/world/diagnostic.hpp)、[World 接口](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/world/world.hpp) | severity/kind/phase、对象和实体身份、字段、Source 位置、消息、serial、repeats；最多 1024 条，报告丢弃总数 | 通用线程安全日志、进程时间线或持久化服务 |
| [World report](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/world/world.cpp) | 按对象/阶段/字段合并连续失败；消费后在故障持续期间不重复发同一事件 | 所有时间点的完整事件历史；这是有意的业务降噪，不能简单删除 |
| [Source Diagnostics](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/source/diagnostics.hpp)、[按调用合同](../source-diagnostics.md) | 调用状态、程序/代码版本、Source 位置、资源前后状态、计数和可选计时；报告拥有文本 | 全局日志；合同明确同线程、按调用持有，不维护全局历史 |
| [Workspace::log/log_repeated](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/editor/workspace.cpp) | Project Output 文本表、逐行显示、连续文本合并；保留 256 行，每行至多 2048 个 UTF-16 code units 后加省略号 | 可查询的结构化日志、按 category/severity 过滤、完整文件存档 |
| [PlayProcess](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/application/play_process.cpp)、[输出消费](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/application/project_execution.cpp) | stdout/stderr 共用管道，主线程按预算读取，转到 Project Output | 跨进程结构化记录和可靠持久化；输出缓冲超过 64 KiB 会截断，未报告截断计数，未完成长行也可能被清空 |
| [Runner](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/application/project_runner.cpp) | 领域诊断格式化后输出；明确打印 World dropped_diagnostics 增量 | Editor 同等覆盖：当前 Application 取诊断循环没有对应丢弃计数提示 |
| [RHI](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/rhi/rhi.hpp)、[Renderer](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/src/renderer/renderer.hpp) | 结构化 Result、错误码、终态、目标可绘制性、帧状态、统计；底层有 debug layer 与 gfx 文本输出 | 统一事件日志。RHI native_result 是 GfxResult，不是原始 HRESULT；Renderer skipped 合并了多个原因 |
| [验收 gate](../../tools/acceptance-gate.ps1) | 运行独立目录、逐目标 stdout/stderr、汇总、耗时、验证级别、进程树和超时证据 | 普通产品运行自动拥有相同日志；文件内容也不自动决定测试失败，仍依赖正确的检查和退出码 |

`src`、CMake 和锁文件未发现 Quill、spdlog 或其他统一 logger 依赖；Foundation 当前没有日志模块。原生输出主要是分散的 printf/fprintf，业务诊断经 Workspace 展示。`Error/Result` 是传播与恢复合同，不应被“记一条日志然后继续”取代。

## 完备性判断

当前能力足以支撑不少定向验收和业务错误定位，但还不适合把一次用户运行或多进程故障完整重建出来。优先级不是增加更多打印点，而是保存能区分原因的证据：

1. **一致的记录元数据**：级别、分类、时间、进程/线程、运行标识；涉及帧、准备任务、文档或资源请求时加其现有身份。避免仅靠文本和接近的时间拼接因果。
2. **持久化与保留政策**：正常 Editor/Runner 启动即有可定位的独立日志；预算、轮转、截断/丢弃计数、flush 和关闭顺序明确。stdout 重定向不是产品持久化合同。
3. **失败边界**：日志后端写盘/格式化失败不能覆盖原始业务错误，不能递归再记同一个失败；异常终止不承诺异步尾部必达。崩溃转储是另一个能力，不由日志库自动提供。
4. **线程与生命周期**：worker 能安全记录；编辑器输出在宿主线程消费；异步记录拥有所需文本，不捕获 Session/Runtime/Surface 裸引用；多个 Runtime/Context 能区分，不依赖唯一全局业务上下文。
5. **可控制的噪音和成本**：分类/级别过滤，Release 可以裁掉详细诊断；窗口/准备状态只在变化、异常或定向 trace 时记录，不能每帧落盘。日志不替代现有 frame timing/GPU profiling。
6. **证据而非断言摘要**：Renderer 的失败输出应包括请求/接受/capture 帧号、状态、draw count、输入/目标尺寸；菜单需同时知道逻辑 source、实际 popup 接受帧和相关原生事件。换一个输出库不会自动补齐这些观测点。

以上是现状与建设方向，不是建议立即一次实现全部能力。尤其不为日志先建立远程采集、通用事件总线或全局服务定位器。

## 过程审查与判错：不能交给日志级别

这里的过程审查指运行过程可回溯及开发验收可自动判错，不是安全审计级持久存档。三种责任必须分开：

| 责任 | 应回答的问题 | 权威来源 |
|---|---|---|
| 业务结果与诊断 | 操作成功、失败、取消、被隔离还是不可继续？ | 现有 Result、World/Source 诊断、进程退出与设备状态 |
| 过程记录 | 哪个请求走到哪一步，采用了哪个结果，为什么没有显示？ | 在真实状态转换处记录现有运行/文档/请求/帧身份，串起请求→准备→采用→接受帧；不凭时间排序猜因果 |
| 日志输出与验收 | 证据是否送达、丢了多少；本用例是否达到预期？ | 输出端报告自身健康；验收检查公开结果、指定诊断和必要结束标记，不以日志文本替代行为断言 |

**已经确认的差距：** [gate](../../tools/acceptance-gate.ps1) 以子进程退出码/超时决定目标结果；[headless](../../cmake/run-headless-acceptance.cmake) 和 [RHI runner](../../cmake/run-rhi-acceptance.cmake.in) 也依赖退出码，[Renderer default](../../cmake/run-renderer-acceptance.cmake.in) 另检查 PASS 标记。这些外层不能补救消费者内部漏检。前次 RHI 普通 warning 注入曾被存活检查清掉而整体 PASS；[当时修补](https://github.com/ySion/FrostFoxAtelier/blob/25aa632a28f5edd95427bfcf522fb5af717653b6/acceptance/rhi/preparation_cases.cpp)是在清队列前判错，不是换了输出库。该漏检已修复，不声称当前仍存在。

**建议先确定的最小规则：**

1. **检测点先保存结果，再异步输出。** 不能等日志 worker 写盘后才将测试判失败，也不能因 UI 过滤、队列丢弃或后端错误把失败变成成功。严重错误需要保留原始首因；日志失败走不递归的紧急 stderr/系统输出路径，不能覆盖业务错误。
2. **区分 severity 与 outcome。** World 默认 isolate，错误可只隔离一个对象，Play 不必退出；预期失败测试也会产生 error。默认拒绝本用例范围内的非预期 warning/error，但豁免须限定来源/稳定码/阶段/对象及数量，期望未出现也失败；不以“包含 error 字符串”、全局忽略或临时调低级别判错。哪些 warning 属于产品允许状态由对应合同决定。
3. **状态转换优先于逐帧打印。** 原样记录 not_ready、skipped、accepted 与拒绝原因；ready、Live ACK、接受帧及 GPU 完成各自命名，不能统一写 success。保留请求号、实际采用/显示号与原因。正常帧静默，失败时输出有界前序事件；持续故障按对象/事件合并并报告次数，不能只按相同文本全局消重。
4. **缺证据也必须可见。** 完整验收需要预期子案例覆盖、终态、退出码和必要证据；取消、超时、缺终态、日志丢失影响必要证据时不能记完整 PASS。诊断子集单独标注，重跑不得覆盖原失败。现有 gate 的运行目录、配置、验证级别和进程树收尾继续复用。
5. **区分正常关闭与崩溃。** 正常关闭先停止生产者，排空判错所需记录，再结束 sink/backend；崩溃、强制 TerminateJobObject、断电是不同保证。崩溃转储另做评估，不能拿异步 flush 或 signal handler 承诺零丢失。

当前最先值得补齐的是：Editor 显示 World dropped 增量，Play 截断有计数，按运行落盘并保留结构化元数据，以及关键准备/菜单/文档切换的原因记录。这些是静态观察到的能力缺口；本轮未对这些产品链路做丢失压力测试，也未实现上述规则。

## 选型边界

### 继续纯自研

如果需求永远只有少量同步 stderr 文本，现有直接输出可以继续工作，新增依赖未必划算。但一旦要求文件轮转、多输出端、线程安全、限流和优雅关闭，自研会把工程时间花在非项目差异化能力上。目前跨进程输出和丢弃策略已经分散，继续逐处追加容易扩大这种不一致。

### 直接全仓使用库接口

Quill 和 spdlog 都能很快提供格式化和输出能力，但不能解决领域诊断、错误隔离、历史、Runtime 身份和编辑器线程归属。全仓依赖其类型也会固定业务 API 与库的生命周期/全局注册表选择，不推荐。

### 自有小接口与单一成熟后端（推荐探索）

自有部分只拥有真实的项目合同：category/severity、必要上下文、宿主初始化/关闭、编辑器有界记录消费，以及现有 Diagnostic 到日志的桥接。文件、控制台、锁、轮转等复用库。保持一个事实源，不在 World 和日志后端分别再造同一份错误状态。

首个增量宜从 Application/Runner 的启动、停止和失败边界开始，加文件与控制台输出；保持 World/Source 的 Result 和诊断 API 不变。Quill 原型验证异步路径及其预算；spdlog 可用同步小路径作复杂度对照，需要异步时再比较同等队列政策。不是建设可运行时切换的双后端框架，也不再套一层通用日志队列。最终只保留经过验证的一个实现。

若采用 Quill，进程宿主持有服务，同进程多个 Runtime 记录各自身份，而不是每个 Runtime 启停一个全局 backend。稳定原生模块可以延后格式化；语言/JIT 边界先形成拥有型数据，由稳定宿主调用日志库。编辑器 sink 只复制并投递有界记录，不持有业务对象、不直接访问 UI。自有部分承载这些真实边界，不为未知需求增加接口。

## Quill：优势、代价与接入门槛

2026-10-29 核对的官方稳定版本为 [v13.1.0](https://github.com/odygrd/quill/releases/tag/v13.1.0)，2026-10-17 发布，固定源码提交 `78c0696c5be589488d882c280796dfa06bdcd492`。Quill 为 [MIT](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/LICENSE)，标准目标 [quill::quill](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/CMakeLists.txt#L319-L408) 是 C++17 起的 header-only INTERFACE library，内置独立命名空间的 [fmtquill 12.2.0](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/bundled/fmt/base.h#L1-L28)，也须保留其许可。它不是用 `std::format` 就能消除 bundled fmt 的 spdlog 配置路径。

上游存在 [Windows clang-cl workflow](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/.github/workflows/clang-windows.yml#L24-L75)，不等于本项目 Clang/LLD 23.1.1、MSVC STL 14.50.35717、C++23、MD/MDd、`/W4 /WX /EHsc` 已验证。Windows 队列用 [_aligned_malloc/_aligned_free](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/core/BoundedSPSCQueue.h#L278-L350)，不能因项目替换全局 new/delete 就声称所有内存都经 mimalloc。

### 真正有区分度的优势

| 维度 | Quill v13.1.0 | 对照与项目含义 |
|---|---|---|
| 普通日志的正文格式化 | 前台编码参数，后台解码和格式化；每生产线程 SPSC 队列 | spdlog async 在前台形成正文。Quill 有望减少前台格式化和共享队列竞争，但后台执行的数据/代码必须保持有效，收益须测量 |
| flush | 控制事件 + 完成应答，调用者等待后台处理 | spdlog async flush 仅入队。Quill 更适合正常关闭取证，但不是 I/O 成功返回值、全并发生产者屏障或断电保证 |
| 结构化与上下文 | named placeholders、JSON sink、线程 MDC、backtrace 日志缓冲 | 可以复用基础设施，但不是项目诊断 schema、任务上下文传播或崩溃调用栈 |

源码入口：[前台编码](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/Logger.h#L105-L181)、[后台解码/格式化](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/BackendWorker.h#L1019-L1096)、[flush 等待](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/Logger.h#L604-L662)。跨线程按当前可见队首时间戳合流，[grace period](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/BackendOptions.h#L124-L198) 不是因果保证；请求/任务序号仍由业务提供。

### 直接影响本项目的六项门槛

1. **尽早判错不能等后台。** 普通日志路径[不做 fmt 编译期格式校验](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/docs/faq.rst#L82-L95)；格式错误在后台生成替代正文并通知，通常继续运行，原级别也不自动升级。`error_notifier` 还报告队列扩容/阻塞等状态，不能把它每次调用都解释为业务失败。验收必须单独保留结果和日志设施故障；原型需验证格式检查策略，不能为低延迟牺牲开发期错误可见性。[格式失败路径](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/BackendWorker.h#L2404-L2453)、[通知节流与异常处理](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/BackendWorker.h#L463-L496)。
2. **默认输出不等于完整保存正文。** [JsonSink](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/sinks/JsonSink.h#L102-L241) 默认主要存格式模板和字符串化的 named args。普通 `LOG_INFO(logger, "{}", diagnostic_text)` 在默认 JSON 中会留下 `message: "{}"`，不是实际诊断文本；下述双配置反例已复现。结构化输出应明确保存 rendered message、稳定诊断码和现有上下文。另外[默认字符检查](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/BackendOptions.h#L42-L45)只认可 ASCII 等字符；含字符串参数时可将 UTF-8 非 ASCII 字节转成 `\xNN`，中文资源名和诊断需明确配置，不能悄悄改变正文。
3. **字符串复制不代表 JIT 安全。** 默认 string/string_view 实参[复制内容](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/core/Codec.h#L168-L273)，但队列还保存 metadata、logger 和 decoder 函数指针。[runtime metadata DEEP](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/Logger.h#L498-L532) 只深拷贝文本，不延寿可卸载代码；[DeferredFormatCodec](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/DeferredFormatCodec.h#L31-L51) 还把用户对象访问移到后台。backtrace 也可能[保留静态 metadata 指针](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/TransitEvent.h#L96-L116)，普通 flush 后不能直接卸载。应由稳定宿主完成库调用，提交独立拥有的文本/标量；不能把 Runtime 对象、描述符或 formatter 借给后台。
4. **必须明确背压和内存预算。** [默认前台队列](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/core/FrontendOptions.h#L40-L72)为 UnboundedBlocking，每线程初始逻辑容量 128 KiB，最大增长节点 2 GiB；不是总内存上限。固定队列 backing allocation 为[两倍逻辑容量](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/core/BoundedSPSCQueue.h#L62-L84)，后台另有按条数限制的 staging。BoundedDropping 丢新记录，但控制事件仍可能等待入队；[丢弃通知](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/BackendWorker.h#L1570-L1635)按线程汇总，不是每 logger 的可靠审计账本。需约束单条大小、线程/队列预算、UI 消费和满时行为，不能只勾选 bounded 就宣称全流程有界。
5. **flush、停止和持久化是不同合同。** flush 无超时；[sink flush 异常](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/BackendWorker.h#L1955-L1973)通知后仍可完成应答。Windows FileSink 普通 flush 是[排出 WriteFile 缓冲](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/sinks/FileSink.h#L479-L497)，不是这里调用 fflush；只有启用 fsync 才尝试 [FlushFileBuffers](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/sinks/FileSink.h#L713-L741)，且受最小间隔影响。正常关闭[先停生产者再 drain/join](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/docs/faq.rst#L205-L239)；UI sink 完成投递不等于 UI 已消费。进程共享 backend，[一套 FrontendOptions](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/core/FrontendOptions.h#L20-L30)和[线程 MDC](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/docs/mdc.rst#L90-L98)也不能当成各 Runtime 的隔离服务。
6. **不默认接管 Windows crash/取消处理。** 可选 [console/SEH handler](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/SignalHandler.h#L440-L542)走普通编码和等待；Windows [不支持其 alarm 超时保护](https://github.com/odygrd/quill/blob/78c0696c5be589488d882c280796dfa06bdcd492/include/quill/backend/SignalHandler.h#L52-L69)。Ctrl+C/Break 在有 logger 的路径 flush 后 `_Exit(EXIT_SUCCESS)`，与验收区分取消/成功冲突。宿主应继续拥有退出码、fatal、SEH/转储策略；Critical 只是级别，backtrace 是日志环而不是调用栈。不能承诺崩溃尾部完整。

上述是固定版本的源码/文档合同；已运行的子集及未覆盖边界见下节验证记录。若大量语言诊断最终先在宿主生成正文，Quill 延后格式化的优势会部分让出；若实际高频路径主要是稳定原生标量日志，其设计更有吸引力。以是否满足证据完整性、生命周期和成本预算决定，而不是用纳秒级上游排名决定。

## UE 与 spdlog 的一手资料对照

UE 值得借鉴的是分工，而非完整复制其宏和全局体系：[category 的默认/编译期/运行时 verbosity](https://dev.epicgames.com/documentation/unreal-engine/logging-in-unreal-engine?application_version=5.8)、[输出设备](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FOutputDevice?application_version=5.8)和[编辑器历史/过滤](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/OutputLog/FOutputLogModule?application_version=5.8)是不同职责。官方的 `Saved/Logs` 文件与 Output Log 面板是不同消费者；UE Fatal 包含终止语义，不能直接对应到只记录 critical 级别的 spdlog 调用。此处使用 UE 5.8 公开文档视图作设计参照，不声称核验其私有实现或当前稳定发行状态。

本次核对 [spdlog v1.17.0](https://github.com/gabime/spdlog/releases/tag/v1.17.0)，固定源码提交 `79524ddd08a4ec981b7fea76afd08ee05f83755d`；MIT 许可，默认 bundled fmt 12.1.0，也支持 `SPDLOG_USE_STD_FORMAT`。接入需保留实际分发依赖的许可。以下约束影响本项目方案，不应被“成熟库”三个字掩盖：

- [异步队列](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/async_logger.h#L21-L68)有 block、overrun_oldest、discard_new 三种策略；默认共享池 8192 条、1 worker。不是无锁队列；多 worker 可能重排，共享池满时也不会自动优待错误日志。正文格式化仍发生在生产线程。
- [async flush](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/async_logger-inl.h#L33-L78)是排队请求，不是完成屏障；[文件 flush](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/details/file_helper-inl.h#L24-L105)通常是 fflush，不是 FlushFileBuffers。显式 shutdown 仍需安排外部保留池/sink 的生命周期，不能承诺崩溃零丢失。
- [callback sink](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/sinks/callback_sink.h#L15-L37)不自动切到 UI 线程；[_mt 的锁](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/sinks/base_sink-inl.h#L25-L48)也不能授权任意并发修改 logger 配置或重入回调。
- [MDC](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/mdc.h#L15-L48)是 TLS，不支持 async。程序/文档/请求身份要在生产侧捕获；[source_loc](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/common.h#L315-L326)里的文件名/函数名是裸指针，不能直接借用短命的 JIT 动态文本跨队列。
- [编译期宏裁剪](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/spdlog.h#L278-L348)不等于直接 `logger.debug()` 调用的运行时过滤，也不是 UE 的完整逐 category 编译期上限；禁用级别时是否求值昂贵参数应明确验收。

这些事实支持“自有薄语义 + 成熟后端”，不意味着任何库的默认配置已满足项目合同。spdlog 本轮仍仅为源码/文档对照，未做本机编译或性能比较。

## CPU-only 验证（2026-10-29）

隔离临时 CMake 工程直接使用上文固定 Quill 源码、项目 `windows-clang.cmake` 和已有 mimalloc 产物，不改根构建与依赖锁。Dev/Release 均以 C++23、MDd/MD、`/W4 /WX /EHsc /permissive- /utf-8` 编译；第三方头按 SYSTEM 引入，没有放宽宿主警告。复用正式 `global_new_delete.cpp`，每个进程实查 new 属于 mimalloc heap 且 `mi_is_redirected()` 为真。所有检查独立于 assert，Release 也执行。

首轮 18 次进程运行，加 DLL/满队列 Error 补充 4 次，共 22 次；20 次 exit 0，2 次宿主判错对照按预期 exit 23，无超时。另有两配置各一正一负的编译期格式检查：有效类型通过，无效类型因 `invalid format specifier` 常量表达式失败而拒绝。这里“符合预期”包括暴露默认配置的不足，不代表产品验收或无故障认证。

| 检查 | Dev / Release 实际观测 | 结论边界 |
|---|---|---|
| 默认文本与 JSON | 中文被转义；位置参数正文在默认 JSON 中仅留 `{}`；named args 保存为字符串值 | 默认设置不符合本项目正文保真要求，并非断言日志库自身合同违例 |
| 文本适配 | 关闭 ASCII sanitizer、派生 JSON sink 增加 rendered 字段后，中文/引号/反斜杠/换行、显式长度 NUL、70,013 字节正文精确往返；另用 PowerShell JSON parser 独立读回 | 只验证这组合法 UTF-8 输入；未定义所有控制字符、非法编码或单条上限政策 |
| 无效格式 | `LOG_INFO(..., "bad-format={:d}", "not-an-integer")` 正常编译，后台产生 fallback/通知，仍为 Info，后续记录继续；默认 JSON 未保留 fallback 正文 | 非法格式不是自动业务失败；宿主记录设施故障后，独立对照确实 exit 23；预期业务 Error 不触发库内部 notifier |
| 编译期对照 | 显式 `fmtquill::format_string<char const*>` 拒绝上述格式，`format_string<int>` 通过 | 可复用 bundled fmt 补固定位置格式检查；尚不是完整项目宏、named placeholders 或动态格式方案 |
| 真实文件失败 | 文件路径指向目录时 CreateFileW 失败，GetLastError=5；第二个句柄 LockFileEx 后，后台 WriteFile 失败，GetLastError=33，但 flush 正常返回且文件仍为 0 字节；解锁再 flush 后两条正文精确落盘，无重复 | 证明 flush 完成不等于 I/O 成功；只证明这条缓冲写失败可恢复，不推广为所有 sink/磁盘故障可靠重试 |
| 4 KiB BoundedDropping | 后台由真实文件 sink 回调暂停；2,000 条接受 31、拒绝 1,969，接受记录按序完整落盘；同生产线程追加的 Error 也被丢弃，另一个线程的超长单条被拒绝；通知为 1,970 + 1 | 不优待 Error，不能把业务结果只放在日志里；通知按线程汇总，不能当成结构化逐事件账本 |
| 4 KiB BoundedBlocking | 生产者完成 31 条后阻塞；释放文件 sink 后全部 2,000 条按序落盘；8,232 字节编码记录超过容量时抛错，不永久等待 | 阻塞/丢弃是不同成本合同，不存在默认“低延迟且零丢失” |
| 动态文本寿命与 flush | 先阻住后台，再提交动态 file/function/format/string_view，覆盖并销毁原存储；DEEP 宿主入口仍精确交付。flush 在 80 ms 观察窗口未返回，释放后完成，blocking removal 也完成 | 验证拥有型文本和后台完成应答；不证明 UI 已消费、断电持久或任意自定义 codec 安全 |
| 实际 DLL 卸载 | 生产 DLL 经 EXE 中的稳定回调提交文本；调用返回后 FreeLibrary，确认模块已卸载，再放行后台，正文与动态源位置完整 | Quill 宏/decoder 在 EXE，DLL 不调用库；不是任意 DLL 直接使用 Quill 后卸载，也不替代真实 Source/JIT 退役测试 |
| 正常关闭 | 4 个生产线程各写 257/263/271/277 条，join 后不先 flush，直接 stop；1,068 条全部落盘，每线程序号无缺失/重复/倒序 | 有限生产者的正常关闭通过；没有验证持续生产、崩溃、停机超时、回调锁反转或 backtrace 退役 |

本机证据在 `.amp/in/artifacts/quill-cpu-20261029/`：`run1/summary.csv`、`supplement/summary.csv`、逐进程 stdout/stderr 与实际 text/JSON 文件、`json-roundtrip.log`、两配置构建/预期编译拒绝日志、编译命令和版本 manifest。子进程隐藏运行，各有 30 秒 watchdog；没有窗口、GPU 设备或 GPU 锁，也未跑全仓验收。记录了夹具进程内存，但它包含采集副本、文件缓冲和分配器开销，不作为 Quill 性能/预算结论。临时源码和构建产物在收尾移除，保留结论与原始输出。

### 下一步与未验证项

验证支持进入**一个最小宿主接入设计**：明确固定格式检查、拥有型记录/稳定身份、文本保真、设施故障保存及满时政策，然后选择 Application/Runner 启停/失败→文件/控制台→编辑器有界消费。现有 Result、领域诊断与行为断言继续作为判错事实源；不新增通用事件总线或双后端框架。

尚未验证产品整合、真实 Source/JIT、自定义 deferred 类型、UI/Play 桥接、长期积压总内存、崩溃/SEH/取消及持久化保证，也未比较 Quill 与 spdlog 的前台尾延迟/后台 CPU/吞吐。性能选型需相同 payload、输出、flush、队列预算和丢弃政策；不能把本轮可运行结论当作性能胜出。若满足项目合同的适配抵消了 Quill 收益，仍可选择 spdlog，不保留两套实现。
