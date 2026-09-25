# 近期 GC 路线：非移动对象前提下的筛选

核查日期：2026-09-21。范围是自有静态语言、JIT 与 C++ 原生共享对象身份，句柄带代际，并能在 Tick/stage 安全点推进的运行时。以下是研究笔记，不是既定设计。

## 结论与排序

1. **CDPT（PLDI 2026）**最贴近“精确根 + 阶段协议 + 自动收环”：只数根、堆内边靠 tracing，并以 phase barrier 协调根发布；但它是 Rust 非阻塞数据结构库，未以游戏/JIT/C++ 互操作或帧预算评测。
2. **Nofl / Whippet（arXiv 2025）**是严格可选的非移动 Immix：默认可原地标记、精确回收对象间洞；但论文实现是完整 STW，lazy sweep 会把工作转嫁给后续分配。
3. **CIRC（PLDI 2024）**最直接可拆作并发 RC 基础：非移动、epoch/SMR 协作和递归及时回收，可作为“跨线程/原生发布的强拥有句柄”层的候选；循环仍要弱边或另一套 cycle collector。

LXR 是重要的反例与可拆机制来源，**不是本项目完整方案**：原算法和 2025 延续版都会复制存活对象。其论文吞吐和尾延迟数字不得迁移到“禁移动”的变体，更不能迁移到游戏帧性能。

## 1. CIRC：Concurrent Immediate Reference Counting（PLDI 2024）

一手资料：[PLDI 论文 PDF](https://jhyeon.kim/papers/pldi24.pdf)、[ACM DOI](https://doi.org/10.1145/3656383)、[作者 Rust 实现](https://github.com/kaist-cp/circ)、[可复现实验包](https://doi.org/10.5281/zenodo.10801608)。作者为 Jung、Kim、Parkinson、Kang；发表于 2024 年 6 月。

**论文事实。** CIRC 将安全内存回收（SMR）与 RC 组合：用 epoch 给指针字段和计数附带“最后可达”信息；引用计数归零后能安全地递归处理长链，而非等待一批延迟任务。普通读遍历取得未计数 `Snapshot`，其有效性由 EBR 临界区保护；需长期持有或发布时提升为计数 `Rc`。原子 load/store/CAS 都在该协议内。论文报告 CIRC 内存使用随时间不增长、仅略高于底层 SMR；读多负载接近 EBR，重度竞争且大量 RC 更新的最坏案例在 EBR 的 35% 之内，HP 版本相对对应 CDRC 最多高 55% 吞吐。

评测是两台 Linux 服务器上的非阻塞队列、树、skip list、双向队列等并发数据结构；不是帧循环、游戏对象图、JIT 生成写入或 Windows 客户端。实现明确说明 RC/AtomicRc 形成的环不会自动回收，必须用 `Weak` 断开；因此它只能负责无环拥有关系，或需要配合周期性追踪 cycle collector。没有复制或压实路径，适合稳定对象地址/代际句柄的前提。

**项目推论。** 可把它看成“外部/跨线程强拥有”的候选协议，而不是把每次脚本字段读写都变成原子 RC。统一的引用写入入口可在发布、替换和移除强边时维护计数/epoch；临时遍历由 stage guard 保护。若 Tick 是唯一推进入口，epoch 前进、延迟销毁队列和可回收批次可在那里结算。是否承担所有托管边，要先量写密集 ECS/行为树/消息队列；论文不证明该选择的游戏成本。

## 2. Nofl：Precise Immix / Whippet（2025 arXiv）

一手资料：[论文 HTML](https://arxiv.org/html/2503.16971)、[PDF v1](https://arxiv.org/pdf/2503.16971v1)、[作者 Whippet 源码](https://github.com/wingo/whippet)。Andy Wingo 于 2025-03-21 提交；这是 arXiv 预印本，非已验证的生产 GC。

**论文事实。** Nofl 以每 16 payload bytes 一个 side-table mark byte（6.25% 元数据上界）取代 Immix 的 line mark：可精确找到对象间的任意对齐洞，常规 collection 原地 mark；`mmc` 的 large-object space 是 mark-sweep。它是**可选**搬迁而非必需搬迁：只有 `GC_COLLECTION_COMPACTING` 才准备 evacuation；若不用该 collection kind，或不选 evacuation target，存活对象不移动。论文实际评测的 `mmc` 配置仍包含 optimistic evacuation，因此其结果不等于纯 non-moving 变体的结果。

所有 Whippet collector（包括 mmc）在论文版本都是 STW，trace work 全在 GC pause；它虽提供 cooperative safepoint 和 write-barrier fast path，尚未证明可把标记切进 Tick。sweep 是 lazy：mutator allocation 扫描 mark bytes 才发现洞；最坏会反复扫描长寿命、少洞 blocks，工作从 pause 转为 allocation 延迟。作者已为完全空 block 加 eager sweep，因降低 mutator 找空 block 的成本使所有微基准总时间下降；并把 concurrent sweep、长寿命 “sedimentary” block 规避列为 future work。

评测只是在 Whiffle Scheme-to-C workbench 的六项微基准，Ryzen Threadripper/Linux；多 mutator 由平行独立实例模拟，真实跨线程共享堆行为留作 future work。`mmc` 在 6 个配置中的 5 个紧堆情形最低开销，但 `earley` 紧堆很差甚至超过 90 秒 timeout；论文自己称无法由微基准得出正式结论。没有游戏、Windows、C++ 原生边或帧预算证据。

**项目推论。** Nofl 是“地址稳定 + precise region hole reuse”的具体方案：先固定为 non-compacting collection，移除 forwarding 与 evacuation reserve 后才能满足硬约束。其 side metadata 还能承载 pinned、generation、field-log bits；这很适合统一对象元数据，但大小/对齐、对象描述和原生暴露地址须共同设计。若采 lazy sweep，必须把 allocation slow-path 耗时纳入 Tick 预算；否则需要新的、可切片的 sweep 路径，不能宣称论文已解决。

## 3. CDPT：Concurrent Deferred Partial Tracing（PLDI 2026）

一手资料：[ACM DOI/摘要](https://doi.org/10.1145/3808310)、[作者实现与限制](https://github.com/kaist-cp/cdpt)。作者为 Kim、Park、Kwon、Kang；发表于 2026 年 6 月。

**论文事实。** CDPT 复兴 partial tracing：只为 roots 维护 RC，根可精确识别；从计数非零对象 tracing，因此可自动回收环，堆内遍历不更新计数。CPT 的 phase consensus 协调并发 phase、无需挂起 mutator；进入 phase-critical section 才能获取新 local pointer 和安全改写 shared object。CPT 可在 section 内新建 RC pointer，获得的持有指针可在退出 section 后继续解引用。CDPT 将一部分 root RC 更新换为每线程单写多读、HP-like array 的 atomic store；collector 由 phase barrier 安全并发扫描该精确 root array。论文用 Yuasa deletion barrier 保持 weak tricolor invariant：覆写/移除边时把旧 target 变灰；没有宣称省去 heap-store barrier。论文在并发数据结构微基准和生产缓存库 Moka 宏基准中与 BDWGC、CIRC、RCU/hazard-pointer 比较，报告优于前两类自动方案并接近后两类手工方案。

作者实现是 Rust 库：`Shared`/`AtomicShared` 写入自动执行屏障，遍历必须持有可并存的 guard，`Local` 只在 guard 生命周期有效。它列出无 finalizer、`Send + Sync` 限制和比手工回收更高峰值内存等限制。论文/仓库并未给出游戏对象、Windows、JIT、帧预算或 C++ ABI 的实验。直接资料没有把“可保持任意外部裸地址与现有代际句柄”作为目标；在本项目采用前须审计其对象定位与原生逃逸契约。不能把“无 STW”误读为每个 Tick 工作有硬上界。

**边界与项目推论。** 论文没有给 phase-critical section 长度的上界，也未证明 stalled/长 section 时 collection 进度或内存上界；所以 stage guard 若跨帧或持久保存 `Local`，不能声称仍有相同推进性质。最有价值的是“根与长时拥有”同“stage 内遍历”分层：前者走 RC/发布，后者避免每边 RC 更新；phase barrier 可映射到 Tick/stage。它可能比 LXR 的全堆 RC 更贴近句柄代际，但根精确性、原生根注册、guard 遗漏和跨 stage 保存 `Local` 都必须成为统一运行时契约。先做小型、非移动原型和真实游戏负载测量；不应直接引入其 Rust API 模型。

## LXR：需明确排除的移动部分，以及可拆部分

一手资料：[PLDI 2022 论文](https://www.steveblackburn.org/pubs/papers/lxr-pldi-2022.pdf)、[OOPSLA 2025 产品化论文](https://www.steveblackburn.org/pubs/papers/prod-oopsla-2025.pdf)、[作者 artifact/source 指引](https://github.com/wenyuzhao/lxr-pldi-2022-artifact)、[MMTk 当前状态](https://www.mmtk.io/status)。

**原算法事实。** LXR 是 RC 为主、偶发 SATB 并发追踪收环的 Immix collector；一个 field-logging 写屏障同时服务 coalescing RC、SATB 和 remembered set。Coalescing 的精确定义是：在 epoch `t→t+1` 中，同一 heap pointer field 首次被写时，记录被覆写 referent 以便对 `t` 的 before-image decrement，并记录 field address；epoch 末只对它届时的 after-image referent increment，期间中间值被忽略。根则在 brief STW epoch 边界捕获。这直接对应“同一字段一 stage 多次更新可合并”，但原文合并单位是**GC epoch**，不是游戏 stage，且论文仅测该 barrier 的 mutator overhead 1.6%。它不能推出持久大图、不同更新率或游戏性能。Immix 的 line 回收确实使其“多数”回收无需复制，但每个 RC pause 会复制幸存的年轻对象以去碎片；SATB 完成后还会从碎片 block 复制存活对象。故 LXR 从定义上不是 non-moving，不能称为“只取其非移动实现”。PLDI 评测在 OpenJDK 11 的 Lucene/DaCapo 等 Java 工作负载，非游戏。

**2025 延续事实。** OOPSLA 论文的优化 LXR 仍在 final SATB pause evacuation（复制）碎片 block 的存活对象；它修复生产化缺项并优化 tracing，在 DaCapo Chopin 和一项匿名的 hyperscale Java service 上报告相对 G1 的改进。状态是通过多数内部 JDK 测试、可接收分流 live traffic，但“尚未 serving live traffic”，仍有 JDK 21 rebase 和延迟目标未完成。MMTk 也标为尚未合入 master 的 fork，且框架整体未宣称 production-ready。

**禁移动后的可拆变体。** 可以研究其单写屏障、SATB 可跨多个 RC epoch、懒惰 decrement、survival-rate trigger 和 Immix line 原地释放；必须删除 young copy、evacuation set 与 forwarding/remembered-set 的搬迁职责，改以非移动 sweep/line reuse 对抗碎片。这样会形成新 collector：LXR 的所有论文性能、暂停和碎片结论均不再适用。尤其不要把 Lucene 的 QPS 或 Java 99.99% latency 当作游戏帧预算证据。

## 未取得的证据与下一步

- 未找到 2024–2026 的一手研究，直接在“Windows 游戏 + JIT + C++ 原生统一对象身份 + 显式 Tick 安全点 + 永不移动”这一完整组合上评测；这不是不存在的证明。
- CDPT 的公开论文足够说明 phase/根分层和 section 内外边界，但没有取得其完整 artifact 的逐项定位证明，故没有断言它可原样保持 FrostFoxAtelier 的裸地址/代际句柄；也未取得长 critical section 下推进/内存界的主张。
- 下一步若进入设计，应先确定唯一引用 API（强根、堆内强边、弱边、原生/外部根）和 stage guard，再用实际游戏场景测量 allocation、边写入、环比例、region 局部性与可接受的碎片上限；之后才在 CIRC、CDPT 或 Nofl 的可拆机制间取舍。
