# 工业低延迟 GC：Generational ZGC 与 Generational Shenandoah

核查日期：2026-09-21。仅记录截至该日已发布的 OpenJDK/Oracle 一手材料；两者都是 JVM 技术，不是 FrostFoxAtelier 的实现承诺。项目已确定**对象不移动**：ZGC/Shenandoah 在此处是机制参考与排除对照，不是整体候选。

## 已核实的事实

- **状态。**ZGC 在 JDK 15 已成为 product feature；JDK 21 的 JEP 439 引入分代 ZGC，JDK 23 令它成为 ZGC 默认模式，JDK 24 删除非分代模式。因此截至核查日，启用 ZGC 即使用分代 ZGC。[OpenJDK ZGC 历史](https://wiki.openjdk.org/spaces/flyingpdf/pdfpageexport.action?pageId=34668579)；[Oracle JDK 24 迁移说明](https://docs.oracle.com/en/java/javase/24/migrate/significant-changes-jdk-24.html)
- **状态。**Generational Shenandoah 由 JEP 404 以 experimental 进入 JDK 24；JEP 521/JDK 25 移除了 `UnlockExperimentalVMOptions` 要求，但没有改为默认模式。当前用 `-XX:ShenandoahGCMode=generational` 选择，默认仍是单代 `satb`。[OpenJDK CSR/JEP 521](https://bugs.openjdk.org/browse/JDK-8354079)；[OpenJDK 启动参数源码](https://github.com/openjdk/jdk/blob/master/src/java.base/share/man/java.md)
- **ZGC 移动与读屏障。**ZGC 依靠带状态的指针和每次堆引用读取时的 load barrier；慢路径可修复引用并在重定位期间通过转发表得到新地址，使压缩/重定位可与应用并发进行。[OpenJDK/Dev.java 深入说明](https://dev.java/learn/jvm/tool/garbage-collection/zgc-deepdive/)
- **ZGC 分代。**对象页带 young/old 世代属性，并有 `remember` 与 remembered-set 位图入口；这说明分代并非只加年龄计数，还要维护跨代边。[OpenJDK `zPage.hpp`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zPage.hpp)
- **Shenandoah 模型。**默认 `satb` 是并发三阶段 mark-evac-update；分代模式仍是 SATB 并发收集，但为年轻代/老年代加入跨代记录。其维护卡表的公开开发记录显示，dirty-card 清理一度占 init-mark pause 的可观部分，后来目标是移至并发 reset 阶段。[OpenJDK 启动参数源码](https://github.com/openjdk/jdk/blob/master/src/java.base/share/man/java.md)；[OpenJDK 开发讨论](https://mail.openjdk.org/pipermail/shenandoah-dev/2025-March/025644.html)
- **根与暂停。**低暂停不是无暂停：ZGC 仍把阶段协调做成很短的暂停；JEP 376 已将线程栈处理的大部分从 safepoint 移到并发阶段。Shenandoah 文档也说明更大的 root set 会拉长暂停（除非特定版本把部分根处理并发化）。[ZGC 官方介绍](https://inside.java/2023/11/28/gen-zgc-explainer/)；[Shenandoah 官方页](https://wiki.openjdk.org/spaces/flyingpdf/pdfpageexport.action?pageId=25002018)
- **代价。**并发标记、重定位和屏障把 GC 工作转移到 GC 线程及 mutator 热路径；ZGC 官方说明因此损失部分吞吐。更激进的 `ZFragmentationLimit` 会以更多 CPU 换取压缩；空闲页提交/取消提交本身也有成本。[ZGC 官方介绍](https://inside.java/2023/11/28/gen-zgc-explainer/)；[OpenJDK 参数源码](https://github.com/openjdk/jdk/blob/master/src/java.base/share/man/java.md)
- **原生互操作。**JNI 的局部/全局引用由 VM 管理，不能当作恒定对象地址；数组访问可能使用固定或复制，具体行为取决于 VM 与接口。不能把某一种 collector 的 critical-section 行为推广为所有 JVM 都必须停止 GC。[JNI 设计规范](https://docs.oracle.com/en/java/javase/19/docs/specs/jni/design.html)

## 对 FrostFoxAtelier 的设计推论

- 可借鉴：把“每帧推进”限定为显式 GC stage 调度；将根快照/确认、并发或增量标记、选择回收集、回收/析构分开计量。引擎 Tick 是调度机会，不能替代根协议、终止确认或 native 借用协议。
- 可借鉴：语言层所有托管引用访问有统一 codegen/运行时入口时，才可能选择性加入**标记期写屏障**、卡表/remembered set 和 safepoint poll；首版单线程非移动标记—清扫不应预置 ZGC 式 load barrier。
- **nonmoving 的直接收益。**对象地址不变，故无需 ZGC/Shenandoah 为并发搬迁而配置的 forwarding、remapping、对象读屏障和 evacuation reserve；C++ 可持有地址的契约也更简单。这不把稳定 `ObjectId` 当作安全性的替代品：它只是身份，根集合仍须精确可见。
- **nonmoving 仍不能免除。**增量/并发可达性分析中的强引用写入仍要经屏障或等价 dirty 记录；根的注册、替换和外部容器发布仍须安全点或同一协议；若以后并发，GC 线程仍会与游戏线程争用 CPU、缓存和内存带宽。分代优化也仍要维护跨代边，因而卡表/写屏障仍在热路径。
- Windows C++ 引擎适合先提供显式 root handle、外部引用注册和短期 borrow API。pin 是移动式收集器的互操作代价，既然项目 nonmoving，不应反向引入为常规对象 API；若某类外部资源需要排他访问，应以其自身生命周期/锁契约处理。
- 可设定的工程目标是“GC 工作预算可观测、各阶段具备继续执行的位置”；当前项目尚无这些实现或测量，不能据 JVM 方案承诺硬实时或零停顿。
