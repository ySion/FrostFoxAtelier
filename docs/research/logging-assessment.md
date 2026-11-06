# 日志体系评估：自有语义与成熟后端

状态：探索，不是依赖引入或接口定案。2026-10-29，核对 Windows x64 当前未提交工作树；本轮不引入日志库，也不迁移现有调用。窗口失败的运行证据另存本轮审核记录，不能用日志方案代替原因验证。

## 结论

项目拥有自有的领域诊断和编辑器输出，但还没有贯穿引擎、编辑器、独立运行进程的统一日志服务。不能说“没有日志”，也不能把当前 Project Output 等同于 UE 的日志体系。

建议保留自有诊断语义，以 spdlog 作为通用日志后端的首选候选。不建议继续自行实现文件轮转、格式化、输出锁和异步队列；也不建议把 spdlog 类型直接暴露给 World、Source 或自有语言。先完成小范围集成验证，再决定是否正式接入，不能依据上游微基准声称本项目会提速。

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

`src`、CMake 和锁文件未发现 spdlog 或其他统一 logger 依赖；Foundation 当前没有日志模块。原生输出主要是分散的 printf/fprintf，业务诊断经 Workspace 展示。`Error/Result` 是传播与恢复合同，不应被“记一条日志然后继续”取代。

## 完备性判断

当前能力足以支撑不少定向验收和业务错误定位，但还不适合把一次用户运行或多进程故障完整重建出来。优先级不是增加更多打印点，而是保存能区分原因的证据：

1. **一致的记录元数据**：级别、分类、时间、进程/线程、运行标识；涉及帧、准备任务、文档或资源请求时加其现有身份。避免仅靠文本和接近的时间拼接因果。
2. **持久化与保留政策**：正常 Editor/Runner 启动即有可定位的独立日志；预算、轮转、截断/丢弃计数、flush 和关闭顺序明确。stdout 重定向不是产品持久化合同。
3. **失败边界**：日志后端写盘/格式化失败不能覆盖原始业务错误，不能递归再记同一个失败；异常终止不承诺异步尾部必达。崩溃转储是另一个能力，不由日志库自动提供。
4. **线程与生命周期**：worker 能安全记录；编辑器输出在宿主线程消费；异步记录拥有所需文本，不捕获 Session/Runtime/Surface 裸引用；多个 Runtime/Context 能区分，不依赖唯一全局业务上下文。
5. **可控制的噪音和成本**：分类/级别过滤，Release 可以裁掉详细诊断；窗口/准备状态只在变化、异常或定向 trace 时记录，不能每帧落盘。日志不替代现有 frame timing/GPU profiling。
6. **证据而非断言摘要**：Renderer 的失败输出应包括请求/接受/capture 帧号、状态、draw count、输入/目标尺寸；菜单需同时知道逻辑 source、实际 popup 接受帧和相关原生事件。换一个输出库不会自动补齐这些观测点。

以上是现状与建设方向，不是建议立即一次实现全部能力。尤其不为日志先建立远程采集、通用事件总线或全局服务定位器。

## 选型边界

### 继续纯自研

如果需求永远只有少量同步 stderr 文本，现有直接输出可以继续工作，新增依赖未必划算。但一旦要求文件轮转、多输出端、线程安全、限流和优雅关闭，自研会把工程时间花在非项目差异化能力上。目前跨进程输出和丢弃策略已经分散，继续逐处追加容易扩大这种不一致。

### 直接全仓使用 spdlog

能很快得到格式化和输出能力，但不能解决领域诊断、错误隔离、历史、Runtime 身份和编辑器线程归属。全仓依赖其类型也会固定业务 API 与库的生命周期/全局注册表选择，不推荐。

### 自有小接口与 spdlog 后端（推荐探索）

自有部分只拥有真实的项目合同：category/severity、必要上下文、宿主初始化/关闭、编辑器有界记录消费，以及现有 Diagnostic 到日志的桥接。文件、控制台、锁、轮转等复用库。保持一个事实源，不在 World 和日志后端分别再造同一份错误状态。

首个增量宜从 Application/Runner 的启动、停止和失败边界开始，加文件与控制台输出；保持 World/Source 的 Result 和诊断 API 不变。优先同步后端，减少线程池/退出顺序复杂度；若真实负载测到同步 I/O 干扰，再启用有界异步，并明确满队列是阻塞还是丢弃，报告丢弃数量。不能因为库支持 async 就默认开启，也不能把 flush 等同于断电持久化。

## UE 与 spdlog 的一手资料对照

UE 值得借鉴的是分工，而非完整复制其宏和全局体系：[category 的默认/编译期/运行时 verbosity](https://dev.epicgames.com/documentation/unreal-engine/logging-in-unreal-engine?application_version=5.8)、[输出设备](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FOutputDevice?application_version=5.8)和[编辑器历史/过滤](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/OutputLog/FOutputLogModule?application_version=5.8)是不同职责。官方的 `Saved/Logs` 文件与 Output Log 面板是不同消费者；UE Fatal 包含终止语义，不能直接对应到只记录 critical 级别的 spdlog 调用。此处使用 UE 5.8 公开文档视图作设计参照，不声称核验其私有实现或当前稳定发行状态。

本次核对 [spdlog v1.17.0](https://github.com/gabime/spdlog/releases/tag/v1.17.0)，固定源码提交 `79524ddd08a4ec981b7fea76afd08ee05f83755d`；MIT 许可，默认 bundled fmt 12.1.0，也支持 `SPDLOG_USE_STD_FORMAT`。接入需保留实际分发依赖的许可。以下约束影响本项目方案，不应被“成熟库”三个字掩盖：

- [异步队列](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/async_logger.h#L21-L68)有 block、overrun_oldest、discard_new 三种策略；默认共享池 8192 条、1 worker。不是无锁队列；多 worker 可能重排，共享池满时也不会自动优待错误日志。正文格式化仍发生在生产线程。
- [async flush](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/async_logger-inl.h#L33-L78)是排队请求，不是完成屏障；[文件 flush](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/details/file_helper-inl.h#L24-L105)通常是 fflush，不是 FlushFileBuffers。显式 shutdown 仍需安排外部保留池/sink 的生命周期，不能承诺崩溃零丢失。
- [callback sink](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/sinks/callback_sink.h#L15-L37)不自动切到 UI 线程；[_mt 的锁](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/sinks/base_sink-inl.h#L25-L48)也不能授权任意并发修改 logger 配置或重入回调。
- [MDC](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/mdc.h#L15-L48)是 TLS，不支持 async。程序/文档/请求身份要在生产侧捕获；[source_loc](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/common.h#L315-L326)里的文件名/函数名是裸指针，不能直接借用短命的 JIT 动态文本跨队列。
- [编译期宏裁剪](https://github.com/gabime/spdlog/blob/79524ddd08a4ec981b7fea76afd08ee05f83755d/include/spdlog/spdlog.h#L278-L348)不等于直接 `logger.debug()` 调用的运行时过滤，也不是 UE 的完整逐 category 编译期上限；禁用级别时是否求值昂贵参数应明确验收。

这些事实支持“自有薄语义 + 成熟后端”，而不是支持现在直接开启全局 async。没有在本库安装、编译、基准测试 spdlog；本轮只完成代码/文档调查。

## 接入前的最小验证

- 锁定版本/来源/hash，匹配现有 C++23、clang-cl、MD/MDd 和 Dev/Release；使用 compiled target，检查 `std::format` 路径以避免不必要的另一份 fmt。先实际编译，不把上游兼容声明当本项目验证。
- 一个真实 Runner 同时向文件/控制台输出，编辑器接收结构化或已定义的桥接记录；核对 UTF-8、长行、并发来源、独立运行标识和关闭后的尾部。
- 用小容量/不可写路径主动验证输出失败、截断/队列溢出政策以及首错保留；不靠 mock 的调用次数证明日志可靠。
- 在真实工作负载下比较关闭日志、正常日志和详细诊断的成本；不为本轮探索另建基准框架。
- 本轮不做全仓迁移。先证明这一个端到端路径，再决定后续覆盖范围。
