# Array 存储优化依据

资料与采样日期：2026-09-30，测量基线为 [db6ec54](https://github.com/ySion/FrostFoxAtelier/commit/db6ec54)。本文保留安全边界、实现取舍和历史测量，不代表当前版本重新采样的结果；当前行为见 [array.hpp](../../modules/core/foundation/include/foundation/array.hpp) 与[基础库](../foundation.md)。

## 存储模型

Array 继续独占连续存储，Windows x64 下为指针、大小、容量三个字段，共 24 字节。现有增长是 `capacity + max(capacity / 2, 8)`，再满足本次总长度；reserve 不构造预留元素，clear 和删除保留容量，squeeze 显式收缩。保留检查索引、自身 Span 追加、过对齐分配、move-only 元素，以及构造失败清理。扩容先构造追加后缀，再以 `move_if_noexcept` 处理旧前缀。

## C++23 的安全边界

**事实。** C++23 的 `allocator<T>::allocate` 开始数组对象的生命周期，但不开始每个元素的生命周期。`memcpy/memmove` 在复制前隐式创建目标对象，仅适用于 implicit-lifetime 类型；非 implicit-lifetime 子对象的生命周期不会因此开始。trivially-copyable 两个独立完整对象之间可以复制对象表示；这一许可不能单独回答原始槽位如何开始生命周期。[N4950 allocator](https://github.com/cplusplus/draft/blob/n4950/source/memory.tex#L1550-L1591)、[N4950 implicit creation](https://github.com/cplusplus/draft/blob/n4950/source/basic.tex#L3100-L3114)、[N4950 memcpy](https://github.com/cplusplus/draft/blob/n4950/source/strings.tex#L5204-L5210)、[在线条款](https://eel.is/c++draft/basic.types.trivial)

**实现取舍。** 在 Array 元素约束下，按实际表达式设置保守快路径，而不是给类型增加可自定义的 relocation 标记：

| 操作 | 所需条件，均另要求 `is_trivially_copyable_v<T>` |
| --- | --- |
| 从 `const T&` 构造复制、追加 | `is_trivially_copy_constructible_v<T>` |
| 扩容处理旧元素 | `is_nothrow_move_constructible_v<T> ? is_trivially_move_constructible_v<T> : is_trivially_copy_constructible_v<T>` |
| 复制赋值的已有元素 | `is_trivially_copy_assignable_v<T>` |
| 删除时从 `T&&` 压缩已有元素 | `is_trivially_move_assignable_v<T>` |

平凡且可用的选中复制/移动构造函数提供 implicit-lifetime 的依据；其对应基类、成员操作也必须平凡。Array 已要求平凡或其他不抛异常的析构，符合字节路径的类型具有平凡析构。复制构造与已有槽位赋值仍应分别判断。[N4950 类性质](https://github.com/cplusplus/draft/blob/n4950/source/classes.tex#L283-L290)、[平凡构造条件](https://eel.is/c++draft/class.copy.ctor)、[move_if_noexcept 的准确选择](https://eel.is/c++draft/forward)

**关键反例。** 类型可以具有默认复制构造和带副作用的 `template<class U> T(U&&) noexcept`。它仍可 trivially-copyable，但从 `T&&` 构造会选中模板，不能用 memcpy 跳过。模板赋值同样参与重载；复制路径应明确使用 `const T&`，删除路径检查 `T&&`。这也是上述操作级条件的必要性。[构造与模板、移动声明](https://github.com/cplusplus/draft/blob/n4950/source/classes.tex#L1320-L1426)、[模板赋值参与重载](https://eel.is/c++draft/class.copy.assign)

新旧分配块以及合法自身追加的活元素与末尾空槽互不重叠，可使用 memcpy；有序删除的左右区间重叠，应使用 memmove。零元素提前返回，保留现有长度上限和对齐分配；不把容量槽位当作已构造元素。

## MSVC STL 对照

源码固定于 Microsoft/STL `f023531d8fd0fd668ba9331b349637930611781a`，提交日期 2026-09-22；它是上游快照，不声称等同项目锁定的 MSVC 14.50.35717 安装头文件。

**事实。** vector `_Calculate_growth` 使用旧容量的约 1.5 倍，并检查上限、满足请求大小。复制赋值最终进入 `_Assign_counted_range`；容量充足时给活前缀赋值、构造新增尾部或销毁多余尾部。该逻辑允许容量复用，但在已修改前缀后构造尾部可能失败，不能据此推导普遍强异常保证。单元素末尾扩容按 nothrow-move/copy 分支提供强保证；批量追加源码明确采用不同保证。[vector 固定源码](https://github.com/microsoft/STL/blob/f023531d8fd0fd668ba9331b349637930611781a/stl/inc/vector)

`_Uninitialized_copy/_Uninitialized_move` 仅在迭代器的实际引用表达式可平凡构造、默认 allocator 构造行为等条件成立时走字节路径；否则逐元素构造并由 backout 销毁已完成部分。`_Trivial_cat` 分别判断 bitcopy-constructible 与 bitcopy-assignable，印证操作级判断，不只是查看一个类型标签。[xmemory](https://github.com/microsoft/STL/blob/f023531d8fd0fd668ba9331b349637930611781a/stl/inc/xmemory#L1851-L1988)、[xutility](https://github.com/microsoft/STL/blob/f023531d8fd0fd668ba9331b349637930611781a/stl/inc/xutility#L4841-L4899)

## 容量复用与可观察行为

复制赋值只在上表的平凡构造、平凡赋值条件成立且容量足够时复用缓冲；其他情况先构造完整副本再替换，保留失败时目标不变，并支持可复制但不可赋值的元素。

没有采用“只要复制构造和赋值都不抛就复用”的更宽条件：非平凡元素的复用会把复制构造、旧元素销毁改为复制赋值，这是对象生命周期事件的真实变化。“不抛异常”不等于“无副作用”。

有序删除可在平凡移动赋值分支一次 memmove。`remove_if` 暂保留逐元素的谓词、搬移顺序：闭包可能观察数组中此前已压缩的位置，按连续存活段延后搬移会改变后续谓词观察。没有新契约时不为批量压缩改变调用顺序。

## Unreal 的可借鉴范围

**事实。** Epic 的 UE 5.8 文档明确说明 TArray 假定元素可通过原始字节 trivially relocate；该前提覆盖其自己的值类型体系。RemoveAt 保持顺序；RemoveAtSwap 不保证顺序，文档给出 O(Count) 对 O(ArrayNum) 的区别。两者默认可能按 slack 自动收缩，可用 `EAllowShrinking::No` 或 FNonshrinkingAllocator 禁止。[TArray 模型](https://dev.epicgames.com/documentation/en-us/unreal-engine/array-containers-in-unreal-engine)、[RemoveAt](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TArray/RemoveAt)、[RemoveAtSwap](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/TArray/RemoveAtSwap?lang=en-US)

**取舍。** 借鉴有序删除与尾部交换的操作区分；保持本项目删除不缩容、显式 squeeze 的合同。UE 的 relocatable 前提不等于 C++23 trivially-copyable，不能由此字节搬移 String、智能指针或其他非平凡资源，也不依赖尚未实现的语言 relocation 能力。Array 的独占语义与 SharedArray 的显式共享分开。

这些优化只改变原生存储执行。当前反射 Array 仍拒绝含 GC 引用的值，`Array<ObjectId>` 不自动成为根；持有引用集合继续通过 `ReferenceSequence` 和原有写屏障履行生命周期责任。字节类型特征不能授予 GC 操作权限。[当前基础库合同](../foundation.md)

## 2026-09-30 同机采样

**实际实现。** 只采用上述操作级平凡字节路径和容量复用；非平凡元素继续完整副本替换。`RelocationSource = decltype(std::move_if_noexcept(std::declval<T&>()))` 直接表示现有选择，避免复制一份选择条件。append 与扩容共享后缀先构造、前缀后处理的提交/回退链；预留 emplace 在单个构造成功后提交大小。没有新增分配器层或可自定义 relocation 许可。

**历史采样条件。** Windows x64，AMD Ryzen 9 3950X，正式 release preset、C++23 和项目锁定工具链，全局共享 mimalloc（注：现行 `Array` / `InlineArray` 已进一步升级为直接调用 `mi_malloc_aligned/mi_realloc_aligned/mi_free` 原地扩容，见[基础库](../foundation.md#arrayinlinearrayfixedarray-与-span)）。当时的 `array_measure.cpp` 每场景预热 1000 次、7 个样本取中位数；常规 50000 次，过滤与 String 场景 10000 次。统计页刷新在计时外，消耗结果并检查预热后活堆占用和最终释放。外层批次与输入相同，使用 MSVC std::vector 作为工业对照。当时的性能采样入口已移除，当前 [foundation_acceptance](../../acceptance/foundation/main.cpp) 只验证容器语义与缓冲释放，不复现下表测量。

修改前冻结了原 Array 头文件、采样程序、可执行文件与分配器 DLL，后续保持相同采样程序。先取初始基线，再旧→新及新→旧各做一次顺序采样；没有并行运行计时程序，也没有固定 CPU 核心和频率。下表范围覆盖三个旧版运行和两个新版运行的各自中位数。保守速度比为“旧版最小中位数 / 新版最大中位数”，避免放大其中一次旧版复制/赋值明显偏慢的结果。工作负载是有明确规模的引擎式批次，尚未代表真实游戏帧预算。

| 场景 | 旧版 ns/op 中位数范围 | 新版 ns/op 中位数范围 | 保守速度比 | 分配次数/op 旧 → 新 |
| --- | ---: | ---: | ---: | ---: |
| 4096 整数复制 | 3209.0–10380.5 | 184.7–187.4 | 17.13× | 1 → 1 |
| 4096 整数预留追加 | 3362.9–3602.4 | 156.5–160.9 | 20.90× | 0 → 0 |
| 2048/4096 整数复用赋值 | 2489.1–7906.5 | 130.5–141.1 | 17.64× | 1 → 0 |
| 4096 整数复制后扩容 | 5736.2–5905.9 | 455.8–464.5 | 12.35× | 2 → 2 |
| 4096 整数移除并补回 32 项 | 2598.4–2689.7 | 134.0–135.6 | 19.17× | 0 → 0 |
| 4096 整数追加后过滤 | 7588.1–8327.3 | 2949.2–3004.6 | 2.53× | 0 → 0 |
| 64 项整数增量构造 | 131.3–140.2 | 131.2–145.8 | 0.90× | 6 → 6 |
| 4096 整数值初始化 | 171.5–189.7 | 162.9–169.6 | 1.01× | 1 → 1 |
| 512 个 64 字节数据包复制 | 728.0–796.3 | 586.2–633.1 | 1.15× | 1 → 1 |
| 64 个拥有型 String 预留追加 | 743.5–765.4 | 639.1–670.1 | 1.11× | 32 → 32 |
| 64 个拥有型 String 赋值 | 749.6–845.2 | 686.4–718.8 | 1.04× | 33 → 33 |

该次 MSVC STL 同场景：整数复制 184.2–184.6 ns，预留追加 155.8–162.1 ns，复用赋值 144.3–159.2 ns，移除/补回 32 项 141.0–153.2 ns；Array 的主要批量操作在相近范围。64 项增量构造的两次对照分别略快和约 11% 慢，未确认稳定速度收益；保留原有增长策略，每批仍为 6 次分配，对照 vector 为 12 次。数据包与 String 的收益较小，不作为主要收益依据。

拥有型 String 赋值保留 33 次分配/批次，对照 STL 复用为 0 次，其中位数约 289.5–318.9 ns，快于 Array 的 686.4–718.8 ns。这里保留 Array 的“复制失败不改变目标”保证及原生构析事件；String 复制可能分配失败，没有改为部分提交的逐元素赋值。

容量复用也保留调用者主动预留的内存：该场景 reserve(8192)，新版在赋值后维持 8192 槽位，按 API 推导为 32 KiB 请求缓冲；旧版每次复制替换，最终 4096 槽位对应 16 KiB。调用者可在需要归还容量时显式 squeeze。分配器取整字节、实际分配次数见原始 JSON Lines；所有采样最终活堆恢复基线。

当前功能验收见[基础库](../foundation.md#验收)。上表仅保留历史汇总，不是当前构建的性能承诺；当前没有独立的 Array 性能采样入口。
