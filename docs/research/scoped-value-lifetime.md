# 作用域拥有值与 C# stackalloc 的区别

调研日期：2026-10-02。范围：确定性清理、堆缓冲所有权和同步临时借用；不提出新增语言特性。

用户描述的 String/Array 是 **RAII 作用域拥有值**：值本体可以放在局部栈存储，内部大缓冲仍可以分配在堆上，离开作用域时通过值的析构释放资源。Microsoft 的 C++ 说明正是这种“局部对象拥有堆资源”的模型，并说明局部对象及其成员按生命周期自动销毁。[Microsoft：RAII](https://learn.microsoft.com/en-us/cpp/cpp/object-lifetime-and-resource-management-modern-cpp?view=msvc-170)

## 三个不同问题

| 机制 | 回答的问题 | 与本项目目标的关系 |
| --- | --- | --- |
| C++ RAII | 谁拥有资源，何时结束生命周期？ | 最接近拥有型 String/Array/记录；所有者销毁时释放其资源。[RAII](https://learn.microsoft.com/en-us/cpp/cpp/object-lifetime-and-resource-management-modern-cpp?view=msvc-170) |
| C# `using` / `Dispose` | 如何保证退出路径执行清理？ | 接近编译器插入清理；`return` 和异常仍清理，多个资源逆序释放，编译器改写为 `try/finally`。[using](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/statements/using) |
| C# `stackalloc` | 把一段缓冲分配在哪里？ | 分配的是栈内存块，只支持 unmanaged 元素；方法返回时收回，不能显式提前释放。这不是堆缓冲所有权协议。[stackalloc](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/operators/stackalloc) |

“不依赖 GC”不等于“所有内容在栈上”。RAII 可以清理堆资源；`stackalloc` 的具体承诺是栈缓冲随方法返回收回，也不等同于内层词法块退出立即回收。[RAII](https://learn.microsoft.com/en-us/cpp/cpp/object-lifetime-and-resource-management-modern-cpp?view=msvc-170)、[stackalloc](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/operators/stackalloc)

## 借用可以参考 Span，但不能混同于拥有者

`Span<T>` / `ReadOnlySpan<T>` 包装已有连续存储的引用，存储可以来自托管堆、原生内存或栈；Microsoft 将 owner、consumer 和使用期限分别建模。因此它们适合类比临时可写/只读借用，不能用来解释 String/Array 自身的拥有责任。[Span](https://learn.microsoft.com/en-us/dotnet/api/system.span-1)、[内存使用指南](https://learn.microsoft.com/en-us/dotnet/standard/memory-and-spans/memory-t-usage-guidelines)

C# 的 `ref struct` 编译期规则限制逃逸、装箱、捕获以及跨挂起使用；引用字段不能比被引用的值活得更久。C# 13 已允许在部分 async/iterator 代码中使用它们，故准确的参考点是“借用不能跨越相关挂起边界”，不能笼统说所有 async 函数都禁止 Span。[ref struct](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/builtin-types/ref-struct)

这组规则也不自动提供 Rust 式独占可写借用：Microsoft 的使用指南仍单独要求消费者协调并发访问与使用期限。本项目已有合法只读/可写别名，以及禁止使借用失效的原生调用契约，应沿用这些明确约束。[内存使用指南](https://learn.microsoft.com/en-us/dotnet/standard/memory-and-spans/memory-t-usage-guidelines)、[项目借用契约](../native-value-borrows.md)

## 游戏引擎中的直接参照

Epic 将 `TArray` 定义为拥有元素序列的值类型；复制得到独立数组，销毁数组会销毁元素。若元素是原始指针，被销毁的是指针值，指向对象仍由其他机制管理。因此容器的值生命周期与被引用游戏对象的生命周期需要分别表达。[Epic：Array Containers](https://dev.epicgames.com/documentation/unreal-engine/array-containers-in-unreal-engine)

## 对当前实现的指导

本项目已经由同一 Runtime 提供布局和值操作，编译器负责已构造值的逆序清理；String 短文本沿用 SSO，长文本与 Array 缓冲仍由基础库分配。源码 struct 承载这些字段时，建议复用记录的递归生命周期组合，让字段随记录完成构造、复制、赋值和销毁，保持同一入口与类型身份。这里是基于现有项目契约的实现判断，外部资料没有规定项目的具体接口。[源码拥有值](../source-owned-values.md)

“无 GC 引用”只说明不需要追踪该值内的对象引用；它本身不能证明复制、移动、析构及原生回调能够保持借用有效。现有安全值操作能力和 BorrowSafe 契约仍应一起成立。当前增量保持同步区间，不需要为了字段包含堆缓冲引入任意 C++ 深栈挂起。[源码拥有值契约](../source-owned-values.md#借用与值操作契约)

本次没有需要新增用户决策的语义分歧。未来若要让拥有值跨挂起或让借用返回，需要另行明确存储位置、取消清理和借用寿命；它们不属于本次 struct 字段增量。
