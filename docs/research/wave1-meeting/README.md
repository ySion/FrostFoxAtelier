# Wave 1 双线研讨会议章程

日期：2026-09-25。状态：研讨与路径实验已完成；结论、证据边界与后续顺序见下列交付入口。本轮不改生产实现。

**后续修复状态**：用户随后授权修复A-F1/C-D1，两处已进入正式实现及验收，开发/发布Runtime、Business、JIT均PASS；见[修复证据](../../../out/review/wave1-fixes/verification.json)与[当前README](../../../README.md)。本目录会议结论及原始baseline/validation保留为修复前的历史快照，不再代表当前源码未修改。

## 交付导航

- [主持裁决与推进顺序](decisions.md)：两个已复现缺陷、性能候选/负结果、并行边界及下一步验收门槛。
- [本机证据与复现入口](evidence.md)：19个实验构建、46次运行、隔离A/B及明确未覆盖项。
- [UI布局完整交接](../../modules/ui-layout/session-handoff.md)与[RHI完整交接](../../modules/rhi/session-handoff.md)：各自六份文档；路径已证实，生产模块尚未实现。
- [内容基线](baseline.json)与[最终文档/证据校验](../../../out/review/wave1-meeting/validation.json)：不把历史验收、新实验和未来模块合同混为一谈。

| 意图 | 探索报告 | 反方报告 |
| --- | --- | --- |
| 类型与生命周期 | [types](types/explore.md) | [types challenge](types/challenge.md) |
| 根与GC | [gc](gc/explore.md) | [gc challenge](gc/challenge.md) |
| 源码与诊断 | [source](source/explore.md) | [source challenge](source/challenge.md) |
| JIT执行与资源 | [execution](execution/explore.md) | [execution challenge](execution/challenge.md) |
| 请求、发布与退休 | [reload](reload/explore.md) | [reload challenge](reload/challenge.md) |

支线取舍与质询：[依赖组合](parallel-modules/map.md)、[RHI](parallel-modules/rhi.md)、[布局](parallel-modules/ui-layout.md)。以下保留本次会议的范围与执行章程。

## 主持职责与边界

- Main 主持主线/支线、证据分级、分歧裁决和最终文档整合，不把各代理意见简单投票或拼接。
- 主线：对当前 Wave 1 按行为意图切块，执行审查 → 问题导向研究 → 优化候选 → 路径证实。功能已验收不等于当前版本性能已定型。
- 支线：分析模块粘黏性和依赖，比较候选，选出 1–2 个能在其他 session 推进的模块；每个入选模块拥有独立文档目录。
- 本次先研究、对抗和验证，不扩展生产功能、不更改已确认核心语义、不升级依赖、不自动提交/部署。需要行为变更的修复列出独立授权/验收门槛。
- 仓库当前没有 .git 或 .codegraph。固定当前文件内容哈希与已有正式验收证据，不编造 diff/commit，不建立索引；版本化语言规范保持不变。

## 主线意图切块

| 编号 | 意图 | 代码范围 | 双人席位 |
|---|---|---|---|
| A | 定义共享类型、布局并履行值/对象生命周期 | src/runtime/runtime.hpp/.cpp、native.hpp | WaveTypesExplore / WaveTypesChallenge |
| B | 保存对象身份、报告引用并分步回收 | runtime.cpp 的 RootState、屏障、trace/collect 与 execution 根协议 | WaveGcExplore / WaveGcChallenge |
| C | 把源码转换为满足明确语义的定型程序与诊断 | src/language/source.hpp/.cpp、LANGUAGE-v0.0.1/v0.0.2 | WaveSourceExplore / WaveSourceChallenge |
| D | 安全执行定型代码、跨宿主 ABI 并管理 Windows 代码资源 | execution.hpp/.cpp、business_backend、windows_unwind、宿主 Invocation/thunk、构建 ABI | WaveExecExplore / WaveExecChallenge |
| E | 使请求、发布、调用和退休遵守版本及寿命规则 | business.cpp 的 BusinessProgram/Entry/Lease/Request/Candidate、Runtime 发布准入 | WaveReloadExplore / WaveReloadChallenge |

## 支线席位

- WaveModuleMap：比较 RHI、UI 布局、平台窗口/输入、资源管线、任务调度等候选的依赖与可独立验收程度，提出 1–2 个模块组合。
- WaveRhi：研究 RHI 候选的最小完整目标、成熟能力复用、后端取舍、GPU 生命周期与业务 Runtime 的关系。
- WaveUi：研究 UI 布局候选的语义、现成布局实现复用、测量/缓存、与文本/渲染/Runtime 的关系。
- 三方必须互相质询。RHI/UI 是候选，不因用户举例就自动通过；不要把全文 UI 系统/渲染器混同于布局/RHI。

## 证据与讨论规则

1. 每位 reviewer 先独立出初稿，再发给指定搭档/支线其他席位；不可等待别人完成才开始。
2. 报告分别列 Spec（是否符合已确认契约）和 Standards（工程规则/可能异味），避免风格意见遮蔽真实缺陷。问题标签为：已证实缺陷、可证伪风险、设计取舍、性能假设、已覆盖/不成立。
3. 每项附当前源位置、完整触发链/反例或成本推导、可能方案 A/B、主张的失效条件；外部研究只用一手文档/源码，注明版本或访问日期、适用边界，不搬运别的运行时语义。
4. 各主线搭档交换最多三个关键争议，至少完成一轮引用证据的质询与回应；在自己的报告记录反方论点、回应、修正及未解决项。没有意见相同即算“对抗完成”的捷径。
5. 不能用多方同意替代实测。路径状态区分：源码证实、外部机制支持、本机实验通过、仅推断、受阻。过去版本的基线不当成当前优化收益。
6. Reviewer 不改 src/acceptance/cmake/工具链，不运行构建、验收、测试、formatter、lint 或压力负载；允许只读研究和写自己的报告/一次性实验源码。所有可执行验证由 Main 统一运行，避免性能测量互相干扰。
7. 每块最多提出三个值得本轮验证的动作，优先足够小的真实入口实验，不建 mock/单元测试框架，不制造另一个类型或执行世界。
8. 代码异味仅是判断线索：命名不明、重复代码、职责放错位置、成组参数、缺少承载真实语义的类型、重复分支、散弹式修改、职责发散、投机抽象、长访问链、纯转发层、拒绝的继承；必须结合项目规则和具体风险，不能按类名/文件行数直接判错。

## 文件所有权与输出

- 主线个人报告：docs/research/wave1-meeting/<types|gc|source|execution|reload>/<explore|challenge>.md。
- 支线个人报告：docs/research/wave1-meeting/parallel-modules/<map|rhi|ui-layout>.md。
- Main 独占本目录 README.md、decisions.md、evidence.md、baseline.json 并负责最终整合。入选后的模块文档分别交WaveUi、WaveRhi独占编写，完成后回交Main复核；不是多人同时修改同一份交接稿。
- 一次性实验/原始数据：out/review/wave1-meeting/。实验源码每个作者用自己的编号前缀，报告必须给 Main 精确运行办法、预期可观察结果和反例。
- 每篇个人报告必须可独立阅读：问题/意图 → 当前证据 → Spec/Standards → 候选与反证 → 讨论记录 → 建议路径及验收 → 来源。
- 最终入选模块目录须覆盖目的/非目标、依赖与所有权、第一里程碑、接口方向、风险/决策、验收、研究来源、下一 session 的具体启动入口。未决语义明确列出，不伪装成已冻结 API。

## 当前验收依据

正式构建与当前能力：README.md、docs/engineering.md、docs/runtime-model.md、docs/language-contract.md、CONTEXT.md。

刚完成的兼容方法体更新：out/review/hot-reload-acceptance-evidence.json。开发/发布 Runtime、Business、JIT 均通过；原生/描述型热更新各 11 个隔离场景。这里只重用仍对应当前源码的证据，不无故重复整套验收。

## 用户补充的检索要求

本次会议外部搜索统一使用 Exa；需要阅读全文时使用 Exa 内容获取或直接读取已知一手 URL。此前已核实的一手证据保留，不以聚合摘要代替官方文档/源码。所有 reviewer 已同步此要求。
