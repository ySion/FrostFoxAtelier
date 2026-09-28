# C 意图探索：源码 → 定型程序与可诊断失败

> 历史快照：本文依据当轮 baseline.json；旧接口、文件名、行号与未实现判断仅描述当时状态，不是现行规则或待执行任务。后续结果与现行入口见[历史索引](../README.md)。

作者：WaveSourceExplore。状态：独立审查与搭档质询完成；Main 已验证41项前端性质和8项生成代码语义，并复现长名诊断缺陷；active-name 隔离 A/B 的精确补丁与验收已交 Main。日期：2026-09-25。

## 1. 意图、范围与证据口径

本席位负责词法、解析、定型、作用域、引用临时值传递及编译复杂度；只读 source.hpp/source.cpp，为 AST 消费者追至 business_backend.cpp 的布局及分派。不新增语法、解释器或第二个类型世界。

固定依据：会议 README 与 baseline.json；source.hpp SHA256 de912e178e102976fe3e2f1748da55ce808859549375fa2006b7fe385d9bcd1f；source.cpp SHA256 bc1220616e9200711f46ae907c704270f88c87dc0fafeb883aa7f960ebbea6c9。baseline 的哈希匹配和 dev/release 通过是既有同源码证据，不是本席位新实测。本席位没有运行构建、编译器、测试或压力负载；以下区分源码证据、成本推导及待测项。

## 2. 当前真正支持的子集

权威读法是 LANGUAGE-v0.0.1.md 未覆盖规则 + LANGUAGE-v0.0.2.md 明确覆盖的引用增量，不是两种可选模式：一个 void 入口，恰好 float/uint 两个可写参数；ASCII 标识符和字节行列；uint32/float32、严格类型、+/-/* 与比较；块、if/else、while、return；单一宿主完整对象类型、持有/weak/null/self/new/observe/try_acquire；绑定标量成员和一个标量实参的 void 方法。引用初始化器可省略且默认空，标量不可省略。无普通函数互调、类型声明、bool 局部、隐式数值转换、引用真值转换、引用字段、多对象类型或异常捕获。

v0.0.2 替换 v0.0.1 的八保留字、self 不能为值、只有标量局部及固定根槽说明。字段赋值接收者先求值再求 RHS 是 v0.0.2 §5.2 的明确增量，不应把 v0.0.1 §8.2 当成当前反例。没有版本开关，不建议增加双解析器维护历史文本。

## 3. Spec：忠实性与失败边界

### 3.1 已覆盖/未发现反例的路径（源码证实）

- Lexer::next/peek/advance（source.cpp:35-103）：实际长度控制 EOF；NUL 在注释外拒绝、注释内按字节跳过。LF 唯一换行，CR/TAB 各增一列；BOM 不剥离。数字先扫描小数/指数并拒绝 ASCII 后缀，再由 from_chars 检查完整转换、范围和有限浮点。与 v0.0.1 §§3–5 一致。
- Parser::identifier/object_type/validate_environment：保留字禁止作声明名和类型路径段；完整对象名与宿主绑定精确相等。字段/方法仅接受 f32/u32 并各自拒绝重名。Environment 是已验证 Runtime 成员的借用描述，前端不重建注册表。
- Parser::statement/local/block（source.cpp:229-331）：初始化器定型及分号通过后才加入 visible_；禁止仍可见同名，块退出缩回 visible_；Statement::locals 仅含该块直接声明索引。后端 block 倒序清理这些引用，不清外层引用；参数 locals[0:2] 不进入函数体直接局部清理表。
- Parser::add/expression/unary/postfix/primary（source.cpp:213-460）：定型树保存 selector 与独立 receiver 边；括号复用节点，+ 新增 unary 节点，故 +(lvalue) 不可赋值。比较产 predicate；引用只支持 ==/!=；strong/weak 赋值同形态或 null；observe/try_acquire 不接收无形态 null。
- 消费者闭环（business_backend.cpp:50-64,227-377）：所有 Expression::Kind/Statement::Kind 均有消费分支；create/observe/acquire 取得相应临时槽；receiver、参数按顺序降低。完整表达式及条件在控制体前清理临时槽；赋值/声明先复制到目标再清临时值；字段 receiver 在 RHS 前求值并保持至写入结束。未看到 AST 分支静默丢失。
- Parser::parse 返回自有 Program；Token/Environment 借用不逃出编译，source/function/type/local 名均拥有字节。Rejected 转 expected，部分 AST 随 Parser 销毁；不修改调用者旧 Program。business_backend.cpp:467-505 仅在解析/IR/链接成功后写 *output，保留既有句柄。
- return 与常量假分支后的 AST 仍完整静态检查；后端跳过不可达代码不允许非法源码绕过检查。

### 3.2 诊断与原子性的准确边界

Diagnostic 只有 position/message；source_name 属于调用上下文，C 编译边界格式化 source_name:line:column。成员错误可落 receiver 起点，默认空节点可落名字位置；v0.0.1 §13.1 不保证最小错误字符。consume 和声明前瞻可能提前检测后续词法错误，因此首个检测错误不等于最小偏移错误。没有多错误恢复/稳定编号是公开限制，不要求新框架。

compile_source 只捕获内部 Rejected；分配失败等异常未伪装成语法错误，外层 noexcept C 入口有异常转诊断。v0.0.1 §12.4 不承诺完整 OOM 恢复。执行失败不回滚业务写入不等于编译失败原子性破坏。

### 3.3 验收映射及缺口

business.cpp:1048-1133 执行优先级、独立块同名复用、嵌套循环、早退、uint 回绕、NaN/负零；2392-2454 覆盖定位、保留旧代码、长左链和深括号拒绝、注释外 NUL；1152-1713 覆盖强弱局部、保活/回收、循环、失效身份、完整表达式临时值及拒绝矩阵。会议仍应直接验证当前 frontend 有效/无效临界、Program 块归属及输入字节释放后的产物所有权；历史 probe/fuzz 数字不当成本轮证据。

## 4. Standards：成本、所有权与 Module 深度

### S1：宽作用域查询成本（源码成本事实；收益未测）

unused_name 扫 visible_；声明前一次，local 中再次。N 个同块不同名局部（另有两个参数），重名检查为 2*sum(i+2,i=0..N-1)=N²+3N 次比较，不含参数自身的一次比较。primary 逆序查找，读取早期 float 参数需 N+2 次比较。环境重名检查另有 F(F-1)/2+M(M-1)/2，现有 F=3/M=1 很小。

docs/engineering.md 允许首阶段线性名称查询，不将 O(N²) 自动判成缺陷。A：只消除重复重名扫描，保留 vector，改变常数而非阶数。B：若实测斜率和绝对延迟值得，加入 Parser 私有 active-name 表，块退出按 visible_ 后缀撤销；保留索引、不遮蔽和诊断语义。B 的哈希/分配/名字寿命成本须计入，不公开通用符号注册 Module。

### S2：总槽数不等于活跃峰值（源码事实；非安全缺陷）

SourceLowering 为所有引用 local 及 create/observe/acquire 节点分配独立槽，包含互斥分支和 return 后不执行 AST。N 个依次退出的 { T owner; } 仍产生 N 个用户强槽，最大同时活跃用户强局部却是 1。槽号不复用不等于身份泄漏，清理存在。但 Invocation 根数组及退出清理按总槽数，多个条件 return 可放大 IR。

应与 D 席位测量，不马上引入寄存器分配/SSA 活跃分析/GC 新协议。候选仅在 Lowering 内重用不重叠词法块/完整表达式存储位；清理和队列撤销责任不变，错误路径不误清活槽。若收益不明显，保留单调布局的可审计性。

### S3：复用已有深 Module，不搭新框架

compile_source 是小 Interface，封装词法/解析/绑定/定型/定位，Program 是消费者 Seam。扁平 vector+NodeId 不逐 AST 节点分配；Token string_view 仅在编译期借用。复用自有 Program、位置、receiver 边和直接局部表，补产物性质实验即可；不按文件长度拆分、不盲建 arena/源文件管理器/诊断编号/恢复系统。ValueType 与类型名字节不成为第二 Runtime 类型权威。

## 5. 最多三项优先动作（质询后）

1. **宿主诊断保真**：由 Main 运行 Challenge 的长源名真实 prepare 反例；源码已证明 2048 缓冲前置任意长源名会截掉请求编号、位置和原因。修复位置是宿主错误表示/格式化，不是 Lexer/Parser。A 为请求身份、position、message 独立保存，最终再展示；B 为接入层明确长度限制或有界显示名但保留编号/位置/原因。A 更完整，B 更小但必须明确输入契约；仅放大缓冲不解决无上界问题。Main 未运行前不写本机已复现。
2. **当前前端性质与规模证据**：已写 out/review/wave1-meeting/source-explore-probe.cpp，真实 compile_source，无 mock/随机输入/替代解释器。properties 模式检查临界、字节位置、引用语义、AST 边/块归属、输入所有权；scale 模式对照宽局部、独立小块、最老参数/最新局部读取与顺序引用块。先证实绝对成本再决定删除重复扫描或私有名字表，不把线性选择自动判错。
3. **引用槽成本核对（Main/D）**：真实 atelier_compile_business+atelier_business_layout 编译不执行顺序块及多个条件早退程序，记录槽数/编译时间/代码量。默认空强局部顺序块推导 strong_slots=2+N、weak_slots=0；不符先修正推导。只有收益抵偿清理/失效规则复杂度才复用互不重叠存储位，不上完整活跃分析，不修改 GC。

## 6. 完成的互读与质询

已完整互读 docs/research/wave1-meeting/source/challenge.md，通过 hub 与 WaveSourceChallenge 完成一轮含具体源位置的问答，不用“双方同意”替代实验。

### 6.1 名字查询、槽布局与 arena

- 反方：docs/engineering.md 已明确线性名字查询为阶段选择，只有成本阶数不足以证明本轮真实业务卡顿；单调槽也是明确可审计的选择；Token 已 string_view，AST 已扁平，arena 未自然减少分配。
- 回应：承认不是 Spec/Standards 硬缺陷。unused_name 的 N²+3N 比较是源码可推导事实，收益仍需有限规模绝对时间及端到端占比；新驱动加入 newest_local_lookup 对照 old_parameter_lookup，避免把声明本身成本误归为查找。没有把 arena 列成动作。
- 修正：优先级从“优化名字表”收敛为“测量后选择 A/B”；slot 只论成本，不声称 dead 源码创建对象或身份泄漏。
- 证伪条件：若规模曲线/真实延迟并非瓶颈，就保留现有实现；若 layout 返回值不符 2+N，先检查条件和推导，不制造优化收益。

### 6.2 规范版本与求值次序

- 争议：v0.0.1 先 RHS 与 v0.0.2 receiver 先 RHS 是否冲突。
- 证据：v0.0.2 明文覆盖规则、§5.2 receiver 先于参数/赋值 RHS；backend.cpp:326-338 明确先 expression(target.receiver)，再 RHS，再写入。
- 修正：反方撤销语义冲突疑问；两方均按 v2 覆盖解释，不改历史规范、不新增版本开关。
- 证伪条件：真实带不同副作用的两个 receiver/右侧 new 路径若执行顺序相反，才重开；单凭历史 v1 文字不能定缺陷。

### 6.3 长源名诊断：定位责任而非扩大结论

- 反方 C-D1：business.cpp:581-605 允许任意非 NUL 源名，追加 [request#N] 后 prepare 仅传 2048 字节缓冲；backend.cpp:490 先格式化完整源名。2100 字符名字使可见消息只有前 2047 个名字字节，请求编号、行列、原因全丢。business.cpp:1927-1929/1951/2348-2350 验收要求请求特定诊断。
- 我的初始质询：C ABI 的 caller-sized 缓冲本就有界，backend.hpp 只保证失败不改 *output，不能把 snprintf 本身或 Parser 的 Diagnostic 判错。前端 std::expected 仍含完整 position/message；probe 专门检验长源名时这一点。
- 源证据回应：runtime-model.md:254 的固定容量说明针对执行 thunk；263/271 与上述请求验收支持源名/阶段/根因及请求身份，并未规定长合法名字例外。真正问题是 BusinessProgram 接受任意源名而错误展示丢掉全部核心，责任在宿主 prepare 边界。
- 修正结论：接受为源码已证实的宿主诊断保真缺陷（本机复现待 Main）；不是旧代码覆盖、陈旧候选发布、对象/根泄漏，也不归咎 source.cpp。一个错误文本保留结构身份或有界展示策略即可，不扩语言语法。
- 未决/证伪条件：Main 应记录两个不同 request 的消息长度、[request#N]/行列/原因以及旧 Entry 执行结果。若实际仍完整，重新检查编译链/缓冲条件；若只旧代码存活，并不能证伪诊断缺陷。方案 A/B 的对外错误形态需要 Main 决策，不由研究代理擅改。

## 7. 一次性驱动及 Main 精确入口

文件：out/review/wave1-meeting/source-explore-probe.cpp。生产编译源为 src/language/source.cpp；使用 business_backend.hpp 中真实字段 selector 常量，不调用其 DLL 或构造假 BusinessServices。只有 std::expected 返回的真实定型 Program/Diagnostic；结构断言不是另一套 parser/type checker。properties 与 scale 两模式相互独立，不自动运行压力测试。

已核对 out/release/compile_commands.json 的 source.cpp 配置：固定 clang-cl、-clang:-std=c++23、/EHsc、/O2 /Ob2、-MT、/W4 /WX、/permissive- /utf-8 /GR-。Main 可在已由 tools/enter-dev.ps1 准备的 PowerShell 中执行以下一次性命令；不写 CMake、不换依赖，不将该命令称为正式全验收：

```powershell
$repo = (Get-Location).Path
$compiler = Join-Path $env:ATELIER_LLVM_ROOT 'bin/clang-cl.exe'
Push-Location out/review/wave1-meeting
try {
    & $compiler /nologo -TP -clang:-std=c++23 /EHsc /O2 /Ob2 -MT /W4 /WX /permissive- /utf-8 /GR- /DNDEBUG /D_ITERATOR_DEBUG_LEVEL=0 "/I$repo/src" "/I$repo" source-explore-probe.cpp "$repo/src/language/source.cpp" /Fe:source-explore-probe.exe /link /incremental:no
    if ($LASTEXITCODE -ne 0) { throw 'source-explore probe compilation failed' }
    & ./source-explore-probe.exe properties
    if ($LASTEXITCODE -ne 0) { throw 'source-explore properties failed' }
    & ./source-explore-probe.exe scale
    if ($LASTEXITCODE -ne 0) { throw 'source-explore scale failed' }
} finally { Pop-Location }
```

预期而非本机结果：properties 退出 0 且所有记录 PASS；128 高左链/62 对括号/124 个一元+/63 个空嵌套块通过，对应下一档拒绝。CRLF/TAB 的 missing 为第二行第二列，CR-only 不换行，UTF-8 注释按字节列；comment NUL 通过，外部 NUL/BOM 拒绝。强弱形态及直接块 locals/自有名字断言通过。规模为每种64/256/1024/4096，先一次 warmup 后五次样本，输出 min/median/max。计时包括 compile_source 内的解析/分配与返回，不包括生成、结构验证、返回 Program 析构；因此不能直接比较历史含析构的数字。不同 workload 字节/节点数不完全相等，以输出计数和同族缩放为准，不声称名字查找被完全隔离。

本席位仅写驱动并静态检查，没有自行运行；Main 已统一完成首次生产前端验证，原始结果与本机数值见下一节。Main 已完成A/B副本：同41性质与独立6反证通过，宽作用域收益和小输入回退分别见§8.4–8.5；候选未合入生产，不推广为JIT收益。D 席位 WaveExecExplore 已接手真实槽布局，见 execution/explore.md §7 与 out/review/wave1-meeting/execution-probe.cpp：顺序null块N=1/16/64、条件早退N=16/64，native/described双类型；默认空强局部预期strong=2+N、weak=0。其全部16场景/80次compile还分开first execute、warm batch、reclaim和retire，但不提供IR/codegen/link细分或实时峰值；运行入口为 ./out/release/execution-probe.exe --measure，由Main统一构建运行，C不重复执行。后续Main已完成该实验，槽数和编译aggregate回证见§8.6；未测内部阶段仍明确保留。

## 8. Main 本机证据与最小 active-name A/B（新增）

Main 已用当前正式 Release 宿主编译参数/依赖编译本驱动，并独立编译同哈希生产 source.cpp。已读取 source-explore-probe-build.json、-properties.json 与 -scale.json：properties 退出 0、41 项 PASS；scale 退出 0。执行者是 Main，本席位未运行编译/测试。实际构建用正式宿主 -MD、global_new_delete/mimalloc 和目标依赖，不是上节更轻量的 -MT 独立命令；重现实测应优先复用 build.json 的完整命令。

| 工作负载 | n=64 中位 ms | n=256 | n=1024 | n=4096 |
| --- | ---: | ---: | ---: | ---: |
| wide_unique | 0.0254 | 0.1866 | 3.3532 | 40.8693 |
| sibling_scope | 0.0174 | 0.0725 | 0.2575 | 1.0851 |
| old_parameter_lookup | 0.0415 | 0.3042 | 4.3812 | 57.0574 |
| newest_local_lookup | 0.0400 | 0.2441 | 3.5143 | 39.0045 |
| sequential_reference_blocks | 0.0256 | 0.0971 | 0.3624 | 1.5667 |

这些是同次有限规模、每点五样本的当前基线，不是端到端 JIT 延迟、吞吐或优化收益。查询对照的 float/uint 字段和字符串长度不同，不可直接相减声称精确查找成本；宽局部的源码成本推导与时间曲线共同支持做一个最小 A/B，而不是直接修改生产代码。

### 8.1 实验约束

Main 将 src/language/source.cpp 复制到 out/review/wave1-meeting/source-active-names.cpp 后仅对副本作下列替换。A 是当前 source.cpp；B 是副本；两者都用同 source-explore-probe.cpp、同正式编译/链接参数，只替换 frontend 编译单元，绝不能同时链接 A/B 两份 compile_source。src、版本规范、CMake 不变。若 baseline hash 不符，先重新定位，不能模糊替换。

B 仅增加一个 Parser 实例内的 active-name 索引；值仍为 Program.locals 的 uint32 索引，不新增符号身份。visible_ 原样保留作为词法退出撤销日志；仍禁止遮蔽，不建每 scope 一份 map/对象框架。map 的 key 必须来自 Token::text 的源码字节，绝不借用 Program.locals[*].name：后者 vector 扩容及 string 移动会使 SSO 借用失效。源码在同步 compile_source 期间有效；索引随 Parser 销毁，不能进入 Program 或后端。

### 8.2 五处精确补丁

1. source.cpp include 区，在 `#include <cmath>` 后加入：

```cpp
#include <unordered_map>
```

2. 将完整 `Parser::unused_name(const Token&) const` 函数体替换为下述内容；原错误文字、位置不变。**保留 statement 声明处和 local 内两次调用**，这样 duplicate-name 与非法初始化器同时存在时仍按当前时机拒绝，不为了少一次 O(1) lookup 改变诊断顺序：

```cpp
    void unused_name(const Token& name) const {
        if (active_names_.contains(name.text))
            reject(name.position, "A local or parameter with this name is already visible; shadowing is not supported.");
    }
```

3. `Parser::local` 中保持已有 `program_.locals.push_back(...)`、`visible_.push_back(index)` 次序，在后者之后、`return index` 之前加入：

```cpp
        active_names_.emplace(name.text, index);
```

若 map 分配抛错，整个 Parser 被销毁且没有成功 Program 逃出；这与已有 vector/string 分配失败的边界相同，不假称完整 OOM 恢复。unused_name 已排除重复；不要把哈希表顺序当声明序或 Program 局部序。

4. `Parser::block` 中保留 `consume()` 和 `result.locals.assign(...)`，在 assign 后、`visible_.resize(visible_before)` 前加入：

```cpp
        for (const auto index : result.locals)
            active_names_.erase(std::string_view(program_.locals[index].name));
```

result.locals 已只含当前块直接局部；内层块已自行删除，外层/参数不得删除。erase 使用拥有名字的字节作查询，不存该 string 的借用；string_view key 指向原 source。这保留兄弟块同名复用与嵌套可见性。

5. `Parser::primary` 最后现有从 `for (auto iterator = visible_.rbegin(); ...` 到循环闭括号的名字解析循环替换为：

```cpp
        if (const auto found = active_names_.find(token.text); found != active_names_.end()) {
            const auto index = found->second;
            const auto& local_value = program_.locals[index];
            return add(Expression{Expression::Kind::local, local_value.type, token.position,
                no_node, no_node, index});
        }
```

保留之后 `reject(... Unknown identifier ...)` 原样。在成员 `std::vector<std::uint32_t> visible_;` 后加入：

```cpp
    // Keys borrow source token bytes for this synchronous Parser only.
    // Local IDs remain the existing Program indices; visible_ logs scope exit.
    std::unordered_map<std::string_view, std::uint32_t> active_names_;
```

以上“第五处”同时包含使用点与私有成员声明；不触碰 validate_environment、AST、诊断格式或 LLVM 降低，不添加 reserve 猜测、跨编译缓存、arena 或第二名字注册器。

### 8.3 A/B 验收与否决条件

- 先用相同41项 properties；A/B 均须退出0。特别关注引用初始化不可自见、禁止内层同名、兄弟块重名不同类型、独立块 locals、Program 输入释放后拥有字节、错误后再次成功。
- 再同5工作负载×4规模；记录 min/median/max、节点数与源字节数。B 预期降低宽作用域阶数，也可能在 n64/独立小块因哈希/桶/节点分配更慢。不能只公布最好数据。
- 不把“unordered_map平均O(1)”推广成不可信输入最坏时间界，也不把 A/B 速度等同于端到端 JIT 同比例提速。新增容器分配/保留量需在下一真实需求决定是否引入第三方紧凑容器，当前只试标准容器最小实现。
- Map 对源码字符内容的借用不新增调用方义务：原 Lexer 本就要求编译期间输入保持有效、不可同时改写。若未来异步 parser 改变生命周期，必须重新拥有输入；本轮无异步扩展。
- 只在性质不变、宽场景改善显著且小场景回退可接受时，才建议后续授权生产改动。本轮交付是隔离实验，不提前宣布优化已完成。

### 8.4 Main 已完成的隔离 A/B 与额外反证

已读取 source-active-probe-build.json、-properties.json、-scale.json。构建记录明确：同一 source-explore-probe.cpp、同正式 Release host -MD/mimalloc/依赖，只把 frontend TU 从生产 source.cpp 换成 out/review/wave1-meeting/source-active-names.cpp。局部检查副本的 include/contains/emplace/direct-locals erase/find 与§8.2相符。B 的 properties 仍41项PASS，scale退出0；各点源字节/局部/表达式/语句数量与A一致。生产source.cpp未合入变体。

下表为 n=4096、每点五样本的parse中位时间，不包含返回Program析构：

| 工作负载 | A 现行 ms | B active-name ms | 本次观察 |
| --- | ---: | ---: | --- |
| wide_unique | 40.8693 | 1.0115 | 约40.4倍基线/候选比值 |
| old_parameter_lookup | 57.0574 | 1.9374 | 约29.5倍 |
| newest_local_lookup | 39.0045 | 2.2114 | 约17.6倍 |
| sibling_scope | 1.0851 | 1.1514 | 候选慢6.1%，绝对增加0.0663ms |
| sequential_reference_blocks | 1.5667 | 1.7020 | 候选慢8.6%，绝对增加0.1353ms |

不能隐藏小输入成本：原scale的n=64，sibling_scope为0.0174→0.0206ms（+18.4%，+0.0032ms），顺序引用块0.0256→0.0298ms（+16.4%，+0.0042ms）。其他点完整保留原始JSON；B并非所有输入都更快，哈希/节点/桶成本不能被宽函数收益抹去。

Main 又执行 source-active-counter-a/b 的 counter 模式，原始文件 source-active-counter-{a,b}-counter.json：均退出0，新增6反证全部通过，Main确认完整AST及错误位置/文字输出逐字一致。本席位已读两份输出；重复参数为offset21/line1/column22，重复x带missing仍首先already-visible为43/1/44，退出scope后uint x=x仍Unknown为47/1/48；兄弟异型同名、外层引用及短名k0跨64个混合长短局部增长后仍绑定index2的产物一致。**这6项单列，不更改原41项计数。**

### 8.5 小输入完整生命周期成本：不能混用计时口径

已读 source-active-counter-{a,b}-small.json。small 每点9批×1000次，包含 compile_source **及返回Program销毁**，数值为每次微秒；不能与§8.4只计compile的毫秒直接拼接或推导析构耗时。

| 工作负载/规模 | A µs/次 | B µs/次 | 取舍 |
| --- | ---: | ---: | --- |
| wide_unique n=0 | 0.4245 | 0.5113 | 约+20%，绝对+0.0868µs |
| wide_unique n=1 | 0.7077 | 0.8324 | 小输入回退 |
| wide_unique n=8 | 2.8704 | 2.8561 | 本次接近，不宣称确定收益 |
| sibling_scope n=0 | 0.4092 | 0.5002 | 小输入回退 |
| sibling_scope n=1 | 0.8376 | 0.9212 | 约+10% |
| sibling_scope n=8 | 3.1390 | 3.4701 | 小块重复成本仍在 |
| sequential_reference_blocks n=0 | 0.4095 | 0.4992 | 约+22%，绝对+0.0897µs |
| sequential_reference_blocks n=1 | 0.9045 | 1.0593 | 约+17% |
| sequential_reference_blocks n=8 | 4.1426 | 4.6701 | 约+13%，绝对+0.5275µs |

结论收敛：实例内索引通过当前41性质和独立6反证，在本次宽作用域parse上有很大改善，但小输入/独立小块有回退，**继续研究候选，不贸然生产合入**。不是“生产已优化”“JIT同比加速”或不可信输入最坏资源上限。A/B独立进程先后运行，非交错/跨机器统计；未测分配事件、峰值/常驻内存。真实编辑场景分布、端到端JIT占比及新增内存成本仍是判断门槛，不以结果不足为由再堆第三方容器、arena、混合阈值策略或增量缓存。

### 8.6 D 席位槽成本的本机回证

已读 execution/explore.md §7.1，来源为Main的 execution-probe-measure.json：16场景/80次compile均PASS。顺序null块N=1/16/64实际强槽3/18/66、弱0，2+N推导已被当前正式DLL验证，不再只是预测。N64顺序块compile aggregate中位为原生13.724ms/描述13.470ms；条件早退块为110.051ms/109.757ms，槽数相同。

这支持优先定位每个return复制全槽清理的代码规模成本，而非宣布slot数组分配主导：未单测IR构建/codegen/link，约96ms差值不能全归某一步，也无allocator事件/峰值证据。其execute数据以128次受检调用一批统计，不能误读为每调用。下一步应在D拥有的Lowering路径里比较保留词法活期的清理IR共享/静态槽复用方案，C不复制执行实验或修改根规则。

## 9. 对方一次性反例的静态复核

已读 out/review/wave1-meeting/source-counter-semantics.cpp 并追 execute（business.cpp:350-397），向 WaveSourceChallenge 回复：distinct-new 例的回调 root_count=3 来自宿主持有 receiver、执行 receiver 和 snapshot；两个条件 new 的临时根在分支前撤除，但没有主动 GC，所以 live_objects=4。binary 左侧 null 字段失败使 RHS new 不执行，执行后仅 receiver+snapshot 两对象；字段/方法临时 receiver 先 new，RHS/参数再失败，故三对象。执行返回前 explicit_cleanup 已在 RAII 之前检查；失败仅保留 host receiver 一根，成功另保留返回 snapshot 一根。没有发现反例断言明显不符源码。

证据边界：四例关注求值/失败后的停止及显式清理，不证明任意 GC interleaving；diagnostics 模式 exit0 表示测量完成，必须看 DEGRADATION_OBSERVED/NOT_OBSERVED 字段判断，不得把进程成功说成缺陷修复。Main 统一运行，双方不自行执行。

### 9.1 Main 后续本机结果

已读 source-counter-semantics-diagnostics.json 与 -semantics.json：均进程退出0。诊断实验实际输出 DEGRADATION_OBSERVED：2100字节源名、request序号2/3，两条错误均2047字节且完全相同；request身份/位置/原因三项均未保留，旧 Entry 执行仍正常。这把§6.3的源码推导推进为当前本机可复现的P2宿主诊断保真缺陷，不改变“非Parser/非发布安全/旧代码未损坏”的定位。

语义实验在 NativeMachine/DescribedMachine 各四例共8例 PASS：distinct_new_and_return、binary_left_failure、assignment_receiver_before_rhs、call_receiver_before_argument；均入口polls=1且显式清理。它们验证本次列出的求值和失败停止性质，不构成通用故障/任意GC交错证明。所有实验均由 Main 统一执行，原始输出仍保留本目录 out 记录。

### 9.2 active-name 补丁的反方二次静态审查

WaveSourceChallenge 已读§8五处补丁，检查：key借Token原source而非可移动locals；erase仅用拥有名字的字节查找；参数留到Parser结束；禁止shadow令hash find不存在优先级歧义。未找到明确寿命/作用域反例，但要求独立加测 duplicate-name+坏RHS 的诊断顺序、短/长名字混合及locals增长、块退出后外层绑定、兄弟块同名不同类型。已同意作为同41性质之外的定向加测，不能篡改旧41计数或冒称原基线已含这些组合。

## 10. 来源

均为本仓库一手规范/源码：LANGUAGE-v0.0.1.md §§3–14、LANGUAGE-v0.0.2.md §§2–7、docs/language-contract.md、docs/engineering.md、CONTEXT.md、src/language/source.hpp/.cpp、acceptance/jit/business_backend.cpp、business.cpp。未借外部运行时语义替代本项目契约；本轮结果由 Main 统一运行并记录。
