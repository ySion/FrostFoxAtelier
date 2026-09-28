# D 独立反方：执行安全与成本边界

> 历史快照：本文依据当轮 baseline.json；旧接口、文件名、行号与未实现判断仅描述当时状态，不是现行规则或待执行任务。后续结果与现行入口见[历史索引](../README.md)。

日期：2026-09-25。席位：WaveExecChallenge。审查对象为会议 baseline.json 固定的当前实现，不是不存在的 Git diff。生产只读；本席位没有编译、运行、测试、格式化或施加压力负载。所有新探针交 Main 统一执行。

## 1. 裁决摘要与证据等级

**Spec：未发现当前已承诺业务路径的确定性违约。Standards：未发现确定性工程违规；“全链安全／零浪费／已经提速”等扩大表述不成立。** 两份一次性源码已交付；Main 已统一执行 semantics 探针三个模式及低层 finalize 探针，四次运行均退出0、stderr为空，原始证据见§8。低层 mixed 分区的自然拒绝与释放已本机确认，但仍不能推定业务 COFF 可达性。

- **源码证实 + 既有本机验收**：ABI 隔离、固定状态错误、显式根清理、同步 active-code pin、真实 Windows 元数据回溯、登记后 action/emission 错误回退。此前 dev/release 通过及匹配范围来自 baseline.json；本席位不重跑。
- **外部机制 + 源码 + Main 本机确认的低层能力边界**：真实 SimpleSegmentAlloc 的 Standard-only/Finalize-only 成功；mixed 两分区共享 reservation，自然发生无效地址拒绝，回移清理后两地址均为 MEM_FREE。使用当前正式/MT与回移对象，不是模拟失败。**未证实当前业务 COFF 图生成 Finalize 分区；探针无 unwind action，不能升级为业务缺陷、实际展开清理证据或业务泄漏。**
- **源码证实的成本，不是测量收益**：额外引用槽使 Invocation 构造非空 vector；槽数按静态声明/临时表达式计，不按最大同时活期计。不可达分支也能增大布局。Frame 不分配堆不等于完整 execute 不分配。
- **未证明**：真实 allocator OOM、RtlAdd/RtlDelete/VirtualProtect/VirtualFree 任意失败的全链恢复；当前引用/重载版本的优化提速；任意 C++ 异常穿过生成帧；源码调试器/PDB 集成。

## 2. ABI、错误和活动 owner 的反例筛选

### 2.1 真正成立的边界

`BusinessServices` 是私有验收 ABI：固定宽度标量、函数指针、不透明上下文、调用方缓冲；CodeInfo/Layout 是借用名字和简单聚合视图，不是稳定跨工具链的公共 C ABI（business_backend.hpp:20–132）。宿主拥有 Runtime、Frame、ObjectRoot、Invocation；LLVM、string、shared_ptr 和删除代码留在隔离 DLL。cmake/jit.cmake:40–74 固定 SDK Release、/MT、无 RTTI/断言、_ITERATOR_DEBUG_LEVEL=0；不能把宿主 /MDd 或 /MD、DLL 的原生优化配置、生成业务代码的优化等级混为一层。

实际异常消费链为 `Runtime::call` 的 catch→宿主 `bridge` catch→状态整数→IR failure cleanup，不是让异常穿过生成帧（runtime.cpp:1504–1532；business.cpp:113–132；business_backend.cpp:71–94,100–137,171–187）。Runtime catch 创建 Error 时仍可能分配；bridge 使用固定缓冲保存首错，不能由此推导持续 OOM 恢复。

`atelier_invoke_business` 在生成执行前复制 `code->state` 到局部 active_code，之后不读外层 code，直到生成入口返回才释放（business_backend.cpp:536–549）。`CompiledBusiness` 声明中 jit 晚于 diagnostic，析构顺序使 LLJIT 清理时 reporter 所指 diagnostic 仍活着（26–35,428–430）；这里没有证明出的 reporter 悬空问题。

`verify_active_code_lifetime(false/true)` 已由原生 callback 实际抓到业务生成帧、撤销外层 owner，并可随后手抛 bad_alloc；返回前清根、退出后展开表不可查且页 MEM_FREE（business.cpp:991–1046）。这是真实 active-code pin 证据，但只覆盖同步销毁代码 owner，**不是**允许销毁 Runtime/Frame、卸载 DLL、并发销毁或任意 native C++ 异常跨 JIT。

### 2.2 最小错误优化反例

若把 active_code 的共享持有改成 borrowed `Entry` 以减少一次所有权操作，原有 `verify_active_code_lifetime(false)` 已足以反驳：首次 native callback 清掉最后外部 owner 后，仍有多个生成迭代和返回地址要使用该分配。方案 A 保留当前 pin；方案 B 仅在另一已验证拥有者覆盖同一完整调用区间时移除重复持有。**失效条件**：若不能证明最后回调之后、返回之前仍有持有，不接受 B；不要把引用计数成本存在等同于它主导耗时。

## 3. Windows unwind 与资源释放

### 3.1 生产者、消费者、资源拥有者均已追到

- 生产者：`windows_unwind.cpp:18–104` 在 COFF fixup 后检查/排序连续 .pdata，以 allocation actions 配对安装和撤表；`business_backend.cpp:90–94` 保留异步 unwind metadata，并声明 NoUnwind。
- 消费者：`native_client.cpp:53–82` 使用 RtlCaptureContext→RtlLookupFunctionEntry→RtlVirtualUnwind(UNW_FLAG_NHANDLER)，逐帧更新上下文，并对无表 leaf 弹返回地址。不是用帧指针扫描或仅查询一个表项。该消费者验证上下文回溯，不验证语言异常 handler 搜索/执行。[W1][W2]
- 资源拥有者：回移 `JITLinkMemoryManager.cpp:263–340` 先保护页、执行 finalization actions；出错沿 failFinalize/abandon 回收。正常 deallocate:464–500 先反序运行撤销 actions，再释放页。Plugin 的空 notifyRemoving/notifyFailed 与所有权在 allocation action 一致，不能因插件没维护另一张表就报漏清理。
- LLVM 23.1.1 `runFinalizeActions` 先 reserve 反向清理数组，再按成功顺序收集 Dealloc；后续 Finalize action 返回 Error 时反序撤销已成功项。[L2] 这说明配对机制，不覆盖 reserve 或后续元数据分配的资源耗尽行为。

### 3.2 覆盖与未覆盖必须分列

| 触发 | 当前证据 | 不能推导 |
|---|---|---|
| 已登记之后的 finalize/action 或 emission 故障 | backend.cpp:60–130 的真实 ORC 注入；既有验收检查撤表及 MEM_FREE | RtlAdd/VirtualProtect 自己失败已被试过 |
| native callback 撤 owner 后手抛 bad_alloc | business.cpp:991–1046，状态化并清根 | 根表、LLJIT 元数据、错误字符串分配耗尽仍可恢复 |
| RtlAddFunctionTable 返回 false | 插件转 LLVM Error，源码路径存在 | 内存极低时创建该 Error 一定成功 |
| RtlDeleteFunctionTable 返回 false | report_fatal_error，故意不继续释放仍可能被系统引用的代码页 | 可恢复退休或自动重试；这是明确 fail-stop 取舍 |
| VirtualFree/保护失败 | 本轮真实mixed分区自然触发VirtualFree无效地址，回移释放已确认；VirtualProtect及其它系统失败未覆盖 | 任意失败无资源留存；更不等于已验证整个进程无泄漏 |

在正常 deallocate 中，FinalizedAllocInfo 在运行撤销/释放前已经销毁；releaseMappedMemory 失败会返回 Error，但该函数不保存可重试页拥有者（464–500）。LLVM `LLJIT::~LLJIT` 把 endSession 错误交给 reporter；当前 reporter 只写即将随 CompiledBusiness 销毁的 diagnostic（business_backend.cpp:428–430；[L3]）。这是**系统失败下诊断/恢复策略的可证伪边界**，未找到当前受支持路径能必然触发，不能报成确定泄漏。若今后承诺资源耗尽后继续服务，需另行明确 fail-stop、可观测拒绝或资源隔离策略，不能本轮顺带重写内存管理器。

### 3.3 不用 mock 的真实系统调用反证

回移 `allocate:399–417` 先取得一个 slab，再把 Standard 起点设为 slab base、Finalize 起点设为 base+StandardSegs；`finalize:279–285` 先 release FinalizationSegments。锁定 LLVM Windows/Memory.inc 的 releaseMappedMemory 对非零块执行 `VirtualFree(Address,0,MEM_RELEASE)`；Microsoft 要求此地址为初次 reservation 基址。[L1][W3]

**推导**：Standard 和 Finalize 都非空时，后者是内地址；应该自然得到系统失败，进入回移的撤 actions→abandon 路径，最终从原基址释放整个 reservation。Standard-only 与 Finalize-only 是必要阳性对照，不用假装系统 OOM。

`out/review/wave1-meeting/execution-counter-finalize.cpp` 调真实 SDK `InProcessMemoryManager::Create`、`SimpleSegmentAlloc::Create/getSegInfo/finalize`、`deallocate` 与 VirtualQuery。查询紧跟 finalize/deallocate、早于字符串格式化和下一次单元分配，避免地址复用误判。没有替换 OS 调用、复制 allocator 或生成另一套对象世界。

**Main 本机结论**：三个分区组合均符合上述预期，mixed拒绝后两地址为MEM_FREE（原始结果及精确构建见§8）。**明确限制**：此探针没有 .pdata/action，不证明 unwind 撤销；它验证的是实际系统释放错误与回移 abandon。当前业务是否产出 Finalize 分区未知。未来工具链/策略改动若使两分区不再共享AllocationBase、mixed finalize成功或失败后不是MEM_FREE，必须重新判断此边界，不修改结果去维持旧预期。

## 4. 四类优化的语义代价

| 候选 | 最小反例/源证据 | A/B 与否证门槛 |
|---|---|---|
| 提前去根、根合并 | v0.0.2:115–120 明定词法活期；`verify_reference_collection:1211–1270` 在最后一次显式使用后强局部仍跨完整新 GC 存活，块外 weak-only 才回收 | A 保持词法根；B 只做不相交活期槽着色或稳定小存储，不先做最后使用点撤根。若 weak try_acquire/析构时机、独立赋值/清理或根撤销游标改变则否证 |
| 少 poll、批量 poll | v0.0.2:129–133 冻结当前入口及每条实际回边；`safe_point:225–228` 为 poll 后六次快照服务；错误前修改不回滚 | A 保留同步点；B 只在相同点合并冗余解析，保留 hook 与错误次序。减少频次不是本轮合法等价优化，须独立协议/授权及请求滞后、内存峰值、错误前副作用验收 |
| 跨服务直接字段/裸地址缓存 | `Runtime::resolve_field/resolve_path:1396–1420` 解析活身份及原生地址回调；`verify_stale_source_references:1382–1463` 在真实 poll 销毁旧对象并造替身，复制旧身份合法但解引用拒绝 | A 当前检查桥接；B 仅在已证明无销毁/迁移且借用完整覆盖的区域消除重复检查。非移动 GC 不证明活性；跨 poll/原生边界未经重新证明即否证。当前标量字段不需引用屏障，不应机械附加；未来引用字段直接写必须保留现有屏障 |
| 复用 LLJIT、删除版本 pin | 固定 atelier_source_entry，每候选 builder.create；服务表绑定和整个 CompiledBusiness 退休（business_backend.cpp:406–465） | A 独立单元隔离；B 共享初始化但隔离版本命名/JITDylib/tracker。旧 Lease+同名新候选+链接失败+独立退休任何一项破坏即否证；需测编译/发布/长期留存，不只首个编译时间 |

### “零浪费”和提速的反证

SourceLowering:48–64 给每个引用声明和 create/observe/acquire AST 临时量分配静态槽；清理扫描全部槽（178–187）。Invocation:89–111 超过两强槽时建完整根 vector，有弱槽时建观察 vector，Frame 本身仍不分配。

新 `execution-counter-semantics.cpp layout` 比较同输出函数：A 无引用局部；B 三个 `if (steps < 0)` 内各有一个强局部、观察局部及 observe 临时值。steps 是 uint，所以分支永不进入，但按现有生成逻辑预期 A=(strong2,weak0)，B=(strong5,weak6)。真实 Invocation hook 同时查看实际容器尺寸，最终 receiver/snapshot 值和根数相同。这里反驳的是“完整调用无额外存储工作”；不统计 allocator 调用数、不声称这些数组是主导瓶颈或已经发现提速百分比。一次 snapshot 仍是实际 Runtime 对象，不能被栈根证明隐去。

伙伴提出的默认正值 N 次循环 `10N+17` 次生成→host服务，经 `advance_machine.source`、copy_snapshot:217–224 与 safe_point:225–228 复核成立：1 create +(N+1) poll +6(N+2) snapshot字段服务 +3N 业务服务 +1 publish +2 clear。只适用于默认无引用局部、无错误完整路径，不含准入/元数据查询/Runtime 内部回调。调用次数不是时间占比。SDK `JITTargetMachineBuilder.h:153` 默认 CodeGenOptLevel::Default；没有显式 IR pipeline 不等于后端无优化，更不能把 host Release 叫作 JIT IR O2。

## 5. 双向质询记录与修正

独立初稿先写入本文件，再直接通知 WaveExecExplore；随后阅读 `execution/explore.md` 并完成双向带源码回应。

| 争议 | 对方主张与我的回应 | 修正/裁决 |
|---|---|---|
| owner 异常是否仍缺证 | 我要求区分已覆盖直接 execute pin 与真实 OOM。对方引用 business.cpp:991–1046，确认两条 owner+异常分支已覆盖，Rtl/VirtualFree 故障未覆盖 | 不重复报“owner 可销毁导致 UAF”；手抛 bad_alloc 不能作为 allocator OOM 审计结论。Runtime/Frame/DLL 的先存续条件保留 |
| 成本三命题 | 对方要求反证 CRT三层、10N+17及Frame/Invocation区别。我逐条回读 cmake/jit.cmake、SDK默认等级与safe_point实现；接受事实，要求限定默认标量成功路径且不推出耗时主导 | 双方接受；优化研究只承诺同语义归因，当前引用/重载增量没有新吞吐结果 |
| poll 是否已进版本契约 | 我曾误称§5.3仅约束表达式临时量、精确poll来自adapter。对方给出§5.3完整原文；我回读108–135，确认临时规则在§5.2，§5.3确实冻结本次位置 | **撤回我的异议并修正报告**：本轮不减poll；未来通用调度可另定，不将当前增量永久扩张为全部语言语义 |
| 共享 JIT/混合slab | 对方承认共享 JIT需版本隔离，混合slab未证实业务触发；我补锁定Windows Memory.inc和VirtualFree一手契约，并交真实最小驱动 | 不按赞成人数裁决；由Main运行三种segment组合。即使低层边界复现，也只说明能力/回退边界，业务可达性另证 |

Main 后续已确认 mixed slab 的预期低层结果（见§8），该项不再待验证。仍未决：当前业务图是否使用 Finalize、任意系统失败/持续 OOM 的恢复策略，以及任何优化的当前版本收益；这次自然无效地址失败不覆盖前述全集。证伪条件已随各项列出。

## 6. Main 可执行路径与最多三项优先动作

### 动作一：当前语义与槽成本反证

文件：`out/review/wave1-meeting/execution-counter-semantics.cpp`。真实包含 `../../../acceptance/jit/business.cpp`，不复制绑定/GC/执行器。用当前 `build-provenance.json` 中 host compile/link 改输入输出；**不编译另一份 business.cpp.obj**，避免重复定义。保留原 global_new_delete.cpp.obj、Runtime静态库、jit_probe/native_client导入库、mimalloc及当前系统依赖。Release host保持/MD，不改为LLVM侧/MT。

建议输出 `out/release/execution-counter-semantics.exe`，以现有DLL和advance_machine.source为同目录依赖。运行入口：`& ./out/release/execution-counter-semantics.exe layout`；预期原生/描述型各打印两行，0块为2/0槽，3块为5/6槽，均poll=1，清理后零根/零对象。`semantics` 模式调用真实 lexical collection和循环槽复用，再令第二次真实poll hook抛错；预期poll=2、已提交cycles=1、首错原文保留、清理前检查通过、返回后仅receiver根，然后归零。改变同步频次会改变这个当前适配器的可观察错误点。

`owner` 模式仅调用既有 verify_active_code_lifetime(false/true)，用于将来改pin时的精确门槛，本轮不必为了制造新结果再重复已有证据。直接字段候选的额外现有入口为 `verify_stale_source_references(runtime,binding,false/true)`；该入口已源码核实但未塞入本次layout探针。

### 动作二：自然系统失败与回移清理

文件：`out/review/wave1-meeting/execution-counter-finalize.cpp`。使用当前SDK头，compile沿正式jit_probe的/MT、NDEBUG、_ITERATOR_DEBUG_LEVEL=0、/GR-、C++23；直接链接当前正式 **third_party/llvm/JITLinkMemoryManager.cpp.obj**，以及正式LLVM静态依赖和所需系统库。不能仅链接jit_probe.lib，因为这些C++ API不是它的导出ABI；也不能偷偷使用上游未回移archive member替代正式对象。Main已接受按当前正式依赖组建独立exe。

所需符号：`llvm::jitlink::InProcessMemoryManager::Create`、`SimpleSegmentAlloc::Create/getSegInfo/finalize`、`deallocate`、`llvm::orc::SymbolStringPool`、`llvm::toString` 与 Kernel32 VirtualQuery。建议输出 `out/release/execution-counter-finalize.exe`；执行 `& ./out/release/execution-counter-finalize.exe`。

精确预期：Standard-only finalize成功，页保持MEM_COMMIT，deallocate后MEM_FREE；Finalize-only finalize成功即MEM_FREE；mixed两地址共AllocationBase但不同起点、finalize拒绝、回移后两地址MEM_FREE。最后 `Execution resource boundary: CONFIRMED` 只表示预期边界复现，不表示mixed成功受支持。错误文本本地化，不以英文文案相等作验收。

### 动作三：选择一个实测优化，不全面重写

仅当前槽/编译/桥接计量证明瓶颈后，选择“不相交词法槽复用”或“同同步点快照工作合并”一个单因素；共享LLJIT放在编译成本主导后。A为当前完整路径，B必须保持动作一语义、旧版本失败隔离/独立退休及原生/描述型一致。先记录首次调用、稳态、修改到发布、长期留存分别是哪一段；无内部阶段计时边界就保留aggregate，不异CRT相减伪造LLVM分段。旧S/F/P变体收益不稳定、未合入这一负结果继续保留。

## 7. 一手来源与适用范围

全部外部材料访问于2026-09-25；未作泛化搜索，直接读取已知官方URL。上游锁定llvmorg-23.1.1；本地回移以当前baseline和版本断言为准。Windows说明只用于当前x64进程内配置，不代表其它架构、任意C++ EH或调试器支持。

- [W1] Microsoft RtlAddFunctionTable：动态函数表供栈回溯/展开使用，成功/失败返回值。https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtladdfunctiontable
- [W2] Microsoft RtlVirtualUnwind：前一帧上下文及NHANDLER/EHANDLER/UHANDLER区别。https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlvirtualunwind
- [W3] Microsoft VirtualFree：MEM_RELEASE必须使用reservation原基址、size=0；不能释放单个后半分区。https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree
- [L1] LLVM 23.1.1 Windows/Memory.inc：VirtualAlloc按allocation granularity保留/提交；releaseMappedMemory使用VirtualFree MEM_RELEASE。https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.1/llvm/lib/Support/Windows/Memory.inc
- [L2] LLVM 23.1.1 AllocationActions.cpp：reserve、成功动作登记及反序回退。https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.1/llvm/lib/ExecutionEngine/Orc/Shared/AllocationActions.cpp
- [L3] LLVM 23.1.1 LLJIT.cpp，析构函数864–867：endSession错误进入ErrorReporter。https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.1/llvm/lib/ExecutionEngine/Orc/LLJIT.cpp
- 本地契约：会议README/baseline、docs/engineering.md、CONTEXT.md、docs/runtime-model.md执行协议、docs/language-contract.md、LANGUAGE-v0.0.2.md §§5.1–5.3；两份新探针只写本席位独占路径。Main已补回移文件到固定基线；本席位不再重复哈希验证。

## 8. 交付状态

报告及两份真实入口源码完整。双向质询已完成并记录一项实际纠错；未改任何生产/验收/构建/版本规范文件。本席位未执行验证；Main 已统一构建并运行 semantics 探针的三个模式，全部退出码0、stderr为空，原始证据如下。

| Main 本机执行 | 观测结果 | 原始文件 |
|---|---|---|
| layout | 原生/描述型均为0死块：strong2/weak0、无扩展根/观察数组；3死块：strong5/weak6，扩展根元素120字节、观察元素288字节；各poll=1，真实结果与清理断言通过 | `out/review/wave1-meeting/execution-counter-semantics-layout.json` |
| semantics | 原生/描述型均在poll=2报告错误；已提交cycles=1；返回后仅receiver根（1），后续归零；完整新GC词法保活和循环撤根复用断言通过 | `out/review/wave1-meeting/execution-counter-semantics-semantics.json` |
| owner | 外层owner销毁后正常返回，以及销毁后手抛bad_alloc的两种路径均通过；实际代码/展开信息到退出才释放 | `out/review/wave1-meeting/execution-counter-semantics-owner.json` |

这是功能和静态槽存储观测，不是 allocator 次数/占用测量或吞吐优化结果。120/288字节是元素大小乘容器size，不含分配器元数据、容量余量、对象快照及其余运行时存储；raw 的整进程elapsed_ms含启动/编译/检查，不能当热路径时延。

### Main 已完成的低层资源反证

原始结果：`out/review/wave1-meeting/execution-counter-finalize-default.json`；准确构建命令：`out/review/wave1-meeting/execution-counter-finalize-build.json`。Main 使用当前正式 jit_probe 的 /MT、NDEBUG、_ITERATOR_DEBUG_LEVEL=0、/GR-、C++23，直接链接正式 `CMakeFiles/atelier_jit_probe.dir/third_party/llvm/JITLinkMemoryManager.cpp.obj` 与正式 LLVM 静态依赖，没有混入 host /MD 对象。

| 真实分区组合 | finalize 返回及即时页状态 | 裁决 |
|---|---|---|
| Standard-only | accepted=1；Standard 为0x1000（MEM_COMMIT）；后续deallocate及MEM_FREE断言通过 | 标准寿命控制通过 |
| Finalize-only | accepted=1；Finalize 为0x10000（MEM_FREE） | 纯finalize寿命控制通过 |
| Standard+Finalize | shared_reservation=1；accepted=0；两地址均0x10000（MEM_FREE） | 自然系统失败与回移abandon释放已确认 |

退出码0、stderr为空、末行 `Execution resource boundary: CONFIRMED`。mixed 返回两条 `Attempt to access invalid address.`；源码链是首次释放内部Finalize基址失败、abandon再次尝试同一内部地址仍失败，再释放Standard原基址成功，不能把两个错误文本计成两份页泄漏。

**最终边界**：已证实真实内存管理器组合的能力限制及该分支资源释放，不是业务COFF触发证明；该探针没有.pdata/展开action，不能冒充真实unwind回退，也不证明任意系统失败/OOM恢复。当前业务泄漏和优化收益均没有新结论。D席位所交探针的统一执行已完成，无待运行阻塞；剩余问题是明确列出的范围限制，不能由已有PASS扩张。
