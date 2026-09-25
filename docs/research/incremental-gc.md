# 增量 GC：切片、写屏障与责任边界

核查日期：2026-09-21。范围：UE 的 UObject 增量可达性分析，以及成熟追踪 GC 的并发标记机制；不把本文当作 FrostFoxAtelier 的既定设计。

## 已核实的事实（来源直接证明）

- UE 当前文档把 Incremental Garbage Collection 标为 **Experimental**，并明确要求出货时谨慎使用；因此不能把它表述为已成熟、无条件可用的方案。[Epic：Incremental Garbage Collection](https://dev.epicgames.com/documentation/unreal-engine/incremental-garbage-collection-in-unreal-engine)
- UE 原有 mark-and-sweep 的 reachability analysis 在单帧完成并暂停 UObject 处理；增量模式把此阶段分到多帧，并以 `gc.IncrementalReachabilityTimeLimit` 设置每帧**软**时间限制。它解决的是大对象图可达性扫描造成的单帧尖峰，而非给整个 GC 或游戏 Tick 提供硬实时上界。[Epic：增量可达性分析与配置](https://dev.epicgames.com/documentation/unreal-engine/incremental-garbage-collection-in-unreal-engine)
- Epic 明说限制仍可能超出；并且该可达性分析并非完全线程安全，工作线程操纵对象可能漏标、过早回收，建议只在单线程构建（如专用服务器）启用。[Epic：已知限制](https://dev.epicgames.com/documentation/unreal-engine/incremental-garbage-collection-in-unreal-engine)
- UE 在增量扫描期间通过 `TObjectPtr` 的 `UPROPERTY` 跟踪引用：一次赋值会立即标记被引用对象，这就是 GC 写屏障。项目若仍用原始 C++ 指针承载该类 `UPROPERTY`，文档警告对象可能被过早回收。[Epic：写屏障要求](https://dev.epicgames.com/documentation/unreal-engine/incremental-garbage-collection-in-unreal-engine)
- UE 还可独立启用 `gc.AllowIncrementalGather`；而 `IncrementalPurgeGarbage` 的删除也接受软时间限制，并要求调用方避免在 pending purge 前改变 `RF_Unreachable`。所以“把标记放进 Tick”不自动涵盖 gather、销毁、内存释放或其他停顿源。[Epic：增量 GC 配置](https://dev.epicgames.com/documentation/unreal-engine/incremental-garbage-collection-in-unreal-engine)；[Epic：增量清除 API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/IncrementalPurgeGarbage)
- Go 运行时是成熟的并发三色 mark-sweep，源码说明其在标记阶段启用写屏障；编译器为堆对象的指针字段更新插入屏障。[Go runtime：GC phases](https://go.dev/src/runtime/mgc.go)；[Go runtime：write barrier](https://go.dev/src/runtime/mbarrier.go)
- Go 的官方说明给出原因：mutator 在标记时若让已扫描（黑）对象指向未标记（白）对象，收集器会丢失该对象；写屏障把新可达白对象转灰。最终仍需要短暂停止世界来检查潜在灰对象来源，说明增量/并发并不等于硬实时或零停顿。[Go 官方文章](https://go.dev/blog/go15gc)
- 该模型源自可公开取得的经典并发 GC 工作；其主题即收集器与制造垃圾的进程并发协作。[Dijkstra 等，1978，ACM DOI](https://doi.org/10.1145/359642.359655)；[论文全文 PDF](https://www.cs.tufts.edu/~nr/cs257/archive/edsger-dijkstra/on-the-fly.pdf)

## 对 FrostFoxAtelier 的设计推论（非外部文档承诺）

- Tick 切片应是“在每帧有限工作预算内推进一次 GC 状态机”的统一入口：降低标记长尾对帧时间的冲击；它不证明每帧总成本有上界，因为单个不可分操作、屏障/同步、根扫描、终止确认、sweep/finalizer 和调度延迟都可能超预算。
- 对首版插入屏障方案，**强引用**写入应遵守统一的写入契约，在标记活跃时执行写屏障或记录等价的 dirty/重新扫描信息。可以内联及按证明消除屏障，不要求所有写入经过动态函数调用；未经等效保护的裸字段写入可能破坏“黑对象不指向白对象”不变量。
- 根集合不是例外：注册、移除、替换根，及原生/外部容器发布引用，必须在安全点冻结，或纳入同一屏障/dirty-root 协议；否则新根可在两次切片之间把白对象变为可达而未被发现。
- 弱引用提升为强引用必须先以同一生命周期状态确认目标仍存活，再按强引用写入契约发布；一旦完成不可达判定并提交回收，或进入显式销毁，提升不得把它“复活”。标记尚未结束时暂未标记的对象不等于已提交回收。此责任属于运行时引用 API 和调用者契约，不能由 Tick 切片补救。
- 时间预算应命名为 soft budget，并记录实际耗时与超额原因；只有在把每类临界区、根协议和销毁路径都纳入可证明边界后，才可讨论更强的延迟承诺。
