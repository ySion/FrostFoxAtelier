# D：执行、ABI 与 Windows 代码资源——Explore

> 历史快照：本文依据当轮 baseline.json；旧接口、文件名、行号与未实现判断仅描述当时状态，不是现行规则或待执行任务。后续结果与现行入口见[历史索引](../README.md)。

作者：WaveExecExplore。依据：会议 README、baseline.json 固定内容（不是 Git diff）；2026-09-25。状态：独立初稿及双方证据质询已完成，Main统一运行结果已补录§7.1；本席位不构建、不执行、不修改生产文件。

## 1. 结论与证据等级

**Spec：未发现已证实违约。Standards：未发现确定缺陷；存在可测的适配器固定成本与编译单元初始化成本。** 以下“源码证实”不等于本轮实测。baseline 记录当前生产哈希匹配此前 dev/release 通过证据；旧吞吐数字不能冒充引用/重载增量后的当前基线。不能用“有 shared_ptr/间接调用”直接推导慢，也不能把宿主 /Od、/O2 当成生成业务代码的 LLVM 优化等级。

## 2. 准入至清理的完整调用链

| 阶段 | 实际生产链与责任 | 证据 |
|---|---|---|
| 入口所有权 | Entry::invoke 锁定弱 State，读取一次 current 并持有 Version；Lease 复制本版本持有。Contract 持有 Binding/元数据，历史 SourceRequest 不持有代码 | business.cpp:472–603 |
| 入场 | execute 先 ExecutionScope::enter；Runtime 检查 alive、原生 callbacks、根 traversals，再增加 active_executions。之后检查 Runtime、接收者活身份/类型、代码与编译槽布局，最后构造 Invocation、登记 receiver | business.cpp:350–399；runtime.cpp:749–771 |
| 存储 | Invocation 的两个 ObjectRoot 是内联栈槽；strong_slots>2 时另建完整大小 vector；weak_slots 非零时另建观察 vector。ExecutionFrame 借用稳定槽，不自行堆分配 | business.cpp:89–111；execution.hpp:22–57 |
| DLL pin | atelier_invoke_business 调用前复制 code->state 到局部 active_code，后续不读 code；生成函数返回后才释放 CompiledBusiness 持有。外层 owner 可被同步回调撤销，但 Runtime/Frame/DLL 不能提前失效 | business_backend.cpp:536–551 |
| 生成执行 | create snapshot → entry safe_point → body → finish copy_snapshot → publish snapshot → clear all → return；每个可失败服务调用检查状态并跳 failure cleanup | business_backend.cpp:102–140、177–192、218–230 |
| 原生调用 | business_advance→frame.object→Runtime::call→CallbackScope→真实 Invoker；返回后才允许回边 poll，不将整个 JIT 包进 Runtime::call | business.cpp:184–204、641–663；runtime.cpp:1504–1534 |
| 错误 | bridge 捕获 Result/std::exception/未知异常，用首错固定512字节缓冲保存；IR失败边清强弱槽。Runtime::call 内仍可能为 Error 分配字符串，catch 不扩展持续 OOM 恢复保证 | business.cpp:113–132；business_backend.cpp:104–108、177–183 |
| 返回/退场 | publish 用 frame.take，活身份检查后移动原根；宿主在 Frame 析构前检查强槽已撤、弱槽已清，再检查状态/output。Invocation/Frame 析构先于 activation，活动计数覆盖准备到帧清理 | business.cpp:206–215、376–399；execution.cpp:64–76 |

Runtime gate 与 GC gate 不同。ExecutionFrame::poll 检查准入/正工作预算；idle 且无请求返回零工作，否则必要时 begin，再只推进一次 collect_step。已有轮次不需再次 request；request 与已有周期合并。活动 execution 不阻止自己的合法 GC，而阻止方法体发布；函数结束不自动排尽 GC（execution.cpp:87–101）。

## 3. 一次调用可证明的成本

### 分配、根与共享持有

- Frame 零堆分配不等于 execute 零分配。默认标量代码的强弱 vector 长度均为零；有额外强槽时每次建一个非空 ObjectRoot vector，有弱槽时另建观察 vector；无跨调用容量复用（business.cpp:106–111）。
- 每次成功调用都创建一个真实 snapshot：Runtime::create 对实例一次对齐 operator new，可能扩容对象槽/遍历存储/根表；源码 new 另算，不把对象分配归因 ObjectRoot（runtime.cpp:1207–1258）。
- receiver hold、snapshot create 各增加根。RootState 有 freelist，暖态登记不必每次扩容，不是每根 new 节点。强引用copy用 hold登记，空持有也登记；try_acquire 成功直接移动已有登记；观察copy只写 observations（runtime.cpp:65–139、1301–1348；business.cpp:230–277）。
- ObjectRoot 的 shared_ptr 保护 RootState，不是业务对象引用计数；move/take不新增根。execute 有 DLL 内 CompiledBusiness pin，Entry 另有 owner_.lock、Version复制，Lease 有 Version复制。各自保护不同失效边界，不能未做寿命反例就替换为裸指针；所有权操作数不证明耗时占比。
- 退场含显式clear、frame.empty/weak清空检查、Frame析构遍历强槽兜底，均随静态槽数增大。SourceLowering 为每个引用声明、每个create/observe/acquire AST节点分配槽；循环复用同一AST槽，不每迭代扩槽；不相交词法块目前不做槽着色（business_backend.cpp:49–64、145–164、184–202、299–364）。

### 字段、安全点：次数不是时间

新增桥接使用 bind_machine 缓存 FieldId路径，不按字符串查找。每次 read/write 仍走 frame.object、活代际、路径/类型检查；原生字段每级地址适配，描述型走偏移；地址仅借用到当前服务返回（runtime.cpp:1396–1421、1484–1501）。advance_described 自己的 type_of/find_field/read/write 属旧fixture额外成本，不归给缓存桥接（business.cpp:647–663）。

默认 advance_machine、amount>0、N个完整迭代、无错误：生成→宿主服务共 **10N+17**：create=1，poll=N+1，snapshot字段读写=6(N+2)，advance=N，业务cycles读写=2N，publish=1，clear=2。amount≤0/NaN 时为7N+17；空body为17。safe_point先poll再copy_snapshot，故Hook看上次同步快照。idle/no request仍有桥接/准入/预算/统计，随后六次字段桥接不消失。“仅计数循环”不是纯算术基准。上述不含Runtime内部地址适配、原生回调和外层ABI查询。

## 4. 编译与两个 CRT 域

真实链：host compile_business / prepare → C ABI → compile_source（词法/语法/绑定/定型）→ compile → native target初始化、detectHost、Large/PIC、每候选新LLJIT → 绝对服务符号 → SourceLowering+verifyModule → addIRModule → lookup强制物化 → JITLink/展开登记 → BusinessCode。失败只在最终成功后写output，lookup失败合并源名/materialization/ORC根因（business_backend.cpp:406–502）。

| 维度 | 当前事实 | 不可声称 |
|---|---|---|
| 宿主/Runtime/native DLL | atelier_target：Debug /MDd，Release /MD；配置通常分别 /Od、/O2 | 宿主Debug=JIT O0 |
| LLVM隔离DLL | cmake/jit.cmake:40–74 核实SDK Release、静态CRT、无RTTI/断言；自身 /MT、NDEBUG、_ITERATOR_DEBUG_LEVEL=0、/GR-；宿主配置优化标志仍是另一维度 | 全项目改成Release CRT、STL可自由跨域 |
| 业务机器码 | detectHost+Large/PIC；无显式IR优化流水线；锁定SDK JITTargetMachineBuilder.h:154 默认CodeGenOptLevel::Default | 没IR pipeline=后端不优化；Release=手动O2 pipeline |

边界只传固定宽度数值、函数指针、不透明上下文/句柄、调用方字符/标量存储与简单聚合视图。LLVM/std::string/shared_ptr/异常/delete留DLL；Runtime/根/Frame/Invocation留host。CodeInfo/Layout名字是借用，不能跨最后owner/pin后保存。extern C是名称/调用边界，不是公开稳定、跨工具链、可由C直接编译的通用ABI。

SDK预编译库是Release，不代表dev配置下隔离DLL自有源码必定/O2：atelier_target仍继承对应配置的编译优化选项，而/MT、NDEBUG、iterator level另行固定。不得将DLL、宿主、生成业务的三个优化维度混成一个“Release JIT”。

异常路径还须分段：execute在进入DLL前构造Invocation vector或登记根可能抛std::bad_alloc，并未承诺所有host OOM都化为Result；已建立的ExecutionScope经C++ RAII释放计数。编译C ABI内的解析/LLVM异常在DLL捕获，用调用方诊断缓冲返回；生成服务的C++异常在host bridge内截获。/EHsc与NoUnwind不等于捕获任意访问冲突/SEH，更不等于支持跨JIT C++异常。回调销毁CodeOwner时，DLL pin只需覆盖生成函数返回；后续host frame检查不再读取code/其借用名字，而Entry/Lease的Version仍覆盖外层调用。

## 5. Windows资源与负结果

Plugin PostAllocation校验image span/RVA；PostFixup验证原位、连续、对齐.pdata并排序区间，把add/delete作为配对allocation actions。manager先页保护/清指令缓存再登记；退休先逆序dealloc actions（RtlDeleteFunctionTable）再释放代码页。撤表失败fatal防止OS持有已释放页，非可恢复发布失败（windows_unwind.cpp:16–108；third_party/llvm/JITLinkMemoryManager.cpp:261–330、463–500）。

LLVM 23.1.1版本守卫backport处理finalize失败：前置失败abandon；部分动作失败runFinalizeActions回滚；登记后finalize slab释放失败先runDeallocActions再abandon。插件notifyRemoving/notifyFailed不另建表是分配所有权设计，非漏清理。

**Main本机确认的低层能力边界**：已读取 `out/review/wave1-meeting/execution-counter-finalize-default.json`、对应 `execution-counter-finalize-build.json` 及探针源码。当前 `/MT`、正式backport对象、LLVM静态依赖链，未混入host `/MD`/Runtime；真实SimpleSegmentAlloc的Standard-only与Finalize-only两个control成功。mixed两个分区位于同一reservation，finalize自然返回 `Attempt to access invalid address`（非mock/故障替换），accepted=0；回移清理后两个地址立即查询均MEM_FREE。整探针exit0、stderr空、CONFIRMED，证明“混合分区当前不支持，但该实测失败路径清理成功”。**它不证明当前业务COFF会产生Finalize分区，不含Windows unwind allocation action，也不证明业务泄漏、全面系统资源恢复或OOM恢复**。本席位只吸收Main证据，未运行。

保留负结果/限制：默认LLJIT曾无Windows展开登记；完整COFFPlatform曾有COMDAT/导入跳板问题，移除路径不成立，故当前是受控IR+inactive platform+allocation plugin。没有任意C++对象静态初始化/TLS/跨JIT C++ EH；没有PDB/源码断点/局部变量调试，RtlVirtualUnwind栈回溯不是源码调试。Large model因已遇远距PCRel32限制，不未经反例改Small。

此前快照、字段桥接、idle-poll三个单因素变体收益不稳定，均未合入；此前记录未重测引用/兼容发布后的吞吐。本轮Main新增的当前版微基线见§7.1，不以旧最优数作优化对照，也不改变原有变体未合入的负结论。

## 6. 最多三项优化研究路径

Main实测补录后，研究优先次序为②→①→③：先定位静态槽与早退清理IR的规模问题；编译总段尚不足以证明LLJIT setup主导；已有S/F/P负结果不因本次baseline通过而作废。以下均是研究候选，不是生产改动授权。

1. **编译初始化摊销，先测再改。** 先分区测当前编译/首次调用/热调用/退休；后端总段主导才研究复用target/LLJIT setup。固定atelier_source_entry、服务隔离、失败候选不伤旧版、不同tracker独立退休先证实。A独立LLJIT简单隔离；B共享不可变setup/隔离单元。若瓶颈是前端或等待、或共享破坏寿命，不推进；不能拿total减另一个CRT前端时间叫JITLink时间。
2. **引用槽存储/静态槽复用。** A完整vector；B只在不相交词法活期做slot reuse/小容量稳定栈，或有明确owner的容量复用。不得提前撤词法根、覆盖嵌套活帧、搬动借用或删RootState pin。先测0/少/多槽及native/described；若分配不主导或峰值槽不降，拒绝复杂化。
3. **同语义快照/字段工作合并。** 只在每个现有同步点减少重复解析，不少poll、不改hook/错误先后。A六次字段服务；B完整校验的受控服务内相同读写，保留原生适配和地址有效期。任一hook/错误/副作用改变即否证；旧负结果使它不是默认实施命令。

## 7. 当前版一次性探针（Main已执行，Reviewer未运行）

文件：`out/review/wave1-meeting/execution-probe.cpp`。它直接包含当前 `business.cpp` 私有adapter，链接正式Runtime/JIT/native DLL，不复制Runtime或Lowering；不会编译历史protocol变体。编译规则来自本批 `build-provenance.json` 当前CMake/Ninja命令，而不是旧探针的ABI猜测。

**阶段与边界**：

- compile_total：host compile_business直到真实lookup物化返回；包括frontend、fresh LLJIT、Lowering、codegen、link/unwind。每场景5次，第一进程编译单列；不含进程启动/DLL加载/源码字符串准备。未增加生产内部计时边界，故不虚报IR/机器码/链接各占比。
- first_execute：每个保留代码单元第一次真实受检execute；不是整个进程首次机器码执行。包括根/帧准备、真实snapshot分配、服务、结果移交以及驱动take，不等于纯机器码。
- warm_batch：128次调用，预热2批、记录9批，统计中位/P95并保留每批原始时间；单个receiver连续累积，128个snapshot分别核值。无内部GC请求，idle poll仍执行，随后reclaim是单独计时。
- reclaim：释放输出根后调用原adapter的collect，**工作预算1**，完成必要旧周期和完整新周期；检查回到1个receiver根/对象。不是旧基线预算256，不做数字横比。
- retire：释放CodeOwner到LLJIT析构返回，区间外用RtlLookupFunctionEntry/VirtualQuery立即确认展开表消失及MEM_FREE；在下一次JIT分配前检查，避开地址复用假象。

**真实场景与源码可证伪预期**（native/described各一份，共16场景、80次compile）：空body强2弱0/poll1；默认正值16步强2弱0/poll17；strong/weak/try_acquire body强5弱2/poll1；顺序不重叠null块N=1/16/64强2+N弱0；带条件早退N=16/64同布局且steps=0从首块返回。后两者真实槽数如不同即驱动报错，不把预测冒充结果。所有body执行后检查逐个snapshot、receiver、显式cleanup、精确poll、root_count；每场景结束对象/根归零，最终NativeState::live回到进入前。

Main构建入口：已确认由Main的本批 `waveBuildProbe` 直接按当前 `build-provenance.json` 派生CMake ABI命令，**不另建重复build.json**。只替换host main/business两个对象为本include驱动，保留allocator对象、正式Runtime/JIT/native客户端库；不编译额外host前端，不改两CRT边界。精确运行入口为 `./out/release/execution-probe.exe --measure`。Main串行启动新进程，保留jsonl/stdout、stderr和退出码；首轮用于路径证明，后续交错/机器状态控制由Main统一决定。9样本的nearest-rank P95等于样本最大值，不是尾延迟保证。该方法说明原为执行前合同；Main实测数值与通过范围补录于§7.1，仍没有分配事件实测。

**探针覆盖限制**：没有root/allocator细粒度事件计数，没有实时峰值采样，没有请求内GC负载，没有Entry/Lease分派开销的独立基准，没有IR/codegen/link内部分段，未改变JIT优化参数，也不验证系统调用故障。这些都不是隐藏在total里的已验证项目。

### 7.1 Main 本机实测补录：当前版路径通过

证据：`out/review/wave1-meeting/execution-probe-measure.json`。由Main统一构建/执行，本席位只读取、解析与汇总，未重跑。退出码0、stderr为空；16个summary及最终complete均PASS，记录80次compile、80次retire、16次first_execute和144个正式warm_batch（每场景9批）。根、对象、snapshot、poll、显式清理及代码/展开退休断言均在这次真实运行中通过。

下表每对数字均为**原生 / 描述型**。compile是完整编译aggregate中位数（毫秒），排除全进程唯一首次编译样本；原生empty因此剩4次，取中央两值平均，其余各5次。execute是**128次受检调用组成一批**的9批中位数（微秒），不能读成每调用、纯机器码或完整游戏Tick。

| 源码场景 | 实际强/弱槽 | compile总段中位 ms（原生/描述型） | 暖execute中位 µs/128次（原生/描述型） |
|---|---:|---:|---:|
| 空body | 2 / 0 | 3.474 / 3.357 | 86.6 / 90.4 |
| 默认正值16步 | 2 / 0 | 4.526 / 4.480 | 731.7 / 840.8 |
| strong/weak/try_acquire | 5 / 2 | 4.391 / 4.416 | 130.7 / 134.4 |
| 顺序null块 N=1 | 3 / 0 | 3.195 / 3.151 | 98.4 / 101.6 |
| 顺序null块 N=16 | 18 / 0 | 5.707 / 5.834 | 238.0 / 244.1 |
| 顺序null块 N=64 | 66 / 0 | 13.724 / 13.470 | 693.7 / 696.3 |
| 条件早退块 N=16 | 18 / 0 | 11.565 / 11.412 | 128.3 / 132.2 |
| 条件早退块 N=64 | 66 / 0 | 110.051 / 109.757 | 230.3 / 240.8 |

补充分段：

- 唯一process_first_compile是39字节原生empty，**6.657 ms**；不含进程启动/DLL加载，不能称完整冷启动。各保留单元first_execute为4.9–12.4 µs，后续场景已受进程缓存和Runtime容量历史影响。
- 16场景reclaim批中位为**14.3–14.6 µs/128个输出**；预算1、撤输出根和完整收集计在该段，非旧基线的预算256。默认16步execute样本P95为739.0 / 855.4 µs/128次；9样本nearest-rank P95实际就是各组最大值。
- 80次独立CodeOwner退休耗时合并中位**56.85 µs**，样本范围36.8–93.9 µs；包括不同代码形状，只用于描述本探针样本，不形成通用退休SLA。每次都在下一次JIT分配前通过表消失/MEM_FREE检查。
- 如需摊销值，默认16步是5.716 / 6.569 µs/调用，empty是0.677 / 0.706 µs/调用；这些只是批时间÷128，仍含完整host协议和驱动，不把差值拆成某项validation/shared_ptr/分配成本。

**推论和否证边界**：顺序块的实际2+N槽已由源码推导升级为Main本机实测；词法同时活跃最多一个null局部仍需66静态强槽。N=64 early-return编译总段约110 ms而同N顺序块约13.5–13.7 ms，支持优先定位SourceLowering每个return复制全槽清理的规模成本；源码有重复清理IR，但本探针未单测IR构建、机器码生成或链接，不能把这约96 ms差值全部归给其中一阶段。执行测量也同时改变声明/hold/clear数量，**不证明vector分配主导**。因此第二研究路径优先同时检查静态slot reuse与受词法活期约束的清理IR规模；暂不优先共享LLJIT或重跑已有负收益桥接变体。

这是一进程、固定顺序、两次预热后的当前路径/初步微基线，不是优化A/B、跨进程置信区间或性能改进。记录未提供CPU亲和性/频率锁定证明，不假定已控制。Main全进程墙钟约1.691秒还包括日志和计时外断言/设置，不等于各执行段总成本。

## 8. 至少一轮证据质询已完成

搭档独占稿：`execution/challenge.md`。本稿独立落盘后已用hub发送路径+三个争议，随后完整读取对方稿并交换源证据。

| 反方论点/质询 | 本方回应及证据 | 修正/未决与证伪条件 |
|---|---|---|
| owner撤销+bad_alloc是否覆盖真实pin，是否可称OOM恢复 | business.cpp:991–1046确实通过native callback捕获生成栈、撤CodeOwner、可选手抛bad_alloc；business_backend.cpp:545–546局部active_code仍存活；错误后检查部分推进/根/代码释放 | 确认是直接execute的DLL pin路径，不是allocator耗尽；RtlAdd/Delete/VirtualProtect/VirtualFree自身故障未穷举。此范围已由对方接受 |
| 提前去根、少poll可能改变weak取得、错误及副作用 | 候选仅不相交词法块槽复用，绝不last-use撤根；当前v0.0.2:113–118要求词法活期；poll之后快照同步意味着Hook/失败顺序有观察 | 对方曾反驳§5.3没有明确poll位置；本方给出129–133“入口和每条实际循环回边的位置不变”的原文位置，对方回读后明确撤回。修正为“当前增量合同，不永久冻结一般语言调度”；未来改变需独立授权/验收 |
| 共享LLJIT固定入口、失败与最后owner退休是否仍独立 | compile:406–457每新LLJIT自然隔离atelier_source_entry与服务符号；共享绝非仅缓存builder。要以不同JITDylib/可区分符号+tracker等真实机制处理旧租约与候选失败 | 当前仅性能假设；若新同名候选改变旧版本调用、失败伤旧资源或退休早于最后pin，则否证。未实现共享方案 |
| 本方三个成本论点是否成立 | 搭档逐条核对CodeGenDefault、safe_point、默认body与Invocation，确认10N+17仅默认标量无错路径、不含host准入；Frame零堆不等于execute零分配 | 双方不以一致意见代替测量；所有收益仍未证实。探针加入顺序块1/16/64与early-return，C席位只做frontend避免重复 |
| mixed Standard/Finalize slab是否自然触发Windows释放失败 | 对方源码追踪后由Main运行真实SimpleSegmentAlloc：两个单分区control成功，mixed同reservation自然拒绝，回移清理后两地址MEM_FREE；证据文件及ABI见§5 | 已从可证伪风险升级为本机确认的低层能力边界；当前business生成图是否含Finalize仍未证实，实验不含unwind action。不升级为当前业务缺陷/泄漏或全OOM恢复；本席位未重跑 |

## 9. 一手资料：仅解决具体争议

访问日期均为2026-09-25；检索采用Exa，官方内容直接回读。本文机制资料与源码证据分开，不将API存在当作本项目配置已通过。

- **LLVM锁定23.1.1**：项目SDK `JITTargetMachineBuilder.h:154`、`Support/CodeGen.h:81–86`，并核对官方同tag源码 `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.1/llvm/include/llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h`。默认CodeGenOptLevel::Default为后端等级2；这不创建中端IR O2流水线，也不是host /O2的自动传播。没有使用滚动main推断锁定SDK。
- **Microsoft Win32 x64**：`https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtladdfunctiontable` 与 `https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtldeletefunctiontable`（Win32滚动API文档，本日访问；不据页面推断特定SDK内部实现）。动态机器码需提供函数表，删除API作用于之前登记的表；支持add/delete配对职责，不自动证明任意失败都恢复。Add页面推荐更动态场景考虑callback，不等于禁止每版本一次生成/退休的表方案，也不证明换callback更快。
- **Microsoft CRT文档，MSVC文档视图170**：`https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries?view=msvc-170`。独立CRT状态/分配释放需匹配，支持保持DLL内LLVM/STL/异常所有权；本项目共享mimalloc不能据此推导STL ABI/CRT状态已经统一。实际二进制配置仍由本地CMake及锁文件决定。
- **LLVM ORCv2概览**：`https://llvm.org/docs/ORCv2.html`，滚动设计文档只作“lookup物化、JITDylib/资源可组合”机制依据，不用于推翻锁定版本的COFF默认层或宣称ResourceTracker自动等待活跃执行。当前pin及退休安全来自本项目源链。

本地权威：会议README/baseline；engineering/CONTEXT/runtime-model；jit-execution-design §§3–4、7、9、10.2；上列局部源段；LANGUAGE-v0.0.2 §§5.1–5.3；jit-industrial-research的旧基线及S/F/P负结果。旧COFF/调试限制和旧功能通过均按既往记录引用，未在本席位重跑。

## 10. 交付与状态

交付个人报告与一次性源文件；构建/执行均由Main统一完成，不另交重复计划文件。本席位未改生产代码/语言规范，未启动构建、运行、测试、格式化或压力负载。已读取Main的execution-probe-measure.json并压缩补录：16场景、80编译与退休PASS，128调用批次的执行/回收及完整编译aggregate边界均保留。当前槽布局预测已有本机证明；成本归因和优化收益仍未证实，不把该当前基线称作性能提升。未验证项继续保留原证据等级，无需重跑。
