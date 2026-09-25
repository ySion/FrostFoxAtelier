# E 独立反方：请求、发布与退休的反证审查

状态：完成当前源码审查及与 WaveReloadExplore 的双向证据质询；Main 已以当前正式 Release 参数编译并执行 E1/E2，8+4 个变体全部 PASS、退出 0、stderr 为空，原始证据与构建配方见“Main 返回的本机证据”。基准为会议 baseline.json 的当前实现哈希（含正式链接的 third_party/llvm/JITLinkMemoryManager.cpp），不存在 Git diff。本席位没有执行构建、测试、验收或压力负载，也未重跑 Main 的实验；历史同哈希验收和此次 Release 增补实测分别列证，不声称此次增补已覆盖 Dev。

## 问题与边界

目标不是把热更新适配器改称生产模块，而是尝试推翻当前窄契约：同一 Runtime、不可变类型与服务、单源码入口、同步单线程、显式静止点发布、最后持有者退休。对象身份、请求序号、代码版本与外部 DLL 存续期分别论证。

已完整读取会议章程与 baseline.json，并读取 docs/engineering.md、CONTEXT.md、docs/language-contract.md、docs/runtime-model.md 的执行/发布契约，以及 jit-execution-design.md 第 10.2 节。版本规范不改；生产文件不改。

## 当前证据与版本偏序

源码位置均为当前冻结基线：

- business.cpp:580–591：先分配请求的源名/源码/Contract 快照，成功后才推进 latest_request；拒绝输入和序号耗尽不接受新版本。
- business.cpp:593–611：prepare 只编译请求自己的快照，持有 State 但不读取可变对象图；prepare 成功不是发布成功。
- business.cpp:613–629：必须 owner、Contract、latest_request 相同，且 sequence > published_request；兼容、静止点和原依赖检查全部在改变 current 前。
- business.cpp:556–567：Entry 每次锁弱 owner，检查 open，复制一次 current；之后只用局部持有者。Lease:540–547 固定 Version，并同样复制局部持有者。
- runtime.cpp:749–770 + execution.cpp:12–21：Runtime 级 active_executions 覆盖同一 Runtime 的多个入口；这是同步计数，不是并发安全承诺。
- business.cpp:347–395：ExecutionScope 先于 Invocation 建立、晚于帧清理结束；字段/方法服务每次回到 Runtime，用代际校验而不跨服务缓存对象地址。

偏序：request accepted(M) < request accepted(N) ⇒ M 永远不能再次成为可发布候选；N 准备失败也不撤回该顺序。已发布 P 可以继续执行；Lease(P) 与 pending(N) 同时持有代码是有意所有权，不是泄漏。不同程序的序号不比较，仅发布准入共享 Runtime。

## Spec：逐项反证结论

### 1. 已否决：同 Runtime 跨入口或旧租约绕过发布准入

verify_reload_active_calls (1970–2058) 不仅测试本程序：原生借用、PollHook、嵌套真实入口、嵌套显式 ExecutionScope 返回后、旧 Lease 和未版本化 execute 都尝试向第二个 Program 发布；pending 保留并在最外层返回后显式消费。standalone_guards (2084–2155) 另覆盖无 JIT 活动时的原生、遍历、真实 GC 析构回调，以及暂停 GC 的合法发布。不能把“跨入口漏计数”作为缺陷。

失效条件：未来新入口不经过 ExecutionScope，或挂起任务把作用域提前结束；那是新入口设计的验收义务，不是本版已有漏洞。

### 2. 已否决：发布的两次赋值本身造成当前同步链可观察半提交

published_request 与 current 确实不是原子变量，但当前只有单运行时线程；兼容/依赖检查不执行业务回调，提交后的旧代码析构由 LLVM 资源释放承担，不能由“两个写操作”直接推出观察到混合状态。宿主通用并发访问和活动 DLL 卸载均被契约排除。OOM 全局回调/非法 const_cast 元数据也不能偷偷成为反例前提。

保持限制：失败返回保旧只描述提交前可恢复失败；退休失败不能宣称回滚。windows_unwind.cpp:31–38 在撤表失败时 fatal，故不是可恢复卸载事务。

### 3. 已由新增 Release 实验补证：最后一个 Entry/Lease 在自己的回调中销毁

owner_destruction (2228–2294) 在 2233 保留独立 lease，外层 Program/Entry 被删除后它仍保持 Version；因此该场景证明 owner/Entry 可消失及 Lease 可继续执行，不证明“活动局部引用是最后代码持有者”的情况。version_retirement (2205–2226) 在返回后才放弃租约。源码 542 的局部 Version 和 business_backend.cpp:545 的 active_code 双重保护此前仅有组合层的源码证明；Main 返回的新增 Release E1 现已补足该层八种交错，不再把它列为当前 Release 未实测风险。

反方自限：现有 verify_active_code_lifetime (991–1045) 已在未版本化 execute 中销毁唯一 CodeOwner 并做 Windows 实际资源观测，不能声称整个仓库完全未覆盖最后 owner 释放。新实验只补 BusinessProgram/Lease 层，不能重复包装已有通过。

实验 E1：关闭 Program，保留唯一堆上 Lease；通过其 invoke，在第一次原生回调内 reset 该 Lease，立即查询代码页与展开表仍在；继续执行/另取抛错分支，返回后且不进行新代码分配之前，必须无表且 MEM_FREE。并增加无额外 Lease 的活动 stable Entry/Program 同步销毁路线；两种入口×两种接收者×成功/失败共八个变体。Main 已执行并全部通过，活动中保留、返回后立即退休的断言均成立。

### 4. 已由新增 Release 实验补证：busy 之后的新请求夺权与旧候选退休

active_calls 保证 busy 后原候选仍能重试；request_order 保证新请求/最新失败永久废弃旧候选，但两者在不同场景。本次增补实测交错是 M 因活动调用 busy → 同一活动调用中接受 N → N 解析/链接失败 → 旧调用继续执行 → 外层返回 → M 必须 stale（不是成功重试），且 current=P；显式丢弃 M 才退休 M。Main 的 Release E2 已在原生/描述型×解析/真实链接失败四个变体中全部通过，随后新成功版本也按预期执行。

源码 619 的重检已支持预期，不是已证实缺陷。实验 E2 必须走真实 request/prepare/try_publish 与原生/PollHook 回调，不模拟另一个顺序器。N 的失败不能将 latest_request 倒退；拒绝 M 不得偷走 caller-owned 代码。

### 5. 可证伪风险：调用中对象销毁与重载错误清理的组合证据

Runtime::destroy 只拒绝原生借用，不因为 ObjectRoot/ExecutionScope 存在而禁止所有显式销毁。poll 返回后 hook 可显式销毁；下一次真实字段/方法访问要重新拒绝旧代际。verify_identity_boundaries (866–900) 和 stale_source_references (1382–1458) 已验证这一机制；11 个 reload 场景自身没有把 busy 候选、接收者销毁/槽复用与最终显式重试组合起来。

这是覆盖组合而非新漏洞。独立初稿曾提议在 E2 同时加入接收者销毁/槽复用；WaveReloadExplore 反驳其混合因素，不利于定位版本偏序失效。接受该质询：E2 只补 busy→新请求失败→旧候选 stale。调用中显式销毁的已有证据精确为 verify_identity_boundaries:882–898 和 stale_source_references:1439–1455；前者在真实 hook 内销毁/重建后必须 invalid_object，替代对象保持 99/7/25，后者区分陈旧复制与实际解引用。与重载全部组合仍未单独实测，暂不提出第三套组合实验或缺陷。

## 11 个场景实际证明到哪里

| 场景 | 已有证据 | 不可外推 |
|---|---|---|
| backend_metadata / contract_limits | 自有字符串、实际服务指针、确切类型名、真实链接错误、序号上限 | 动态 DLL 重新装载、跨 ABI 版本 |
| bootstrap | 未发布/兼容更新/身份与地址连续、外来候选/死亡事件拒绝 | 通用方法表或布局迁移 |
| request_order | 确定性乱序、最新失败不回退、重复请求拒绝 | 后台线程完成/关闭竞态 |
| active_calls | 同 Runtime 跨入口、嵌套、旧 Lease、legacy 执行统一守卫 | 跨 Runtime 必须相互阻塞或通用调度器 |
| standalone_guards | 独立借用/遍历/真实 GC 回调拒绝，暂停 GC 可发布 | 任意元数据原位修改仍兼容 |
| failure_release | 早退、原生失败、poll 失败清理后可发布 | 所有 OOM 与资源故障恢复 |
| version_retirement | 返回后放弃最后旧 Lease 撤表/放页 | 回调中删除正在 invoke 的唯一 Lease |
| owner_destruction×2 | Program 与 Entry 活动销毁、有独立 Lease 保持，关闭事件拒绝 | 把 Runtime/Frame/DLL 一起销毁仍安全 |
| history_retirement | 源码/诊断历史不持有代码，候选显式保留/释放，地址复用控制 | 完整堆泄漏或物理工作集下降 |

## Standards：模块与成本判断

### 移到生产模块——有条件，不等于完成能力

可以提炼的语义是 Request/Candidate/Version/Entry/Lease 的所有权与提交边界，但 Contract::bind_machine、固定 progress/state/advance、业务字段索引和快照返回仍是机器适配器。仅将 business.cpp 搬入 src 或改名 Program，不会得到任意 MethodId 分派、多发布单元失效或持久 ABI。反例：另一个真实类型没有 state.heat，现构造直接拒绝；这不是重命名可修复的。

A 保留验收私有入口，先补可执行组合证据；B 由真实第二业务适配器驱动提炼受检查发布单元，保留 Runtime 权威和 DLL 隔离。固定范围下选 A，B 需另获功能范围。

### 通用调度器——现在不必要

反例：在 PollHook 自动提交以缩短等待，会与 active_executions 冲突；若为“方便”绕过 busy，就违反最外层调用静止点，且仍有同 Runtime 第二入口正在执行。只在工具命令返回后显式 retry 已可完成全部现契约，不需要任务图或后台线程。只有真实跨线程编译/取消/队列公平性问题出现，才需要调度，不以确定性乱序场景冒充后台调度。

### 复用编译实例——性能假设，不能无偿替换整单元所有权

每次 compile 在 business_backend.cpp:406–458 新建 LLJIT，且都导出 atelier_source_entry。这保证一个 Version 的末持有者对应一个完整 LLJIT 单元。共享单一 LLJIT/主 JITDylib 的直接反例是多个候选同时定义同一符号；共享会话销毁又不能退休单个旧版本。若要复用，必须先定义每版本符号空间/ResourceTracker、诊断归属、服务绑定和失败候选撤销，而不是缓存 builder 就计作“复用”。

第二个具体反例：business_backend.cpp:428–430 的 session error reporter 捕获当前 CompiledBusiness 的 raw state.get()；当前 LLJIT 内嵌于该 state，且 diagnostic 成员比 jit 晚析构，因此生命周期匹配。若只将 LLJIT 指针改成跨 Version 共用，不同时迁移该诊断所有者，旧 state 退休后 reporter 就不能安全继续指向它；共享 session 的最新 reporter 也不能自动代表每个候选的错误来源。此处是候选方案的失效条件，不是当前实现缺陷。

E3 复用搭档 A1 的真实 request→prepare→publish→lease-release→close 分阶段量测，不另造计时框架。无 Lease 时 current move 同步析构旧 Version/LLJIT，所以 publish 总时长包含退休；有 Lease 时只是把成本移到最后释放，不能据此宣称发布算法更快。历史保存文本/共享 Contract，不保机器码，MEM_FREE 不能证明全部堆占用不增长。固定范围保留现实现，数据不足不删 compatible/unchanged 检查。

## 外部 Runtime 与 DLL 生命周期前提

- Contract 只借用 Runtime* 和元数据身份，不延长 Runtime；ExecutionScope 析构还会回调 Runtime::leave_execution。故 Runtime/Invocation/槽存储必须覆盖活动调用，不能通过 shared_ptr<Version> 推导 Runtime 延寿。
- 宿主服务函数地址必须在候选、已发布版本、租约及活动后端调用的有效期内保持可调用且语义不被原位替换。服务地址相等不是卸载后新 DLL 恰好重用地址时的身份凭据。
- backend DLL 不但执行生成调用，还负责 atelier_destroy_business；caller-held stale/abandoned Candidate 也是代码持有者。关闭 Program 并不授权卸载 DLL。business_backend.hpp 明确所有 code owners 和 invocations 均受约束。
- Request/history 可以保留快照文本但不保留机器码；关闭后 prepare 会先拒绝。不能借此调用已卸载 DLL，亦不能在 owner 仍 open 而 Runtime 已毁时发布。
- 外部前提一旦取消，需要单独的 Runtime/DLL owner token/卸载门闩协议；这不是本轮承诺，也不应把故意违反前提的崩溃报成已证实缺陷。

## 讨论记录

双方先独立落稿，再互读文件并通过 hub 直接交换。对方报告：`docs/research/wave1-meeting/reload/explore.md`。

| 对方主张/质询 | 本方源证据回应 | 修正、未决与证伪条件 |
|---|---|---|
| 当前 unchanged 深比 + rebind 是已确认合同，没测不能先删 | 同意；business.cpp:403–474 的公开 Metadata 快照不是内部 Locator/Invoker 的完整可变版本协议；依赖不可变前提仍不可丢，重复检查仅是成本假设 | 不建议删除或变成名称散列；若后续量测发现它主导，必须另行证明借用期间验证仍成立并取得行为更改授权 |
| publish 的 current move 同步析构旧 LLJIT，只测 publish 会混入退休 | business.cpp:628 → Version::code → atelier_destroy_business:549；CompiledBusiness::jit 是 unique_ptr，确实同步释放。保留 Lease 不是省去释放，只改变发生地点 | E3 复用搭档 A1，分别列 no-lease publish、pinned publish、last-lease release；不把费用转移当改进 |
| 历史不保代码不等于没有占用，Contract/文本必须分开 | SourceRequest:497–506 仅弱 State + 强 Contract + 自有文本；无 Version 边。MEM_FREE 是旧代码页证据，不是完整内存审计 | 接受 A1 保留/释放历史×保留/释放 Lease 的正交模式；不提出通用缓存/调度器 |
| 本方 E1 是 BusinessProgram 层证据缺口，但底层已有唯一 CodeOwner 活动销毁 | 同意；991–1045 的 active_code 反证了“全仓毫无最后 owner 覆盖”，但 2233 的独立 Lease 又使 reload_owner_destruction 不能证明宿主 Version 唯一外部 owner 消失 | E1 补 stable Entry 和唯一 Lease 两路，删除所有外部代码持有后回调中仍有表/页、返回后无表且 MEM_FREE；成功/失败与双来源各测，不冒充已知缺陷 |
| 本方 E2 不应同时加接收者销毁/复用，避免失效归因混杂 | 接受。调用中销毁本来就有 882–898、1439–1455 的真实证据；重载偏序应独立核查 | 已从提交 Main 的 E2 源码移除对象销毁，保留 busy→较新 parse/真实link失败→继续旧调用→返回后旧candidate stale/显式释放 |
| 共享 LLJIT 不应直接替换整单元 | 本方补充两个反例：固定 atelier_source_entry 符号；setErrorReporter 捕获 state.get() 与整单元寿命绑定 | 双方选择保持整单元；共享方案未实现、未测，只有真实测量与独立资源/诊断设计齐备才重开 |

已完成至少一轮带源码反证的双向回应；双方意见相同不被当作实验通过。随后 Main 返回 E1/E2 的 Release 原始日志，十二个变体全部通过：未出现页提前消失、返回后仍保留、旧候选覆盖或活动调用切换因子。该实测仅封闭指定交错，不外推 Dev 新增场景、并发、任意 DLL 卸载或完整资源故障恢复。

## 优先动作（最多三项）

1. E1（Main Release 已通过）：无独立额外代码持有者的活动 Entry/Lease 自销毁，真实 API + 页/展开查询，成功/失败与原生/描述型双来源（8 个隔离变体）。
2. E2（Main Release 已通过）：busy→较新 parse/link 失败请求→继续旧版本→返回后旧候选 stale + 显式候选退休（4 个隔离变体）。
3. E3（已复用 Explore 的 Main 实测补录）：当前请求/编译/发布/最后租约释放/关闭量测支持保留完整版本单元；是否提炼模块、共享 LLJIT 或建设调度器仍以真实调用者和可归因数据为门槛，不另外增加计时框架。

## 给 Main 的精确实验入口

一次性源码：`out/review/wave1-meeting/reload-counter-interleavings.cpp`。直接 include 当前 `acceptance/jit/business.cpp`，只增加探针调用，不拷贝/模拟生产状态机；不调用完整验收入口。包含现有代码会保留部分未执行函数的链接引用，故继续链接真实 runtime、jit_probe、jit_native_client、mimalloc。

Release 可按 `out/review/wave1-meeting/build-provenance.json` 的现有编译/链接参数执行如下命令（**仅 Main 运行，本席位未执行**；从仓库根目录，现有 Release 产物已就绪）：

```powershell
. ./tools/enter-dev.ps1
$cc = './.tools/llvm-23.1.1/bin/clang-cl.exe'
$ld = './.tools/llvm-23.1.1/bin/lld-link.exe'
$obj = 'out/review/wave1-meeting/reload-counter-interleavings.release.obj'
$exe = 'out/release/reload-counter-interleavings.exe'
& $cc /nologo /TP /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_MBCS /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG -clang:-std=c++23 /MD /W4 /WX /permissive- /utf-8 /Iacceptance /Isrc /c out/review/wave1-meeting/reload-counter-interleavings.cpp "/Fo$obj"
if ($LASTEXITCODE -ne 0) { throw 'counter probe compile failed' }
& $ld /nologo $obj out/release/CMakeFiles/atelier_jit_acceptance.dir/src/memory/global_new_delete.cpp.obj "/out:$exe" /machine:x64 /INCREMENTAL:NO /subsystem:console /include:mi_version out/release/_deps/mimalloc-build/mimalloc.dll.lib out/release/atelier_runtime.lib out/release/atelier_jit_probe.lib out/release/atelier_jit_native_client.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib
if ($LASTEXITCODE -ne 0) { throw 'counter probe link failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'counter probe failed' }
```

Dev 配置必须从 `out/dev/compile_commands.json` 复制对应 business.cpp 参数及同配置库（包括 CRT/Debug 选项），不能把 Release `/MD /DNDEBUG` 与 dev 对象混链。探针 EXE 放到相应配置目录以使用原 DLL；本例不依赖读取默认 advance_machine.source。

精确预期：

- E1 共 8 行：原生/描述型 × stable/lease × 原生正常/异常。回调删完外部 Program/Entry 或唯一 Lease 后，RtlLookupFunctionEntry 非空且代码页是 MEM_COMMIT/PAGE_EXECUTE_READ；调用返回前完成 generated cleanup；返回后、任何新 JIT 分配前查无展开表且 MEM_FREE。正常状态 progress/heat/cycles 为 1.5/1/3，异常为 0.5/0/0；弱事件拒绝，准入恢复。
- E2 共 4 行：原生/描述型 × 解析错误/真实 missing-business_poll 链接错误。在旧调用第 2 次 poll 先得到 busy，然后接受并失败新请求；旧调用继续得到 1.5/1/3，返回后 pending 必须 stale 且仍持有自己的代码。显式丢弃 pending 才撤表/放页；新成功 factor=4 请求发布后状态为 3.5/5/7。
- 每隔离变体清根收集后对象/根归零、NativeState::live 回到场景前、publication 准入成功；最终唯一汇总是 `Reload counter-evidence: PASS (E1=8, E2=4)`，退出 0。任何断言失败都须保留真实错误，不能删除断言/放宽预期以获得 PASS。

## Main 返回的本机证据

本节仅吸收 Main 已执行的结果，本席位未重跑。构建配方：`out/review/wave1-meeting/reload-counter-interleavings-build.json`；原始运行数据：`out/review/wave1-meeting/reload-counter-interleavings-default.json`。

- 配置为 **Release**，按当前正式 host 的 CMake flags/链接依赖构建；实际执行文件是 `out/release/wave1-reload-counter-interleavings.exe`，无参数。上节为可复现的等价手工配方，实际输出文件名及参数以 build.json 为准。
- 原始记录 `exitCode=0`、`signal=null`、`stderr=""`，stdout 含 8 条 E1 与 4 条 E2 `[pass]`，末行为 `Reload counter-evidence: PASS (E1=8, E2=4)`。
- **E1 已补证**：native/described × stable/lease × native_failure=0/1 八种情况全部 `final_owner_retired=1`；在回调中丢弃全部外部代码持有者后仍有可执行页和展开登记，生成帧返回后它们立即撤销，根清理及业务状态断言通过。
- **E2 已补证**：native/described × newest_failure=parse/link 四种情况全部 `stale_retry_rejected=1 caller_candidate_retired=1`；较新失败请求没有复活曾 busy 的候选，拒绝不抢走调用方所有权，主动释放候选才退休，后续成功请求仍可正常发布。
- JSON 另记录一次进程总耗时约 167 ms；它包含启动和全部功能探针，不是发布/退休分段基准，不据此推导性能收益、尾延迟或编译实例复用价值。

仍未覆盖：此次新增变体的 Dev 运行、真实跨线程编译/关闭、Runtime 或 DLL 提前销毁、所有资源故障恢复、接收者销毁与更新偏序的全部笛卡尔组合。后者已有独立对象失效基线证据，按质询结论不加混合探针。固定范围最佳路径不变：保留完整 LLJIT 版本单元和显式发布，性能结论交给 Explore A1 的对应原始证据，不从功能 PASS 推导。

E3 已按约定复用搭档 `explore.md` 的“Main 本机实测补录”：八个独立 Release 进程×16版全部通过。本席位读取其带原始JSON路径的汇总，不重算或重跑；该补录显示 pin 只是把同步退休从 publish 移到最后 Lease 释放，并没有消除它；历史仍保留文本/Contract 而不阻止代码页退休。它支持当前保留检查及整单元设计，不支持宣称共享 LLJIT、通用调度器或生产模块搬迁已有必要性。精确数值与测量边界以该节及其八份原始JSON为准，避免在两个报告复制一套易失配的性能表。

## 外部一手机制核对

检索使用 Exa，已知官方页面直接读取；访问日期 2026-09-25。

- LLVM ORCv2 官方文档（23.1.0 文档，不冒称为本地 23.1.1 实测）：同一 JITDylib 中重复定义同名函数会冲突，不同 JITDylib 可以各自定义；单模块资源移除需在添加时使用显式 ResourceTracker。此机制支持“复用需要版本资源/符号隔离”，不能证明当前 workload 的速度。来源：`https://releases.llvm.org/23.1.0/docs/ORCv2.html`，Design Overview 与 How to remove code。进一步核对本地固定 SDK 23.1.1 的 `include/llvm/ExecutionEngine/Orc/Core.h:82–112,721–743` 和 `LLJIT.h:125–147`，确认 ResourceTracker::remove、JITDylib::clear、addIRModule(ResourceTrackerSP, ...) 的真实声明；clear 明确不运行 static deinits。
- Microsoft FreeLibrary：模块引用计数归零后卸载；持有一个应用层 C++ 版本对象不能据此推出 DLL loader 引用计数增加。对应本项目为借用前提，不建议为了验证禁止的 unload 交错而制造崩溃。来源：`https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibrary`（所取页面元数据 updated_at 为 2025-07-01；Win32 文档，未验证任意第三方加载器）。
- Microsoft VirtualQuery：返回地址区域的状态、类型、保护属性；这解释 require_code_state 的代码页证据，而不是进程私有堆泄漏证明。来源：`https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery`。
- 本地实际销毁顺序另由锁定 backport 源码证实：`third_party/llvm/JITLinkMemoryManager.cpp:464–503` 先逐个运行 DeallocActions，再 releaseMappedMemory；对应 `windows_unwind.cpp:31–38,92–105` 撤销函数表。因此“表撤销先于页释放”不仅依赖泛用 ORC 文档；失败恢复范围仍按现契约排除。

## 来源

- docs/research/wave1-meeting/{README.md,baseline.json}；out/review/hot-reload-acceptance-evidence.json（历史本机通过，同哈希，非本次重跑）。
- docs/engineering.md；CONTEXT.md；docs/language-contract.md；docs/runtime-model.md；docs/research/jit-execution-design.md:241–259。
- acceptance/jit/business.cpp:347–629、868–900、991–1045、1382–1458、1707–2389。
- acceptance/jit/business_backend.hpp；business_backend.cpp:26–35、406–458、536–549。
- src/runtime/runtime.cpp:749–770、1263–1275；src/runtime/execution.cpp:12–27；src/jit/windows_unwind.cpp:16–38、92–119。

当前审查未确认违反窄契约的发布/持有/退休缺陷。E1/E2 曾识别的组合证据缺口现已由 Main 的十二个 Release 变体补证；未覆盖的并发、外部 Runtime/DLL 提前销毁、完整资源故障恢复及更多组合仍只作为范围边界，不升级为已证实 finding。E3 引用的成本证据支持保留现有检查与完整 LLJIT 版本单元，不把重命名、迁移目录或成本转移计为能力完成。
