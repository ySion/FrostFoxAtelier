# B / GC 与根协议：探索席

状态：独立初稿、互读与带证据质询已完成；Main 已统一执行七组真实 Runtime 探针，每组三轮，原始 JSON 与确定工作量结论已吸收（见第7节补记）。固定源码审查，不是 Git diff 审查，基线为 baseline.json 当前实现。本席没有自行编译或运行；只分析 Main 本次原始结果，不沿用旧版本数字。

## 1. 意图与证据等级

保存对象身份、明确谁保活、在协作位置推进回收；不把“强类型引用值”自动等同于“已登记外部根”。Spec 依据 CONTEXT.md、docs/language-contract.md、docs/runtime-model.md；Standards 依据 docs/engineering.md。正式验收通过仅引用会议基线，源码阅读不是重跑。标签：源码证实、性能假设、设计取舍、已覆盖/不成立。当前未发现可报告的已证实语义缺陷。

## 2. 业务可观察状态转移

| 转移 | 源码与不变量 | 业务含义 |
|---|---|---|
| 发布对象→首根 | runtime.cpp:1206–1262 create 构造→根→提交 Slot；根失败清理 | 不交付未受保护的活对象 |
| 引用值→持有者 | runtime.hpp:113–158；runtime.cpp:151–189 | ObjectReference 只携带类型/形式/身份；ObjectRoot copy 新登记、move 转移、reset 仅撤根 |
| 观察→取得 | runtime.cpp:1320–1346 校验 weak/活身份，再 roots->add | 同一单线程调用先登记再返回；不是锁，不阻止显式 destroy |
| 失效→复制/访问 | runtime.cpp:1191–1204、1280–1318 | hold(ObjectReference) 可复制陈旧强身份；hold(ObjectId)/type_of/解引用拒绝；registered 不证明目标存活 |
| idle→roots | runtime.cpp:774–789 reserve、epoch++、root_limit=entries.size | begin 预留不计 step.elapsed |
| roots→mark | runtime.cpp:443–453 逐登记位置检查 active，新根回调补记 | 历史空位置也算 work；limit 外新根不会漏标 |
| mark→sweep | runtime.cpp:455–476 队列/游标全空，pending=live-marked，live=marked | 一次逻辑死亡提交；取得/访问立即失败，析构可尚未发生 |
| sweep→idle | runtime.cpp:479–491 逐槽；release 先 storage=null 再析构，recycle 增代际 | 完整析构一单位；清扫新对象本轮存活，旧身份不绑定新对象 |
| 撤根/毁对象→取消借用 | runtime.cpp:320–383、416–432、726–739 | 队列解链/活动游标清空；locator 撤根后不读返回槽，调用方仍守同步借用期限 |

### 根/槽规模

R=count 是当前活跃登记，包含空/失效对象根和值根；V=value_entries.size 是当前活跃值根，overlaps_storage 只扫 V，remove swap-pop 并维护反向位置（65–145）。这已排除对象根与历史根对重叠检查的影响，不应重复建议。

H=entries.size 是登记表历史最高位置数；add 优先 free_head，remove 不缩表，通常随峰值并发登记而非累计 hold 次数增长。S=slots.size 是对象登记最高位置数，正常回收复用，代际耗尽位置退休。根 index 不公开为长期裸身份，句柄管理登记生命周期且复用前解链，缺少根代际不是已证实缺陷；对象引用可保存陈旧身份，所以对象必须有代际。

## 3. Spec：正确性与交接

### 引用形式不是根登记

ReferenceSlot 只存 ObjectId，TypeId/strong/weak 在共享 FieldStorage；ObjectReference 传递时携 description。GC trace_fields 只含 strong 及含 strong 的内联记录；visit_references 则枚举强、弱、失效非空引用，目的不同（runtime.cpp:1027–1029、679–708）。contains_references 不能当作 contains_strong 或析构判据。

### 屏障与取消

三个写入口汇入 write_reference_at：校验→写入→strong shade；shade 活身份/epoch 去重后入灰队列（337–349、1429–1482）。whole-value 构造成功、赋值成功或部分失败后均 shade_value（624–635、1156–1182）；返回错误不漏已写边。删边/撤根不撤销已标记，未登记外部值也可保守补标：浮动垃圾允许留一轮，不等于永久根/泄漏，下一完整周期应回收。

trace_one locator 返回后检查 trace_owner；撤根后不解引用返回槽/不重建内联游标，traversals 禁止当场再登记（416–432、1348–1356）。取消不授权调用方在同步借用仍活动时释放存储。

### 执行槽与消费端

execution.cpp:30–58 先获新根再替换，失败保留旧槽；take 活性检查再 move，clear/析构复用撤根。poll 先校验，仅有请求且 idle 时 begin，每次至多一个 collect_step（89–101）。business_backend.cpp:145–186 产生强/弱槽与 clear；business.cpp:215–308 的 reference_value/change_reference/clear_slot 明确分派，weak 写 observations 而不入 ObjectRoot，try_acquire 成功移动已登记根，失败生成 typed-null 强槽。快照 take 的活性要求不能推广为一般失效引用复制语义。

### 已覆盖边界

acceptance/runtime/main.cpp:991–1049 覆盖混合环/非移动/逻辑死亡/清扫新建/旧代际；1051–1098 覆盖已扫描字段新强边/弱边/晚根/新对象；1105–1152 覆盖外部值和失败赋值；1154–1227 覆盖暂停游标/撤根/排队存储释放/登记复用/各阶段关停。step_collection 检查 work 上限和进展，不承诺硬时间；未涵盖历史表/极宽深结构的当前性能数据。

## 4. Standards：成本与可证伪假设

### 历史扫描/容器保留

roots 扫 H，sweep 扫 S，空位置也一单位；overlap 只扫 V，不可混称。即使 roots/live 归零，长期 Runtime 后续每轮仍扫描历史空表。entries/slots 未收缩，RootState 又可由晚于 Runtime 的根继续保留。对象内存释放不等于元数据/working set 归零；只证明机制，具体字节/RSS 待测。

无 trace_fields 的空对象精确工作预测：历史 H 根、当前一根/一对象，总 work=H+5；历史 S 对象/根位置，当前一根/一对象，work=2S+4；逐对象立即撤首根但累积 S 个无根对象，先 GC 回收，再空堆 GC，H=1 且 work=S+4。公式含三次阶段转换、每个已标记对象一次灰取项，是可证伪工作计数，不是毫秒。

### 软预算不是硬实时

native locator、单对象完整原生/描述型析构、allocator free 含在一个 work。大描述型非平凡析构即使用显式栈，也能拖长一个 sweep 单位；mark 则对宽字段、深内联进入/退出逐步推进。max_time 只在单位间检查，活动步至少一单位；max_work 是调度颗粒而非固定时间。max_time=0 省中间读钟，正预算每单位读钟需配对测量。elapsed 不含 poll 内 begin、业务屏障/根操作/宿主 Tick。

### 屏障与执行槽成本

单 strong write 的 shade 是身份/epoch/链接操作；whole-value shade_value 同步走所有强边，即使目标已黑。赋值和屏障在 step 外；prepare_walk 可为回调重入扩容，begin reserve 属启动成本。把工作挪到 mutator 不等于帧收益。

ExecutionFrame 非空替换先 add 后 remove，峰值可能多一登记；free-list 避免按循环迭代无界增长。“执行槽复用”不等于“原根登记始终不变/首次扩容不存在”。

## 5. 最多三条路径

1. **真实 Runtime H/S 与尾延迟探针**：独立进程输入规模、预算、轮数；采阶段 work/steps、step elapsed 与外层 wall p50/p95/p99/max、总周期时间、reclaimed、roots/live/pending。峰值 1/10^3/10^5，同当前活集合；H/S 改变但 work 不变则推翻公式，不先改代码。
2. **宽/深与同步屏障分项**：宽强字段 1/32/1024/16384，深内联 1/32/256/1024；分别测 begin、mark work、step tail、assign_value idle/mark、单槽 write。注册不混入稳态；若尾部由析构主导，mark 索引不能解决。
3. **仅量测证明历史扫描显著后比较索引**：A 保持现状/按生命周期重建 Runtime；B 活跃根索引/槽占用摘要。暂停中 swap-pop/new-root 去重/取消必须同步设计，不把重排 vector 直接当稳定 cursor；不得移动活对象。比较 mutator、总周期及元数据保留并保持现有语义。

## 6. 已完成的双向质询

双方先独立初稿，再互发报告路径和最多三项争议。本席读取 gc/challenge.md，WaveGcChallenge 读取本稿；双方通过 hub 完成带源证据/反例的回应。以下不是以一致替代实测。

| 反方主张/反例 | 本席回应与源证据 | 修正及证伪门槛 |
|---|---|---|
| locator 撤根后取消检查已存在，同一 locator 再加根被 traversals 禁止，不能虚构 UAF | trace_one:416–432 在地址回调后先检查 owner；RootState::add:90、root_value:1349 拒绝遍历时新增；未找到合同内反例 | 记已覆盖/不成立，不升级缺陷。只有守住同步借用期限仍读取消地址，才推翻结论 |
| 宽 described 非平凡析构也是单个 sweep 单位；elapsed 漏 begin/屏障 | collect_one:479–486→release:726–739→destroy:662–681 完整析构；execution::poll:95–101 先 begin 后 step | 保留该成本。探针同时计 poll wall 和 step.elapsed；first_poll 包含 begin，差值含准入/计时开销，不能全部归因 reserve |
| 已撤根对象同轮尚活是保守策略；历史表扫描不是泄漏 | shade 337–349、root_removing 381–383；撤根只取消未完成借用，不撤回已标记 | 下一完整新周期仍不收敛才调查泄漏；已扫/未扫字段必须分开。重叠 ValueRoot 撤一份后剩余根继续扫描完全合法 |
| 稠密根索引 swap-remove 可把末项未扫根移到 root_cursor 已扫前缀，导致漏根 | 接受具体反例：active=[A,B,C]，cursor=1 后删 A，C 移到 0；继续 1 会扫 B 而漏 C | 路径3不再称透明优化。需稳定快照/变更补记或等价机制；不得直接扫描可重排的 active vector，不能重排 ObjectId 所依赖的 slots |
| 成本公式应给统一式且区分单步/整轮 | 搭档独立核算 W=H+S+Q+T+3；Q 为对象/值根队列取项，T 为实际 trace 字段进入及退出单位 | 三个空对象公式成立，前提是固定图、无收集步间 mutator 增删、无 trace 字段、无代际耗尽。W 是整轮 work，单步仍不超过 max_work；时间/分配不能由 W 推出 |

**明确变化**：补齐固定图前提；将活跃索引提议明确降为需授权的合同/调度变化，因为现有空登记/槽检查本身计 work，跳过空洞会改变 work_done 和到达死亡提交所需 poll 数。保持最终可达性不代表保持现有每步可观察时序。没有提出自动重排 slots、改变引用语义或换收集器。

**尚未解决且可证伪**：Main 当前探针已支持历史表/宽深记录的确定工作量公式，并观察到预算组合改变 poll 次数及整轮 wall；尚不能证明生产负载中的稳定 p99、内存问题或最优预算。正 max_time 的独立读钟成本、跨机器结果及同步屏障与析构谁主导真实 Tick 仍未隔离，不把三轮观察外推为硬实时保证或长期尾延迟分布。

搭档的 gc-counter-lifetimes.cpp 是补充正确性反例：重叠值根仅撤一份、取消后下一轮收敛、对象旧代际/新槽、257 个原生叶的单 sweep 单位。该源码由对方拥有，本席没有修改或运行。

## 7. 已交付的真实 Runtime 规模探针

文件：out/review/wave1-meeting/gc-explore-probe.cpp。只调用现有 Runtime、ExecutionFrame；不 mock，不访问 Impl，不复制 collector。参数为 `mode scale rounds max_work max_time_ns`。模式及精确预期：

| 模式 | 数据/固定集合 | 每轮预期 work |
|---|---|---|
| roots | H=N 次同时根登记后仅留一根/一空对象 | N+5 |
| slots | 同时 N 根/N 对象后显式销毁 N-1，仅留一根/对象 | 2N+4 |
| empty-slots | 逐个创建立即撤根，先统一回收；H=1、S=N、空堆 | N+4 |
| wide | 一目标对象、含 N 强槽的外部记录 ValueRoot；H=2、S=1 | N+9 |
| deep | 一目标、D 层内联记录链（最底一强槽），唯一外部值根；H=2、S=1 | 2D+8 |

所有采样周期图固定；每轮校验 work、reclaimed=0、live 与 pending=0，结束再撤根收集归零。wide/deep 随后分开量 idle 和 mark 期间 assign_value，包含赋值及同步屏障，不伪装成 step.elapsed。源缓冲区不登记，只有目标记录根维持存活。

### 输出与量测限制

CSV 的 cycle 行包括：规模、轮次、两种预算、实际 work/polls、reclaimed、roots/live、总周期 wall、poll wall p50/p95/p99/max、step elapsed p50/p99/max、first_poll/first_step、按调用入口 phase 归属的 work。assignment 行单独给 idle/mark 操作耗时分布。

- 首轮单独看冷启动，后续轮次才作稳态；first_poll 包含 begin 的 reserve，step elapsed 不含。总 wall 包含 probe 循环/采样开销，不是裸 collector CPU 时间。
- samples 在计时前预留最多 W 项；排序、CSV 输出在计时后。样本缓存仍影响进程保留和 cache，不得从该程序 working set 直接归因 RootState。
- max_work>1 时一个 poll 可跨阶段，work_entry_* 仅表示“进入 poll 时的 phase”，不是精确阶段工作分解；要核对单位归属先用 max_work=1。
- 分位数是排序下标 floor((n-1)*p/100)，样本很少只能作冒烟，不宣称统计尾延迟稳定。正式规模运行各组合独立进程，至少20周期、重复进程并保留原始CSV；先检查机器安静和正式库哈希。
- 未报告 alloc bytes、RSS 或内存泄漏实测。需要容器保留证据时另由 Main 记录进程 private bytes/working set 与分配器统计，扣除探针数据、类型元数据和分配器缓存；不为测量偷偷暴露生产 Impl。

### Main 执行矩阵

只由 Main 编译运行，本席未执行。从仓库根先按 README 执行 `. ./tools/enter-dev.ps1`；正式 release 产物已存在且匹配基线时，单独编译本一次性源码，复用正式 Runtime 库与分配器覆盖对象：

`clang-cl /nologo /EHsc /O2 /MD /W4 /WX /permissive- /utf-8 -clang:-std=c++23 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Isrc out/review/wave1-meeting/gc-explore-probe.cpp out/release/atelier_runtime.lib out/release/CMakeFiles/atelier_runtime_acceptance.dir/src/memory/global_new_delete.cpp.obj out/release/_deps/mimalloc-build/mimalloc.dll.lib /Foout/review/wave1-meeting/gc-explore-probe.obj /Feout/release/wave1-gc-explore-probe.exe /link /include:mi_version`

依据为 out/release/compile_commands.json 与 CMakeLists.txt:33–52；本命令不编译复制的 Runtime、不改 CMake。exe 放 release 目录以使用既有 mimalloc.dll/redirect DLL，不混用 Debug CRT；若正式产物哈希不匹配，应由 Main 先重建正式目标，不能沿用未知旧库。allocator 覆盖对象及 import lib 路径已在当前目录验证存在，命令本身未执行。

最小冒烟命令（在真实工具链准备和编译成功后）：

- `out/release/wave1-gc-explore-probe.exe roots 1 2 1 0` → 每轮 work=6。
- `out/release/wave1-gc-explore-probe.exe slots 10 2 1 0` → 每轮 work=24。
- `out/release/wave1-gc-explore-probe.exe empty-slots 10 2 1 0` → 每轮 work=14。
- `out/release/wave1-gc-explore-probe.exe wide 8 2 1 0` → 每轮 work=17。
- `out/release/wave1-gc-explore-probe.exe deep 8 2 1 0` → 每轮 work=24。

规模矩阵：roots/slots/empty-slots N=1/1000/100000；wide N=1/32/1024/16384；deep D=1/32/256/1024。先小后大，每次独立进程；比较 max_work=1/64 与 max_time_ns=0/50000，并以20轮起步。另用极小正预算验证“至少1单位”，不把操作系统计时分辨率或观测超时直接判成错误。任何工作公式/生命周期断言失败则停止性能归因，回到该真实入口定位。

若 Main 只需先运行一种，优先 `roots 100000 20 64 0` 和同当前活集合的 `roots 1 20 64 0`；它最清楚地区分历史登记表与当前根数。性能改善结论必须使用本探针产生的当前数据，不引用旧版本基准。

### Main 本机验证补记：2026-09-25

证据等级：**Main 本机实验通过，本席读取原始结果**。以下七个 JSON 均为本次 gc-explore-probe，args 指定 rounds=3，exitCode=0、signal=null、stderr 为空；不与旧版数据混用，也不是本席重跑。每组三轮 work 一致，探针的固定图、reclaimed=0、live/pending 与最终归零断言均未触发。

| 模式/规模 | max_work / max_time_ns | 每轮 work（三轮一致） | 各轮 polls | 各轮整周期 wall（µs） |
|---|---|---:|---|---|
| roots / 10000 | 1 / 0 | 10005 = H+5 | 10005 / 10005 / 10005 | 1022.3 / 987.3 / 982.3 |
| slots / 10000 | 1 / 0 | 20004 = 2S+4 | 20004 / 20004 / 20004 | 1973.4 / 1995.8 / 1975.0 |
| empty-slots / 10000 | 1 / 0 | 10004 = S+4 | 10004 / 10004 / 10004 | 1004.2 / 996.8 / 1094.6 |
| wide / 1024 | 1 / 0 | 1033 = N+9 | 1033 / 1033 / 1033 | 116.0 / 102.7 / 102.6 |
| deep / 512 | 1 / 0 | 1032 = 2D+8 | 1032 / 1032 / 1032 | 112.1 / 105.2 / 105.1 |
| slots / 10000 | 512 / 0 | 20004 | 40 / 40 / 40 | 59.7 / 60.4 / 60.1 |
| slots / 10000 | 512 / 1 | 20004 | 6179 / 6186 / 5800 | 1187.2 / 1179.2 / 1213.7 |

**确定性结论**：五种固定图的工作公式在这些规模上全部符合源码预测。empty-slots 每周期 root_count=0、live_objects=0，仍有10004 work，直接区分“没有当前对象”与“没有历史扫描工作”；不是存活泄漏证据。wide/deep 每周期根与对象均为1，工作来自强边/内联游标，不是大量对象。该确认只覆盖这些场景，不证明任意 mutator 交错的统一计数式。

**预算比较**：同一 slots=10000 固定图在三个预算组合下总 work 均为20004。关闭时间限制且 max_work=512 时40次 poll 完成；max_work=1 时需20004次。批量版本本次总 wall 较低，但单 poll 最大值为1.9/1.7/1.7µs，体现不同调度颗粒；差额同时含少进出 poll、少计时、少 probe 采样等变化，不能全部归因 collector 算法改进，更不是已经优化生产代码。

max_time=1ns、max_work=512 不保证每次 poll 恰好一个 work：本次 polls 为5800–6186，而非20004。计时样本含0、100ns等值；源码根据单位间的实际读钟值检查，1ns输入不等于纳秒级抢占或保证可实现的计时精度。该组第三轮 poll_max=111.6µs、step_max=111.4µs，是一次墙钟长值；没有调度/中断/CPU trace，不能判断原因，也不能认定某个 GC 单位消耗111.4µs CPU，更不能由三轮推出稳定尾延迟或硬上界。

**同步赋值观察**：wide=1024 的 idle/mark 样本中位数为3.6/8.3µs；deep=512 为6.6/13.3µs。与 mark 期间整值屏障新增同步遍历的源码机制相容，但两个 phase 各仅3次操作、固定先 idle 后 mark，未隔离顺序、缓存及首次补标影响，不宣称可推广倍率。探针的 floor 分位数定义在 n=3 时 p95/p99 都取中间样本，不能用作可信尾部分布。

**限制**：表中 wall 取 cycle CSV 的 total_wall_ns，不是 JSON 外层进程 elapsed_ms，包含测量循环/采样开销。work_entry_* 只按 poll 入口阶段归类，512/0 的 work_entry_mark=0 是跨阶段归属结果，不是跳过标记。没有当前分配/RSS/生产Tick数据；三轮不替代原建议的重复进程与更充分采样。

原始文件均在 out/review/wave1-meeting/：

- gc-explore-probe-roots-10000-1-0.json
- gc-explore-probe-slots-10000-1-0.json
- gc-explore-probe-empty-slots-10000-1-0.json
- gc-explore-probe-wide-1024-1-0.json
- gc-explore-probe-deep-512-1-0.json
- gc-explore-probe-slots-10000-512-0.json
- gc-explore-probe-slots-10000-512-1.json

路径1/2已从纯源码预测推进到上述规模的真实入口证实；路径3仍须明确生产目标、授权计量变化后再评估，不能因本次批量优势直接批准活跃索引或更换收集器。

## 8. 外部机制与适用限制

检索经 Exa，核验 Lua 官方《Lua 5.4 Reference Manual》§2.5、§2.5.1，访问日期2026-09-25，版本范围5.4：其 step multiplier 按分配量关联标记/清扫工作，step size 控制间隔和对应工作，较大步长可退化为非增量行为；官方同时说明 GC 参数效果跨平台/发布版本不可移植。来源：`https://www.lua.org/manual/5.4/manual.html#2.5.1`。

这里只支持“步骤大小、总吞吐、暂停颗粒应分别分析”的机制对照；不是 Atelier 的暂停上界证明，不移植 Lua 阈值、不沿用其对象/弱表/终结复活语义。本项目软预算结论的直接证据仍是 runtime.hpp:267–282、runtime.cpp:792–813。

## 9. 来源、交付和阻塞

本地来源：会议 README/baseline；CONTEXT.md；docs/engineering.md、language-contract.md、runtime-model.md；runtime.cpp:65–216、226–510、624–816、1020–1082、1156–1482；runtime.hpp:113–179、267–282；execution.cpp:23–102；business.cpp:89–111、215–308、1141–1265；business_backend.cpp:145–186；acceptance/runtime/main.cpp:595–674、991–1227。

交付两个独占文件：本报告与 gc-explore-probe.cpp。互读与一轮质询完成，Spec/Standards 分列，最多三条路径。本席未改生产/规范/CMake，也未自行编译或执行；Main 已统一运行七组、每组三轮并返回成功，本席吸收其原始 JSON（第7节）。无研究阻塞。确定工作量与预算组合行为已得到这些入口场景的本机支持；稳定尾延迟、内存占用和生产Tick表现仍未证实。

