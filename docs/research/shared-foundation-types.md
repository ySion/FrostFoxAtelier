# 共享基础类型与调用契约

状态：第 1 步已获授权并实施，dev/release 端到端验收通过。原生字符串、视图与数组首片已实施，当前合同见[基础库](../foundation.md)；现行调用合同以 [Runtime 协议](../runtime-model.md#共享函数签名与结果)、[自动反射](../reflection.md)及 [LANGUAGE](../../LANGUAGE.md) 为准。

## 调研起点与本次落地

调研基线 `519c072` 的原生反射调用仅支持 `float`、`std::uint32_t` 按值传参，以及对应的标量或 `void` 返回；语言与 JIT 调用桥也采用这个有限集合。`TypeKind` 只有 scalar、record、object。`ValueForm` 描述内联值、强弱引用及引用序列，不能拿它代替 C++ 参数的借用方式。

相关入口是 `src/runtime/native.hpp`、`src/runtime/runtime.hpp`、`src/language/source.hpp`、`src/jit/source_backend.hpp` 和 `src/source/detail/bridge.hpp`。生成器只是这些契约的生产者；放宽宏检查不会自动打通语言和执行链。

现有记录类型已经具备初始化、复制、赋值、销毁及包含对象引用时的追踪机制，应沿这条链扩展。现有 `with_native` 提供受限原生借用范围，但不能自动保证任意动态容器的元素地址稳定。

## 工业依据

资料核对：2026-09-30。以下区分官方事实与项目推断；以下前期论证保留调研时的语境；原生首片的实际取舍见文末。

1. **String 是运行时定义的类型。** .NET `System.String` 是不可变的 UTF-16 码元序列。CoreCLR v10 的 `StringObject` 继承 `Object`，包含长度与字符存储；CoreLib 与运行时共同实现基础类型。这能说明统一语言基础库的做法，不能证明它与 `std::string` 同布局，内部布局也不是跨运行时稳定 ABI。[String 契约](https://learn.microsoft.com/en-us/dotnet/api/system.string?view=net-10.0)、[CoreCLR v10 实现](https://github.com/dotnet/runtime/blob/v10.0.0/src/coreclr/vm/object.h)、[CoreLib](https://github.com/dotnet/runtime/blob/v10.0.0/docs/design/coreclr/botr/corelib.md)

2. **原生调用另有表示与生命周期契约。** P/Invoke 按声明把 string 封送成字符指针等表示；按值传入且指定 UTF-16 等条件时，可以固定字符存储并直接使用。该优化共享的是字符缓冲区，不会把整个托管对象变为 C++ 字符串。结构布局和字段封送同样需要明确配置。[字符串封送](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/default-marshalling-for-strings)、[固定条件](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/best-practices#string-parameters)、[结构封送](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/customize-struct-marshalling)

3. **提前编译不消除对象模型。** `LibraryImport` 在编译期生成封送代码；NativeAOT 提前生成机器码并携带所需运行时。Unity 官方 2015 年 IL2CPP 示例仍使用独立的对象头、长度、UTF-16 字符数组，并展示转换为 `std::string` 与 P/Invoke 缓冲区的过程。因此“生成 C++ 即与 STL 同布局”没有依据；历史 IL2CPP 示例也不能充当今天的稳定 ABI。[LibraryImport](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/pinvoke-source-generation)、[NativeAOT](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)、[IL2CPP 字符串](https://unity.com/blog/engine-platform/il2cpp-internals-debugging-tips-for-generated-code)、[IL2CPP 封送](https://unity.com/blog/engine-platform/il2cpp-internals-pinvoke-wrappers)

4. **Byref 包含别名与读写语义。** C# `ref` 读写调用者已初始化的变量；`out` 要求返回前赋值；`in` 只读且允许临时变量。它们不赋予独占访问。async 方法仍禁止 `ref/in/out` 参数；C# 13 放宽的是不跨 `await/yield` 使用的 ref 局部。由此推断，C++ `T&` 不能自行表达 out，参数传递模式应与类型分别建模。[参数规范](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/keywords/method-parameters)、[C# 13 规范](https://github.com/dotnet/csharplang/blob/main/proposals/csharp-13.0/ref-unsafe-in-iterators-async.md)

5. **托管引用不等于裸地址。** CoreCLR 要向 GC 报告引用及内部指针；可能触发 GC 的原生运行时代码必须保护它们。C# 裸指针不会被 GC 自动追踪，指向托管存储时需要固定对象。共享布局本身不能解决存活、移动或悬空问题。[CoreLib GC 契约](https://github.com/dotnet/runtime/blob/v10.0.0/docs/design/coreclr/botr/corelib.md)、[裸指针规则](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/unsafe-code#pointer-types)

6. **Statepoint 服务于 GC 与编译代码的协作。** LLVM 用它描述安全点处活跃指针、关联基对象及重定位结果；移动后不能继续使用旧 SSA 指针。它不是字符串或 JIT 的必选前提。推断：若采用稳定句柄、显式根和受限借用，可先履行现有生命周期契约；若允许可移动对象裸指针跨 GC 调用存活，必须引入 statepoint 或等效协作机制。非移动 GC 仍需要正确报告根。[Statepoint](https://llvm.org/docs/Statepoints.html)、[LLVM GC 与 shadow stack](https://llvm.org/docs/GarbageCollection.html)

7. **脚本可以直接使用已注册的 C++ 值类型。** AngelScript 的标准字符串扩展直接注册 `std::string`，能够与接收或返回该类型的 C++ 函数配合。一般非平凡值类型需要登记大小、构造、复制及析构等行为；原生按值调用还要满足平台调用约定。它证明共用原生值类型可行，但不会替本项目定义分配所有权、GC 或跨 DLL ABI。[字符串扩展](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_addon_std_string.html)、[值类型注册](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_register_val_type.html)

8. **引擎基础库和反射绑定有成熟实践。** UnrealEngine-AngelScript 使用 Unreal 反射自动绑定 C++ 类型、属性和函数。Unreal 将可变字符串、名称标识和本地化文本分别建模；EASTL 提供面向游戏运行时和工具的容器，官方仓库列出 Frostbite 的使用。这些案例支持控制引擎基础类型，但不意味着现在需要实现完整容器库或复制 Unreal 的所有文本类型。[自动绑定](https://angelscript.hazelight.se/cpp-bindings/automatic-bindings/)、[Unreal 字符串类型](https://dev.epicgames.com/documentation/unreal-engine/string-handling-in-unreal-engine)、[EASTL](https://github.com/electronicarts/EASTL)

本次已增加 i32、独立枚举身份、AENUM、三种参数传递模式及对应语言/JIT 调用。i32/u32 底层枚举、宿主变量和字段别名、只读临时值、错误后的写入、协程局部同步借用及热更新签名检查，由[真实生成消费者](../../acceptance/reflection/calls.cpp)覆盖；该步骤当时未引入 String、StringView 或通用 Array；后续原生首片见文末。首版枚举保留数值别名及未命名底层值；async 签名禁止借用参数。

## 本轮形成的统一模型

同一份类型定义回答“是什么类型”；值布局与生命周期操作回答“如何存放和管理”；函数参数再独立说明“如何传递”。原生注册、反射生成器和语言编译器共同生产这些信息，保留类型化的直接调用路径。

| C++ 表达 | 建议语义 | 约束 |
| --- | --- | --- |
| `int` | 当前 Windows x64 目标映射为有符号 i32 | 编译期检查位宽；`int32_t` 若为同一 C++ 类型，不重复注册身份 |
| `enum class E : int32_t` | 独立枚举类型身份和明确底层表示 | 保留枚举项名称、数值及别名；不自动混同整数 |
| `T` | 按值参数 | 非平凡类型沿用已登记的生命周期操作 |
| `const T&` | 调用期间只读借用 | 可以按语言规则绑定临时值；只读限制作用于此访问路径 |
| `T&` | 调用期间可写借用 | 必须绑定可赋值存储，不能自动解释为 out |
| `StringView` / `Span<T>` | 不拥有存储的视图 | 单独规定有效期与失效条件；首版不允许逃逸 |

### 可写引用的不变量

- `func(x, x)` 的两个参数必须指向同一存储；混合 `const int&` 和 `int&` 时也必须观察到相同的修改。逐参数复制再写回会破坏这个语义，不能作为透明实现。
- 初期仅允许借用跨越同步调用。async 函数参数及保存到协程帧的值不能携带未经管理的借用地址；async 函数内部仍可在一次不挂起的同步调用期间使用借用。
- 局部变量需要可寻址存储；对象字段还要保持所属对象存活并处于合法借用范围。容器元素在借用期间必须禁止导致地址失效的变更，未建立此约束前应拒绝该类借用。
- 原生函数失败前的写入不自动回滚。现有引用批量更新的原子性是专门契约，不能扩展解释为任意函数调用事务。
- 含对象引用的值需要继续履行根和写屏障协议；不可用可写裸槽绕过它们。对象身份引用与参数借用应保持不同概念。

### 基础库的边界

建议形成引擎和语言共用的小型基础库，从标量、枚举、拥有存储的 `String` 与借用视图开始，再按实际需求增加 `Array<T>` 等容器。内部可以复用成熟实现，工具和编译器内部无需全面替换 STL。

共享内存表示有两种可行路径：直接采用已注册的原生值类型，或定义项目控制的公开类型。后一种更方便控制分配、编码、容器失效和版本契约，但增加维护责任。单纯给 `std::string` 起别名不会产生新的布局保证；统一分配器也不等于统一对象表示。

JIT 可以通过权威布局信息分配值槽，通过宿主操作构造和访问非平凡值，无需将 STL 私有布局写死在生成代码里。现有 LLVM 后端隔离 DLL 的边界继续采用受控 ABI，不暴露 STL 对象或跨边界转移其分配所有权。共享表示的承诺限于约定的进程、构建和工具链，不能自动扩展到磁盘格式、网络或任意插件 ABI。

`std::string` 原生 API 是否直接兼容取决于最终选用的实际类型；如果需要适配，必须说明编码、复制和生命周期，尤其不能悄悄把 `std::string&` 变成破坏别名的临时副本。

语言持有字符串和容器后，正常返回、错误退出、取消与协程挂起都必须正确执行值生命周期。`Array<ObjectRef>` 还需接入现有动态引用存储的追踪与写屏障机制，不能建立独立的 GC 世界。

## 后续顺序与待定项

1. **已完成**：明确参数传递模式，打通 i32、`AENUM` 和标量/枚举借用的注册、生成、语言类型检查及 JIT 调用；先覆盖实际业务使用的签名与枚举底层类型。端到端验收包括同一变量多次传参、混合只读/可写别名、非法临时值和跨挂起借用的诊断。
2. 选定 `String` / `StringView` 的契约，贯通非平凡值调用和协程帧清理。验证原生与语言双向调用、非 ASCII 文本、错误与取消路径。
3. 有实际调用场景后引入 `Array<T>` / `Span<T>`，同时完成元素生命周期、对象引用追踪与借用失效约束。按需求增加 map/set 等能力。

原生基础库首片已经明确字符串编码与索引单位、可变性、复制语义及视图有效期，现行合同见[基础库](../foundation.md)。枚举未命名数值已允许，源码 flags 运算尚未提供。未来若支持可跨挂起的借用，需要明确其所有者、追踪和失效机制。


## 对按需自有基础库的判断

用户确认的总体方向是引擎与语言共用自有基础类型，按需实现，不引入 EASTL。这个方向契合本项目控制布局、分配和生命周期的目标。建议将目标限定为小型核心库；编译工具内部现有 STL 不需要随之迁移。

- **Array<T> 与 Span<T>**：优先完成连续存储、容量管理、追加、构造、删除和切片。明确扩容、插入和删除何时使元素地址失效。非平凡元素必须按构造/移动/销毁契约处理，不能把任何 T 都当成可 memcpy 搬迁。UE TArray 是动态、可调整容量的值容器，其文档也强调元素与分配策略。[TArray 官方说明](https://dev.epicgames.com/documentation/en-us/unreal-engine/array-containers-in-unreal-engine)
- **String 与 StringView**：拥有字符的值与借用字符的视图分别表达。可变、拥有存储的 UTF-8 String 是候选；编码单位、结尾零、嵌入零、复制与移动、非法 UTF-8 的处理要明确。字符串可复用适合的缓冲区机制，但不宜直接公开可任意修改字符的 Array 接口而破坏字符串不变量。
- **视图按值传递仍然借用数据**：Qt QAnyStringView 是不拥有存储的字符视图，主要作为按值传递的接口参数；底层字符串必须覆盖视图使用期。故本次参数模式只能解决 T&，后续还需识别 StringView/Span 内部的借用，不能因外层传值就允许无条件保存或跨 await。[Qt QAnyStringView](https://doc.qt.io/qt-6/qanystringview.html)
- **共享布局还需要共享操作**：原生与语言应使用同一已注册值类型及其生命周期机制。泛型容器的每个实例化类型还需权威类型身份；包含对象引用的元素必须接入既有根、追踪和写屏障协议。统一外壳大小不代表这些责任已自动完成。
- **按收益选择优化**：先明确并完成语义与真实消费链，再决定小字符串优化、内联容量、写时复制或特殊分配策略。复用当前分配设施，避免一轮同时重做分配器、容器、Unicode 算法和完整标准库。

自有实现带来控制权，也将异常安全、边界处理、增长策略和非平凡元素的维护责任交给项目。借鉴成熟实现应吸收适用的不变量与算法，保留所复用代码的来源；具体取舍由真实调用场景验证，而不能仅凭“自研”推断更快。

## UE 对照审计：值容器与引用参数

本节核对 Epic 官方文档与公开头 API，只比较相关契约，不把 UE 的实现细节设为本项目规范。

**值容器与擦除存储要分清。** 日常业务数组对应 `TArray<T>`：拥有元素存储，复制生成独立数组，析构时析构剩余元素，并支持移动。若元素本身是指针或共享句柄，复制数组不意味着克隆所指对象。`FString` 提供复制、移动构造和赋值，是可修改的拥有值；它与 .NET 不可变字符串的选择不同。二者都需要真实生命周期操作，不能因反射能识别类型便按若干整数复制。[TArray](https://dev.epicgames.com/documentation/en-us/unreal-engine/array-containers-in-unreal-engine)、[FString API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FString)

若采用 FArray 作为本项目的名字，其业务语义应对标 TArray<T>。`FScriptArray` 是反射层使用的擦除存储；`FScriptArrayHelper` 将数组地址、元素属性、大小和对齐组合起来，并提供构造元素等操作。因此业务模板、擦除存储和反射操作不是三个可互换的业务类型。推断：我们可以共享底层机制，但不应向调用者暴露第二套数组身份。[FScriptArrayHelper](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FScriptArrayHelper)

**视图按值传递，数据仍是借用。** Epic 明确建议 `FStringView` 按值传参；它不拥有字符，不保证以零结尾，底层存储必须覆盖使用期。`TArrayView<T>` 适合只访问现有元素的函数，不负责扩容或所有权；借用期间不能释放或重分配底层数组。`const TArrayView<T>` 只限制视图自身，元素只读应写 `TArrayView<const T>`。同理，`const Array<View<T>>&` 不能据此认定为深层只读；其中视图指向的数据仍取决于元素视图的契约。[TStringView](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TStringView)、[TArrayView](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TArrayView)

**原生引用与蓝图引脚是两层规则。** 原生 `const T&` 限制经该引用修改 T，`T&` 允许读写；二者都不自动取得所有权或保证无别名。Epic 的蓝图规则把普通可写引用呈现为输出引脚，`UPARAM(ref)` 可使其呈现为输入；这不改变 C++ 参数的读写能力。推断：本项目应维持按值、只读借用、可写借用三种模式，不能因借鉴 UE 就把所有 `T&` 猜成 out，也不能把 const 解释为递归冻结对象图。[蓝图官方说明](https://dev.epicgames.com/documentation/en-us/unreal-engine/exposing-gameplay-elements-to-blueprints-visual-scripting-in-unreal-engine)

**不能连同限制一起照抄。** 上述 TArray 文档假定元素可按原始字节搬迁；“搬迁”不等于复制出两个可分别析构的有效值。自引用地址、外部登记等元素需要另外判断。FString API 暴露的是 `TCHAR` 数组，并区分 UTF-8 字符输入；这不替我们决定 UTF-8、索引单位或 Unicode 合法性。不能将 UE 的字符索引直接视作完整 Unicode 字符语义。

**现在应收口的事项（本项目判断）：** 确定拥有值的复制、移动和失败清理；确定视图失效、只读深度及跨挂起限制；确定字符串编码、长度单位、嵌入零与非法输入规则。主代理已核对当前标量/枚举参数模式与 const 语法，因此下一步缺口是非平凡值的调用、局部和结果存储、错误/取消清理，以及含对象引用元素的追踪，不需重新设计引用语法。该审计时点未新增生产实现；后续获授权的原生基础库落地见下一节。

下一阶段建议先贯通拥有型非平凡值及其生命周期，再接入 String、Array 和视图。自有语言固定 `const Type& name` 顺序是本项目的语法选择；原生 C++ 声明由 Clang 归一化，不因 `const T&` 与 `T const&` 的拼写差异建立两种签名。本轮仅完成设计审计。


## Qt/UE 接口取舍与原生基础库首片

资料补查：2026-09-30。以下采用官方合同作为依据，实现为本项目独立编写，未复制上游容器源代码。

- Qt 隐式共享在写入时分离，非 const 遍历可能触发 detach；已有可写迭代器与复制、分离组合还存在特殊失效规则。项目采用独占缓冲、深复制、移动转移与显式 view，避免引入这套额外状态。[Qt 隐式共享](https://doc.qt.io/qt-6/implicit-sharing.html)、[迭代器问题](https://doc.qt.io/qt-6/containers.html#implicit-sharing-iterator-problem)
- UE 区分保序移除与末尾填洞的 RemoveAtSwap；默认允许容量收缩。Qt QList 区分 reserve、clear 和 squeeze。项目保留 `remove_at/remove_at_swap` 的语义区分，删除不自动缩容，容量由 `reserve/clear/squeeze/release` 明确控制。[UE RemoveAtSwap](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TArray/RemoveAtSwap)、[Qt QList](https://doc.qt.io/qt-6/qlist.html)
- QString 按 UTF-16 码元存储，不能把其长度或索引语义原样搬到 UTF-8。Qt 的 UTF-8 视图与多编码视图说明视图的借用性质及码元长度。项目 String 使用有效 UTF-8、尾部零字符并保留嵌入零；StringView 明确按字节切片，外部或切片输入需在建立拥有 String 时校验。[QString](https://doc.qt.io/qt-6/qstring.html)、[QUtf8StringView](https://doc.qt.io/qt-6/qutf8stringview.html)、[编码单位](https://doc.qt.io/qt-6/qanystringview.html#sizes-and-sub-strings)

上述取舍旨在明确所有权与使用成本，尚无比较基准证明其快于 Qt、UE 或 STL。首片实际接入同一 Runtime 的类型、字段、复制及析构机制，反射数组只接受无 GC 引用的值元素。源码容器操作、非平凡值传参及挂起时的值清理仍是后续工作，不由原生类型存在而自动获得。
