# B 独立反方：根、屏障、游标取消与身份有效性

> 历史快照：本文依据当轮 baseline.json；旧接口、文件名、行号与未实现判断仅描述当时状态，不是现行规则或待执行任务。后续结果与现行入口见[历史索引](../README.md)。

日期：2026-09-25。作者：WaveGcChallenge。状态：独立初稿、点对点交换及一轮带源码反驳已完成；Main 已按正式 Release 宿主配置编译并分别运行 cursor、identity、sweep，三个原始结果均 exitCode=0。作者仅只读核对 raw 并更新报告，未自行运行；生产代码及版本化规范无修改。

## 1. 意图、范围与结论

本席反驳“能保活一次就等于根协议正确”“软预算就是固定帧预算”以及“根计数非零就等于有效对象”。检查同槽新代际、暂停游标、外部借用撤销/复用、晚根、弱取得、hidden native 字段、部分赋值失败和执行错误清理。

**在本轮所读合同及 Main 执行的有限组合中，未发现可证实的新语义缺陷。** 源码和新增本机路径支持身份隔离、撤根取消、统一死亡提交及错误清理；不把有限场景称为穷尽交错证明。Main 已证实一个 sweep 工作单位完整析构 257 个 native 子记录；单次 elapsed_ns=1200 不是时延上界或分位数。没有真实负载数据，不主张更换收集器。

- 固定依据：已完整读取会议 README.md / baseline.json、docs/engineering.md、CONTEXT.md、docs/runtime-model.md 与 docs/language-contract.md。没有 Git 基线，不制造 diff/commit。
- 既有本机证据：baseline.json 声明对应哈希的开发/发布 Runtime 与 JIT 验收通过；这是复用结果，不是本人重跑。
- 本轮新增：局部调用链、工作单位推导，以及 Main 生成的 `out/review/wave1-meeting/gc-counter-lifetimes-{build,cursor,identity,sweep}.json`。已逐份只读核对；第 7 节区分设计预期、实际结果和适用边界。
- 非目标：并发/分代/移动 GC、任意语言记录帧、协程取消、完整 OOM 恢复及任意原生内存安全。当前是单线程可信宿主接口。

## 2. Spec：完整路径与反例判定

### 2.1 同槽复用：核对释放、重建和实际消费端

ObjectId 携 context / index / generation；lookup(ObjectId) 与 resolve_object 检查上下文、存活及代际（runtime.cpp:498–502、1197–1205）。release 先移除灰队列并取消对象游标，置 storage=null 后才析构；recycle 增代际，uint64 上限则退休该槽（726–740、435–442）。create 发布新身份，新生对象在 roots/mark 补标，在 sweep 视为本轮存活（1207–1261）。

合法序列“活动对象游标→显式 destroy→同槽 create→旧身份访问”应在消费端被拒。ExecutionFrame::object 只交身份，host read/write/advance 仍走 Runtime 的 read/write/call（execution.cpp:60–64；business.cpp:147–190）。旧 ObjectRoot 可继续 registered，typed hold 可复制失效身份；这些都不授权新对象访问。

只断言 replacement.id()!=old.id() 不够，还须检查旧 weak 取得失败、旧 raw hold/take 失败且原槽未被错误替换、新对象字段不变、下一完整周期不被旧根保活。第 7 节 identity 路径覆盖这个组合。

### 2.2 暂停游标撤根：先解队列，再废借用，最后允许复用

RootState::remove 先通知 removing，后清登记内容/反向值索引/free-list（115–135）；实际 observer 消费端 root_removing 解链 root queue，并取消该登记拥有的活动游标（351–384）。collect_one 明确分派当前游标→值根队列→灰对象队列，不会把取消事件默默丢弃（455–477）。

native locator 可以撤销当前 ValueRoot；trace_one 在 locator 返回后检查 trace_owner==none，先于读 ReferenceSlot 或压内联子游标（416–433）。此时 traversals 非零，root_value/hold/try_acquire 及 ObjectRoot 复制新增登记都被拒；不能当场复用登记让取消状态“复活”。

两条关键边界：

- **同步借用未返回时 free 不合法。** 取消跨步游标不允许在当前 locator/visitor/整值操作内释放借用存储；同步 shade_value 也遵守此期限。
- **同址重叠的两份 ValueRoot 是独立持有。** 撤一份不能取消另一份；之后再次看到同地址 locator 不自动等于 UAF。value_entries swap-pop 维护反向位置；只要仍有重叠登记，destroy_value 拒绝（115–144、1183–1194）。

cursor 探针在 collect_step 返回后才释放无根存储；重叠对照要求 destroy_value=busy。所有引用内容在 begin 前填好，避免运行中屏障为错误根队列“遮丑”。

### 2.3 晚根、弱取得和 hidden 字段共用同一消费链

root_added 在 roots/mark 对对象立即 shade、对值根插入队列；超过 root_limit 的新登记也不漏标（363–379）。shade 验活身份/epoch 后置色入灰队列，collect_one 取项并 start_trace（337–349、465–470）。try_acquire 验 weak/live 后在返回前 add 根，检查与登记之间无用户回调/收集点（1333–1346）。

三个引用写入口汇入 write_reference_at，只有 strong 新目标补标（1429–1482）。hidden 只控制公开 FieldInfo；所有声明字段进入 type.fields，再从 strong 与含 strong 的内联字段构建 trace_fields（955–964、1014–1034）。改扫公开字段会漏 hidden strong；把 contains_references 等同 strong 则会误保活 weak。

整值构造成功及赋值成功/部分失败都同步 shade_value（624–635、1154–1181）。故“assignment_failed 可跳屏障”不成立：原生可能先写 strong 再抛。原生直接绕过 Runtime 赋值 ReferenceSlot 则违反调用合同，不是合法漏标反例。

### 2.4 统一逻辑死亡，不给清扫留复活窗口

游标/值根队列/灰队列均空才一次提交 pending=live-marked、live=marked、phase=sweep（471–477）。sweep 的 is_live 要求本 epoch 已标记（316–318）；未标记对象即使尚未析构也不能 type_of、hold(ObjectId) 或 try_acquire。typed hold 可保存陈旧身份，但 shade 不复活。

release 在析构前使本对象无效，整个 step 在 callbacks 保护内；同轮判死对象都已不可访问，析构重入 collection 被拒（726–740、792–814）。strong root 不应擅自改成显式销毁锁。

### 2.5 错误清理：生成端和 thunk 分派均已追到

ExecutionFrame::hold/create 先拿新根再替换，失败保留原槽；take 在 move 登记前验证活身份（execution.cpp:28–71）。typed null/stale 可复制，不等于一般语言返回都应套快照 take 的查活规则。

生成端 checked_call 的非零状态进入统一 failure block；success publish 后 clear；clear_user_references 包含 weak 和全部用户 strong，再清 snapshot/receiver（business_backend.cpp:100–136、171–188）。消费端 clear_slot 按位标记分派到 frame.clear 或 observations[index]={}；change_reference 对 copy/observe/try_acquire 有明确分支，取得成功直接 move 已登记根（business.cpp:229–274、294–305）。返回时在 Frame RAII 前检查所有 strong 未登记、weak 清空（379–392），不会单靠析构掩盖 IR 漏清理。

这些是已实现链路的证据，不证明任意未实现控制流、异步取消、跨线程销毁或 OOM 全恢复。

## 3. Standards：必须说明的成本，不是假缺陷

### 3.1 工作单位并不限制内部全部工作

一个 sweep 单位含整对象 described/native 子对象析构及释放（collect_one:479–486→release:726→destroy:653–677）。显式栈消除 described 深递归承载，不把析构拆成多步。**宽 described 对象中许多小 native 析构，也可合成一个长单位**，不仅是某个用户析构特别慢；native locator 同样包含在一个字段单位中。

max_time 仅工作单位之间检查，活动 step 至少一单位（792–814）。begin_collection 预留 trace_stack 在 step 计时外，poll 可能先 begin 再 step（774–789；execution.cpp:89–101）；根操作、whole-value 赋值及 shade_value 也不属 step 预算。正时间预算的读钟成本需配对数据，不凭名称下结论。

以上都在 runtime-model 的预算合同中允许。应分计 caller 外 wall、begin、mutator 屏障、step 分布和整轮总量；单看 progress.elapsed 降低不证明 Tick 改善。

### 3.2 历史峰值、活跃集和累计次数不同

H=entries.size 控制根扫描，S=slots.size 控制清扫，V=value_entries.size 控制重叠检查。H/S 是位置高水位，常规复用下不随累计 hold/create 次数无限线性增长。root_count 包含 typed-null/stale 登记，不等于活对象数；R=0/live=0 仍扫描历史空表是计量合同允许行为，容量保留不自动等于对象泄漏。

固定图、无中途 mutator 增删、完整周期：**W=H+S+Q+T+3**，Q=值根/对象取项，T=实际 trace 字段及进入/退出单位，3=三次阶段转换。独立核对探索方的无 trace 空对象三式：一活对象/历史 H 根为 H+5；H=S、仅一活对象为 2S+4；H=1、空堆历史 S 槽为 S+4。它们不是毫秒；新增、删除、取消或本轮保守标记均要求重算 Q/T 与限界。

## 4. 当前验收真正证明 / 未证明

“已覆盖”来自正式断言和固定哈希基线，不表示作者重跑。

| 领域 | 已有真实断言 | 不能外推 |
|---|---|---|
| 代际/weak | runtime/main.cpp:595–674，显式销毁保留旧身份、替代对象拒绝旧 weak | 实跑 uint64 代际耗尽、全局 context 耗尽、并发 |
| 环与逻辑死亡 | runtime/main.cpp:979–1039，混合环、地址稳定、death-before-dtor、死亡后取得失败、sweep 新生存活 | 任意图/交错穷尽、实时延迟 |
| 晚根/已扫字段/weak | runtime/main.cpp:1042–1092，hidden 已扫字段新 strong、普通写入、根复制、weak 取得/不保活、下一轮收敛 | 未来 barrier 裁剪正确性 |
| 部分赋值失败 | runtime/main.cpp:1100–1147，原生先改引用再失败仍保活；未登记值下一轮不成为根 | 任意错误/OOM 注入或吞吐 |
| 取消/复用/关停 | runtime/main.cpp:1150–1225，活动对象 destroy、撤值根、排队存储释放/替代登记、各阶段关停 | 同步借用内违规 free 安全；任意重叠组合 |
| locator 内取消 | runtime/main.cpp:1350–1425，inline 与 leaf 都不继续追踪，结构重入拒绝 | 任意原生错误地址验证 |
| 活跃值根 | runtime/main.cpp:1428–1460，重叠、move/swap-remove、项复用、10000 历史对象根下正确性 | 10000 正确性场景不是性能基准；根阶段不只扫 V |
| 执行根错误 | business.cpp:734–988，唯一执行根、完整新周期、native/poll 错误、身份拒绝、失败替换保留、explicit_cleanup | 仅 RAII 不能证明生成清理；任意值帧/协程未实现 |
| 源码引用 | business.cpp:1211–1585、1673–1702，强弱、循环槽、早退/异常、stale 解引用、临时值/失败初始化，native/described 隔离 | 任意语言活跃集和通用异常机制 |
| 深内联 | runtime/main.cpp:1480–1589，深值操作、重入/visitor 错误、strong/weak | 宽析构单单位尾部、H/S 当前比例、生产 p99 |

### 本轮 Main 新增的本机补证

以下不是旧基线外推，而是第 7 节三份 Release 原始运行 JSON；均 exitCode=0。

| 模式 | 此次真正证明 | 仍未证明 |
|---|---|---|
| cursor | inline/leaf × 有/无独立重叠根的四组合；无重叠取消后访问计数为0，有重叠独立追踪继续；登记复用及撤根后新周期收敛 | 任意交错、违规同步借用内 free 安全、所有原生地址正确性 |
| identity | 活动对象游标销毁后同槽新代际隔离；stale weak/raw hold/take 拒绝，失败保留旧槽；已扫/未扫边与下一周期收敛 | 代际或 context 耗尽、跨线程交错 |
| sweep | 一个 work 完整逆序析构257个 native 子记录、reclaimed=1且 pending 最终为0 | 1200ns 的上界/分位数、长期和生产析构负载、硬时间预算 |

## 5. 假阳性和优化反例

| 主张 / 候选 | 否决理由 / 失效链 | 分类 |
|---|---|---|
| Root 无 generation 必串代 | handle 拥有独立活动登记，撤销先解队列；没有外露可长期复制裸 index。ObjectId 则确需 generation | 已覆盖/不成立；改定位策略再审 |
| registered 应阻止 destroy | 合同区分自动保活与显式销毁；旧根 registered 且身份未清空属预期 | 已覆盖/不成立 |
| 撤根本轮未收就是泄漏 | shade 后不撤色，须下一完整新周期观察 | 已覆盖/不成立 |
| 撤一根后再见同址 locator 就是 UAF | 另一份重叠根可能合法保活；无根也须等同步借用返回才 free | 假阳性，组合探针补证 |
| hidden 不参与 GC | exposed 只影响查询；改扫公开字段会收错 hidden strong 目标 | 优化反例，拒绝 |
| assignment_failed 不必 shade | 部分失败可已更新 strong；丢屏障会误收 | 优化反例，拒绝 |
| dense swap-pop 可直接替 root_cursor | 已扫位置0被删除，末尾未扫项换入0会漏扫；快照 raw storage 又须响应撤根取消 | 可证伪风险，需重排/取消协议与授权改变空项 work |
| 压缩 slots / memcpy 活对象 | ObjectId.index 属身份；原生对象与合同要求 GC 地址稳定 | 非透明优化，拒绝 |
| 将追踪移到 mutator 就更快 | 总 mutator+poll/周期成本可能不降；不得只展示更小 step.elapsed | 性能假设，待配对测量 |
| root_count=0 可直接跳 sweep | 本轮 gray/保守 shade 与 sweep 新生仍有语义，零根不是队列空/授权撤色 | 优化反例，拒绝 |
| 分代/并发天然合适 | 要重定义单线程、屏障、weak/取消协议；没有当前负载收益数据 | 本轮不选型不移植 |

## 6. 双向讨论记录

双方独立落初稿后，经 hub 互发文件和三项争议；本席已读 gc/explore.md，探索方已确认读本报告。

1. **取消合法性**：本席要求 locator 返回后合法失效借用的完整路径。探索方以 trace_one:416–433、traversals 和 root_value:1348–1356 回应：禁止当场再登记，未发现合同内漏标。修正：维持“已覆盖/不成立”，新增重叠独立根对照，不能将后续同址访问全判 UAF。未决/证伪：无重叠根，collect_step 已返回且旧存储释放后仍被访问；或独立重叠根丢目标。
2. **宽析构/计时**：本席指出大 described 非平凡析构树也可为单单位；探索方沿 collect_one→release→destroy 接受修正，并同意分报 poll wall/begin/step.elapsed。未决：哪项成本主导未测；单单位 probe 不设任意时延门槛，不能把软预算长尾升级为合同漏洞。
3. **公式/索引**：探索方要求独立核算 H+5、2S+4、S+4，本席推出 W=H+S+Q+T+3，并补固定图/无 mutator/空 trace 前提；另给出 swap-pop 将未扫项移到已扫前缀的反例。探索方确认加入重排/取消协议及空项计量授权门槛；slots 不重排身份。未决：新增索引是否值得，须 Main 的规模测量；不能从高水位机制直接推定收益。

探索方随后确认最终 explore.md 已记录这些争议、回应、修正、证伪条件和重叠根假阳性。意见一致不替代实验。

Main 后续实验裁决：cursor 四种有限组合、identity 代际/错误保持及后续周期、sweep 单单位257项析构均已通过，原始证据见第 7 节；取消讨论中这四组具体组合的待验证项已关闭，但保留一般化反例的证伪条件。宽析构工作颗粒已实证，哪类工作主导真实应用及索引是否值得仍未测，不能由一次1200ns观察作结论。

## 7. 三个最小路径与 Main 本机结果（Release）

一次性文件：`out/review/wave1-meeting/gc-counter-lifetimes.cpp`。只调用真实 Runtime、NativeRegistration、ExecutionFrame；定位器为实际 native 注册入口，不 mock、不复制生产逻辑。与探索方 H/S、宽深 mark、整值屏障规模探针互补。

Main 已统一编译并分别运行三个模式，作者未重跑。原始文件均在 `out/review/wave1-meeting/`：`gc-counter-lifetimes-build.json` 记录正式宿主 CMake 编译/链接依赖，configuration=release；三份模式 JSON 均 exitCode=0、signal=null、stderr 为空。

### 动作 1：cursor——四种取消/重叠组合通过

路径：hidden inline/leaf locator 撤根 × 无/有独立重叠根。无根时 step 返回后才 free；重叠时 destroy_value=busy。随后在回收登记位置登记另一份预填值；全部引用内容于 begin 前写入，排除运行中屏障假保活。

原始证据：`gc-counter-lifetimes-cursor.json`，输出四行 PASS：

| inline | duplicate | old_leaf_calls | after_cancel |
|---:|---:|---:|---:|
| 0 | 0 | 1 | 0 |
| 0 | 1 | 2 | 2 |
| 1 | 0 | 0 | 0 |
| 1 | 1 | 1 | 2 |

与预期一致：无重叠根时取消后旧存储没有再次被 locator 访问，旧 target 收回、新 target 保活；有独立重叠根时继续扫描合法且旧 target 保活。源码断言还要求全部撤根后 fresh cycle 的 roots/live/pending 归零。此结果支持这四组合法时序，不授权同步借用未返回时 free，也不是任意内存错误的穷尽证明。

### 动作 2：identity——活动游标销毁、stale 槽与后续周期通过

路径：含两条 hidden strong 的对象扫描 first 后暂停，destroy 有根对象再立即 create；没有其他自由对象槽，free-list 源码保证同槽新代际。旧 weak 不取得、旧 raw hold/take 失败但 stale 槽保留，新 marker=99。

原始证据：`gc-counter-lifetimes-identity.json`，输出 `PASS identity stale-root-retained=1 old-second-dead=1 next-cycle-converged=1`，exitCode=0。与预期一致：first 本轮保守活、second 未扫应死；下一 fresh cycle first 死而 replacement 活；撤 replacement 后仍可回收，即使 stale 登记继续计入 root_count；最终清 stale 槽为零。有限场景不能外推 uint64 代际耗尽或任意并发。

### 动作 3：sweep——单工作单位完整析构257项已证实

路径：一个 described 对象含257个带序号 native record；到 sweep 时对象已判死，weak 取得失败且尚未析构。一次 `{max_work=1,max_time=1ns}` 步进。

原始证据：`gc-counter-lifetimes-sweep.json`，输出 `PASS sweep work=1 reclaimed=1 destructors=257 elapsed_ns=1200`，exitCode=0。源码断言同时检查逆序、恰好各一次与最终 pending=0。由此本机证实完整宽析构仍只计一个 sweep 单位，符合软预算合同，而非预算实现漏洞。

**1200ns 只是这一执行的一次 step 观测，不是上界、p99、真实用户析构成本或未来配置保证。** JSON 外层 elapsed_ms 是整个进程运行观察量，也不是该 GC 步延迟。不能为更小 elapsed 牺牲析构顺序或完整性。

### 本次实际构建与复现入口

实际权威命令保存在 `gc-counter-lifetimes-build.json`，而不是下文早先拟议的精简命令。Main 在 `out/release` 使用锁定 Clang 23.1.1、正式宿主 CMake 的 C++23 /MD /O2 /W4 /WX 等参数，将探针编译为 `out/review/wave1-meeting/gc-counter-lifetimes.obj`；实际链接复用 `atelier_jit_acceptance` 的 global_new_delete 对象、shared mimalloc、atelier_runtime 及正式宿主其余链接依赖。生成可执行文件为 `out/release/wave1-gc-counter-lifetimes.exe`。

复现应先按 README `. ./tools/enter-dev.ps1`，并以 build.json 的实际命令为准；从仓库根分别执行 `./out/release/wave1-gc-counter-lifetimes.exe cursor`、`identity`、`sweep`。本报告只记录 Main 已完成的三个独立模式，不把未单独记录的 `all` 模式称为已运行。不得混链 Dev Runtime/Release CRT；改配置应沿对应 CMake 参数统一全部依赖，不安装/升级工具链。

早先建议的精简命令保留作来源记录，**没有声称 Main 按此精简命令执行**：`clang-cl /nologo -clang:-std=c++23 /EHsc /MD /O2 /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /permissive- /utf-8 /W4 /WX /Isrc out/review/wave1-meeting/gc-counter-lifetimes.cpp out/release/atelier_runtime.lib out/release/CMakeFiles/atelier_runtime_acceptance.dir/src/memory/global_new_delete.cpp.obj out/release/_deps/mimalloc-build/mimalloc.dll.lib /Fo:out/review/wave1-meeting/gc-counter-lifetimes.obj /Fe:out/review/wave1-meeting/gc-counter-lifetimes.exe /link /include:mi_version`。它与实际二阶段命令、对象来源和输出路径不同，后续不应用它覆盖本次构建证据。

未新增生产 CMake 规则，未重新执行全量验收；其既有结果按 baseline 复用。

## 8. 来源、限制与交付

本报告仅使用本仓固定源码、合同和已有验收，没有外部搜索、外部 GC 语义/性能结论。访问日期：2026-09-25。

- 合同：会议 README.md / baseline.json；CONTEXT.md；docs/engineering.md；language-contract.md 所有权/失效句柄；runtime-model.md 外部根、跨步进、软预算与执行协议。
- Runtime：runtime.hpp 身份/根/预算；runtime.cpp:65–215 根表，316–502 标记取消死亡，624–740 值屏障/析构，774–827 收集/关停，955–1081 追踪计划，1127–1482 值/引用入口。
- 消费链：execution.hpp/.cpp；business_backend.cpp:100–188及statement清理；business.cpp:89–309、349–396。
- 正式验收位置见第4节；搭档报告：docs/research/wave1-meeting/gc/explore.md。
- 本轮 Main 原始证据：out/review/wave1-meeting/gc-counter-lifetimes-build.json、gc-counter-lifetimes-cursor.json、gc-counter-lifetimes-identity.json、gc-counter-lifetimes-sweep.json；作者仅只读读取，未更改 raw。

交付：本报告和 gc-counter-lifetimes.cpp，新增原始依据为 Main 的 gc-counter-lifetimes-{build,cursor,identity,sweep}.json。质询及三个 Release 本机路径已完成，作者仅只读核对 raw 并更新本报告，没有自行重跑；无阻塞。通过范围仅限记录的有限路径，时延单次观察不升级为上界或性能结论。
