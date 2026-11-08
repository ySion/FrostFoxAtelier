# Logging：进程日志与诊断记录

进程宿主拥有日志寿命；Application / Runner 桥接领域诊断；Project Output 从 JSONL 增量消费。[logging.hpp](../../../modules/core/logging/include/logging/logging.hpp) 是公共值与生命周期入口，[native.hpp](../../../modules/core/logging/include/logging/native.hpp) 提供稳定原生代码的编译期格式检查。研究依据及真实反例见[日志评估](../../research/logging-assessment.md)；实际构建和验收结果以本次 acceptance-gate 产物为准，接口说明不替代运行证据。

独立目标 `atelier_logging` 不依赖 Runtime、World、Editor、Application 或 RHI；只有一个 Quill 后端，不做可切换后端、通用订阅总线或任意用户 sink callback。Quill 版本由 [dependencies-lock.json](../../../cmake/dependencies-lock.json) 锁定，保留 Quill 和 bundled fmt 的许可。domain 头只依赖标准库；native 便利头**确有 Quill 编译依赖**，但不公开 logger/sink 所有权。

## 公共入口

全部位于 `atelier::logging`，`Result<T>` 是本模块的 `std::expected<T, Error>`，不是 `atelier::Result`，不引入 runtime Error。

| 调用方 | 入口 | 责任 |
|---|---|---|
| 宿主 | `Result<std::unique_ptr<Run>> start_run(RunOptions options = {}) noexcept` | 显式开始进程 run，返回唯一拥有者 |
| 宿主 / 查询 | `Run* current_run() noexcept` | 借用受控的活动入口，不自动初始化 |
| 宿主 / UI | `const RunInfo& Run::info() const noexcept`；`Health Run::health() const` | 目录、运行身份与拥有型健康快照 |
| 宿主 | `FlushReport Run::flush() noexcept`；`FlushReport Run::finish() noexcept` | 显式完成应答、sink 检查、健康发布；不能用返回 void 代表成功 |
| 普通动态文本 / JIT 桥 | `SubmitResult submit(Entry entry) noexcept` | 非模板稳定宿主入口，拥有正文及动态源码信息 |
| 调用方过滤 | `bool enabled(Severity severity, std::string_view category) noexcept` | 不分配、无 I/O；直接 submit 调用不会阻止实参先求值 |
| 稳定原生 | `ATELIER_LOG(level, category, code, format, ...)` | 先过滤后求值，返回 SubmitResult；位置格式编译期校验 |
| 稳定原生 / 带字段 | `ATELIER_LOG_WITH(level, category, code, context, format, ...)` | context 是 `native::Context { span<const Field>, optional<Repeat> }` |
| Output | `Result<std::unique_ptr<Reader>> Reader::open(std::filesystem::path directory)` | 一目录一 reader，无本进程 Run 也能使用；允许目录尚未出现 |
| Output | `ReadBatch Reader::poll(ReadBudget budget = {})` | 增量读取、拥有型 Record、累计 Reader 状态与最近健康文件快照 |
| Application / Output | `void Reader::source_ended() noexcept` | 宿主确认生产进程实际退出后通知；没有正常 final 也能排空并标不完整收尾 |

`native::write` 的签名是 `template<Severity Level, Argument... Args> SubmitResult write(std::string_view category, std::string_view event_code, const Context& context, std::source_location source, std::type_identity_t<native::FormatString<Args...>> format, Args&&... args) noexcept`，要求 `Level != off` 且至多 16 个参数；格式参数不参与类型推导。

## 生命周期与目录

1. **每进程最多一次成功 start**；失败的初始化完整回滚后才允许重试。无同进程 restart；多个 Runtime 共享此进程设施而不是共享业务上下文。start/flush/finish 由同一宿主线程串行调用。
2. Run 必须在 Application、Project、World、设备等业务拥有者创建前存在。业务作用域销毁、worker join、最外层 catch 中记录原始异常均发生在 finish 之前。调用方停止一切生产者后才能 finish，接口不代替线程协调。全局静态析构不再记录。
3. `current_run()` 在成功 start 后非空，在 finish 完成后为空；指针仅在宿主保证的生命周期中借用，不能与 shutdown 竞争。finish 幂等，Run 对象中的 info/最终 health 仍可查询。析构只能做紧急收尾；未显式 finish 要保存 `lifecycle_violation`、走非递归紧急输出，不能写出“已审查成功”。
4. `RunOptions.directory` 空时创建 `%LOCALAPPDATA%\FrostFoxAtelier\Logs\<run_id>`。已给目录必须绝对且尚不存在，含 NUL 在任何 IO 前返回 `invalid_options`，以独占创建拒绝复用；不回退到工作目录，不截断旧 run。`run_id` 是不复用的日志运行标识，不是新的业务对象身份。
5. Editor 为 Runner 预选自己的 run 目录下尚不存在的 `runner-<launch ordinal>` 子目录，通过进程启动参数传该路径及父 `run_id`。Runner 自己 start、写盘和 finish。Editor 可在启动子进程前 open 对应 Reader；日志不进入 Live ACK 或 Control 链路。具体命令行拼写由 Application 集成实现，公共类型不依赖它。
6. 每 run 有 `run.json`、`events.jsonl`、最多八个 `events.1.jsonl`…`events.8.jsonl`、`health.json` 和活动锁。`.1` 是最近的轮转文件；文件名会移位，**不是文件身份**。文件名本身不承担记录顺序。
7. 默认根目录保留最近 20 个已结束 run；只清理本模块 manifest 合法、最终标记存在且能独占获取活动锁的目录，启动时执行。显式目录的跨 run 保留归调用者，本模块仍执行该 run 的文件轮转。活动、异常未完成或未知目录不自动删除，因此“20”不是根目录总硬上限；不能仅凭 PID 或目录年龄判死。活动锁在整个 Run 生命周期保持。

`start_run` 的可恢复配置、路径和 IO 拒绝返回 expected 错误；`submit`/native::write 的输入与预算拒绝返回提交状态。CPU 分配、线程建立失败，以及 Quill 固定配置或生命周期不变量破坏走唯一 fatal，不转换成 `internal_failure` 后继续。`finish` 仍等待真实排空和 join；若 stop 返回后 backend 仍在运行则 fatal，不发布伪完成。初始化正常失败仍用不递归的 stderr/系统输出报告，由宿主消费结果。

自有配置检查、身份生成、默认目录、文件打开/写入/原子发布通过显式结果传错；`start_run` 在发布活动 Run 前检查每一步，正常失败共用作用域回滚：退休已建 logger、关闭 sink 和活动锁、清宿主 producer，只删除本次确实创建的 run 目录，不删除已存在的目录或父目录。`run_json → path_text` 直接消费 Platform 严格 UTF-16 转换结果，孤立代理项仍为 `invalid_options` 并保留 native_code，合法路径保留 generic `/` 字节；不通过异常搬运 IO 或路径错误。

默认根保留使用显式的目录枚举、条目查询和递增错误；枚举失败中止本次保留，不按部分候选表删除。未知、无法读取、超限或未完成的元数据不列为候选；活动锁共享拒绝不删除对应目录，其余锁/删除失败保留原 `retention_failed` 分类。

## 记录、身份与源码

`Entry` 是领域提交数据；`Record` 额外保存 run/process/thread/producer 序号、时间和输出序号，所有可变文本及字段都有自己的所有权。

- **唯一记录键**：`(run_id, process_id, producer_id, producer_sequence)`。OS thread ID 可能复用，因此不能拿它代替 producer。生产线程首次被准入时分配 producer ID，run 内不复用；序号从 1 开始。过滤、结构无效及生产者上限拒绝不占序号；其余已准入尝试在入队前占序号，包括大小拒绝、队列拒绝和内部失败。
- `record_sequence` 是单一结构化 sink 在序列化时分配的从 1 开始的输出序号，不是唯一身份，也不等于写盘成功数量。跨线程 Quill 合流、时间戳先后和它都不建立业务因果。请求、帧、代码版本等用原有业务值关联。
- `category`、`event_code` 为稳定非本地化 ASCII 标识：分别 1–64、1–128 字节，字符限字母、数字、`_`、`.`、`-`。`off` 只用于阈值，不能成为 Entry 级别。Source/World 的 Error、Result、隔离策略、持续失败去重仍是权威。
- `fields` 最多 **64 项**，仍共同受记录总字节预算约束；键唯一，1–48 字节，字符规则同 category。值仅 bool / i64 / u64 / 有限 double / 拥有型 UTF-8 string；单个字符串至多 4096 字节。字段名与单位由稳定 event code 的生产者维护，不接受任意反射、formatter 或回调。推荐复用 `document`、`asset_id`、`scene_local_id`、`request_generation`、`frame`、`code_version`、`phase` 等已有含义；Source 计数/资源快照用独立命名标量，例如 `counter.native_call`。领域字段路径可用 `field_path` 字符串按 JSON Pointer 编码（`~`→`~0`、`/`→`~1`），不丢路径，也不把全部 payload 偷塞进 message。
- **不另造 ObjectId**。宿主调用已有 `ObjectId::diagnostic_identity()`，把拥有型 ASCII 结果作为 `object` / `entity` 等 string 字段提交：`null` 或十进制 `context:index:generation`，与记录的 run/process 联合限定进程内诊断关联。它不赋予对象存活性，也不是资产持久身份或反序列化入口。项目文档/资产/scene-local 身份仍可一同保留；不能把地址、ObjectId 哈希、私有内存布局当作无冲突身份。logging 只接收这个值，不依赖 Runtime、不建业务对象注册表。
- `SourceLocation.file` 保留生产者的原始文件名；原生相对路径可携带当时的绝对 `root`。Source 快照另用 `disk_path` 保留已保存文件的绝对来源，不能把 `engine/...` 快照名拼到工程 root。没有磁盘映射的虚拟来源仍能记录，但不允许按当前工程猜位置。file/root/function/disk_path 各至多 4096 UTF-8 字节且不含 NUL；非空 root/disk_path 必须绝对。
- line、column 均 **1-based**，column 是 **UTF-8 字节列**；0 表示未知，不表示第一列。version 为原始 Source 版本，offset 为原始文件的 **0-based UTF-8 字节偏移**。桥接若没有可靠列转换应填 0，不能拿 UTF-16 列冒充。native 编译器提供的列也须符合此定义，否则记 0。跳转按记录路径/root 与版本检查，不让切换工程后的同名文件误命中。
- 正文允许合法 UTF-8、嵌入 NUL 与控制字符，不能用 strlen 保存 `std::string`/string_view 的显式长度。非法 UTF-8 为 `invalid_record`，不悄悄替换为问号或 `\\xNN`。正文超过 128 KiB 明确拒绝；其他输入结构无效也明确返回状态，不静默截断正文。

Source 的 `path/version/offset/line/column/disk_path` 完整转入上述对应字段；只有带磁盘映射的 version 才用于文件内容摘要检查，虚拟请求序号不是磁盘摘要。缺失 Source 使用空文本、零行列和 null version/offset，不捏造位置。Source 逐调用 Diagnostics 的桥接是按需能力，**不能为了日志默认打开昂贵的逐 tick / 逐调用诊断或计时**。对象诊断身份由 Runtime 的上述正式接口导出，本模块不规定其私有布局。

### 显式 repeat，而不是相同文本合并

`Entry.repeat` 缺省表示独立一次。设置时 `Repeat.identity` 是 1–256 字节的非 NUL UTF-8，生产者在 `(run_id, process_id, category, event_code)` 内提供稳定、无歧义的领域重复组；两个 World/文档的局部 serial 相同不能误合并。它只标识诊断重复组，不创建业务身份。

`Repeat.count > 0` 是**本条带来的发生次数增量**，包括本条所代表的首次发生。UI 对该重复组累计，不能把每条当历史累计总数再相加。已有领域接口若返回累计数，桥接在源端计算 delta；没有新 delta 不重复提交。唯一记录键去重必须在 repeat 累加之前。无显式 repeat 不凭相同 message 合并，跨 group 也不合并。

## 两种提交路径

**稳定原生**：只有整个 run 生命周期都不卸载、并链接进同一宿主二进制的代码使用 native.hpp（包括宿主链接的静态库）。当前 `atelier_logging` 为静态目标，不能把它另行链接到 DLL 后期待共享同一个进程服务；header-only Quill 的 registry/TLS 也不能在 DLL 中复制。包括常驻 DLL 在内的其他二进制模块，均通过宿主提供的稳定 submit 入口提交。Dev 编译最低级别 trace，Release 为 info；运行时再按 RunOptions.minimum 与最多 64 项 category 精确覆盖过滤，阈值启动后固定。宏只在运行时过滤时计算 category 一次，event code、Context 和格式参数都在过滤通过后求值；编译裁掉的级别不求值 category。每个启用参数求值一次。

`native::FormatString` 只从编译期字符数组构造，内部 `fmtquill::format_string` 仍执行完整类型/语法检查；随后在 consteval 内复用 fmt 的解析器检查本项目政策，不复制语法、不在前台格式化。宏与直接 write 使用同一类型，不接受 runtime format、外部 fmt 格式对象或运行时 string_view。参数仅受约束的算术标量、char 数组、std::string、std::string_view，拒绝任意类/枚举/裸指针/自定义 deferred codec；枚举显式转成稳定数值或名称，char 数组要求有效 NUL 终止，带 NUL 文本用显式长度视图。正文格式化在后台，字符串与 Context 在调用返回前编码复制。source metadata、decoder、内部 envelope codec 都必须驻留。不要为了适配 schema 把所有普通原生正文提前 std::format。

动态宽度、动态精度（包括 `{:{}}`、`{:.{}}`、`{0:{1}}`、`{0:.{1}f}`）一律编译期拒绝，诊断区分宽度和精度。所有浮点格式的静态精度不得超过 `maximum_message_bytes`（131072），超限诊断指出浮点精度；默认精度、字符串精度、静态宽度及转义括号保持 fmt 语义。这是有意的保守收窄：NaN/Inf 和去尾零后原本很短的有限值也受限，例如旧 `{:.131073g}` 配 1.25 可以输出 `1.25`，现在拒绝编译；不是静默截断。精度上限避免 fmt 精度与指数运算溢出，不保证最终正文不超限。允许的精度配 f/e/a 或静态大宽度仍可能展开超过 128 KiB，交由原 sink 记录 `output_too_large`，不输出截断正文。

Entry 的文本在准入边界校验；编码与已准入诊断快照使用 JSON 的已验证写入入口，不重复扫描 UTF-8。快照截断只保留完整码点。后台新生成的正文先单次校验 UTF-8，再检查大小，再按已验证文本写入；两者同时失败时，首因仍为 `invalid_utf8`。JSON Value 只借用稳定 DOM，正常导航不拼路径，失败才定位并生成含数组下标的 JSON Pointer；移动 owner 保留借用，销毁 owner 或替换子树使对应旧借用失效。

**动态/JIT/可卸载 DLL**：提交拥有型 Entry 到稳定宿主中的 `submit`。库宏、metadata、decoder 模板实例化均在不可卸载宿主；不得借用业务对象、DLL 内静态 metadata、函数指针或 DeferredFormatCodec 对象。动态 file/root/function/message 都复制到队列；返回后可释放源文本及卸载生产模块。此入口是当前锁定工具链/同 CRT 的 C++ 合同，不宣称跨编译器或语言 ABI；JIT 的既有宿主桥负责组包，不把 std::string ABI 写入语言定义。

```cpp
namespace log = atelier::logging;
auto started = log::start_run({.process_name = "editor"});
// 检查 started；初始化失败走宿主紧急输出，不创建 Application。
auto run = std::move(started.value());

const auto queued = ATELIER_LOG(log::Severity::info, "application", "application.started",
    "Process {} started", run->info().process_id);

log::Entry diagnostic;
diagnostic.severity = log::Severity::error;
diagnostic.category = "world";
diagnostic.event_code = "world.behavior_failed";
diagnostic.message = "行为执行失败：原始诊断正文";
diagnostic.source.file = "E:/OriginalProject/Source/player.source";
diagnostic.source.line = 12;
diagnostic.source.column = 5;
diagnostic.fields.push_back({"scene_local_id", std::string{"player"}});
diagnostic.repeat = log::Repeat{"document-7/world-2/diagnostic-19", 3};
const auto submitted = log::submit(std::move(diagnostic));

// 正式调用点在业务销毁、join 和最外层 catch 之后：
const auto final_report = run->finish();
// 宿主审查 report；severity/first_error 本身不等于退出失败。
```

上例是 Interface 用法，不是完整 main；关键是 Run 拥有者在业务 try/catch 外，finish 在其后。`accepted` 只代表入队，不代表已显示或已写入文件。

## 队列、健康、首因与完成

| 固定初始政策 | 值 / 边界 |
|---|---|
| 前台 | 唯一 FrontendOptions：BoundedDropping，逻辑容量每线程 256 KiB；Quill backing 为两倍 |
| 生产者 | 同时最多 128 个仍存活或待排空 producer，含宿主；准入前不能触发 Quill TLS 队列建立；线程退出且已接纳记录排空后可复用槽位，producer ID 永不复用 |
| 单条 | 实际编码入队总尺寸上限 192 KiB；正文上限 128 KiB；UTF-8 JSON 行连换行上限 2 MiB |
| 后台 staging | initial = 8、hard = 8 **每生产线程**；soft = 8 **全局软阈值**；停机等待所有队列排空。固定版本要求 soft ≤ hard，不能用早期草案的 256/8 组合 |
| 后台周期 | 初始 sleep 100 μs；健康文件最多每秒刷新一次，flush/finish 时立即尝试发布 |
| 文件 | 16 MiB 轮转，8 个备份 + 当前文件；启用覆盖最旧备份；不启用 crash/signal 接管或 backtrace |
| 同步保存 | 首个 error/critical 快照最多 8 KiB；设施/notice 首条详情各最多 2048 字节；无预注册匹配表或观察器 |

这是可执行初始设置，不是性能胜出、前台无分配、全流程硬内存上限或最大运行线程需求的测量结论。队列、后台字符串和 STL 开销分开计算；格式宽度展开可能产生大于输入的临时结果。原生输出大小只能在格式化后确认，超限记录设施 `output_too_large` 并不输出假成功正文；严格限制格式化临时峰值需要另做实现验证，不声称已保证。显式字段总量及实际编码尺寸都要检查，不能只用 message.size 估算。

短命线程的槽位通过固定版本 Quill `ThreadContextManager` 协调退休：新 producer 准入时，在 manager 锁内比较已登记 context 身份；只有 Quill 已移除 context 才回收槽位。该版本后台仅在线程已退出、SPSC 和 transit buffer 均排空后移除 context。比较不解引用过期指针；准入串行化，并在创建新 TLS context 后清除可能在扫描后被后台退休、恰好被新分配复用地址的旧槽位，避免 ABA 产生永久占位。producer ID 单调不复用。宿主控制线程在 start 预留一个槽位，flush 不绕过并发预算。整个进程不能另用其他 Quill frontend/自行启停 backend。这是与锁定版本内部机制的明确耦合，升级必须重查；并发预算不是进程总硬内存上限。

Quill TLS context 初始化中的 CPU 分配失败走 fatal，不缓存 exception_ptr，也不重试半初始化的线程状态。队列满与并发名额耗尽仍分别显式拒绝，不与 CPU 内存耗尽混同。持久格式中的 `internal_failure`/`allocation_failed` 枚举保留以读取旧证据，不再作为本机 CPU OOM 的恢复通道。

- **精确拒绝计数**来自本模块每次提交的真实返回：Quill log_statement 的 bool、准入/大小/有效性检查；不从英文 notice 反推。filtered 和无 run 不计作队列丢失；每次失败只落一个提交拒绝分类。串行 finish 后计数为最终精确值；生产中的 Health 各计数单调，但不承诺跨字段同一时刻的事务快照。
- 设施故障单独计数并保存 `first_facility_issue`。自有 sink 在真实 open/write/flush/rotation/summary 边界记录 typed fault；不能依赖 Quill 将异常转成英文通知后再解析。Quill `error_notifier` 只增加 `backend_notifications` 并保留首条原文，这些可能是正常丢弃 notice，也可能是格式化失败，**未分类即仍有不确定性**，不能一概当业务失败或一概忽略。固定版本内部吞掉的失败路径必须另作边界观测，不能假称成功。
- 同步首因在通过过滤和结构校验后、队列容量决定前保存。因此队列满或尺寸拒绝不抹掉首因；已过滤、无效输入和 producer-limit 拒绝不保证捕获领域首因。首个 error/critical 是**最先抢到宿主保存槽的合格提交**，不声称跨线程真实因果最早；业务 Result 原始失败仍由业务拥有者保存。
- 原生首因只保存模板与领域 metadata/fields，`message_is_template=true`，不为此同步格式化所有原生 error；拥有型路径保留已给正文。8 KiB 限制按 UTF-8 边界裁剪快照、设置 truncated，不冒充完整正文。内部存储在 start 时预留，故障记录不得依赖再次成功入队。**不提供诊断选择器、匹配计数或订阅回调**；验收同步锁存业务结果，checkpoint 读取结构化文件核对必要日志与精确期望，不让产品 logger 承担验收观察框架。
- `flush()` 是无超时的阻塞控制操作，控制消息也可能等待队列；不是持续并发生产者的全局截断屏障。`backend_acknowledged` 只说明后台完成应答，`sink_flush_succeeded` 必须来自该次自有 sink flush 的真实结果；`summary_published` 是健康文件替换成功。不调用或暗示 FlushFileBuffers，因此不保证断电落盘。
- `finish()` 的先决条件是全部生产者停稳；排空、sink 最后检查、停止后台、取最终健康并发布 final summary 后返回。报告里的三个 bool 不能替代之前的拒绝、设施故障和未知 notice。完成 run 不代表业务成功，ERROR 也不自动终止进程。
- 健康文件发布失败时不能递归记日志补救；本地返回/health 保留 `summary_failed`，磁盘消费者看到旧时间或缺 final，不能伪造最新成功。flush/finish 不承诺任意 deadline、强制终止、崩溃或 SEH 下零丢失。

当前 sink 使用直接 Win32 `WriteFile`，没有额外 stdio 用户缓冲；`sink_flush_succeeded` 检查本次操作的句柄、当前路径与文件身份，并保留此前写失败。它不是 `FlushFileBuffers`/耐久性证明。显式 flush 结果与周期 flush 分别保存，后台周期检查不会覆盖该次显式结果。写失败可能已写半行，故停止该文件后续输出，不自动重试拼接；轮转失败也停止该 sink 后续文件修改，保留 typed health。析构紧急收尾保存 `lifecycle_violation`，只发布非 final 快照，不能冒充显式 finish。设施详情和首条 notice 限 2048 字节；合法 UTF-8 保留原文前缀，罕见非法 UTF-8 以 `raw-bytes-hex:` 保留可逆字节前缀，仍不推断分类。

sink 构造只建立值，创建者在交给 Quill 前显式打开文件；每次完整写入成功后才更新文件大小。写入、打开及轮转拒绝保留原 `FacilityFault` 和 native_code，写失败状态不会被后续 flush/finish 应答抹掉。健康快照发布也消费原子写结果：写临时文件或 rename 失败不破坏旧目标；rename 后关闭句柄失败仍报告失败，但目标可能已被替换，不宣称回滚。临时文件处理沿原政策，不新增重试或清理策略。

现有 sink 的 `close()` 若关闭失败，会记录 `file_flush_failed` 和原 `native_code`；轮转的 close 步骤不据此短路，只有后续 delete、rename、open 按既有规则判定为拒绝时才停止后续轮转，不保证关闭失败后停止或回滚。

## 持久格式 v1

`events.jsonl` 为 UTF-8、无 BOM、每条一个 JSON object + LF；message 必须是 **rendered 正文**，不是 Quill 默认 JsonSink 的格式模板。关闭 Quill ASCII 字符转义 sanitizer；JSON 层按标准转义引号、反斜杠、NUL/控制字符，读回须逐字节还原合法 UTF-8 正文。不能在正文前拼接不可逆元数据再靠字符串拆解。

文本链使用 [core/json](../../../modules/core/json/include/json/json.hpp) 的 yyjson DOM 和显式 Result，不再依赖 nlohmann。对象键按 UTF-8 字节序写出，控制字符的十六进制用小写，非 ASCII 和 `/` 不转义；有限 double 使用 yyjson 的最短往返输出。日志保持 compact，只有 JSONL 行追加 LF。共享 writer 同时提供 2 空格 pretty 和独立的末尾 LF 选项，排序只暂存成员引用，不复制整棵 DOM；输出为字节 Array，不重复验证 UTF-8 或转换成 String。

JSON core 拥有语法、UTF-8、解码后重名键和入口传入的 event-depth 检查；logging 仍使用上限 16，并拥有文件/行大小及 schema 规则。schema 解码不重复检查深度或重名。语法错误带字节位置，重名和类型错误带 JSON Pointer 结构路径。Document/Builder 独占 DOM；Value 和字符串视图不得超过所属 DOM 的寿命，Builder 的节点不能跨 owner 使用。

JSON writer 迁移只改变写出字节，不改变日志 schema v1 或字段含义；日志没有持久化 JSON 字节摘要。Reader 对合法成员顺序、空白和数值拼写不作 writer 字节比较，因此同一 schema 的旧日志直接经当前解析路径读取，不增加旧格式分支。自有 IO 和 native 路径的结果迁移不改变合法 JSON 字节。

固定顶层成员：`schema`、`run_id`、`process_id`、`thread_id`、`producer_id`、`producer_sequence`、`record_sequence`、`timestamp_unix_ns`、`severity`、`category`、`event_code`、`message`、`source`、`fields`、`repeat`。映射至 Record / Entry；`persistence` 不落 JSON，由 Reader 成功解析完整行后设置为 jsonl。

- schema/process_id/source.line/source.column 为 JSON 整数，其余所有 64-bit 整数按规范十进制 **字符串** 保存，避免跨消费者 53-bit 精度损失；无前导 `+`/多余零。timestamp 是 Unix epoch 纳秒，不是排序保证。
- source 固定为 `{file, root, function, line, column, version, offset, disk_path}`，缺省 version/offset 为 null。无位置时文本为空、行列 0。
- fields 为键到 `{type, value}` 的对象，type 只能 `bool`、`i64`、`u64`、`f64`、`string`；整数 value 是十进制字符串，f64 必须有限。这样非负 i64 不会悄悄变成 u64。
- repeat 为 null 或 `{identity, count}`，count 是非零 u64 十进制字符串。无未知字段兼容分支；结构、范围、UTF-8、重复键及当前 schema 都要验证。

```json
{"schema":1,"run_id":"example-run","process_id":42,"thread_id":"7","producer_id":"1","producer_sequence":"9","record_sequence":"8","timestamp_unix_ns":"1793232000000000000","severity":"error","category":"world","event_code":"world.behavior_failed","message":"失败\n正文","source":{"file":"player.source","root":"E:/OriginalProject/Source","function":"tick","line":12,"column":5,"version":null,"offset":null,"disk_path":""},"fields":{"scene_local_id":{"type":"string","value":"player"}},"repeat":{"identity":"document-7/world-2/diagnostic-19","count":"3"}}
```

`run.json` 是 `{schema, run}`，run 成员按 RunInfo 同名保存（路径为绝对 UTF-8 字符串，parent_run_id 可 null）。`health.json` 是 `{schema, run_id, process_id, updated_unix_ns, final, report}`，report/health 按公开结构同名，枚举用声明的 snake_case 拼写，optional 用 null；其中捕获的 Entry 用同名对象，fields/repeat/source 编码同上。文件采用同目录临时文件 + 原子替换，发布成功的 report.summary_published 为 true；失败时不能替换成半份 JSON。仅 final=true 且 health.state=finished 表示审查式收尾完成，不表示无错误。Reader 拒绝 manifest 与记录 run/process 不符，健康快照也必须同 run。

## Reader 与 Output 消费合同

Reader 不在后台 sink 中调用 UI，也不经 UI 反向背压唯一 Quill worker。一目录一实例、一个 owner 线程，不保存 UI 历史。默认每次 poll 最多 **256 KiB 新读字节 / 256 条完整行处理**，2 ms 为工作单元间协作检查而非操作系统 I/O 硬超时；坏行也消耗行预算，manifest/summary 的读取也消耗字节预算并允许跨 poll。正时间预算至少允许一个有界输入工作单元，避免目录扫描较慢时永远没有读取进展；零/负时间预算不做读取。单个解析工作单元可能超过时间预算。没有后台 writer 时，Reader 同样可读归档。

`Reader::source_ended()` 由 owner 线程在确认生产进程实际退出、或确认归档不会再写入后调用，幂等且不做 I/O；停止请求、超时意图不等于实际退出。有效 final summary 也可表明输入结束。`ReaderStatus.finished` 只表示**输入已结束且所有可取得的保留内容已检查完**；`has_final_summary` 单独表示存在有效正常收尾证据，二者不可混用。`incomplete` 粘性保存缺终结证据、残缺尾或其他已观察数据损失；正常 final 仍不能抹掉之前的损失。

| 观测 | Reader 的明确行为 |
|---|---|
| 尚未建立目录/manifest | 输入仍活动时 waiting_for_run；source_ended 后检查完可用内容即 incomplete + finished，不无限等待也不凭空解释无 manifest 记录 |
| 活动文件末尾没有 LF | 保存最多 2 MiB 的半行，partial_line=true；后续接着读，不提前解析为坏 JSON |
| 当前正常 EOF | caught_up=true；不是 run 已结束，也不是所有生产者 barrier；partial_line 可同时为 true |
| 输入已结束后仍残缺尾行 | incomplete_lines 增加、incomplete=true、保存 incomplete_line issue，丢弃这段残尾并继续收尾，不等待永远不会来的字节 |
| 已 source_ended 且最终 EOF 仍缺有效 final | has_final_summary=false、incomplete=true，保存 missing_final_summary issue；finished 可以为 true，不伪造正常终结证据 |
| 轮转后的旧文件残尾 | 明确 incomplete_line，绝不能把旧尾和新文件头拼起来 |
| 轮转/保留删除未读文件 | rotation_losses 增加，设置 unquantified_loss，报告路径/offset；有多少条未知就不编造 |
| 坏 JSON、坏 schema 或非法 Record | invalid_lines 增加、保存对应 issue，跳过该完整行，继续后续合法行 |
| 坏 manifest / health | invalid_metadata 增加，保留 issue 与最近合法快照；不混入 JSONL 坏行计数。按已读取的原子文件版本更新，不用可回拨的墙钟比较新旧；final 回退为非 final 明确报错 |
| 超过行上限 | oversized_lines 增加，有界丢弃到下一个 LF；不无限缓冲，也不生成被截断的假 Record |
| 序号断续 | sequence_gaps 只计观测到的不连续区间或 final 输出序号与已读高水位不符；可能是过滤、范围起点、拒绝、坏行、I/O 或保留，不推断队列 drop |
| 已确认重复唯一记录键 | duplicate_records 增加并不再次返回该 Record，不让 repeat 累加两次 |
| 低于已交付输出序号高水位的其他记录 | out_of_order_records 增加并跳过；可能重放或坏文件倒序，不凭有界历史断言它必然曾交付 |
| I/O 错误 | io_failures + latest_issue，保留可重试游标，不伪造 EOF 或 final |
| poll 分配/快照构造的 CPU 内存耗尽 | 按唯一 fatal 合同终止，不作为可恢复批次交付错误；不承诺致命退出后的游标恢复或继续交付 |

Reader 的 I/O 失败经显式结果进入原状态通道；元数据读取失败关闭当前句柄、清理未完成缓冲，保留已加载版本，后续 poll 可重读。扫描中的轮转竞态只结束本次扫描并留待下次 poll，不是空文件集、EOF 或新增错误码；本批已有记录仍正常交付。Reader 与 sink 的文件身份检查复用同一个显式结果入口；sink 的 flush 检查失败仍记录 `file_flush_failed` 和原 native_code，并将本次结果置为失败，成功的身份检查也不能抹掉先前写失败。

Reader 打开/读取、sink 检查/写入、原子快照及启动 IO 的系统错误文本统一经 Platform 输出 UTF-8，不再沿 `what()` 或 CRT 本地编码；这是有意的诊断文本变化，错误分类及既有原生错误码通道不变。自有 Logging 已不以异常传错；Quill 后台内部仍有异常边界，见下方 W5 限制。

Reader 打开文件时允许 writer 的 read/write/delete sharing，以 Windows 文件身份与偏移跟踪 rename；不能仅按 events.N 的名字续读。按最旧保留文件到当前文件消费，新旧目录快照不一致时重扫而不拼接。利用输出序号高水位与文件游标定位进度，最近 4096 个唯一记录键作有界去重；已经离开这个窗口的旧输出序号只能判重放/倒序，不假称精确知道过去是否交付。producer 只受并发预算限制，不能用“run 一生只有 128 个 producer”作为 Reader 内存有界的前提。重复/倒序异常均须可见，不能以无限 set 掩盖问题。运行中 health 只有最新成功快照，UI 显示 updated_unix_ns/是否 final，过期不被解释为健康。

```cpp
auto opened = atelier::logging::Reader::open(runner_directory);
if (opened) {
    auto batch = opened.value()->poll();
    // 移交 batch.records 给 Output；分别展示 batch.status 与 batch.health。
    // 正式消费者持有 Reader 跨帧继续 poll，不每帧重新从头 open。
}
```

Output 拥有缓存与过滤，不把缓存混入 Reader：**最多 4096 条 / 8 MiB 原始正文总字节**，两个条件任一到达即淘汰最旧条目。记录自己的 `evicted_records` 和淘汰正文字节，并明确展示；这不是生产 drop、Reader 缺段或文件删除。过滤只改变可见行，不抹掉健康状态；缓存预算之外还要考虑 source/fields/索引开销，不把 8 MiB 宣称为全部 UI 内存。

Output 共用 `entry_problem` 的领域校验，不维护第二套字段规则。健康展示只在记录、计数或来源状态变化时重建；排空的 Reader 立即释放工作存储，仅保留最终状态、健康与目录。宿主 `filtered` 不进入展示缓存；明确拒绝的用户消息可保留为未持久化记录，不确定的内部提交结果不能重投。

编辑器的 Session 准备、材质加载和 Application 操作失败保留原始 Error：正文不添加对象、路径或回退说明前缀，`error_code` 保存业务错误码；存在 source 时，`error.path/version/offset/line/column/disk_path` 六个字段完整保存。对象、阶段、属性路径等上下文仍在独立字段中。导航位置使用日志的有界行列，无法表示的行列记为 0，原值仍可从 `error.*` 读取；没有 source 不推导位置。材质只在错误已标识 Reader 的同一路径和内容版本、且 disk_path 为空时关联该精确输入的保存文件，不覆盖原业务原因的定位。

宽容加载仍保留作者值并使用既有替代资源，严格读取（包括已缓存的替代资源）返回原错误。通知失败不撤销已完成的提交；提交状态与原始错误分别记录，取消和历史语义不变。Application 经同一正式日志/Reader 链把错误交给 Project Output，不依赖临时 stderr；日志本身的容量、过滤和失败边界仍按本模块合同处理。

Runner 的 stdout/stderr 是独立原始字节流，唯一消费入口保留流身份、原目录、offset、已知丢弃/放弃字节及未知尾损失。跨块 UTF-8 最多保留三个待续字节；无换行尾段照常交付。原始字节转为父宿主 `process.raw` 的 `raw.stdout` / `raw.stderr` 记录，非法字节编码为 `\xNN`、原反斜杠加倍，`bytes`/`offset` 仍指原字节，不冒充子进程 JSONL。`raw.transport` 独立记录 EOF、读取错误和损失变化；停止后继续排尾，下一次成功启动前完成交接。

CodeDocument 上限为 1 Mi UTF-16 code units 且拒 NUL，因此 UI 对控制字符/NUL 做可见转义，**原始 Record.entry.message 保留不变**。128 KiB UTF-8 正文即使按每输入字节最多六个可见字符转义也小于该文档上限；不要把不限长 metadata 拼进同一正文文档再截断。使用全文详情而不是永久缩略原文；空消息、换行、中文、NUL 都要能查看/导出。源列跳转在原始 UTF-8 与 UI UTF-16 之间显式换算。

无进程日志宿主的既有文档消费者可把**同一种 Record**直接注入 Output，必须标记 `Persistence::not_persisted`，不经 logging 自动启动 Run，也不绕到隐式后台。独立 Output/文档会话显式拥有一个临时日志流 run_id 和自己的 producer/sequence；这是展示流身份，不是业务对象注册表。注入点验证身份非空且序号唯一，不能用全零键去重所有文档诊断；若调用方缺身份，由独立 Output 会话在接纳边界补齐。该流不写 JSONL，UI 明示“未持久化”。已有正式 Run 的生产者只 submit；**正式宿主记录仅从 JSONL/Reader 到 UI，不同时直接追加**，避免 double output。日志模式不能在记录路径上静默在两种来源间切换。

## 实现位置与明确取舍

- [logging.cpp](../../../modules/core/logging/src/logging.cpp)：显式唯一 Run、固定过滤、producer 准入/退休、同步首因、精确拒绝计数、typed sink 健康、轮转/默认根保留、原子健康快照及 finish。首因文本/字段预算是 8 KiB；为了首因保存不再次分配，start 预留字段/字符串容量，其实际 backing 与 STL 开销大于 8 KiB，不冒充进程内存硬上限。
- [detail/native.hpp](../../../modules/core/logging/include/logging/detail/native.hpp)：唯一 BoundedDropping frontend、实际编码尺寸检查及 resident decoder 提交。metadata 常量由宿主 cpp 定义；结构化 envelope 放入深拷贝 runtime tags，正文/标量参数仍由后台格式化。没有用正文前缀拼接元数据。`native_source` 返回 `Result<SourceLocation>`，来源路径拒绝在 native::write 映射为 `invalid_record`；对未获证明的编译器列填 0，相对文件名携带构建时固定的编译工作目录，不读取运行中的工程或 cwd。无 NUL 终止的 char 数组在准入前显式拒绝，不消耗 producer 序号或覆盖首因。
- [format.cpp](../../../modules/core/logging/src/format.cpp)：共享 schema、UTF-8、命名标量和规范十进制校验；拒绝重复 JSON key、BOM、未知成员及超过 16 层的异常嵌套。native.hpp 不包含 JSON 库；JSON 是模块私有依赖。
- [reader.cpp](../../../modules/core/logging/src/reader.cpp)：Windows 共享读/写/删除句柄及文件身份游标；目录快照同时复核存在与缺失的名字，轮转竞态留待下次 poll，不拼接文件。保留一个跨 poll 活动句柄，扫描最多九段；字节预算包含分片读取的 manifest/health（单个最多 256 KiB），记录预算包含坏行；半行有界至 2 MiB，去重窗口 4096 个键。无 Runtime、UI、Quill backend 或 UI 历史依赖。
- **轮转取舍**：固定版本 Quill RotatingFileSink 某些删除路径忽略错误，不能作为 typed health 权威。采用一个自有 Quill Sink 内的固定 Win32 write/rename/delete/close 政策，逐项检查结果；不引入另一条生产队列、可切换后端或通用 sink 框架。本文的命名/轮转容量公共合同不变。
- [quill.cmake](../../../cmake/quill.cmake) 按 [dependencies-lock.json](../../../cmake/dependencies-lock.json) 的 URL 和 SHA256 获取并校验官方 commit 归档，不引用 Temp 克隆。headers 标为 SYSTEM，关闭 upstream 示例/测试/安装；构建输出目录部署 `LICENSE.quill.txt`、`LICENSE.quill-fmt.txt`，保留 bundled fmt 完整首部许可。JSON 的既有许可证接线继续复用。

Quill 的 [atelier.patch](../../../third_party/quill/atelier.patch) 修改两个原位入口。`LoggerManager::parse_log_level_from_env` 不读取 `QUILL_LOG_LEVEL`，直接记为已解析。这是有意删除第三方环境配置功能；宿主创建 logger 后立即设 TraceL3，实际过滤唯一来自 RunOptions，合法环境值原本就会被覆盖，现在非法值也不再拒绝启动。`BackendWorker::_get_local_time_str` 在 localtime 转换失败或 strftime 返回 0 时给出 `[time-unavailable]`，成功仍使用原 `%X`；数量、thread_id、5 秒相同通知去重和 error_notifier 流程不变。此 helper 也服务后台 warning 和 unbounded queue 通知；不更改共享 TimeUtilities、文件名时间或结构化 timestamp_unix_ns。准确丢弃计数仍来自提交结果，不从时间前缀或通知数量反推。

构建保存两个原始 header 与补丁 hash，每次补丁变化从原像重放并校验，部署 `atelier.quill.patch`。升级 Quill 必须重新核对环境变量唯一读取点、创建至 TraceL3 之间无消费者、时间 helper 的所有消费者、补丁重放及以下选项和异常路径，不能复用旧版本原像。`FormatString` 直接复用 bundled fmt 的 detail 解析接口；升级还必须重新核对类型映射、编译期解析上下文、精度/指数范围证明及编译拒绝和成功字节验收，不能仅以 patch 能应用作为兼容证明。

采用两个上游支持的开关：`check_backend_singleton_instance=false` 失去跨模块重复 Quill 副本的检测，单常驻宿主合同仍必须遵守；`QUILL_NO_THREAD_NAME_SUPPORT=ON` 失去 Quill 线程名字，持久 schema 只使用 thread_id，不改变身份。它们不关闭日志文件锁，也不使多个 Quill registry 合法。

W5 前仍保持 `QUILL_NO_EXCEPTIONS=OFF`，不提前改变全仓 EH 设置。QUILL-RESULT 的编译期格式合同与诊断时间降级已消除本模块先前登记的可恢复异常来源；在下表固定合同继续成立的前提下，W5 将 `QUILL_NO_EXCEPTIONS=ON` 与独立的 `FMTQUILL_USE_EXCEPTIONS=0` 配合 `/EHs-c-`、`_HAS_EXCEPTIONS=0` 使用的本模块前置条件已满足。剩余不变量违例和 CPU 分配/线程建立失败按 fatal 处理，而不是继续运行。整仓 W5 仍需其他前置片、有效编译命令和双配置完整门禁；不宣称所有未使用的 Quill 功能都已无异常。

| 锁定 Quill / bundled fmt 的残留点 | 本项目不可恢复或不可达的依据 |
|---|---|
| fmt `parse_arg_id`、`parse_dynamic_spec`、`parse_format_specs`、`parse_replacement_field`、`check_char_specs`、`format_string_checker`、`report_error` | 字面量的语法、类型、引用及本项目政策在 consteval 内检查；运行时格式、命名参数、自定义 formatter 不受支持 |
| char 数组的 cstring/string presentation 差异 | 数组经 `detail::argument` 编码时归一化为 `string_view`；政策检查从实际编码后的类型推导，拒绝 `{:p}` 等后台字符串格式不支持的规格，外层原始参数类型检查仍保留 |
| fmt `dynamic_spec_getter`、`handle_dynamic_spec` | 所有动态宽度/精度编译期拒绝，不靠运行时实参值侥幸通过 |
| fmt `adjust_precision`，`format_dragon` / `format_float` 的两处调用，以及浮点 `write` 的 e/E 精度 +1 | 静态精度 ≤131072，Windows 浮点范围由静态断言限定，远离 int 溢出；不是把格式输入错误改成 abort |
| fmt 字符串空指针、`default_arg_formatter` 缺参、UTF-16 转换和 FILE/console 错误 | 原生参数转为有效借用视图且先校验；参数数量已静态检查；不使用 fmt 宽字符/文件输出，日志 IO 由自有 Win32 Result 路径承担 |
| `LoggerManager` 重名/pending-removal；`LoggerBase` 空 sink/Backtrace；`Frontend` cast/source logger | 唯一成功 start、唯一 run_id、单一 FrontendOptions、先打开有效 sink 再发布；不使用 logger 复制和 Backtrace |
| `BackendWorker` 配置校验/递归 stop/错误 metadata；`Logger` 递归 flush/控制消息装不下 | 固定合法 8/8/8 staging 与非负时间选项，宿主串行 flush/finish，固定 runtime metadata，控制记录适配固定队列 |
| `BackendWorkerLock`；`BackendUtilities` affinity/线程名；`ThreadUtilities` 线程名查询 | singleton 检查关闭，不设置 CPU affinity，线程名支持关闭 |
| `PatternFormatter`、`TimestampFormatter`、`StringFromTime`、`BackendMdcState` 格式配置 | 固定合法配置；正文 pattern 仅 `%(message)`，不启用 Time 属性或用户 MDC |
| `TimeUtilities` 的 localtime/gmtime/timegm | 通知原位消费系统返回值；不使用共享转换器生成日志正文时间，亦不使用 Quill 文件 sink |
| `InlinedVector` 容量/下标；`BoundedSPSCQueue` 容量 | 固定合法队列配置、内部索引/长度不变量；损坏不能当可恢复输入 |
| `BoundedSPSCQueue` 对齐分配；fmt/STL 分配与 std::thread 建立 | 宿主 CPU 资源耗尽走 fatal，不承诺异常退栈或正常关闭 |
| MetricManager、SinkManager 命名工厂、UnboundedSPSCQueue、Filesystem、SignalHandler、独立 File/RotatingSink | 项目不开放这些第三方功能；自有 sink 直接构造、BoundedDropping、无 Quill signal 接管。未来启用必须重新审计 |

## 验收入口与限制

| 正式目标 | 验收内容 |
|---|---|
| `logging_acceptance` | 六个独立真值代表：文本/字段拥有型往返、合法 UTF-8 半行恢复、一次真实轮转、线程退出与 finish 排空、文件锁故障清理、DLL 拥有型边界；期望取自固定输入 |
| `project_integration_acceptance` | 真实 Runner 结果与退出码、精确领域 warning/error 期望、完整 final、强杀残尾及原始输出传输 |
| `editor_document_acceptance` | 共享 Entry 校验、显式 repeat、双缓存预算、源码映射、原始字节转换与增量文件投影 |
| `editor_output_acceptance` | 正式窗口中的过滤、完整只读正文、复制、源码焦点、慢 UI 淘汰与窄 dock |

沿用仓库门禁，不直接并行启动窗口目标。例如只验收日志核心：

```powershell
pwsh -NoProfile -File .\tools\acceptance-gate.ps1 -Targets dev:logging_acceptance,release:logging_acceptance
```

验收不代替业务 Result：预期隔离故障仍可成功退出，正常案例中的额外 warning/error 则失败。DLL 验收只证明稳定宿主的拥有型边界与真实卸载，不保证所有解码都发生在卸载之后，也不扩大为任意 JIT/DLL formatter 安全。原生静态格式宽度展开的瞬时内存仍由 Quill/fmt 分配，不承诺严格临时峰值；文件系统 I/O、flush 与 finish 无硬超时或崩溃保证。所有未知后台 notice 继续作为健康不确定性保留。
