# A / 独立反方：类型、布局与值生命周期

> 历史快照：本文依据当轮 baseline.json；旧接口、文件名、行号与未实现判断仅描述当时状态，不是现行规则或待执行任务。后续结果与现行入口见[历史索引](../README.md)。

作者：WaveTypesChallenge。日期：2026-09-25。依据：会议 baseline.json 的当前内容哈希，不存在 Git diff。本席位未构建、运行、测试或测量，生产代码只读。

## 1. 意图和边界

从可恢复错误、别名、重入、失败初始化与复杂度寻找反例，不预设 bug。不把可信接口的调用方违约、完整 OOM 恢复、并发或任意布局热更当漏洞。已完整读取会议 README.md、baseline.json，并读取 docs/engineering.md、CONTEXT.md、runtime-model.md 和 language-contract.md 的对应契约。

## 2. 独立结论与 Main 实测状态

本席位始终未执行构建/实验；以下新增结果由 Main 统一执行，本席位已直接读取原始 JSON（§10），不能归功于 reviewer 自测。

| 类别 | 结论 | 证据状态 |
| --- | --- | --- |
| 接受 | 发布前完成验证、布局及分配准备；未找到可恢复注册错误泄漏身份路径 | 源码：runtime.cpp:836–1090；验收：1230–1323 |
| 接受 | 描述型构造逆序清理已完成字段，原生子对象交给 C++；赋值失败只有基本保证 | 源码：550–623、628–679、1154–1180；验收：185–218 |
| 接受 | 按 TypeInfo::size 检查别名；自赋值 no-op，部分重叠拒绝，ValueRoot 保护重建/销毁 | 源码：36–54、1125–1193；验收：250–265、1428–1459 |
| 已证实缺陷 / P2，最小修法已隔离验证 | 默认成员定位器将重载一元 & 当物理地址，导致错误读写及漏掉真实强边；生产仍未修 | Main 基线 address-default.json 退出 2；隔离 addressof variant 退出 0 且 scalar/trace/cleanup 全 PASS，见 §10.1 |
| 接受 / 本例通过 | 失败清理 × 512 层析构回调重入，外层逆序、原地址恰好一次并可重试 | Main 当前基线 unwind-default.json 退出 0，见 §10.2；不宣称任意重入或 OOM 已证明 |
| 待证 | 注册二次扫描是否值得事务局部索引 | 复杂度源码证实；耗时收益未证实，见 §5 |

## 3. Spec：默认定位器可能映射到另一份合法活值

### 触发链

NativeRegistration<T>::field<Pointer>（src/runtime/native.hpp:73–80，错误表达式在第 79 行）仅排除 const/volatile，然后生成 `return &(static_cast<T*>(object)->*Pointer)`。合法原生记录成员若定义 operator&()，这里调用其重载，不保证取得真实成员地址。它仍可满足 default_initializable、nothrow_destructible，并通过 representation 校验；不需要伪造 NativeLayout 或手写错误 locator。

反例：Inner 内含 u32 number，operator&() 返回另一个持续存活的 Inner sentinel；Outer 内联 Inner。两者正常注册，Outer::inner 使用默认 locator。Runtime::resolve_path（runtime.cpp:1403–1419）消费地址并交给 read/write（1484–1501），结果嵌套 number 读写 sentinel 而非 Outer::inner；两个 Outer 可意外别名。sentinel 类型、对齐和生命周期正确，不依赖 UB/悬空指针。相同适配还由 shade_value/trace_one/trace（386–431、682–709）消费；含 ReferenceSlot 时有读取错误实例引用边的源码路径。初稿对此仅作推导；Main 随后在未改生产的本机基线上得到 edges=0、target_alive=0，已证实本例漏强边并回收目标（§10.1）。

### 契约、方案与证伪

README.md:116 规定原生字段由成员指针定位，138 要求真实布局及完整引用。本例未手写错误适配，native.hpp 也未禁止 overloaded-& 类型。WG21 N4950（2023-05-10）的 [specialized.addressof] 第 1 段明确 std::addressof 在重载 & 存在时仍取得实际地址；已核实官方 cplusplus/draft 仓库 n4950 tag 的 source/memory.tex:1818–1838。PDF 首页用于确认版本日期，不依赖未定位的 PDF 页号。

方案 A（接受）：仅换成 `std::addressof(static_cast<T*>(object)->*Pointer)`，配真实 read/write 回归；无缓存、无元数据/生命周期变更。方案 B（否决）：禁止所有 overloaded-& 类型，或要求宿主手工补 locator；会收缩泛型能力并外推本可隐藏的不变量。

预期基线：真实成员为 17、sentinel 为 91；读取返回 91，写入 73 后真实成员仍 17、sentinel 73。修复后预期：读取 17，真实成员 73、sentinel 91、operator& 调用次数 0。若未改 helper 的真实构建已满足修复后结果，则撤销本判断。实验仅交 Main 执行。

## 4. Spec：失败 × 重入缺少交叉覆盖，不升级为缺陷

runtime.cpp:282–315 的 WalkScope 后缀和 prepare_walk 隔离重入；construct 循环复制 WalkFrame，不跨回调持有 vector 元素引用，回调后按 index 回写。596–609 失败时先减少完成计数再 destroy；字段类型由 unique_ptr 所有且发布后不变，工作栈扩容不移动类型。

acceptance/runtime/main.cpp:1507–1523 覆盖重入 initialize/copy/assign，内层操作均成功；1525 在故障注入前把 ReentrantValue::exercise 清空，1540 才恢复。因此原有正式验收中的失败与重入仅分别覆盖。Main 本轮新增一次性组合实验已通过（§10.2），补充了清理中析构重入和内层失败后继续的本例证据；这不自动新增永久 acceptance 覆盖，也不要求修改当前值走访实现。

方案 A（接受）：一次性真实入口反例，外层失败清理中的原生析构对不重叠存储执行更深记录操作，内层故意 construction_failed 后继续。检查已完成资源的原地址、逆序、恰好一次；root_count/live_objects 不变，失败目标可重试。方案 B（否决）：无反例就建通用事务/备份框架或禁止全部重入。回调重建当前被借用的同一份值属于调用方违约，不作为反例。

OOM 边界：README.md:137 明确元数据、根、运行时工作存储和错误字符串分配可继续抛 bad_alloc；reserve 在 Result 捕获之外不是新漏洞。

## 5. Standards：复杂度及优化门槛

register_types:861–870 对每个定义线性检查已发布/staged 名称及 representation；893–903 每个名称/原生依赖再次线性查找；917–931 成对查成员重名。N 个单字段前向链有 O(N²) 名称工作，单个 F 字段类型有 O(F²) 重名工作。显式 DFS 消除递归栈，不把完整发布变成 O(N+E)。10000 层验收只证明可完成行为，不证明吞吐。

反对立即上持久名字缓存：README.md:130 定位名称发现为冷路径，允许缓存 FieldId/MethodId；read/write/call 不重新查名字。若瓶颈只是批量发布，首先考虑事务局部、无独立身份的名字/representation 索引，失败直接销毁，不形成第二份类型世界。没有同机测量，不推荐哈希表或宣称性能缺陷。

反对当前对象 pool：runtime.cpp:1216–1223、1238–1252 已复用登记槽，但真实对象独立分配；池引入保留量、地址复用、代际/析构约束。没有创建/销毁成本占比和保留量数据，不值得改变。

保留 immutable TypeInfo、FieldId 路径与派生 destroy_fields/trace_fields/scalar_leaf；它们来自同一权威定义、承担不同语义。同步 walk_stack 与暂停 GC trace_stack 不应无条件合并。

## 6. 双人质询记录

独立初稿落盘后直接 hub send 给 WaveTypesExplore，交付报告路径和三项具体争议；随后已完整读取 types/explore.md，并完成至少一轮双方带源码和反例的回应。搭档初稿不预设缺陷；本席位也不以其接受意见替代本机证据。

| 对方主张 / 回应 | 本席位回应与依据 | 接受 / 否决 / 待证 |
| --- | --- | --- |
| 初稿认为可信接口下未见发布/寿命错误；互审接受 overloaded-& 反例，精确定位 native.hpp:79 | 接受其消费端复核：runtime.cpp:958–962 只核 representation，1409–1417 只核 owner/index，read/write 直接消费回调地址；sentinel 持续活、同型、正确对齐，不属于宿主伪造适配 | 接受一表达式 addressof 修法；否决禁用所有此类成员或新增框架；本机路径待 Main |
| 原生失败及重入有各自验收，但 1525 禁用 exercise 是组合覆盖缺口 | 同意。WalkScope 后缀、frame 副本、回调后 index 回写是当前实现没有明显坏路径的反证；不把缺少测试升级为缺陷 | 接受交叉探针；否决先加事务备份或禁止重入；若实际顺序/地址/计数失败再升级 |
| 注册去重 Θ(NP+N²)，依赖查找 O(E(N+P))，但只在冷路径 | 接受更准确推导（P 已发布数、N 批大小、E 名称/表示依赖数，另有成员重名 Σ(F²+FM+M²)）；README:130 使其不能被冒称为每帧名称成本 | 仅实际注册可见时测量，收益及局部索引待证；否决未经数据的持久缓存/pool |

搭档另作反质询：不能统一公共枚举与 GC 强边计划。接受其 weak-only 反例：runtime.cpp:1027–1030 只把 strong/含 strong 内联记录放入 trace_fields，386–405 消费为写屏障；682–709 的公共 trace 却输出 weak、strong、失效身份，696 明确保留含引用原生类型中的 scalar locator 顺序。故弱引用记录可能 contains_references=true 而 trace_fields 为空；直接换计划会漏边，删 scalar locator 还会改变允许的回调副作用。已直接 hub 回复此源链并记录：不加同步 trace 缓存，不合并两种语义计划；需要纯 locator 假设必须先独立批准契约。

搭档已将地址反例置于最高优先，撤销“未见缺陷”的笼统结论；双方统一把失败×重入留作可证伪覆盖风险，把注册索引留作条件性性能假设。此为论证修正，不是实验通过。

## 7. Main 优先动作（最多三项）

1. overloaded-& 基线已由 Main 复现，隔离头文件 std::addressof A/B 已通过；等待独立授权后落生产最小修复与永久端到端回归，不把复制头实验等同生产已修。
2. 失败清理 × 深层析构回调重入已由 Main 验证本例通过；接受当前实现，只补覆盖证据，不改生产或建框架。
3. 注册时间可见时，再测 N=256/1024/4096 的同批前向链及宽字段（定义准备不计时），记录耗时/规模比及 TypeId/字符串依赖对照；不给缓存/pool 预先通行证。

全部执行归 Main。本席位没有自行运行，也没有性能测量结论；已读取 Main 本轮三份正确性 raw 并记录于 §10。完整实验、精确编译/链接/运行命令、可证伪预期及配置限制列于 §9。第三项仍为条件性测量，不额外创建通用性能框架。

## 8. 来源与限制

- 当前源码 src/runtime/runtime.hpp、runtime.cpp、native.hpp；SHA-256 见 ../baseline.json。未修改生产文件。
- acceptance/runtime/main.cpp；baseline 只证明旧验收通过，不自动证明本文交叉情形。
- README.md:122–139；docs/runtime-model.md 的成组发布、值操作和借用条款；docs/engineering.md；CONTEXT.md。
- 固定一手机制：WG21 N4950，2023-05-10，[specialized.addressof] p1，官方仓库 n4950 tag 的 source/memory.tex:1818–1838；访问 2026-09-25。源码：`https://raw.githubusercontent.com/cplusplus/draft/n4950/source/memory.tex`；PDF 首页确认版本/日期：`https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/n4950.pdf`。PDF 的 Exa 内容获取曾超时，随后直接读取已知官方文档与固定官方源码；不伪称 PDF 页号已定位。滚动条款也经 Exa fetch 核对：`https://eel.is/c++draft/specialized.addressof`。这些只支持真实地址语义，不证明本机 ABI、结果或性能。没有外部搜索/第三方运行时移植结论。

## 9. 交付实验与 Main 精确入口

两份文件均已写完整、分别只读确认身份；初次交付时未编译/运行，此后 Main 统一执行的 raw 已吸收至 §10。本席位没有自行执行。文件受本席位独占，生产、acceptance、cmake 和版本化语言规范没有更改。

### 9.1 默认 locator 真实地址及引用边

文件：out/review/wave1-meeting/types-counter-address.cpp（89 行，struct Inner 定义 overloaded operator&）。真实入口包括 NativeRegistration::build → register_types → create → read/write、可信槽 write_reference、visit_references、begin_collection/collect_step。sentinel 在 Runtime 外层栈上保持存活，实际成员只在 owner 存活时观察；指针捕获是探针观测手段，不提议跨运行时服务缓存地址。

未修基线的源码预期：`scalar before=91 actual=17 sentinel=73 overloaded_address_calls=2 contract=FAIL`；`trace edges=0 target_alive=0 contract=FAIL`；`cleanup=PASS`；退出码 2。零边和目标死亡直接证明错误 locator 被真实追踪消费者采用，不依赖 mock。

另行授权修复后预期：`before=17 actual=73 sentinel=91 overloaded_address_calls=0`，`edges=1 target_alive=1`，所有 contract/cleanup 为 PASS，退出码 0。若基线已满足后者，撤销缺陷并查明编译源与哈希；若只一部分失败，保留实际输出而不概括更多影响。

### 9.2 构造失败清理 × 析构重入

文件：out/review/wave1-meeting/types-counter-unwind.cpp（168 行，struct Event/Resource，depth=512）。外层描述型值先完成三个原生资源，再故意使第四次构造失败；逆序清理中每个原生析构同步调用同一 Runtime，在独立存储执行 512 层值初始化失败、再初始化成功和销毁。第二轮以 copy_construct_value 触发同一协议。原生 fixture 自己符合无异常析构约束，日志是定长数组；不复制 Runtime 的值走访算法。

检查每个外层完成资源恰好析构一次、地址不变、逆序；内层返回 construction_failed 且无泄漏，外层失败目标可直接重新初始化，source 保活，root_count/live_objects 不变，最后仍能注册新类型。

源码预期：初始化行 `callbacks=3 inner_rejections=3 remaining_live=0`，复制行 `callbacks=3 inner_rejections=3 remaining_live=4`，两行 `reverse_address_exactly_once=PASS retry=PASS`，末行 `registration_after_failure=PASS final_live=0`；退出码 0。任一计数、地址、顺序、重试或后续注册失败是最小证伪。它请求了叠加外层游走后更大的工作栈深度，但没有记录分配器事件，所以不能把通过结果说成已测得某次扩容或 OOM 恢复。

### 9.3 正式配置关联与可复制命令

依据 out/review/wave1-meeting/build-provenance.json（Main 从当前 CMake/Ninja 导出）和 CMakeLists.txt:33–52：C++23、Release /MD、/O2 /Ob2、/W4 /WX、/EHsc、共享 mimalloc 与正式 global_new_delete。必须使用 Main 对当前哈希统一构建的 out/release/atelier_runtime.lib，不能链接历史 ABI 产物。仅需一次性探针，不增加 CMake target；正式配置仍以仓库 CMake 为准。

在仓库根目录 PowerShell，先 `. ./tools/enter-dev.ps1`。对 `$name = 'types-counter-address'` 和 `$name = 'types-counter-unwind'` 各执行以下三条（任何编译/链接非 0 时停止，不运行旧 EXE）：

1. `& "$env:ATELIER_LLVM_ROOT/bin/clang-cl.exe" /nologo /TP /DWIN32 /D_WINDOWS /D_MBCS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DNDEBUG /EHsc /O2 /Ob2 /MD /W4 /WX /permissive- /utf-8 -clang:-std=c++23 /Isrc "/Foout/review/wave1-meeting/$name.obj" /c -- "out/review/wave1-meeting/$name.cpp"`
2. `& "$env:ATELIER_LLVM_ROOT/bin/lld-link.exe" /nologo /machine:x64 /INCREMENTAL:NO /subsystem:console /include:mi_version "/out:out/release/$name.exe" "out/review/wave1-meeting/$name.obj" out/release/atelier_runtime.lib out/release/CMakeFiles/atelier_jit_acceptance.dir/src/memory/global_new_delete.cpp.obj out/release/_deps/mimalloc-build/mimalloc.dll.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib`
3. `& "./out/release/$name.exe"; $LASTEXITCODE`

EXE 留在 out/release，沿用该配置目录的 mimalloc.dll/redirect DLL。当前正式链接 override 对象路径来自 build-provenance；若 Main 统一构建只生成 runtime_acceptance 版本，可等价选该目标正式编译的 global_new_delete.cpp.obj，但必须保留同一 Release CRT/分配器关系并记录实际命令。开发复核须全套改为当前 dev 产物、/MDd、对应 Debug flags 及 mimalloc-debug.dll.lib，禁止混搭；它不是性能结论。

### 9.4 当前状态与阻塞

交付完成、双人质询已完成。报告写入曾受新文件创建权限及编辑器 regex 模式影响，Main 建立占位后专用 literal edit 已成功；临时的文件身份疑虑已分别只读检查解除，没有生产修改。WaveTypesExplore 随后以带路径 grep 独立确认 address.cpp 的 Inner 在第 19 行、unwind.cpp 的 Resource 在第 26 行和 depth=512 在第 130 行，消除了并行读取输出次序歧义；其静态复核认可地址探针使用真实默认 helper、真实强边写入和合法 sentinel 活期，展开探针的预算 3、三次析构回调、内层失败重试及 ID 过滤的逆序/原地址断言自洽。双方均未执行，不把静态互审写成实测。当前不存在缺源码/缺入口阻塞；本机编译、运行及正式修复授权仍归 Main。初次交付时结果全部为预期；收到 Main 回报后已直接读取 raw，最新实测状态见 §10，优先于前面保留的初稿/方案预期。

## 10. Main 统一实测更新

执行者为 Main，本席位仅读取文件并更新报告；没有重新运行、没有修改生产代码。配置关联仍以 build-provenance.json 和 Main 统一构建记录为准。三份 default raw 均为 signal=null、stderr 空；记录中的 elapsed_ms 是整进程观测，不用它推断字段访问、清理或注册吞吐。

### 10.1 地址反例：当前基线复现

原始证据：out/review/wave1-meeting/types-counter-address-default.json。实际退出码 **2**，完整观测为：

- `scalar before=91 actual=17 sentinel=73 overloaded_address_calls=2 contract=FAIL`
- `trace edges=0 target_alive=0 contract=FAIL`
- `cleanup=PASS`

这与源码反例精确一致：Runtime 读取 sentinel 而非成员，写入也改变 sentinel；真实 owner 仍由根保持，但其真实强引用边未被遍历，目标在该收集后失效。该受支持输入上的地址/追踪缺陷现为**源码 + 本机基线实测证实，P2**，不再仅是 GC 风险推断。清理 PASS 不能抵消过程中的错误访问和回收。

随后 Main 的隔离 addressof 头文件 A/B **全部通过**，本席位已直接读取 out/review/wave1-meeting/types-addressof-probe-default.json：实际退出码 **0**、signal=null、stderr 空，完整观测为：

- `scalar before=17 actual=73 sentinel=91 overloaded_address_calls=0 contract=PASS`
- `trace edges=1 target_alive=1 contract=PASS`
- `cleanup=PASS`

对照证据链已读取：types-addressof-probe.cpp 第 1 行改为包含 out/review/wave1-meeting/addressof/native.hpp；隔离头仍包含真实 runtime/runtime.hpp，并显式包含 `<memory>`，成员定位表达式换成 std::addressof。types-addressof-probe-build.json 与 types-counter-address-build.json 记录相同正式 Release /MD、C++23、当前 atelier_runtime.lib、正式 global_new_delete.cpp.obj、mimalloc.dll.lib 及宿主链接依赖；实际 EXE 分别为 out/release/wave1-types-addressof-probe.exe 和 wave1-types-counter-address.exe。不把 §9 的可复制示意输出名冒充 Main 实际运行名。

最小修复方向已在同一正式 Runtime 的本例真实消费者上验证：真实成员读写、零 overloaded-& 调用、1 条引用边/目标保活、最终清理同时恢复。**生产 native.hpp 仍未修，永久 acceptance 也未增加**；隔离 A/B 证明修法针对本根因有效，不等于生产已合入或完整开发/发布回归已执行。下一动作只能是按 Main/用户授权落最小生产修复和永久回归，不需要缓存、pool 或新框架。

### 10.2 失败清理 × 重入：当前基线通过

原始证据：out/review/wave1-meeting/types-counter-unwind-default.json。实际退出码 **0**，完整观测为：

- `initialize callbacks=3 inner_rejections=3 remaining_live=0 reverse_address_exactly_once=PASS retry=PASS`
- `copy callbacks=3 inner_rejections=3 remaining_live=4 reverse_address_exactly_once=PASS retry=PASS`
- `registration_after_failure=PASS final_live=0`

接受当前值走访实现：三个已完成外层原生资源在原地址逆序且恰好一次释放，析构重入中三次内层 construction_failed 后均能继续，失败目标可重新初始化；复制场景的四个 source 资源按预期保留，最终清空且类型注册仍开放。初稿的组合覆盖待证在**这个具体一次性场景**已经闭合，未证实任何值生命周期缺陷，不建议事务备份、禁止重入或重写工作栈。

限制：此结果不代表永久 acceptance 已增加、所有可能重入/别名组合都覆盖、任意 OOM 可恢复或工作栈分配事件已被测量。注册 Θ(N²) 成本的净收益仍无新性能数据；原有不先加缓存/pool 的结论不变。
