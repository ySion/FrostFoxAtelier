# 首批业务调用性能测量方案

> 状态：四路专家审查、交叉质询和定向可行性验证已完成；正式计时驱动及新吞吐基线尚未实施。本轮没有修改生产实现，没有性能提速结论。
> 后续状态：本文保留测量规划时的历史证据。`ABI-RESULT-DIAGNOSTIC` 已修复并纳入正式端到端验收；当前诊断行为见[共享函数签名与结果](../runtime-model.md#共享函数签名与结果)。历史探针的空诊断输出不代表修复后的状态。
> 2026-09-28 更新：正式公共接口的时间与资源基线已由独立[阶段测量](runtime-stage-measurements.md)落实，覆盖标量、原生/描述型字段、协程和代码退休。本文更广的三个业务模型仍是历史规划，不等同于此次测量范围。
>
> 决定：先测机器 tick、纯标量工具规则、UI 选择提交三个业务逻辑模型。它们使用当前 Runtime、源码前端、LLVM/JIT 和既有原生/描述型类型；不代表完整游戏、渲染或产品 UI。九参和求值顺序编码只作正确性/存储边界控制。

## 1. 审查范围与结论

基点为 `a6303748a4a149d363727321244898ac96e30d6c` 的当前工作区，共同函数契约及其标量调用、版本发布依赖为主责。工作区 diff 同时含前一个标量调用增量，不能将全部差异归因于最后一轮。`typed_business.inl` 是未跟踪的当前源码，已纳入审查。

固定的 16 个源码/配置文件 SHA-256 见 `out/review/call-performance-planning/source-manifest.json`；本轮结束核对全部未变化。规范来源为前轮批准的 `implementation-contract.json`、`docs/runtime-model.md`、`LANGUAGE-v0.0.2.md`；工程准则见 `docs/engineering.md`。

### Standards

确认违规 **0 项**。关键判断：

- `Invocation` 持有 `const Binding&`，不是每次深拷贝成员目录。目录/元数据准备在冷端；实际参数、活身份和代际仍须逐次核验。
- `Version` 保存自己代码的布局。强槽超过 2、存在观察槽、原生实参超过 8 时分别准备相应 vector；不用九参方法的源码不会仅因目录含该方法而扩展参数缓冲。容量不会在 `Invocation` 销毁后跨版本留存。
- 双重代码持有、结果暂存、显式清根和边界校验有语义责任；不能在没有测量前以“有 shared_ptr/variant/检查”为理由删除。
- 两个 512 字节诊断缓冲的源级初始化、统计更新和清理扫描是待测成本，不是已证瓶颈；优化器最终保留多少工作尚未通过机器码或计时确认。

依据：`acceptance/jit/business.cpp:172-210,512-555,724-734,798-824`、`src/runtime/execution.hpp`。

### Spec

历史确认 **1 项 P2，当时已复现、未修复**：`ABI-RESULT-DIAGNOSTIC`；现已通过生成失败原因与服务状态分离完成修复。

替换的 `BusinessServices::call` 在第二次调用声称成功，却漏写非 void 结果或写错种类时，生成代码正确拒绝结果、保留调用方哨兵并显式清根，但最终报错文字为空。

- 根因：`business_backend.cpp:351-355` 只将状态 1 送入失败清理；`atelier_invoke_business` 清空诊断后直接返回生成状态（708-738）；宿主在 `Invocation::failed == false` 时采用仍为空的后端诊断（`business.cpp:547-551`）。
- 范围：仅防御性 ABI 回调违约路径。标准 `call_method → Runtime::call` 会保留非空首因，不受影响。没有证据表明正常成功结果、输出保留或根清理被破坏。
- 本轮实际证据：开发/发布、原生/描述型、漏写/错种类共 8 组均复现；4 组标准原生错误对照保留原始原因。
- 修复验收：新增可识别“结果契约违约”的非空诊断，不能覆盖更早的原生错误；同时保留非零状态、输出哨兵不变、RAII 前显式清根及双配置验证。不可只在宿主末端补一个不说明来源的通用错误字符串。

该问题不阻断标准服务的成功调用基线，但错误路径不得伪装成成功吞吐样本。本轮只记录缺陷，不宣称修复或零问题通过。

### 测量准备警告

历史 `out/review/source-pipeline-probe.cpp` 仍使用旧 `bind_machine/execute/MethodBinding` 接口；旁边的历史 `business.cpp` 副本、旧 ABI 对象文件及旧吞吐数字都不能直接复用。这是测量资产过期，不是新增生产缺陷。只借用其分段、回收和交错测量方法。

## 2. 本轮已经运行的证据

驱动为 `out/review/call-performance-planning/review-probe.cpp`，直接包含当前 `acceptance/jit/business.cpp`，复用同一 TU 的真实入口与 fixtures，不另造 Runtime、解释器或 mock。编译/链接参数从当前 CMake 生成结果派生，保留各配置的 CRT、mimalloc、正式 DLL 和代码保活机制。

| 定向检查 | 每配置次数 | 实际覆盖 |
| --- | ---: | --- |
| 纯标量工具规则 | 3 | Entry / Lease / raw，输出 9，0 对象/0 根 |
| 机器推进及快照 | 6 | 两种对象 × 三入口；接收者和快照字段、5 次 poll、根交接 |
| 现有九参 UI 控制 | 6 | 两种对象 × 三入口；输出 9、全部字段、实际参数需求 9 |
| 原生嵌套返回控制 | 6 | 两种对象 × 三入口；返回 1129、顺序编码 123 |
| UI 选择提交决策 | 12 | 两种对象 × 三入口 × 两分支；输出 1.5/7.5、字段、1 次 poll、实际参数需求 4 |
| 临时接收者＋九参＋末参失败 | 2 | 外层不调用、部分副作用保留、首因及哨兵保留、显式清根、临时对象可回收 |
| ABI 结果违约诊断复现 | 4 | 故障被拒绝，但两层错误文字为空 |
| 标准原生错误对照 | 2 | 首因非空且不被覆盖 |

开发和发布配置各 **33 次成功场景核对**，共 **66 次**；另有 4 次组合失败保护检查、8 次诊断缺陷复现和 4 次标准错误对照。两个定向驱动退出码均为 0。

没有重新运行完整 acceptance 套件；前轮完整套件 PASS 只作为历史证据。本轮构建当前依赖并运行以上新增定向驱动。没有任何计时或吞吐采样。

完整机器可读证据与日志：

- `out/review/call-performance-planning/review-evidence.json`
- `out/review/call-performance-planning/dev-smoke.log`
- `out/review/call-performance-planning/release-smoke.log`
- `out/review/call-performance-planning/build-dev.json`、`build-release.json`

重放命令（项目根目录）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File out/review/call-performance-planning/run-smoke.ps1 -Configuration dev
powershell.exe -NoProfile -ExecutionPolicy Bypass -File out/review/call-performance-planning/run-smoke.ps1 -Configuration release
```

## 3. 三个主测案例

以下规模、输入比例与种子是明确选择的实验参数，不是玩家遥测或生产负载估计。正式采样前，每档实际输入序列仍需通过独立 oracle；可行性 smoke 不等于穷尽所有参数。

### B1：机器群 tick 与状态快照

真实种子：`acceptance/jit/advance_machine.source`、`acceptance/machine.hpp` 和 `register_machines`。保持既有 `invoke_machine` 的快照输出，不能偷偷删除这项工作后声称同语义加速。

- 首档：1000 个接收者，每个推进 1 步，amount = 0.25。随后分别做“1000 个 × 16 步”和“10000 个 × 1 步”，不立刻展开所有维度组合。
- 首轮全走正分支；零/负 amount 作为独立分支工作集，不混入正分支吞吐。native/described 分组。
- 每个样本计时前将接收者复位到 progress=0、heat=3.5、cycles=0；所有入口重放相同输入，避免长时间 float 累加改变工作负载。
- 独立 oracle：正 amount 推进 S 步后 progress=amount×S、heat=3.5、cycles=S。已实跑小样本 amount=0.25、S=4，接收者及快照均为 1/3.5/4。
- 一次 tick 保存 N 份快照到预分配结果槽。计时包含调用、状态变更、快照创建/同步/根交接及输出入槽；计时外核验每个接收者与快照。
- 随后另计 N 份输出根释放与完整 GC 尾部，最终根数/存活数回到 N。每样本都回收，不能堆积无根快照制造伪“稳态”。报告调用、根释放、回收三部分及可相加的总成本，oracle/日志不计入该总数。
- 源码推导的成功工作量：每对象 S 次 native advance、S+1 次 poll、S+2 次三字段快照同步。计数运行核对后才可标成实测。
- 若另做 snapshot=false，只能命名为不同的宿主输出策略，单列成本；它不是 B1 的替代品或同语义提速证据。

### B2：工具/界面的纯标量选择规则

已验证源码：

```source
uint clamp_choice(uint current, uint delta, uint maximum) {
    uint next = current + delta;
    if (next > maximum) { return maximum; }
    return next;
}
```

- 无接收者、原生方法、对象或快照；测完整已发布入口仍剩下多少固定成本。
- 首档每批 10000 次，随后仅升到 100000 次；固定运行时输入数组，截断/不截断各半，序列和 seed 记录到输出。
- 业务样本限定加法不溢出，不能把上述源码称为饱和加法。uint 回绕另作语言边界控制。
- 已实跑 `(7,4,9) → 9`；正式输入集另包含 `(2,3,9) → 5` 等不截断分支，采样前逐项验证，不能只用一个恒定结果。
- 预分配输出数组，在计时外核对全部结果及校验和；源码预期每次 1 次 poll、0 native 调用、0 登记根，批前后对象/根均为 0。
- 不用批量均值的 P95 冒充单次调用 P95。过短调用只报告批量吞吐和摊销均值，不人为减掉一个空循环后声称得到“纯调用纳秒数”。

### B3：UI 选择提交、原生结果与决策

复用当前 `TypedUiController` 的真实字段/方法，固定使用已经运行验证的 **float submit 决策版**，不再在写报告时换成未运行的其它组合：

```source
float submit(float alpha, uint choice, float offset, uint repeats) {
    self.reset_focus();
    self.apply_selection(self.scale(alpha), choice, offset, repeats);
    if (self.bump(1) > 3) {
        return self.scale(2.0) + self.cursor;
    }
    return self.scale(1.0);
}
```

- 首档 1000 个控件对象，每批各提交一次；再单独扩至 10000 个对象。native/described 分组。
- alpha=0.5、choice=7、offset=1.25 时：repeats=1 返回1.5，字段为 opacity1.5/cursor3.25/selected7/revisions3；repeats=2 返回7.5，字段为1.5/4.5/7/4。两组 oracle 已通过三入口、双表示、双配置。
- 首先分别测两分支，再用固定交错输入测混合分支。后续变动参数来自预生成输入，不在热循环生成随机数。
- `reset_focus` 属于业务源码，保留在计时内；不能为了匹配初态挪到计时外。其余 oracle 和输出检查在外。
- 每调用 5 次 native 方法、1 次 poll、实际最大实参数量 4，无快照。目录仍含九参方法，不能因此按 9 准备此版本的参数缓冲。
- 批前后只有 N 个接收者根/对象；同时核对返回标量和全部四个字段，不能只验一个校验和。
- 这是现有验收类型上的 UI 逻辑模型，不含渲染、真实控件树或事件系统，不称为完整 UI 帧时间。

### 仅作控制的用例

九参 `paint`、返回1129/顺序编码123的 `nested`、临时 receiver 的失败组合用于保护类型/缓冲/求值/根契约。它们不与上述三个业务样本混成一项吞吐，也不冒充产品数据。

## 4. 对照组的可比条件

| 路径 | 固定条件与含义 |
| --- | --- |
| Entry（主结果） | 每次选择当前版本；计时窗口禁止发布 |
| Lease（单因素） | 与 Entry 指向同一个已发布版本；只在上述冻结窗口比较入口选择成本，不宣称能普遍替换 Entry |
| raw `execute_typed`（独立参考列） | 使用当前源码/目录编译，保留完整入口校验；与发布缓存路径不是同一个信任边界 |
| native / described | 各自报告；描述型回调含动态字段查找，差额不能全部归因于 JIT 或内存布局 |

首轮不需要新增 C++ 对照框架。将来如加“同协议 C++ 编排”，必须保留 Runtime::call/read/write、ExecutionScope/Frame、相同根、轮询、快照和错误语义；否则只能标为不同语义的算法计算下限。

不得将 `machine_address()` 观察到的地址带出原生借用回调后解引用。C++ oracle 对嵌套调用显式写 first、next 临时变量，再调用 combine，不依赖 C++ 实参求值顺序。

## 5. 固定采样协议

1. **先 oracle 后计时**：冻结源码、服务表、输入数组、构建和 DLL；每个数据档先核对状态、结果、根、GC初态和版本。错误状态立即使该样本无效，不继续累计“成功调用数”。
2. **只用 Release 评价速度**：开发配置负责正确性。保存工具链锁文件、实际编译/链接参数、二进制摘要、机器/OS/CPU、电源/亲和性设置、后台负载说明，不从旧会话抄机器条件；不自动更改用户系统设置。
3. **冷端分开**：request/输入复制、prepare编译链接物化、try_publish、首次调用分别记录。不得只测 prepare 就称为所有编译准备成本；bind和目录准备不混进稳态。
4. **先小批试跑**：固定顺序和公开 seed（初始采用 `0xC4112026`）。同版的 Entry/Lease/raw 以 E-L-R、L-R-E、R-E-L 轮转，重放同一输入和对象初态。原生/描述型不混池。
5. **预热与重复**：初始3批预热、21批记录，每组在3个串行新进程中重复。若预热仍有明显趋势，延长并记录，不把“3批”当作已经稳定的证明。测量进程不互相并行。
6. **预分配**：输入、结果、快照根容器、时间戳数组在计时前准备容量；输出写入预分配槽仍计入 tick。禁止计时内日志、随机数生成、oracle遍历、排序或容器扩容。
7. **计时器**：主墙钟采用 QPC，启动时缓存频率，保存原始 tick 差和频率；先量化计时调用开销。批长须使该开销相对可接受，同时限制快照存量。线程周期仅作独立辅助指标，不能换算成 elapsed time；不把 QPC tick 当 CPU cycle。
8. **统计单位明确**：B1/B3报告整 tick 或事件批的耗时分布，B2报告批量吞吐与摊销均值；同时保留每批原始数据。P50/P95采用 nearest-rank，写明样本数及单位。少量批次不报告生产 P99 保证；不同进程分别统计后展示差异，不能将跨进程样本伪装成一条连续运行轨迹。
9. **不删慢样本**：慢不是剔除理由。记录已知环境干扰；若重跑，保留原组和重跑原因。收益小于观察到的跨轮次波动时只写“未证实”。
10. **不换安全语义换速度**：相同服务表与hook策略。`statistics=nullptr`不关闭现有统计或清理检查，应包含并注明。重型计数/追踪另跑解释性试验，不能一组有追踪、另一组没有而作速度比。

Windows QPC 的用途及频率缓存依据 Microsoft 文档；线程周期不转换为时间依据 QueryThreadCycleTime 文档。预热/重复和防编译器消除仅借鉴 Google Benchmark 的方法，不引入该库。运行时输入及可观察输出仍是防常量折叠的前提，不把 DoNotOptimize 当作禁止全部优化的保证。

## 6. GC、快照与历史峰值

首轮正式成功基线在 GC idle、无待处理请求、无发布时运行。每个比较组使用独立 Runtime，或证明初态/预热/根峰值一致；不要先跑高峰值验收再复用同一个 Runtime 测小调用。

- `Version::invoke` 当前固定 `request_collection=false`、预算 `{1}`；Entry/Lease没有 raw 的直接请求参数，不能只给 raw 传 true 来冒充相同负载。
- 后续 GC 对照统一在计时前启动相同阶段的周期，或用同样安全点 hook 请求；两组均保留相同 hook 开销。分别记录 idle/requested/paused 状态、work、reclaimed、期末 pending。
- 机器 N 份快照校验后统一撤根，再做独立 GC drain。初次可复用现有 `collect(runtime)` 的预算1，必须记录 drain预算及宿主step调用次数；其成本不能被表述为脱离驱动的收集器极限吞吐。
- 普通标量/UI没有快照消费才使用 snapshot=false。快照策略、轮询数或结果存活期改变时另列场景，不混入同语义比较。
- 既有根登记表会保留历史峰值并在相应 GC 阶段扫描；本轮未修改该策略。单次vector析构不意味着所有 Runtime/分配器历史容量都归零。

## 7. 输出与归因

每个原始记录至少保存：case、源码/二进制hash、配置、进程/轮次、输入seed/hash、对象表示、入口路径、版本、对象/调用数量、steps、快照策略、GC初末态、计时范围、QPC差/频率、错误数、oracle结果、根/对象初末数。

另列而不混淆：

- request/prepare/publish/first_call/steady_tick/output_release/gc_drain 的分段耗时。
- 当前可取的 ExecutionStats：polls/completed_cycles/reclaimed/entered/explicit_cleanup。
- 源码推导的 expected native/字段操作数与独立计数运行的 observed 数，字段命名明确区分。
- `layout.strong_slots/weak_slots/max_call_arguments`、扩展参数/根槽需求，以及实测进程私有内存、回收后的保留量。
- 首轮尚无可靠逐调用堆分配计数器；未测字段写 null/未测。不能用vector阈值或PrivateBytes差值假装测到了分配次数，也不能用内存增量直接断言泄漏。

第一张结果表应回答：一次业务 tick/事件批花多少时间、工作量是什么、入口选择占比是否值得进一步调查、快照及回收尾部是否主导。源码级成本账本不能直接变成百分比归因；需要匹配的单因素实验或profile支持。

## 8. 第二阶段与停止条件

第一阶段先完成三个主测的同路径 Release 基线，再逐项扩规模。GC阶段/根峰值，以及发布/退休尾延迟属于第二阶段，不与所有首轮维度做笛卡尔积。

热更新沿用现有共同声明与旧 Lease，分别量化 prepare、try_publish、第一次新调用、稳定期和最后持有者释放；busy作为独立状态，不计作成功发布。源码版本、旧Lease结果、旧页/展开表退休仍须核验。相同地址的后续复用不能被误判为旧代码未释放。

下列情况立即停止对应测量并保留失败工件：源码/ABI/构建不匹配，非零调用状态，任一结果或副作用不符，输出哨兵被失败覆盖，根未清理，快照缺失，GC初态/版本不符，或旧Lease改指新版本。ABI结果契约违约同样按失败退出，不得继续计入成功吞吐。

不作为本轮前置工作：池化、删除边界检查、改 shared_ptr、typed-native快路径、全局函数管理器、通用benchmark框架、语言扩展、生产框架搬迁或提交/推送。

## 9. 实施入口与当前剩余项

复用当前 `business.cpp` 的 register/bind/compile/Entry/Lease/helpers，从当前 CMake 目标派生外置驱动；不能重复编入其对象或原 main。现有驱动只用于复现与oracle；正式计时应新增独立运行模式/驱动，不把正确性场景混入热测窗口。

顺序：三案例输入集oracle → 少量Release样本检查计时边界 → 固定协议完整采集 → 单因素归因 → 再决定优化。每一项结果均保留可重放原始数据。

仍未完成、且未冒充已完成的事项：正式计时驱动；新Release吞吐/延迟数据；可靠的逐调用分配计数。前三业务模型与所需当前接口已经由本轮定向执行证实可用。

### 公开方法资料

- Microsoft，Acquiring high-resolution time stamps：`https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps`
- Microsoft，QueryThreadCycleTime：`https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime`
- Google Benchmark，User Guide（预热、重复、防优化章节）：`https://google.github.io/benchmark/user_guide.html`
