# 显式 GC stage/Tick：实时调度、非移动 mark-region 与根契约

核查日期：2026-09-21。范围是 Windows x64、自有语言与 C++ 原生句柄共享同一
类型/生命周期体系时，显式轮询 `stage`/`Tick` 可作为 GC 机会的边界；本文不是既定设计。
已有并发标记与写屏障的通用结论见 [incremental-gc.md](incremental-gc.md)，这里不讨论
LXR、ZGC 或 Shenandoah。

## 可借鉴的原语

| 原语 | 一手来源与年份 | 适合借什么 | 本项目必须排除/补足什么 |
|---|---|---|---|
| MMU 的周期调度 | [Bacon、Cheng、Rajan，POPL 2003](https://research.ibm.com/publications/a-real-time-garbage-collector-with-low-overhead-and-consistent-utilization--2) | 用“任一长度为 `W` 的窗口中 mutator 至少得到多少 CPU”表达预算；按时间量子交替运行 mutator 与 collector。它比“本帧做 N 个对象”更接近帧抖动目标。 | 论文的 Metronome 是全增量、mostly non-moving 且会动态碎片整理；其有限复制与读屏障路径不适用本项目。只借调度/预算模型。 |
| 非移动 mark-region | [Blackburn、McKinley，PLDI 2008 PDF](https://www.steveblackburn.org/pubs/papers/immix-pldi-2008.pdf) | 固定 block 加细 line；标记后归还完全空的 block/line，在空 line 中连续分配。它是 non-moving tracing 的低碎片、局部性较好基线。 | 原论文的 Immix 还用 opportunistic evacuation/forwarding 来整理碎片；此处明确禁用，不能把该收益或实现当作设计依据。存活对象所在 line 继续占用，碎片是容量规划成本。 |
| 延迟/合并 RC 的 line 回收 | [Shahriyar、Blackburn、Yang、McKinley，OOPSLA 2013 PDF](https://www.steveblackburn.org/pubs/papers/rcix-oopsla-2013.pdf) | 以每对象 RC 配合每 line live-count；live-count 为零即归还 line。延迟 RC 将局部变量变更推迟到周期性根处理，合并同一窗口内字段多次写入。 | RC Immix 的论文以“首次处理新对象”和 backup cycle trace 的复制做碎片整理；本项目不能使用。RC 仍需每次受控写入的计数/dirty 记录，且循环仍须完整追踪。 |

Metronome 的硬实时论证以“collector 和 mutator 每个可调度量子都有上界”为前提，
并把收集器视为周期实时任务；它不是只要调用一次 `Tick` 就成立。论文的实测
45% MMU 和低暂停是其 Jikes RVM、单处理器、可增量化路径下的结果，不能移植成
Windows C++ 的承诺。预算应先以 `soft_budget_ns` 加可计数工作单位（扫描对象、字段、
line、析构/释放项）实现；只有每个不可分操作、调度延迟与所有参与线程的到达时间都有
已验证上界，才可宣称硬实时。

## stage 到底消除了什么

`stage` 是一个可观察的协作点，不天然是 safepoint。它只有在**所有会持有或发布强
句柄的运行时线程**已经按协议到达、其临时根已发布或清空时，才能成为全局 safepoint。
在这个较强契约下，收集器可只枚举权威根表、已发布的 native root frame、全局/静态根和
任务根，而无须异步挂起线程、检查任意 C++ 栈/寄存器或做并发终止握手。统一类型的
`Strong<T>`/原生句柄应因此有一个可注册 root-frame 的唯一入口，而不是让裸地址逃逸。

这减少的是根扫描的**不确定性和同步成本**，不是根集合本身：持久根、root frame、
外部容器和正在调用的 native 帧仍要扫描或显式报告。对象登记表本身不是根集合，只有
登记为持有入口的项参与保活，否则所有对象都会永久存活。若只让主线程的 `Tick`
进入 stage，而 worker 仍可持有/写入句柄，就不能假定这些成本消失；它们必须也到达、
发布可扫描根、或被计入保守保留。全线程参与尚未由需求决定，应作为 GC 契约的待决项。

## 两种执行链

### 1. 全静止 stage 内完成一次回收

1. `GcStage` 等待所有参与 mutator 到协作点，禁止未登记的 strong-handle 发布。
2. 枚举上述根，完成一次 mark；以 block/line sweep-to-region 归还未标记的 region。
3. 执行受限析构/最终化和 native 资源回收，恢复 mutator。

此路径无需“跨 stage 保持三色不变量”的堆写屏障，也无需对象迁移屏障；最适合作为
首个正确性基线。代价是 mark、sweep、终止处理和析构总量都落在同一停顿中，无法仅靠
显式 Tick 把它变成实时。以 block/line 限制单个 sweep 单元只能缩小其中一段的上界。

### 2. 跨 stage 的增量回收

1. 在受控边界建立根快照或开始根枚举，`GcPump(stage, budget)` 推进可暂停的 mark/
   sweep 状态机；每次只完成有界数量的对象字段或 line。
2. mutator 在下一 stage 前继续运行时，所有强引用字段写入都必须走同一写入原语：
   标记活跃则 shade/dirty 新 referent，或记录待重扫来源。根表注册、移除、替换及
   native root-frame 发布也必须采用等价的 root barrier/dirty-root 协议。
3. 终止时重新处理 dirty work 并确认没有待处理标记；仅随后回收未标记 region。循环
   使用 RC 时，另有低频的可切片 backup trace 负责循环，不能省略。

显式 stage 让屏障工作有稳定的排空点，并可把 root 快照集中在边界；它**不能**允许跨
stage 的强引用写入绕过屏障。否则一个已扫描对象（或新根）在两个 Tick 间指向白对象，
该对象可能被错误回收。写屏障可内联、合并或由编译器在证明安全处消除，但语义入口必须
唯一；原生 C++ 容器的直接字段写同样受该契约约束。

## epoch 的恰当位置

region/epoch 可在非移动设计中延后“已判死的 line/block 或句柄槽”再次复用，直到仍可
读取旧发布视图的线程都越过 retire epoch；这可处理并发读取、ABA 和外部批处理的回收
时机。它不计算对象图可达性、不发现循环，也不替代 mark/RC 的写屏障，因此不是把 RCU
当作 GC。全静止路径通常不需这层延后；跨 stage 且允许读者继续运行时才评估它。

## 对 FrostFoxAtelier 的建议

先定义一个 `GcStage` 参与契约和唯一的强句柄写入/root-frame API，再做非移动
block/line mark-region：完整 stage 可作为最小正确实现，`GcPump` 则沿同一状态机在以后
分片。选择“只主线程”还是“所有持句柄线程”前，不应承诺 root 扫描范围或暂停上界。
Metronome 的 MMU 可用来记录/调参每窗口预算；Immix 的 region 分级最值得复用；RC
Immix 只在确有大量短命、非循环对象且可接受写入记账时作为后续候选，不能取代追踪基线。

非抢占 C++ 仍是硬边界：任意长的 native 调用、锁等待、系统调用、析构或不轮询循环会
延迟 safepoint 和 `GcPump`，使时间预算失效。需要硬上界时，必须限制这些段、在其间插入
协作点，并把操作系统调度与资源释放成本纳入测量；否则文档和 API 应只称 soft budget。
