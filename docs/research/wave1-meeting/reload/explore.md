# E 意图探索：请求、发布与退休

日期：2026-09-25。作者：WaveReloadExplore。独立初稿在读取搭档稿前形成，首次文件写入受路径/权限阻碍，改用独占文件精确替换。生产源码只读，未执行构建/测试/探针。已完整读取会议章程及 baseline.json，依据当前固定源码而非不存在的 Git diff。范围限单入口兼容方法体，不扩展一般依赖失效、布局迁移或后台编译。

## 意图与结论

“请求被接受”“机器码准备成功”“新入口已发布”“旧代码可退休”是四个状态。当前未发现可证实的发布/持有缺陷；重用已有执行准入、根管理与 DLL 所有权机制正确。需要新增的是当前版本连续更新分阶段量测，而不是把既有 11×2 验收和请求号溢出再叫新发现。

publish 可能同步析构旧 Version/LLJIT，只报总发布时间不能判断校验成本。历史不保活代码，也不等于源码字符串、诊断和共享 Contract 没有占用。

## 身份、状态机与持有图

| 身份 | 表达 | 不等同于 |
|---|---|---|
| 程序 | State 控制块，弱所有者比较 | 名字、文件或请求号 |
| 类型 | Binding.runtime + TypeId；FieldId/MethodId | 代码 Version |
| 对象 | 所属 Runtime 的 ObjectId/代际 | 地址、代码请求号 |
| 代码版本 | Version(sequence, Contract, CodeOwner) | 新类型或新接收者 |

状态机（business.cpp:401–630）：

- Program：Open/Unpublished → publish(n) → Open/Current(n)；request(n+1) 只更新 latest，不更改 current；析构 open=false、current.reset → Closed，不可重开。
- Request：名称/源码和控制块分配全成功 → Accepted(n)；prepare → Candidate(n) 或诊断失败。新请求接受即使旧候选失去发布资格，最新准备失败不复活旧候选。
- Candidate：Ready → busy → Ready，待宿主重试；foreign/stale/incompatible 也不消费；成功发布 → Consumed，不能再次提交。同一 Request 可重复 prepare，但 published_request 阻止重复发布。
- Version：候选持有 → current；换版后有 Lease/活动局部则 Retained，否则最后持有释放 → Retired。关闭只撤 current，不吊销既有 Lease。

持有图：

    Program --strong--> State --strong--> current Version --unique--> BusinessCode
                           └strong--> Contract                    └shared--> CompiledBusiness --unique--> LLJIT
    Entry --weak--> State
    Request/history --weak--> State；--strong--> Contract；拥有文本
    Candidate --weak--> State；--strong--> Version；拥有 source_name
    Lease --strong--> Version
    Entry.invoke 局部 --strong--> State + Version
    Lease.invoke 局部 --strong--> Version
    atelier_invoke_business 局部 --strong--> CompiledBusiness
    Contract --borrow--> Runtime；拥有类型/入口名称、四份 TypeInfo 快照
    接收者根 --> Runtime 根表 --> Object（代码租约不保活接收者）

已追踪消费端：Entry/Lease → execute → ExecutionScope::enter → 身份/类型/布局检查 → Invocation/Frame → atelier_invoke_business → active_code->entry；先清理 Frame，再退出 ExecutionScope。Entry/Lease 可被同步回调销毁，因为之后仅用局部捕获。宿主 Version 和 DLL active_code 保护不同外层句柄，不可凭“双 pin”删掉一个；shared_ptr 不授权并发访问 State/Runtime。

## Spec：失败及关闭边界

| 边界 | 当前保证/限制 | 证据 |
|---|---|---|
| 请求非法、NUL 名称、源码超长、号耗尽 | latest 不推进；OOM 沿用原边界 | business.cpp:581–592、1802–1840 |
| 解析/定型/真实链接失败 | current 保旧；最新失败仍压住较早候选 | 594–611、1919–1968 |
| 外来程序/Runtime、乱序、重复发布 | 提交前拒绝且保留候选 | 613–620、1889–1968 |
| busy | compatible 后检查 Runtime；候选保留，外层调用退出后显式重试 | 621–626、1970–2057 |
| 原生借用、遍历、执行中 GC 步 | 回调/遍历/活动计数禁发 | runtime.cpp:745–771、792–799；business.cpp:2084–2154 |
| 暂停 GC 周期 | 无活动执行/借用时可发布，不要求 phase==idle | runtime.cpp:766–771；设计10.2.4 |
| 编译信息/元数据不兼容 | 提交前保旧；不支持原位改变 Runtime 元数据 | business.cpp:437–474、621–626 |
| 成功提交 | published_request 更新、current move 消费候选；无源码初始化 | 627–628 |
| 退休资源失败 | 无 Result 回滚通道，不等于发布未生效；未承诺完整资源故障恢复 | 628；backend.cpp:26–35、549；设计10.2.7 |
| 关闭中活动 Entry/Lease | 捕获版本可结束；弱 Entry/历史 Request 不可新调用/prepare | 537–568、574、2228–2295 |
| 关闭后旧 Lease | 允许受检查调用旧版，前提 Runtime/宿主/DLL 存活 | 538–545、2282–2287 |
| 关闭后未提交 Candidate | 仍是实际代码持有者，释放时退休，不是泄漏/发布权 | 2288–2291、2330–2354 |
| Runtime 先销毁后调用租约 | 违反借用寿命前提，不承诺安全返回错误 | business.cpp:400；设计10.2 |
| 业务错误/早退 | 保留已发生副作用，清根和活动计数，不回滚业务 | 349–395、2157–2203 |

BusinessCodeInfo/Layout 是借用视图。后端拥有名字并保存真实服务地址；prepare 在 CodeOwner 存活时检查 function/source/type、服务表、入口地址与槽数，裸字符串不可跨退休。源码签名由前端检查。TypeInfo 不公开字段内部偏移/Invoker，metadata.matches 不是非法 const_cast/内存破坏侦测器，也不是可变类型版本协议。

## Standards：复制、检查与重用

- **设计取舍**：四份 TypeInfo 仅在 Contract 建立时复制；Request/Version 共享，不是每次更新复制整份元数据。请求自有源码和 DLL 自有名称均解决真实借用寿命。
- **性能假设**：prepare/publish 重复 compatible，execute 另复核布局。不可变 Candidate 可研究复用已验证结果，但 execute 还服务无版本路径，不能整体删守卫；固定小表/字符串成本未测，不先判重。
- **性能假设**：unchanged 每次深比公开元数据再 bind_machine 重扫字段/方法；成本随绑定类型字段/方法数量增长，不随历史更新次数增长。不可变规则允许研究复用，但确切检查是已确认合同，不能擅改为名字或常数散列。
- **设计取舍**：完整 LLJIT 是清楚的退休单元；共享会话不是免费替换，更不能借此虚构一般调用/内联失效体系。
- **源码证实**：不留 Lease 时 current 赋值同步释放旧 Version/CodeOwner；留 Lease 时成本移到最后释放。历史只保留文本和 Contract，不保留机器码。

## 最多三项动作

1. **A1 连续更新基线**：包含当前 business.cpp，原生/描述型 × 保留/释放历史 × 发布时留/不留旧租约，记录 request/prepare/publish/lease-release/close 时间、进程 PrivateUsage、历史字符串字节。每版新代码物化后发布，立即查旧地址防地址复用。对象身份/地址/状态连续，历史不阻止退休；不是优化收益或完整泄漏审计。
2. **A2 busy 成本**：真实 ExecutionScope 内固定次数 try_publish，必须 busy 且候选未消费；出作用域后一次显式发布成功。先减少无意义宿主重试，再根据测量考虑 admission-before-compatible；它改变错误优先级，需独立授权/验收。
3. **A3 调用方持有策略**：明确历史保留、过时候选丢弃、租约释放时机；Program 不拥有历史列表，不增加通用 cache/scheduler/退休服务。文本主导则调用方限留/外存；LLJIT 主导才研究资源共享。

## 质询记录（已完成一轮双向反证）

搭档文档：[challenge.md](challenge.md)。本报告初稿先形成；因报告路径/写权限失败，实际保存晚于接收搭档消息，最终已互读文件并直接互发反证。

1. **反方：活动唯一 Lease/Entry 的最后持有者释放是否真的已测？** 反方指出 owner_destruction:2233 独立保留 lease，不能证明活动局部是唯一剩余代码持有者。我回应：542 的局部 Version 与 backend:545 的 active_code 是源码证明，991–1045 的 verify_active_code_lifetime 已证明旧无版本 CodeOwner 可在活动回调中释放；应区分 BusinessProgram 层组合证据，而非宣称全仓无最后 owner 覆盖。**修正**：现报告的“活动 owner 可删除”只复用已有场景，不外推“唯一 Lease 自销毁已实测”；反方 E1 将补单独稳定 Entry 与唯一 Lease、成功/异常及两种接收者。未决是实际 Windows 资源退出点，不是已证实生命周期漏洞。
2. **反方：busy 后的新失败请求是否改变重试资格？** 反方原 E2 还拟同时销毁/复用接收者。我以 request_order:1944–1959 和 active_calls:1970–2057 的各自范围反问：加入对象复用会把对象失效与版本顺序混成两个变量，不利于反例归因。**修正**：搭档已把 E2 收窄为 M busy → 同活动调用接受 N，N 准备失败 → 返回后 M stale，current=P → 释放 M 后退休；对象销毁组合留为未实测但不升格缺陷。证伪条件：M 重试成功、P 改变、失败请求使 latest 回退、或拒绝擅自释放 M 任一成立。
3. **双方争议：共享 LLJIT、重复校验、历史占用能否立即优化？** 我提出 current move 同步退休会污染 publish 耗时，并指出 TypeInfo 深比+rebind 是当前确切合同；反方以 backend:426–428 的 state.get() 错误报告捕获及固定 atelier_source_entry 补充：直接共享 jit 字段还会引入诊断寿命与符号冲突。**结论**：不改代码/删检查；E3复用我的 A1。历史 Request→Contract 的文本/快照占用和真实 Lease/Candidate→Version 占用分列，MEM_FREE 不证明全堆无增长。未决仅为成本及受控替代设计，证伪需真实分阶段数据/独立版本资源归属方案，而不是同意票数。

### 对抗实验闭环补录

随后完整读取Main生成的 `out/review/wave1-meeting/reload-counter-interleavings-default.json` 及同前缀 `-build.json`：当前正式Release构建、实际产物 `out/release/wave1-reload-counter-interleavings.exe`，退出0、signal=null、stderr为空。E1共8路（原生/描述型 × stable/lease × 原生成功/异常）均报告 final_owner_retired=1；E2共4路（两种接收者 × 最新parse/link失败）均报告 stale_retry_rejected=1、caller_candidate_retired=1，末行明确 `PASS (E1=8, E2=4)`。

因此前述讨论中“BusinessProgram层活动最后持有者/忙后新失败请求”的指定组合已由源码证实升级为**Main本机Release实验通过**；保留讨论当时的反证过程，不再将这两项写作当前待证漏洞。范围不扩展到Dev、任意并发交错、对象销毁/复用的组合、Runtime/DLL先结束或完整资源故障恢复。整进程167.25ms包含启动和所有用例，不作为更新性能基线；未重跑实验。

### 源码席位交叉质询：C-D1 诊断归属

WaveSourceChallenge 指出 request 接受 2100 字节非 NUL 名称，追加 request# 后，prepare 的 2048 字节诊断被 backend:491 的 name-first snprintf 截断，版本后缀/位置/原因可能全部丢失。我查 request:581–590 未找到名称长度上限，确认源码归属风险；建议两个同长前缀的不同失败请求比较输出，并保留短名控制与旧 Entry 执行。对方已纳入 source-counter-semantics.cpp。**此项由 C 的 challenge.md 归档和推动，不重复增加 E 的优先动作；不是发布、GC 或代码退休安全缺陷。** 本片“未证实生命周期缺陷”不否认该诊断缺陷，亦不提前宣称实验通过。

## 探针交付与 Main 执行办法

已交付 `out/review/wave1-meeting/E-reload-probe.cpp`。它直接 include 当前 `acceptance/jit/business.cpp`，调用真实 register_machines、BusinessProgram、ExecutionScope、Entry/Lease 和 require_code_state；不复制生产状态机，不使用 mock，不修改任何生产文件。Main 独占执行，本席位未编译/运行。

### 编译与运行

以 `out/review/wave1-meeting/build-provenance.json` 当前 host 编译/链接信息为依据。在仓库根 PowerShell 先 `. ./tools/enter-dev.ps1`，由 Main 完成正式配置构建后编译一次性探针。下列命令保留 Release `/MD`、同一 mimalloc replacement object、Runtime/JIT/native_client 的当前产物；不要链接原 business.cpp.obj 或 main.cpp.obj，否则重复定义。

    & ./.tools/llvm-23.1.1/bin/clang-cl.exe /nologo /TP /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_MBCS /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG -clang:-std=c++23 /MD /W4 /WX /permissive- /utf-8 /Iacceptance /Isrc /Foout/review/wave1-meeting/E-reload-probe.obj /Feout/release/E-reload-probe.exe out/review/wave1-meeting/E-reload-probe.cpp out/release/CMakeFiles/atelier_jit_acceptance.dir/src/memory/global_new_delete.cpp.obj out/release/atelier_runtime.lib out/release/atelier_jit_probe.lib out/release/atelier_jit_native_client.lib out/release/_deps/mimalloc-build/mimalloc.dll.lib psapi.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib -fuse-ld=lld /link /include:mi_version /MACHINE:X64 /INCREMENTAL:NO /SUBSYSTEM:CONSOLE

链接保留 build-provenance.json 的完整 Windows SDK 默认库列表，另加 psapi.lib 并选同一 lld；不换用历史 ABI 二进制。EXE 放在 out/release，直接使用该目录当前 backend/native_client/mimalloc DLL。

八个模式均为独立进程，避免同进程模式先后缓存混淆：

    ./out/release/E-reload-probe.exe 0 0 0 16
    ./out/release/E-reload-probe.exe 0 0 1 16
    ./out/release/E-reload-probe.exe 0 1 0 16
    ./out/release/E-reload-probe.exe 0 1 1 16
    ./out/release/E-reload-probe.exe 1 0 0 16
    ./out/release/E-reload-probe.exe 1 0 1 16
    ./out/release/E-reload-probe.exe 1 1 0 16
    ./out/release/E-reload-probe.exe 1 1 1 16

参数依次为接收者（0原生/1描述型）、是否保留历史、发布时是否持有旧租约、轮数。建议 Main 将各次 stdout 保存本目录独立 CSV/日志；首轮和后续轮分看，需稳定性能结论再同机重复，16点本身不支持精确尾延迟承诺。

### 量测的精确含义

- request_us 不含 reload_source 文本生成，包含 request 的快照复制/分配。
- prepare_us 包含真实前端、LLVM 编译/链接/物化、编译信息检查及 Candidate 建立，不能仅称 LLVM 发码时间。
- busy_us_per_attempt 是32次真实活动 ExecutionScope下重试均值，包含 compatible、错误构造及断言；busy 返回发生在 unchanged 之前，**不计元数据深比、提交或退休**。
- publish_us 包含静止点、compatible、unchanged、current 转移；无旧租约时还包含旧完整编译单元析构。旧租约模式下，old_lease_release_us 单列真正最后持有释放，比较时应同时看 publish+release，不能把成本迁移当优化。
- invoke_us 包含一次真实执行、快照校验和撤根，不是纯机器码吞吐；failed_request_prepare_us 包含创建类型错误请求、真实 prepare、诊断验证及所选模式的历史保存。
- 每轮交替标量/引用局部，factor在1–4循环，明确记录 references 列；不拿不同源码形态行做同等成本直接比较。
- private_ready 在新旧代码同时存活时采样；private_done 在旧代码退休、临时对象收集后采样。summary 给代码全退休/历史清除后两个采样。PrivateUsage 是进程私有提交量，不是 JIT代码大小，也非RSS；clear 不要求 allocator 立即还页，历史向量 capacity 两模式预留相同且不会在clear时强制收缩。
- retained_text_bytes 为所有被保留 Request.source/source_name 与诊断 string 的 size 总和，不含 capacity/容器/控制块/共享 Contract；它不是精确堆占用。历史 keep 模式每轮2请求+1诊断，16轮应为32请求+16诊断。

### 成败判据与关闭

每轮：新旧同时存活地址不同；busy不消费；发布后无租约旧代码立即失去展开记录且MEM_FREE，有租约则保持直到显式释放；对象身份/存储地址/累积状态连续；最新真实类型错误不替换current；完整GC后仅接收者一根一对象。结束：Program关闭后弱Entry与历史prepare均拒绝；pin模式旧Lease仍可执行，最后释放后代码退休；历史文本仍有效；撤销接收者并GC后根/对象归零且 NativeState::live 回到基线。输出末行必须 PASS、退出0；任何失败停止该运行并保留实际错误，不用零值替代。

本探针不验证后台线程、任意布局迁移、长名称诊断、完整OOM/撤表失败恢复，也不替代搭档的E1/E2。现有 retired-code 查询总在下一次代码分配前执行，避免地址复用误判。

## 外部机制一手依据

检索使用 Exa；核对 Microsoft 官方原页，访问日期2026-09-25。适用Windows当前进程/x64展开查询，不外推其他平台。

- [PROCESS_MEMORY_COUNTERS_EX](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex)：页面更新2024-02-22，PrivateUsage为进程私有内存提交字节，不是单一LLJIT、历史文本或驻留集统计。
- [VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery)：页面更新2024-02-06，查询地址所在连续同属性页区间；本探针仅用当前已知入口地址的状态，不当作完整分配器审计。
- [RtlLookupFunctionEntry](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtllookupfunctionentry)：查询当前有效函数表中给定PC的记录，无记录返回NULL。结合VirtualQuery与项目插件的撤表先于放页约定，判断当前入口的退休；不能单凭NULL证明所有LLVM资源已释放。
- 本地锁定LLVM23.1.1、Windows SDK10.0.26100.0：cmake/toolchain-lock.json。撤表失败是致命错误而非可恢复发布回滚，源码 src/jit/windows_unwind.cpp:31–38；分配action登记/撤销见同文件:92–108。


## Main 本机实测补录（2026-09-25）

**路径状态提升：A1/A2 已本机实验通过。** Main 按当前正式 Release host flags 与当前链接产物编译，并运行八个独立进程，每个16版本；本席位只完整读取既有原始JSON、计算汇总并补文档，未重跑。八份记录均 exitCode=0、signal=null、stderr为空且末行PASS；共128次成功版本发布、128次真实类型失败准备和4096次busy重试。当前可执行产物名为 `out/release/wave1-E-reload-probe.exe`，实际两步命令以 `E-reload-probe-build.json` 为准；前文一行编译命令仅为复现范本。

原始证据均位于 `out/review/wave1-meeting/`：`E-reload-probe-build.json`；`E-reload-probe-0-0-0.json`、`-0-0-1.json`、`-0-1-0.json`、`-0-1-1.json`、`-1-0-0.json`、`-1-0-1.json`、`-1-1-0.json`、`-1-1-1.json`。标记依次为 receiver/history/pin。完整stdout包含每一行的原始微秒值；下表由原始数值汇总，不估算缺失数据。

### 成本：退休并没有因持有租约而消失

排除第1版（没有旧版可退休），各模式取版本2–16的15个样本中位数；单位μs。“合计中位数”先逐行相加再求中位数，并非两列中位数相加。

| 接收者 | 保留历史 | 发布时保留旧Lease | publish中位数 | 最后旧Lease释放中位数 | publish+释放中位数 |
|---|---:|---:|---:|---:|---:|
| 原生 | 否 | 否 | 59.4 | 0.0 | 59.4 |
| 原生 | 否 | 是 | 1.8 | 57.4 | 59.3 |
| 原生 | 是 | 否 | 62.3 | 0.0 | 62.3 |
| 原生 | 是 | 是 | 1.9 | 61.8 | 63.7 |
| 描述型 | 否 | 否 | 58.7 | 0.0 | 58.7 |
| 描述型 | 否 | 是 | 1.9 | 56.8 | 58.4 |
| 描述型 | 是 | 否 | 62.9 | 0.0 | 62.9 |
| 描述型 | 是 | 是 | 1.7 | 54.7 | 56.7 |

**得到的证据**：本机小入口中，提前保留旧Lease将约数十微秒的退休工作从publish移到最后释放；不能将约60→约2μs叫作整个更新路径的30倍优化。保持全部现有compatible/unchanged/rebind检查、但不承担旧版退休的publish，中位数仅1.7–1.9μs。本轮没有得到删掉元数据检查或改变busy错误优先级的必要性证据。不同模式仅各跑一次进程，描述型历史模式的合计差值不证明历史带来收益/回归，也不是显著性检验。

其他原始观测：

- 第1版prepare为7.8284–8.6411ms；其余版本为4.2851–7.3921ms。包含真正解析/定型/LLVM/物化/Candidate成本；未分别量出LLJIT建立与发码，不能将全部归因于某一个实现步骤。源码交替有无引用局部，不能把该范围直接当同一方法体的抖动分布。
- 首个request为1.2–2.6μs，其余为0.1–0.6μs，支持“本次短源码复制不是主要耗时”，不支持大型源码/大量元数据的外推。
- 每版32次busy平均为0.081–0.175μs/次；这是批量平均并含检查/错误路径/计时粒度，不是单次纳秒精确度。busy发生在unchanged之前，不能用这个数给深比元数据定价。
- 无最终Lease时close为29.5–41.5μs；有最终Lease时close均0.1μs，而最后Lease释放为35.7–40.0μs，再次体现退休成本的位置转移。

### 历史、代码与提交内存各自证明到哪里

- 四个keep-history进程均在代码全部退休后保有32个Request和16条诊断；原生记录文本size合计9611字节，描述型9731字节。弱Entry和历史prepare在关闭后仍拒绝；四个pin进程的显式旧Lease在关闭后调用成功，然后最后释放才退休。历史保存不阻止机器码及展开表退休的预期通过。
- 每个进程的private_done在第3–16版保持同一数值，未观察到该16版窗口内持续按版增长；旧版页/展开表的即时查询均通过，最后也通过退休查询。此证据不能推出无限更新稳定、所有LLVM堆对象已释放或完整无泄漏。
- 四个历史进程清除历史耗时1.6–2.9μs，PrivateUsage完全不下降；这不等于历史没有内存成本。文本只有约9.6KB，向量capacity预留不收缩，分配器可保留已释放页；实际堆容量/元数据/控制块也没有被retained_text_bytes计入。
- 八个进程代码全退休且历史清除后，PrivateUsage仍比各自private_start高25,452,544或25,456,640字节（约24.27MiB）。**归因未决**：这只是整个进程提交量的剩余增量，不能由本探针分辨LLVM/通用分配器缓存、其他初始化保留或泄漏。直接代码页MEM_FREE与进程commit不回落同时成立，并不矛盾。
- 每版完整收集后的一根一对象、结束时根/对象归零和NativeState::live恢复均是探针PASS所覆盖断言。没有运行额外压力负载，也没有用旧版本基线冒充本次结果。

### 对三项动作的收敛

A1已完成当前连续更新/历史/退休基线，结果支持保留清楚的整单元所有权；A2已完成busy路径量测，当前不建议为了这一级开销改变校验顺序/合同。A3保留为真实编辑器接入时的调用方持有策略要求；本次约9.6KB历史不足以支持新建缓存、历史配额或共享LLJIT框架。若未来真正观测到准备延迟成为产品问题，优先分离prepare内部耗时再选择实验，而非删除当前已经很小的publication校验。

## 来源与状态

源码审查与Main本机新实验分别标记，见实测补录；本席位没有运行任何实验。已读：会议 README/baseline；engineering.md；CONTEXT.md；runtime-model 单入口段；language-contract；jit-execution-design:241–259。实现：business.cpp:349–630、1715–1840、1919–2355；business_backend.hpp:39–116；business_backend.cpp:26–35、404–549；runtime.cpp:745–799；execution.cpp:12–27；runtime.hpp:240–265。旧开发/发布 PASS 仍只按 baseline 哈希一致声明复用；本次性能数字只来自Main生成的八份E-reload-probe原始JSON。

