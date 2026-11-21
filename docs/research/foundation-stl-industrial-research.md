# 基础库工业依据与选型

资料核对：2026-10-04。本文保留选型依据，不规定迁移排期。当前接口与语言开放边界以[基础库合同](../foundation.md)为准；外部版本用于固定研究证据，项目依赖版本只由[锁文件](../../cmake/dependencies-lock.json)维护。UE 依据为公开官方文档/API，未读取完整引擎源码；外部基准不作为本项目性能证据。

## 标准库与自有基础库

Epic 的规范按设施选择标准库，推荐 atomic、标准 type_traits、initializer_list、numeric_limits 和 cmath；自有实现失去价值时可以迁向标准库。这支持控制业务类型的拥有、失效、错误和语言接线，而不是为消除 `std::` 建立包装。[Epic C++ 规范](https://dev.epicgames.com/documentation/en-us/unreal-engine/epic-cplusplus-coding-standard-for-unreal-engine#useofstandardlibraries)

EASTL 的设计重点是分配控制、扩展容器及游戏优化。固定版本 3.27.01 的 CMake 编译 `.cpp` 并依赖 EABase，并非无依赖的头文件库，allocator 与 std 也不直接兼容。项目不整体引入第二套基础库，EASTL 作为机制参照；其早期速度宣称不能代替 C++23 下的本项目测量。[设计](https://github.com/electronicarts/EASTL/blob/3.27.01/doc/Design.md)、[构建](https://github.com/electronicarts/EASTL/blob/3.27.01/CMakeLists.txt)、[FAQ](https://github.com/electronicarts/EASTL/blob/master/doc/FAQ.md)、[许可证](https://github.com/electronicarts/EASTL/blob/master/LICENSE)

LLVM 的 SmallVector/SmallString 用对象内空间减少小负载分配，也增加每个实例体积；文档更推荐临时栈缓冲。StringRef/ArrayRef 借用数据，不延长寿命，也不把接口绑定到某个内联容量。内联存储的固定成本应由实际长度分布决定。[LLVM 手册](https://llvm.org/docs/ProgrammersManual.html#picking-the-right-data-structure-for-a-task)

## 不能直接移植的类型前提

UE TArray 拥有元素并深复制，文档假定元素可按字节重定位；TMap/TSet 的稀疏存储不保证顺序或连续布局，增长仍可能使借用失效。这些是 UE 自己的类型前提，不授权本项目对任意 C++ 类型做 memcpy。[TArray](https://dev.epicgames.com/documentation/en-us/unreal-engine/array-containers-in-unreal-engine)、[TMap](https://dev.epicgames.com/documentation/en-us/unreal-engine/map-containers-in-unreal-engine)、[TSet](https://dev.epicgames.com/documentation/en-us/unreal-engine/set-containers-in-unreal-engine)

Array 的字节路径按实际选中的构造/赋值表达式判断平凡性；非平凡元素保留真实构析。Array<String> 复制赋值构造副本后替换，换取失败时原值不变，代价可能是比复用目标元素更多的分配；不能以优化之名默默改成部分提交。[Array 安全边界与采样](array-storage-performance.md)

WG21 的 2026-04 P4197 记录 trivial relocation 从 C++26 移除，并讨论 C++29 设计。前沿提案不能代替当前 C++23 的生命周期保证。[P4197R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p4197r0.html)

## Map/Set 后端选择

项目采用 ankerl::unordered_dense。研究固定的 v5.3.0 于 2026-10-03 发布，5.x 使用分组指纹索引，不能沿用 4.x 的 Robin Hood/backward-shift 描述。[固定发布](https://github.com/martinus/unordered_dense/releases/tag/v5.3.0)、[设计](https://github.com/martinus/unordered_dense/blob/v5.3.0/doc/design.md)、[迁移说明](https://github.com/martinus/unordered_dense/blob/v5.3.0/doc/upgrading-to-5.md)

| 候选 | 有关的存储与失败取舍 |
| --- | --- |
后续评估覆盖分组探测、小表成本、Runtime 字段点查与网格焊点。项目已接入复用组掩码的四项小表，大表继续使用同一套 ankerl 索引。[原生哈希表研究与接入结果](native-hash-table.md#64-b-小表的接入结果)

| ankerl dense | 连续值区利于遍历；查询多一次值索引访问，删除搬末尾值并重建索引 |
| Boost unordered_flat_map | 值直接放槽内，删除不搬其他值，rehash 会搬移；适合针对查询/小表成本做对照 |
| Boost unordered_node_map | 独立节点提供地址稳定性，代价是节点分配和间接访问 |
| Abseil SwissTable | flat/node 分开；固定 flat_hash_map 头明确不提供异常安全，不能直接承担项目的失败合同 |
| Folly F14 | Value/Vector/Node 存储及 tiny-table 优化可作参照；仅为 Map 引入完整依赖缺少收益证据 |

来源：[Boost flat 合同](https://www.boost.org/doc/libs/1_88_0/libs/unordered/doc/html/unordered/reference/unordered_flat_map.html)、[Boost 存储设计](https://github.com/boostorg/unordered/blob/boost-1.88.0/doc/modules/ROOT/pages/structures.adoc)、[Abseil 固定头](https://github.com/abseil/abseil-cpp/blob/20260817.0/absl/container/flat_hash_map.h)、[F14 设计](https://github.com/facebook/folly/blob/v2026.09.28.00/folly/container/F14.md)。

选择 dense 的前提是调用方接受结构修改后重新取得借用，不把条目地址或遍历顺序当业务身份。erase 移除旧索引后搬末尾值并重新哈希，因此项目约束元素移动构造/赋值/析构与 Hash/Equal 不抛；这不是上游对任意类型的安全保证。透明 try_emplace 命中时不构造拥有键，但实参表达式已先求值。[固定实现](https://github.com/martinus/unordered_dense/blob/v5.3.0/include/ankerl/unordered_dense.h)

v5.3.0 首次非空索引最少 4 组，每组 88 字节，共约 352 字节，不含值缓冲；这是源码推导，不是本项目测量。空表使用共享 sentinel。小表数量、命中率、条目大小、增删交错、峰值与保留内存都是评估轴；只有真实成本值得时才增加对照，不先维护可切换后端框架。

上游默认 std::allocator/vector 沿项目全局 new/delete 使用 mimalloc。替换私有 value container 需要满足上游协议，单纯改名称没有收益；对外不暴露 vendor 迭代器或桶布局，也不为隐藏模板实现给每张表增加 PImpl 分配。[自定义容器协议](https://github.com/martinus/unordered_dense/blob/v5.3.0/doc/usage.md#custom-container-types)

AssetBrowser 查询前仍可能拼接前缀，透明查询不等于整条路径零分配。目录测量须滚遍中间 viewport，让键表积累完整工作集；头尾可见项验收不能证明测过整个目录。哈希值与表顺序不用于持久身份，确定输出显式排序；普通快速哈希也不自动抵抗恶意碰撞。

## 文本与语言接线的边界

## 跨追加的元素地址

[JoinGroup](../../modules/core/foundation/src/tasks.cpp) 把闭包的地址交给 oneTBB。提交线程可以继续追加；join 等待全部任务退出，再在提交线程析构捕获。[World 行为绑定](../../modules/engine/world/src/execution.cpp)也借出元素地址，成员和 tick 计划缓存 `Behavior*`。注册新类型时可以重新解析已有绑定，失败时只撤销尾部候选；同类型重绑定就地更新，tick 直接使用已解析的指针。两处的 `std::deque` 都满足各自的借用期。仓内 Scatter、Volume 和 Source 并行区间使用 `parallel_for`，当前直接调用 `JoinGroup::run` 的是 Foundation 验收。

RHI 的准备队列将 `shared_ptr<PreparationJob>` 移交给 worker，对象寿命由共享所有权延长。Runtime 的类型先由候选目录持有，再把 `unique_ptr<Type>` 移入发布目录；这一过程还要求跨目录转移所有权。这两类消费者都允许句柄槽位搬移。[RHI 准备队列](../../modules/render/rhi/src/rhi.cpp) · [Runtime 类型发布](../../modules/execution/runtime/src/runtime.cpp)

在锁定的 Windows x64 Release 构建中，以 `c8054269` 的 JoinGroup 为基线，与现有 unordered_dense 依赖的 `segmented_vector` 对照，每段放 8 个闭包。两版都在 join 时释放全部闭包和容器存储。每组任务各自写一个独立缓存行；每个规模预热后执行 65,536 个任务，核对每组结果，新旧交替运行 9 对进程。耗时是每组从构造、提交、join 到校验和析构的均摊时间，表中取 9 次中位数。另用计数分配器记录闭包容器请求的分配次数及峰值字节，仅计元素块与容器元数据的动态分配。

| 每组任务数 | deque 耗时 | 分段耗时 | deque / 分段分配次数 | deque / 分段峰值字节 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1.24 µs | 1.07 µs | 3 / 2 | 144 / 520 |
| 8 | 4.90 µs | 4.59 µs | 11 / 2 | 656 / 520 |
| 256 | 106.42 µs | 115.67 µs | 264 / 38 | 22,480 / 16,640 |

256 项时分段方案在 9 对中有 6 对更慢。单项时它更快，但首段多占 376 字节。当前结论是沿用两个消费者各自的 deque，等实际任务组的规模分布或 World 绑定成本成为瓶颈时再选分段策略。该样本没有测量 World。

地址与迭代位置可分别选型。原生 Map 的 `find` 返回值指针，公开遍历器只存一个条目指针，结构修改使借用失效，容器无需登记并更新活跃遍历器。程序化求值器的 LRU 则在 Entry 中保存链表迭代器：命中后直接 splice，淘汰时直接 erase，两者都省掉一次按节点扫描链表。[Map](../../modules/core/foundation/include/foundation/map.hpp) · [LRU](../../modules/engine/procedural/src/evaluator.cpp)

UE 的 FString、FName、FText 分别承担可修改文本、忽略大小写的驻留标识和本地化语义。名称驻留和本地化不能合并为另一种普通 String。UE 编码概览带有早期平台/UCS-2 描述，不能作为现代能力清单；StringCast 要求正确的源编码类型，普通 char* 不自动表示 UTF-8。[字符串分类](https://dev.epicgames.com/documentation/en-us/unreal-engine/string-handling-in-unreal-engine)、[历史编码概览](https://dev.epicgames.com/documentation/en-us/unreal-engine/character-encoding-in-unreal-engine)、[StringCast](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/StringCast)

项目 String 的布局与校验取舍见[字符串研究](string-storage-performance.md)。UTF-8→UTF-16 转换属于编码/平台边界，转换结果由 UI 持有，不保存源码局部视图，也不给 String 加 UI 方法。simdutf 的验证/转换和 fmt 的 format_to 可作有实测需求时的候选，不构成引入依赖的计划。[Win32 转码](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar)、[simdutf](https://github.com/simdutf/simdutf)、[fmt](https://fmt.dev/latest/api/)

Foundation 的原生能力不自动等于源码开放。描述型元素需要 Runtime 的权威布局与值操作，不能把布局相似的记录强转为 C++ T；Map 的键值关系也不能套连续 sequence_view。含 GC 引用的容器需要根、扫描与写屏障，跨挂起借用需要独立寿命合同，递归容器需要区分内联布局边与间接存储边。相关当前边界由[基础库](../foundation.md#共享反射与-runtime)、[源码类型](../source-type-declarations.md)及 [Runtime](../runtime-model.md)维护。
