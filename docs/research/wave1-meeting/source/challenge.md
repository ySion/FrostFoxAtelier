# C 独立反方：源码语义、资源边界与诊断身份

日期：2026-09-25。作者：WaveSourceChallenge。状态：独立审查、搭档互读及一轮证据质询已完成；一次性真实入口探针已由 Main 统一执行，本席位已读取原始结果（§10）。本席位未自行编译、运行或复跑。

## 1. 意图、证据与审查边界

已完整读取会议章程与 baseline.json，以固定当前内容为依据，不存在 Git diff。源码基线 source.cpp SHA-256 为 bc1220616e9200711f46ae907c704270f88c87dc0fafeb883aa7f960ebbea6c9，source.hpp 为 de912e178e102976fe3e2f1748da55ce808859549375fa2006b7fe385d9bcd1f。遵循 docs/engineering.md、CONTEXT.md、docs/language-contract.md、LANGUAGE-v0.0.1.md 和 v0.0.2.md；v0.0.2 明确替换部分优先。生产源码只读，不构建、不运行、不测试。过去 dev/release PASS 仅按 baseline 引用，不冒充本轮复测。

检查链：Lexer/Parser → owned Program → SourceLowering → BusinessServices 的引用/比较/清理消费者 → Request/prepare 诊断边界。语言不是安全沙箱；无限循环、无硬内存/时间配额是 v0.0.1 §10.3/§14 明示限制，不把限制直接升级成新缺陷。

## 2. Spec：独立初判

### 2.1 C-D1：长源名截断请求身份与位置（宿主诊断保真缺陷，P2；源码证实，Main 本机已验证，见 §10）

链路：business.cpp:581-605 允许任意非 NUL 源名，追加 [request#N]，prepare 仅提供 2048 字节诊断缓冲；business_backend.cpp:488-493 按 source_name:line:column:message 顺序 snprintf。名字本体为 2100 个 a 时，首个错误的可见消息只有 2047 个 a，完整请求编号、行列、错误原因都位于截断后。短名两个不同请求错误相同也可区分，长名则不可区分。此为诊断归属退化，不是陈旧候选可发布或越权执行。

最小源码：`void reload_tick(float delta, uint loops) { self.progress = loops; }`。源名：2100 个 a。预期：编译拒绝、旧代码保留，并保留请求身份/位置；按源码推导当前拒绝与旧代码保留成立，但诊断后两项丢失。现有 verify_reload_ordering 在 business.cpp:1951 明确检查 `starts_with(newest->source_name + ":")`，却只覆盖短名。

责任经搭档反驳后收窄：`compile_source` 的 Diagnostic 自有完整 position/message，C ABI 也由调用方提供有界缓冲；`snprintf` 本身不是越界或前端错误。缺陷是 **BusinessProgram 接受任意长度合法源名，却把 request-specific diagnostic 的全部有效信息放到固定容量之后**。business.cpp:1927-1929、1951、2348-2350 明确要求请求身份可辨识；runtime-model 的执行 thunk 固定诊断取舍并未规定编译请求身份可丢失。这里不引用不存在的 Git diff，也不声称该现象在历史版本首次引入。

方案 A（结构长期正确）：宿主请求失败结果将 sequence/source identity 与前端 Diagnostic 分开保存，格式化最后做并保留错误核心，避免展示名承载唯一关联键。方案 B（最小准入改动）：宿主 request 对源名实施明确的有限长度约束并在编译前拒绝超长名，使所有已接受请求的编号/行列/原因前缀可保留；这是宿主输入政策，需要 Main 授权，不是修改语言数值/引用语义。另一展示方案是保留稳定请求编号及位置，截短名字中间，但必须同步真实消费者与既有 `starts_with(full_name)` 验收，不能偷偷改变错误格式。简单放大固定缓冲只移动反例阈值。

证伪条件：两个不同 Request 的实际错误文本仍保留各自完整请求编号与位置/原因，或存在本轮未见、在 request 入口之前强制名字长度的真实准入契约；仅证明旧 Entry 还能运行不能反驳诊断问题。当前源码未见该长度约束，两个搭档已逐段确认调用链；Main 已执行真实入口并复现相同2047字节诊断，见 §10。

### 2.2 语义反例与已覆盖/不成立项

- 类型与空值：null 不得混为 scalar，observe(null)/try_acquire(null) 应拒绝；typed strong/weak 空局部可转换。same_type、reference binary checks、change_reference/same_reference 消费分派均已检查；未发现新增静态类型穿透。
- 过期身份：observe(stale) 与 identity compare 不读取目标；try_acquire(stale) 返回 typed null，真正字段/方法访问经 frame.object 再由 Runtime 验证存活。business.cpp:1382-1463 已同时覆盖复制、观察、比较和拒绝解引用，不能把 root 当显式销毁租约。
- 求值：二元 left 后 right、call receiver 后 argument、field assignment receiver 后 RHS 的源码顺序明确（backend:226-338）。同一文本两次 new 必须产生两个对象；不能通过通用 CSE 合并。一个 AST 节点在一次表达式求值中不应重复执行；当前树构建和 lowering 未发现重复调用证据。
- 词法清理：block 清理用户局部；完整表达式/条件清理临时；return/failure 清全用户槽。consumer clear_slot 清根或观察槽；ExecutionFrame::clear 调用 reset。business.cpp:1211-1585 覆盖新 GC 周期、循环复用、早退、native failure、失败初始化和临时 receiver。尚缺穷尽组合与每种失败点注入，不称已穷尽证明。
- “短路”：当前没有 &&/||，这不是漏实现的短路错误。应拒绝含逻辑连接的源码；用嵌套 if 验证未选中分支不创建/调用，但未选中分支和 return 后的非法源码仍须拒绝。
- 数值：uint literal overflow 应拒绝而 uint add/sub/mul 模 2^32；float NaN != 为真、有序比较为假、负零保留，源码 lowering 与现有 acceptance 一致。分配/快照/GC 用量不能当数值语义变更理由。
- 词法位置：NUL 不是 EOF；comment 内可跳过 NUL，注释外须拒绝；CR 不换行，LF 换行，TAB/UTF-8 字节按字节列。正式验收覆盖嵌入 NUL；历史 source-pipeline-probe.cpp:240-249 另覆盖 CRLF/TAB、comment NUL 和括号/一元临界。历史探针不自动成为当前哈希的实测。

## 3. Standards：优化反方

### 3.1 C-R1：深度防护不是宽度/总资源预算（已知设计取舍 + 性能假设）

Parser::Depth 为累计递归预算 128，add(Expression) 为树高 128（source.cpp:135-140、216-224）。当前已有长左结合链/160 括号拒绝验收（business.cpp:2436-2441），不能声称完全没有深度保护。宽平局部依次声明每次 unused_name 扫描 visible_，且声明处与 local() 两次检查；n 个同作用域局部存在二次名字比较量。顺序互不重叠块可有 O(n) 节点/槽但不提高深度；SourceLowering 为全部词法局部与 new/observe/acquire 表达式预分配槽（business_backend.cpp:50-65），不是按最大活跃集。return 后合法源码仍前端检查并参与槽布局是既有实现；不是不执行分支仍创建业务对象。

最小生成输入族：`void wide(float a, uint n) { uint v0 = 0; uint v1 = 0; ... }`；这里省略号只描述输入生成族，不是可编译源码。另用顺序 `{ T x = new T(); }` 小块比较 n 与峰值活跃集。建议先做有界 64/256/1024 规模曲线而非压力爆破；深度 128 不是可接受 128 对括号的语义承诺。无证据表明当前真实业务因此超预算。

### 3.2 C-R2：不要以 token/arena 名称证明性能收益（性能假设，未证实）

Token.text 已为借用 string_view，Lexer 只持当前记号；Program 的 expression/statement 为 NodeId 索引的平坦 vector，而不是逐节点 unique_ptr 树。Program 自己拥有源名、函数名、类型名和局部名（source.hpp）。因此全量 token 缓存或“先把 AST 放进 arena”不自然等于减少分配；可能增加同时存活字节、引入借用悬垂和析构责任。真正待测成本为 Statement.children/locals 分散分配、vector 扩容、线性名字查找与 LLVM/LLJIT 建立的相对份额。

默认路径：保持现有语义/所有权，先分离 front-end 与真实端到端编译成本；只在证据说明收益抵偿复杂度时选择局部 reserve/分配布局改动。任何 arena 只能由返回 Program/编译任务的真实拥有者持有，不得在 Parser 销毁时释放仍被降低使用的字符串或节点。

### 3.3 生命周期优化的硬边界与拒绝条件

- Token 视图只在 Lexer/Parser 的同步调用内借用；返回 Program 必须独立于 source、source_name、Environment 字符串和 spans。现有 `source.hpp:66-70` 已明确所有权，这不是可用性能理由取消的复制。
- `NodeId` 允许底层 vector 重分配而索引保持有效；不能在构造期间把 `Expression&` 跨 push_back 长期缓存。没有此类越界实例，不制造当前悬垂缺陷。
- 任何 arena 必须在 Program 及降低消费者之后释放，且非平凡成员 string/vector 的析构责任不能被批量归还裸存储代替。project engineering 的拥有者/借用和 arena 指导足以决定此门槛。
- 补充一手机制：本机锁定 LLVM 23.1.1 的 `.tools/llvm-23.1.1/include/llvm/Support/StringSaver.h:19-53`（读取日 2026-09-25）中 StringSaver 自身只保存 `BumpPtrAllocator&`，UniqueStringSaver 另持唯一化表；该 API 以外部稳定存储保存字符，没有替调用方定义 allocator 超越消费者的生命期，也没有证明它优于当前 std::string。此处只引用随本项目工具链读取到的一手源码，不把它当独立性能基准，不为此迁入 LLVM 依赖到通用 frontend。

## 4. 最小反例、语义哨兵与当前覆盖

以下 `$T` 由真实 `compile_reference` 替换为绑定完整名，分别在 NativeMachine/DescribedMachine 上编译；不是新语法，也不拼接多个函数。完整可编译输入保存在 `out/review/wave1-meeting/source-counter-semantics.cpp`。

| case | 最小触发片段（放入 void/float/uint 入口） | 预期与错误版本反例 | 当前覆盖/本轮状态 |
| --- | --- | --- | --- |
| C-D1 | 源名为 2100 个 a；`self.progress = loops;`，两次 Request | 都拒绝且旧 Entry 可执行；不同 sequence 的错误应可区分。当前源码预测两文本均为相同 2047 个 a | 历史 request 身份断言只用短名；Main 本机已验证，见 §10 |
| S1 | `if (new $T() == new $T()) { self.state.cycles=99; } else { self.state.cycles=1; } self.advance(0.0); return; new $T().advance(delta);` | cycles=1；回调恰一次；两 new 不可 CSE 合并，也不可重复求值；return 后 new 不执行 | Main 本机已验证，见 §10；与旧临时值/早退覆盖互补 |
| S2 | `weak $T empty; float failed=try_acquire(empty).progress + new $T().progress;` | 左侧 null 实际解引用失败，右侧 new 不执行；GC 前 live_objects=2（receiver+snapshot） | Main 本机已验证，见 §10；不是逻辑 && 短路 |
| S3 | `weak $T empty; new $T().progress=try_acquire(empty).progress;` | receiver 先且恰一次创建，再 RHS 失败；GC 前 live_objects=3，用户根已清 | Main 本机已验证，见 §10；若只有2，实际变成先RHS |
| S4 | `weak $T empty; new $T().advance(try_acquire(empty).progress);` | receiver 先创建，再 argument 失败；方法不调用；GC 前 live_objects=3 | Main 本机已验证，见 §10；若方法被调用或对象数不同须追分派 |
| S5 | `if (loops == 0 && delta > 0.0) { return; }` | 应词法拒绝 &&；目前不存在逻辑短路语义 | 源码 Lexer 不支持 &；不要求扩展语法 |
| S6 | `return; uint x=x;` 或 `if (1==0) { uint x=x; }` | 均编译拒绝，不能以不可达规避名字检查 | 搭档 properties 真入口覆盖 |
| S7 | `uint x=4294967296;` / `uint x=4294967295+1;` | 前者拒绝；后者合法并回绕为0；不是统一 overflow 禁止 | 既有拒绝/数值执行已覆盖；properties 补临界 |

S1 回调发生时根数为3（宿主 receiver、执行 receiver、snapshot），对象数为4（另有两个刚撤根、未收集的 new）。S1 成功后的根数为2，S2/S3/S4 失败后的根数为1；都必须 `stats.explicit_cleanup`、`polls==1`。各 case 源码没有实际回边，且未请求 GC；随后释放宿主根并调用现有 collect 后对象/根归零、NativeState::live 回到场景基线。这些精确计数依赖**当前验收快照适配器**，不是永久语言 ABI。

过期身份与复杂词法清理不重复搭一套宿主：继续引用 `verify_stale_source_references`、`verify_reference_collection`、`verify_reference_loop_slots`、`verify_reference_exit`、`verify_reference_temporaries`、`verify_reference_failed_initializer` 的已固定源码。它们已从真实来源对象验证 fresh GC、失效身份复制/比较、直接访问失败、循环撤根和提前退出；尚未证明任意复杂组合、完整 OOM 或并发执行。

## 5. 完成的具体质询与回应

### 5.1 对 WaveSourceExplore 的三项主张

互读文件：`docs/research/wave1-meeting/source/explore.md`；驱动：`out/review/wave1-meeting/source-explore-probe.cpp`。双方先写独立初稿再互发，写入口最初 EPERM 经 Main 预建文件后解决；没有因此修改生产文件。

1. **对方：重复重名扫描产生 N²+3N 比较，是否值得先优化？** 我回应：数量可由 source.cpp 两次 unused_name 推导，但 engineering 明确允许当前线性查找。先以有界同族缩放、绝对耗时、编译端到端占比决定；不把 O(N²) 当安全漏洞。对方加入 newest_local_lookup 对照。我再反驳 old/new case 分别读 float 参数与 uint 局部、文本及节点也不完全同构，不能精确分离查找收益；对方已在报告承认只作趋势对照、源码生成/结构验证/Program 析构在计时外。**未决：真实业务绝对延迟和 end-to-end 占比；证伪条件：宽度斜率/成本不支持，便不引入名字表。**
2. **对方：单调总槽多于活跃峰值，能否认定存储浪费/安全问题？** 我回应：backend 构造分配布局、Invocation 一次构造稳定槽，清理消费者也真实撤根；含 return 后或互斥分支仅扩大布局，并不执行这些 new，也不泄漏身份。大布局与多返回生成 cleanup 的成本成立，但应先测。双方修正为**性能假设/明确取舍**，不立即上 SSA 活跃集、通用 arena 或根协议重写；D 席位承担真实 layout 对照，C 不重复实验。**证伪条件：真实 layout 不随节点数增长，或成本低且所有权复杂化超过收益。**
3. **对方：v2 receiver 顺序是否覆盖 v1 先 RHS？** 我最初保留冲突疑问，互读后以 v0.0.2 §5.2 与 backend:331-338 确认明确覆盖，撤销把历史 v1 文字当缺陷的可能。当前无版本选择开关。S2/S3/S4 是对当前序的哨兵而非自造新语义。**证伪条件：真实生成代码的对象数量/失败先后不符，而非只比较两份版本文字。**

### 5.2 对本席位 C-D1 的反驳与修正

- **对方初始反驳**：通用 C ABI 的 caller-sized buffer 允许有界文本；source.hpp 的 Diagnostic 仍完整，backend.hpp 仅明确 *output 原子性。不能把 snprintf 或 Parser 判错。
- **我的回应**：同意，责任收窄至 BusinessProgram::request/prepare 组合。名字不受长度约束，request#在尾部，1951 验收要求错误携带请求身份；两个2100字节同名失败请求将不可区分。固定容量执行 thunk 文档不能自动授权编译请求身份丢失。
- **对方再查证**：读 runtime-model:254/263/271 及 business:1927-1929/1951/2348-2350 后接受“源码证实的宿主诊断保真缺陷、待 Main 实测”，不是发布安全或旧代码保留失败。WaveReloadExplore 独立读相同链也未找到源名上限，确认此分工并不重复写探针。
- **修正**：C-D1 定为宿主 P2 诊断缺陷；不改 source.cpp、不改语言语义、不宣称并发乱序漏洞。替代方案和证伪条件见 §2.1，Main 决定实际错误形态/准入策略。
- **仍待 Main**：运行 diagnostics 输出两个 sequence、各自文本长度、相等性、request/行列/原因保留情况，且执行旧 Entry。仅旧代码存活不能反证诊断缺陷。

### 5.3 对探针的相互约束

我静态读搭档全部 properties/scale 驱动；其树边小于父索引、直接 block locals、输入释放后名字/selector 所有权断言与当前构造链相符。62/63括号、124/125一元、63/64空block是当前累计递归临界，不对外冻结。对方接受：所有权实验只证明当前 Program 已保存字段，不推广为任意后端借用安全；scale 不计 Program 析构，不能与含析构的历史数字直接比较。我的 counter 驱动已交对方静态检查；任何回报仅称源码检查，不称编译通过。

## 6. 本轮最多三项可采纳动作

1. **C-D1 首先证实并由 Main 决定修复授权。** 跑 diagnostics；当前预测 `DEGRADATION_OBSERVED`，两个消息2047字节、相等、没有各自request#也没有位置/Type mismatch，但短名control和旧Entry执行通过。若符合，采用结构身份或有限名字准入，不盲增缓冲。修复后的验收须保留旧Entry与不同失败请求可辨识两项。
2. **保留求值/清理哨兵，不改语法。** 跑 semantics；两个对象来源×四case，共8条PASS；根/对象及NativeState::live归零。任一失败先定位实际服务和数据，不能删断言换成宽松PASS，也不能用 mock 回调取代真实 Runtime。
3. **复用搭档前端 properties/scale，先性质再成本。** properties 先覆盖数值/词法/深度/拥有字节与直接块局部；有界规模以64/256/1024起步，需要解释斜率时再用搭档4096档，由Main独占性能执行。结合D的layout、frontend相对端到端成本决定是否值得消除重复扫描/局部预留；不并列追加token缓存、arena、通用源管理框架。

不是本轮动作：执行燃料/超时、安全沙箱、一般布尔短路、源码异常、通用增量编译。它们是明示非目标，不用当前有限边界测试声称已获得。

## 7. Main 的确切构建与运行入口

文件：`out/review/wave1-meeting/source-counter-semantics.cpp`。该一次性驱动 include 当前 `acceptance/jit/business.cpp`，复用真实 compile_reference/execute/BusinessProgram/Runtime 入口，没有复制实现、手工AST、假BusinessServices或另一套parser。与既有 reference-doc-examples 驱动相同的TU接入方式。不能再同时链接 business.cpp.obj 或 acceptance main.cpp.obj。

以下只是交给 Main 的命令，本席位没有执行。来源为当前 `out/review/wave1-meeting/build-provenance.json` 的 host compile/link；Release `/MD` 与现有宿主一致，使用当前 global_new_delete 和导入库，而非借用旧ABI探针对象。先确认 Main 的当前 Release 组件可用：

```powershell
. ./tools/enter-dev.ps1
$repo = (Get-Location).Path
$clang = Join-Path $env:ATELIER_LLVM_ROOT 'bin/clang-cl.exe'
$link = Join-Path $env:ATELIER_LLVM_ROOT 'bin/lld-link.exe'
$probe = "$repo/out/review/wave1-meeting/source-counter-semantics"
Push-Location out/release
try {
    & $clang /nologo -TP /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_MBCS /DWIN32 /D_WINDOWS /DNDEBUG `
        "-I$repo/acceptance" "-I$repo/src" "-imsvc$repo/out/release/_deps/mimalloc-src/include" `
        /EHsc /O2 /Ob2 -clang:-std=c++23 -MD /W4 /WX /permissive- /utf-8 `
        "/Fo$probe.obj" "/Fd$probe-compile.pdb" -c -- "$probe.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'source-counter compilation failed' }
    & $link /nologo "$probe.obj" CMakeFiles/atelier_jit_acceptance.dir/src/memory/global_new_delete.cpp.obj `
        /out:source-counter-semantics.exe /implib:source-counter-semantics.lib /pdb:source-counter-semantics.pdb `
        /version:0.0 -machine:x64 /INCREMENTAL:NO /subsystem:console /include:mi_version `
        _deps/mimalloc-build/mimalloc.dll.lib atelier_runtime.lib atelier_jit_probe.lib atelier_jit_native_client.lib `
        kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib psapi.lib
    if ($LASTEXITCODE -ne 0) { throw 'source-counter link failed' }
    & ./source-counter-semantics.exe diagnostics
    if ($LASTEXITCODE -ne 0) { throw 'source-counter diagnostics failed' }
    & ./source-counter-semantics.exe semantics
    if ($LASTEXITCODE -ne 0) { throw 'source-counter semantics failed' }
} finally { Pop-Location }
```

诊断模式故意以 JSON 字段记录“观察到/未观察到退化”，退出0只表示探针运行、control与旧代码断言通过，**不等于产品没有诊断缺陷**。语义模式任何不符抛错/退出1。两个模式互不依赖，不进行耗时/吞吐测量。

## 8. 来源与最终证据账

- 已完整读：会议 README/baseline；engineering；CONTEXT；language-contract；LANGUAGE-v0.0.1 §§3/8–14、v0.0.2 §§2/4–7。
- 当前源码：src/language/source.hpp/.cpp；src/runtime/execution.hpp/.cpp；acceptance/jit/business_backend.hpp/.cpp；business.cpp 中请求、引用服务消费者及上述具体验收函数；docs/runtime-model.md:248-274。
- 外部机制仅本地一手源码：锁定 LLVM 23.1.1 的 StringSaver.h:19-53，访问2026-09-25；只用于所有权/allocator引用边界，不引入外部语言语义或性能结论。
- 历史有限参照：out/review/source-pipeline-probe.cpp:240-249、reference-doc-examples.cpp 及其构建配方；不把历史数字计为当前实验。
- 交付：本报告、`out/review/wave1-meeting/source-counter-semantics.cpp`；搭档互读与至少一轮证据回应已完成。生产代码、验收、CMake、版本化规范均未改。
- 初次交付状态：C-D1 源码证实；S1–S4、边界性质及成本交 Main 运行/记录。后续 Main 证据和 A/B 质询见 §9；本席位没有执行实验。没有 OOM恢复、安全沙箱或完整组合覆盖声明。写入入口阻塞已解除；无其他阻塞。

## 9. 后续 active-name A/B 反方复核（仍属动作3）

### 9.1 新证据与范围

WaveSourceExplore 在完成首轮交付后转述 Main 的统一结果，并在其报告 §8 记录：当前 frontend properties 41项PASS；n=4096 的宽局部与旧参数查询明显比独立小块慢。该结果是 Main 执行、搭档报告的本机证据，不是本席位运行，也不是 JIT 端到端收益。它足以支持**隔离副本**的 active-name 索引 A/B，不直接授权生产修改。Main 独占 `out/review/wave1-meeting/source-active-names.cpp` 副本；本席位只质询方案，不改该文件、src 或搭档探针。

### 9.2 对五处精确补丁的反驳与结论

已读 explore.md §8.1–8.3：Parser 私有 `unordered_map<string_view,uint32_t>`，key 借 Token.text 的原 source；visible_ 仍为退出日志；unused_name 改 contains 并保留两次检测；local 在原 vector push 后 emplace；block 按直接 locals erase；primary 改 find。

- **生命期反方**：key 若借 `Program.locals[index].name`，vector 扩容移动 string（特别是短字符串）会破坏借用。方案明确使用原 source 的 Token.text，调用方在同步 compile_source 内已有稳定字节责任，因此此反例被排除。map 不进入 Program；Program 返回后覆盖/释放源码仍由已有所有权性质验证。不得把该结论推广到未来异步/可变源缓冲。
- **作用域反方**：若按当前 visible_ 全表 erase，会误删父块/参数；若在声明检查前 emplace，会让自身初始化错误地读到自己；若块退出忘记 erase，会拒绝兄弟块重名或读取已失效声明。方案只在成功初始化、分号消费后的 local 中 emplace，只 erase result.locals（直接局部），故当前链条排除这些反例。当前禁止shadow很重要：它保证同一名字最多一个可见局部，find 不需要另建查找优先级；以后开放shadow必须重新设计恢复外层绑定，不能机械沿用此map。
- **错误序反方**：重复声明同时带非法初始化器，应仍先报原来的“already visible”，而不是 Unknown identifier。保留 statement 的预检查及 local 内检查、原 consume/expect 顺序，保持当前首个检测时机；map分配失败只会中止整个Parser，无成功Program逃出，不新增“完整OOM恢复”承诺。
- **成本反方**：新增节点/桶分配与source-key hash不是免费；小输入、独立块可能回退。平均查找改善不是不可信输入最坏时间上限，不能升级为安全沙箱防护。A/B 应同时保存小case和宽case、所有工作负载，不只报告n4096最佳比值；端到端JIT仍未测。当前提案未见新增已证实语义缺陷，结论只是**可进入受控A/B**，不是“已优化完成”。

### 9.3 可直接交 Main 的额外反证输入

下列每条是独立完整源码，A/B必须一致，除明确错误外仍用原真实环境：

```source
void p(float a, uint a) {}
```

预期重复参数拒绝，位置在第二个a。

```source
void p(float a, uint n) { uint x=0; { uint x=missing; } }
```

预期第二次声明x的already-visible错误优先于missing；原错误位置/消息不变。

```source
void p(float a, uint n) { { uint x=1; } uint x=x; }
```

预期初始化器x为Unknown identifier，不能借到已退出的兄弟块，也不能提前看见自身。

```source
void p(float a, uint n) { { uint x=1; } { float x=a; self.progress=x; } }
```

预期接受，后一个x的引用指向f32局部；不是先前u32局部索引。

```source
void p(float a, uint n) { uint x=1; { uint y=x; } self.state.cycles=x; }
```

预期接受，内块退出不删外层x或参数。

另由Main在计时外生成完整文本：先声明短名`k0`，再声明64个不同短名/较长名字以触发locals向量增长，末尾读取`k0`赋给cycles。A/B均须接受且最后local节点索引仍为2；该性质可直接击穿“借用locals字符串”的错误改法，不要求固定vector容量或SSO阈值。若认为现有properties已足够，可只把这条作为单独诊断探针，不把它混入原计时曲线。

### 9.4 搭档对本反方探针的再核查

WaveSourceExplore 已读完整counter驱动并追execute:350–397，确认四例对象/根计数与入口snapshot、未请求GC及返回前显式清理检查相符。双方再次明确：它们证明列出的求值次序、失败后停止和清理，不证明任意GC interleaving；diagnostics模式退出0只表示探针完成，必须读取DEGRADATION字段判断缺陷。这一回应不替代Main的实际运行结果。

## 10. Main 统一执行后的原始证据（最终状态）

本席位只读取已有原始记录，没有重新执行或替 Main 运行。以下结果归属本轮当前实现，不借用历史性能数字。

### 10.1 C-D1 已本机复现，未修复

原始文件：`out/review/wave1-meeting/source-counter-semantics-diagnostics.json`，模式 `diagnostics`，`exitCode=0`、stderr为空。

| 原始字段 | 实际值 |
| --- | --- |
| name_bytes | 2100 |
| first_sequence / second_sequence | 2 / 3 |
| first_message_bytes / second_message_bytes | 2047 / 2047 |
| messages_equal | true |
| request_identity_retained | false |
| position_retained | false |
| cause_retained | false |
| old_entry_ok | true |
| result | DEGRADATION_OBSERVED |

这满足 §2.1 的触发预测，排除“只是推测截断”的不确定性；同时证明缺陷限于诊断归属/定位/原因丢失，旧入口仍可执行。退出0代表探针控制与旧入口断言完成，**绝不代表该缺陷已修复**。结构化身份/有限名字准入仍需另行授权实施。

### 10.2 八个真实语义哨兵通过

原始文件：`out/review/wave1-meeting/source-counter-semantics-semantics.json`，模式 `semantics`，`exitCode=0`、stderr为空。已完整读取stdout中的八条记录：NativeMachine与DescribedMachine各自的 `distinct_new_and_return`、`binary_left_failure`、`assignment_receiver_before_rhs`、`call_receiver_before_argument` 均为PASS；每例 `polls=1`、`explicit_cleanup=true`。

四类GC前对象数量断言分别为4、2、3、3；native调用次数分别为1、0、0、0。驱动只有完成对应对象/根/计数断言及最终collect归零后才打印PASS，因此本机结果支持 §4 的特定求值计数、失败后停止、早退未执行与显式清理结论。它不证明任意GC interleaving、完整OOM、并发安全或所有源码组合。

### 10.3 前端与优化结论维持边界

Main前端41项properties与scale的证据已由搭档记录 `source/explore.md` §8，原始文件为 `source-explore-probe-properties.json`、`source-explore-probe-scale.json`。本反方对active-name副本的额外五条完整源码及跨扩容k0检查见 §9；它们必须与原41项分开计数。首次登记时尚无active-name副本结果；Main随后完成的六项对抗一致性和小输入测量记录于 §10.4。两阶段计时口径不得混用，生产source.cpp仍未因此修改。

### 10.4 Active-name 六项反证通过，但小输入确有回退

Main 随后完成隔离A/B；本席位只读以下原始文件，未重跑：

- `out/review/wave1-meeting/source-active-counter-a-counter.json`
- `out/review/wave1-meeting/source-active-counter-b-counter.json`
- `out/review/wave1-meeting/source-active-counter-a-small.json`
- `out/review/wave1-meeting/source-active-counter-b-small.json`

两个counter进程均退出0、stderr为空，各自显示 `Active-name counterexamples: PASS (6)`。Main已比较完整序列化AST、Diagnostic的偏移/行列/文本，A/B逐字一致。六项分别为 duplicate_parameter、duplicate_before_bad_rhs、expired_not_self_visible、sibling_different_type、outer_survives_inner、source_keys_survive_locals_growth；不是仅“都能编译”。前三项继续拒绝，后三项接受；重复名与非法RHS同时出现仍先报重名，早期短局部跨增长仍解析到原index2。**原41性质与新增6反证分别登记，不把其组合当作穷尽语言证明。**

small原始记录为 `small_compile_and_destroy`：每个点9个样本，每样本1000次，包含compile及返回Program销毁；单位为每次的微秒中位数。下表A为当前生产前端，B为out中的active-name副本。

| 工作负载 | n | A median_us | B median_us |
| --- | ---: | ---: | ---: |
| wide_unique | 0 | 0.4245 | 0.5113 |
| wide_unique | 1 | 0.7077 | 0.8324 |
| wide_unique | 8 | 2.8704 | 2.8561 |
| sibling_scope | 0 | 0.4092 | 0.5002 |
| sibling_scope | 1 | 0.8376 | 0.9212 |
| sibling_scope | 8 | 3.1390 | 3.4701 |
| sequential_reference_blocks | 0 | 0.4095 | 0.4992 |
| sequential_reference_blocks | 1 | 0.9045 | 1.0593 |
| sequential_reference_blocks | 8 | 4.1426 | 4.6701 |

反方结论：n0回退约20–22%，但绝对增加仅约0.087–0.091微秒；n1约10–18%回退，顺序引用块n8约13%回退。wide_unique n8的微小反向差值不宜描述为稳定收益。这不是可忽略不报的结果，也不能只报百分比把亚微秒差异放大为当前业务故障。小输入趋势说明新索引不是无条件改善，node/bucket及销毁成本必须结合真实输入规模分布衡量。

**计时口径隔离：**原wide scale只计frontend compile、不含Program销毁；本表包含销毁。不得把两个系列直接相减、拼成同一速度比或推导JIT端到端改进。语义一致性支持候选的正确性门槛，不证明性能门槛已满足，也不提供不可信输入最坏资源保证。

可采纳裁决保持为：**active-name候选继续研究，暂不合入生产。**先由真实业务输入分布、前端占端到端比例和分配成本决定小输入回退是否值得；不凭本微基准擅加大小阈值双路径、缓存或arena。本轮无新生产缺陷、无新动作编号、无额外执行要求。

最终交付仍为本报告和 `source-counter-semantics.cpp`，唯一已证实缺陷为 C-D1（P2）；其余为已通过的有限语义路径或明确保留取舍的隔离性能候选。生产代码、版本化语言规范、验收和CMake均未由本席位修改，无剩余阻塞。
