# 共享基础类型与调用契约

本文记录参数传递模式与原生 String/Array/视图契约的工业依据和取舍。当前合同见[基础库](../foundation.md)；调用合同以 [Runtime 协议](../runtime-model.md#共享函数签名与结果)、[自动反射](../reflection.md)及 [LANGUAGE](../../LANGUAGE.md) 为准。

## 工业依据

资料核对：2026-09-30。以下区分官方事实与项目推断。

1. **String 是运行时定义的类型。** .NET `System.String` 是不可变的 UTF-16 码元序列。CoreCLR v10 的 `StringObject` 继承 `Object`，包含长度与字符存储；CoreLib 与运行时共同实现基础类型。这能说明统一语言基础库的做法，不能证明它与 `std::string` 同布局，内部布局也不是跨运行时稳定 ABI。[String 契约](https://learn.microsoft.com/en-us/dotnet/api/system.string?view=net-10.0)、[CoreCLR v10 实现](https://github.com/dotnet/runtime/blob/v10.0.0/src/coreclr/vm/object.h)、[CoreLib](https://github.com/dotnet/runtime/blob/v10.0.0/docs/design/coreclr/botr/corelib.md)

2. **原生调用另有表示与生命周期契约。** P/Invoke 按声明把 string 封送成字符指针等表示；按值传入且指定 UTF-16 等条件时，可以固定字符存储并直接使用。该优化共享的是字符缓冲区，不会把整个托管对象变为 C++ 字符串。结构布局和字段封送同样需要明确配置。[字符串封送](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/default-marshalling-for-strings)、[固定条件](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/best-practices#string-parameters)、[结构封送](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/customize-struct-marshalling)

3. **提前编译不消除对象模型。** `LibraryImport` 在编译期生成封送代码；NativeAOT 提前生成机器码并携带所需运行时。Unity 官方 2015 年 IL2CPP 示例仍使用独立的对象头、长度、UTF-16 字符数组，并展示转换为 `std::string` 与 P/Invoke 缓冲区的过程。因此“生成 C++ 即与 STL 同布局”没有依据；历史 IL2CPP 示例也不能充当今天的稳定 ABI。[LibraryImport](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/pinvoke-source-generation)、[NativeAOT](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)、[IL2CPP 字符串](https://unity.com/blog/engine-platform/il2cpp-internals-debugging-tips-for-generated-code)、[IL2CPP 封送](https://unity.com/blog/engine-platform/il2cpp-internals-pinvoke-wrappers)

4. **Byref 包含别名与读写语义。** C# `ref` 读写调用者已初始化的变量；`out` 要求返回前赋值；`in` 只读且允许临时变量。它们不赋予独占访问。async 方法仍禁止 `ref/in/out` 参数；C# 13 放宽的是不跨 `await/yield` 使用的 ref 局部。由此推断，C++ `T&` 不能自行表达 out，参数传递模式应与类型分别建模。[参数规范](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/keywords/method-parameters)、[C# 13 规范](https://github.com/dotnet/csharplang/blob/main/proposals/csharp-13.0/ref-unsafe-in-iterators-async.md)

5. **托管引用不等于裸地址。** CoreCLR 要向 GC 报告引用及内部指针；可能触发 GC 的原生运行时代码必须保护它们。C# 裸指针不会被 GC 自动追踪，指向托管存储时需要固定对象。共享布局本身不能解决存活、移动或悬空问题。[CoreLib GC 契约](https://github.com/dotnet/runtime/blob/v10.0.0/docs/design/coreclr/botr/corelib.md)、[裸指针规则](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/unsafe-code#pointer-types)

6. **Statepoint 服务于 GC 与编译代码的协作。** LLVM 用它描述安全点处活跃指针、关联基对象及重定位结果；移动后不能继续使用旧 SSA 指针。它不是字符串或 JIT 的必选前提。推断：若采用稳定句柄、显式根和受限借用，可先履行现有生命周期契约；若允许可移动对象裸指针跨 GC 调用存活，必须引入 statepoint 或等效协作机制。非移动 GC 仍需要正确报告根。[Statepoint](https://llvm.org/docs/Statepoints.html)、[LLVM GC 与 shadow stack](https://llvm.org/docs/GarbageCollection.html)

7. **脚本可以直接使用已注册的 C++ 值类型。** AngelScript 的标准字符串扩展直接注册 `std::string`，能够与接收或返回该类型的 C++ 函数配合。一般非平凡值类型需要登记大小、构造、复制及析构等行为；原生按值调用还要满足平台调用约定。它证明共用原生值类型可行，但不会替本项目定义分配所有权、GC 或跨 DLL ABI。[字符串扩展](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_addon_std_string.html)、[值类型注册](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_register_val_type.html)

8. **引擎基础库和反射绑定有成熟实践。** UnrealEngine-AngelScript 使用 Unreal 反射自动绑定 C++ 类型、属性和函数。Unreal 将可变字符串、名称标识和本地化文本分别建模；EASTL 提供面向游戏运行时和工具的容器，官方仓库列出 Frostbite 的使用。这些案例支持控制引擎基础类型，但不意味着现在需要实现完整容器库或复制 Unreal 的所有文本类型。[自动绑定](https://angelscript.hazelight.se/cpp-bindings/automatic-bindings/)、[Unreal 字符串类型](https://dev.epicgames.com/documentation/unreal-engine/string-handling-in-unreal-engine)、[EASTL](https://github.com/electronicarts/EASTL)

## 共享表示的取舍

同一份类型定义回答“是什么类型”；值布局与生命周期操作回答“如何存放和管理”；函数参数再独立说明“如何传递”。原生注册、反射生成器和语言编译器共同生产这些信息，保留类型化的直接调用路径。

项目控制 Array/String 的公开类型，以控制分配、编码、失效和版本契约，同时承担维护责任。单纯给 `std::string` 起别名不会产生新的布局保证；统一分配器也不等于统一对象表示。类型本身和 Runtime 中的登记不是两套业务值。

JIT 可以通过权威布局信息分配值槽，通过宿主操作构造和访问非平凡值，无需将 STL 私有布局写死在生成代码里。现有 LLVM 后端隔离 DLL 的边界继续采用受控 ABI，不暴露 STL 对象或跨边界转移其分配所有权。共享表示的承诺限于约定的进程、构建和工具链，不能自动扩展到磁盘格式、网络或任意插件 ABI。

原生适配须保持别名：`func(x, x)` 的两个参数应看到同一存储，逐参数复制再写回不是透明实现。只读引用不代表独占访问，原生函数失败前的写入也不自动回滚。视图按值传递仍借用内部指针；同步借用、跨挂起限制及 GC 引用边界由[原生值借用](../native-value-borrows.md)维护，不能仅凭统一布局绕过根和写屏障。

## UE 对照审计：值容器与引用参数

本节核对 Epic 官方文档与公开头 API，只比较相关契约，不把 UE 的实现细节设为本项目规范。

**值容器与擦除存储要分清。** 日常业务数组对应 `TArray<T>`：拥有元素存储，复制生成独立数组，析构时析构剩余元素，并支持移动。若元素本身是指针或共享句柄，复制数组不意味着克隆所指对象。`FString` 提供复制、移动构造和赋值，是可修改的拥有值；它与 .NET 不可变字符串的选择不同。二者都需要真实生命周期操作，不能因反射能识别类型便按若干整数复制。[TArray](https://dev.epicgames.com/documentation/en-us/unreal-engine/array-containers-in-unreal-engine)、[FString API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FString)

`FScriptArray` 是反射层使用的擦除存储；`FScriptArrayHelper` 将数组地址、元素属性、大小和对齐组合起来，并提供构造元素等操作。因此业务模板、擦除存储和反射操作不是三个可互换的业务类型。它们可以共享机制，但不应向调用者暴露第二套数组身份。[FScriptArrayHelper](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FScriptArrayHelper)

**视图按值传递，数据仍是借用。** Epic 明确建议 `FStringView` 按值传参；它不拥有字符，不保证以零结尾，底层存储必须覆盖使用期。`TArrayView<T>` 适合只访问现有元素的函数，不负责扩容或所有权；借用期间不能释放或重分配底层数组。`const TArrayView<T>` 只限制视图自身，元素只读应写 `TArrayView<const T>`。同理，`const Array<View<T>>&` 不能据此认定为深层只读；其中视图指向的数据仍取决于元素视图的契约。[TStringView](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TStringView)、[TArrayView](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TArrayView)

**原生引用与蓝图引脚是两层规则。** 原生 `const T&` 限制经该引用修改 T，`T&` 允许读写；二者都不自动取得所有权或保证无别名。Epic 的蓝图规则把普通可写引用呈现为输出引脚，`UPARAM(ref)` 可使其呈现为输入；这不改变 C++ 参数的读写能力。推断：本项目应维持按值、只读借用、可写借用三种模式，不能因借鉴 UE 就把所有 `T&` 猜成 out，也不能把 const 解释为递归冻结对象图。[蓝图官方说明](https://dev.epicgames.com/documentation/en-us/unreal-engine/exposing-gameplay-elements-to-blueprints-visual-scripting-in-unreal-engine)

**不能连同限制一起照抄。** 上述 TArray 文档假定元素可按原始字节搬迁；“搬迁”不等于复制出两个可分别析构的有效值。自引用地址、外部登记等元素需要另外判断。FString API 暴露的是 `TCHAR` 数组，并区分 UTF-8 字符输入；这不替我们决定 UTF-8、索引单位或 Unicode 合法性。不能将 UE 的字符索引直接视作完整 Unicode 字符语义。

自有语言固定 `const Type& name` 顺序是本项目的语法选择；原生 C++ 声明由 Clang 归一化，不因 `const T&` 与 `T const&` 的拼写差异建立两种签名。


## Qt/UE 接口取舍

资料补查：2026-09-30。以下采用官方合同作为依据，实现为本项目独立编写，未复制上游容器源代码。

- Qt 隐式共享在写入时分离，非 const 遍历可能触发 detach；已有可写迭代器与复制、分离组合还存在特殊失效规则。项目的 Array/String 保持独占缓冲、深复制和显式 view；共享数据由单独的 SharedArray 及显式 `write()` 表达，不把隐式分离加进普通数组或字符串。[Qt 隐式共享](https://doc.qt.io/qt-6/implicit-sharing.html)、[迭代器问题](https://doc.qt.io/qt-6/containers.html#implicit-sharing-iterator-problem)
- UE 区分保序移除与末尾填洞的 RemoveAtSwap；默认允许容量收缩。Qt QList 区分 reserve、clear 和 squeeze。项目保留 `remove_at/remove_at_swap` 的语义区分，删除不自动缩容，容量由 `reserve/clear/squeeze/release` 明确控制。[UE RemoveAtSwap](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TArray/RemoveAtSwap)、[Qt QList](https://doc.qt.io/qt-6/qlist.html)
- QString 按 UTF-16 码元存储，不能把其长度或索引语义原样搬到 UTF-8。Qt 的 UTF-8 视图与多编码视图说明视图的借用性质及码元长度。项目 String 使用有效 UTF-8、尾部零字符并保留嵌入零；StringView 明确按字节切片，外部或切片输入需在建立拥有 String 时校验。[QString](https://doc.qt.io/qt-6/qstring.html)、[QUtf8StringView](https://doc.qt.io/qt-6/qutf8stringview.html)、[编码单位](https://doc.qt.io/qt-6/qanystringview.html#sizes-and-sub-strings)

上述取舍旨在明确所有权与使用成本，没有比较基准证明其快于 Qt、UE 或 STL。
