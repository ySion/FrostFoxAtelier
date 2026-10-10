# UE-AngelScript 对 FrostFoxAtelier 的借鉴与取舍

研究日期：2026-09-30。本文讨论 Hazelight 的 UnrealEngine-Angelscript 集成，依据其官方文档和实际源码；不把上游 AngelScript 的通用能力当作 UE 分支已经具备的能力。

状态说明：以下比较保留 2026-09-30 的能力基线与建议。其后的同步拥有型 String/Array、词法清理、受控原生算法调用及源码调试核心均已落地，现行实现与验收见[源码拥有型基础值](../source-owned-values.md)和[源码调试](../source-debugger.md)。文中的后续建议不作为当前待办。

## 结论与优先级

最值得借鉴的是：业务对象沿用引擎对象身份和 GC，字符串与容器沿用引擎原生存储，语言编译器补齐原生调用、值生命周期和工具接线。FrostFoxAtelier 已经选择同一个 Runtime 类型体系；后续工作应把这一体系变成业务可以连续使用的语言能力。[对象语义][doc-objects]、[FString 注册][src-string]、[TArray 注册][src-array-registration]

UE-AngelScript 也有 UE 元数据、AS 类型元数据和插件绑定记录；它通过关联和投影让两边理解同一实例、同一存储。不能把这种集成说成两个编译体系完全只有一份元数据，也不能把原生存储说成所有调用都没有复制。[类关联][src-class-binding]、[结构体关联][src-struct-binding]、[调用适配][src-call-context]、[数组复制][src-array-copy]

**建议：下一步优先完成语言局部拥有型 String/Array 的构造与析构，以及受控的原生 leaf 调用，再推进编译器产出的类型定义和小模块函数。** 这会让临时数组、字符串加工、原生游戏算法成为真实端到端路径。多模块依赖图、结构迁移和 JIT 产物缓存应由后续业务需求触发。以下“建议”均是结合本项目现状的设计判断，不是此次已实现功能。

| 阶段 | 建议产出 | 结束条件 |
| --- | --- | --- |
| 现在 | 局部拥有型 String/Array；词法构造、析构；效果已知的原生函数准入 | 一个真实函数能生成和处理临时数组、调用宿主算法，所有返回与错误路径都释放拥有值；借用期间不会发生结构失效 |
| 随后 | 编译器类型定义；小模块多函数；兼容方法体更新；随真实业务接线诊断与工具 | 原生与语言声明共同收集、发布后实例化 Runtime；活跃调用保留旧代码，新调用使用已发布版本 |
| 具体用例出现后 | 多模块依赖闭包、拥有型聚合值的跨帧持有、并发规则、结构迁移、编译缓存 | 分别以真实依赖、悬挂风险、布局变化或启动成本为验收问题 |

## 证据范围与版本

- 官方实现：`Hazelight/UnrealEngine-Angelscript`，分支 `angelscript-master`，固定 commit **`a7890667d1abb289bc2cec16b3da075faf836f2d`**，提交时间 2026-09-30 06:44:10 UTC。[提交][repo-head]
- 官方文档：`Hazelight/Docs-UnrealEngine-Angelscript`，分支 `master`，固定 commit **`3864c72fa3c1ef67413dfaad24417bad01879b4d`**，提交时间 2026-03-23。[文档提交][docs-head]
- 官方语言工具：`Hazelight/vscode-unreal-angelscript`，分支 `master`，固定 commit **`ffc7ab2948589abb04200b7dcedd326eaeaf7a4d`**。[工具源码][src-lsp]
- 本项目比较基线：**`37c34fd` 加首次 native value borrows 实现**，尚未包含随后落地的源码拥有值。本文描述该研究基线，当前能力由语言规范维护。

官网和匿名 GitHub 请求曾出现超时与 404；官方安装文档明确要求 Epic 的 Unreal 源码访问权限。本机已登录账号通过 GitHub CLI 可以合法读取官方源码，因此本次关键实现结论来自上述固定 commit，不依赖第三方镜像。[安装要求][doc-install]

这是一份源码与设计比较，没有运行 UE 性能基准，也没有建立两套引擎的同负载测量。下文所说的“避免某类复制”“省去某段适配”是执行路径事实；对最终吞吐、分配量和帧时间的判断仍需真实场景测量。

## 研究基线已经具备的基础

研究基线已经有同一 Runtime 的类型身份和 GC 对象身份。架构采用同一权威类型定义；目前原生注册和描述型定义生产类型，语言编译器消费 Runtime 目录并产生共享函数签名，源码类型定义生产者仍待实现。`String` 是 32 字节 SSO UTF-8 值，`Array<T>` 是 24 字节值，使用 mimalloc。LibTooling 生成薄宏 `.gen.hpp`，LLVM 是语言执行的 JIT 后端。

研究基线的 `source::Program` 与 `Argument::borrow` 已把同步原生记录、Array 的可写或 const 借用、String 的 const 借用接入源语言。布局使用权威的 size、alignment 与 offset，数组访问有边界检查，借用入口不复制数组元素。该基线的入口禁止所有业务原生回调、对象操作与 async，GC 暂缓到宿主返回。

该基线尚未具备源码拥有型聚合值、按值聚合调用、Array 扩容、多函数、多类型声明和结构热迁移。当前零元素复制借用路径是一个清楚的起点，但它还不能覆盖“函数在本地创建临时数组，调用游戏算法，再返回拥有值”这样的日常业务。现有 `async/yield`、对象执行根、合作取消及旧版本帧保活已经实现；尚未接线的是新增拥有型聚合值的跨挂起生命周期。现行能力以[语言规范](../../LANGUAGE.md)、[Runtime 协议](../runtime-model.md)和[原生值借用](../native-value-borrows.md)为准。

这里需要分清五件事：

1. **类型目录**决定类型身份、字段和方法契约；编译器与原生注册都是生产者。
2. **值布局**决定本机内存如何表示某个值，size/alignment/offset 并不能独自决定函数 ABI。
3. **原生调用 ABI**决定参数如何传递、返回值写到哪里、如何适配 C++ 签名。
4. **拥有与借用**决定谁构造、销毁、扩容，谁能保留地址，以及错误路径如何清理。
5. **GC 根与执行租约**分别保证对象可达和代码/类型资源仍有效；共享对象指针本身不会完成这些工作。

`.gen.hpp` 是从原生声明生成的信息和接线，不是另一套业务类型世界。LLVM、本机 ABI 和类型目录应服从同一契约；可以存在适配器与快路径，但不应让不同执行入口定义不同的语义。

## 机制对照

| 关注点 | UE-AngelScript 的实际做法 | 对 FrostFoxAtelier 的判断 |
| --- | --- | --- |
| 类型身份 | UE 类型、AS TypeInfo 与插件映射关联；多份元数据存在。[源码][src-class-binding] | 保留一份权威 Runtime 类型身份，编译和工具数据从它派生 |
| 对象身份 | 同一地址转换 UObject 与 AS script object。[源码][src-object-address] | 已有统一身份；继续使用 generation、根与执行租约 |
| 原生绑定 | UHT 生成真实函数指针和 caller，反射决定可见性。[源码][src-native-binding] | 借鉴声明自动投影，服务同一 ABI 和效果契约 |
| String/Array | FString/FScriptArray 原生存储；赋值、扩容、返回仍可能复制。[源码][src-string]、[源码][src-array-copy] | 已有零元素复制借用，下一步补局部拥有值与生命周期 |
| 普通 struct 参数 | 脚本参数隐式 const reference。[文档][doc-structs] | 沿用 `T`、`const T&`、`T&` 的显式契约；聚合按值调用另行落地 |
| 值清理 | 默认析构生成、离开作用域与异常清理调用值析构。[源码][src-value-unwind] | 高优先级；对象 GC 与拥有值析构分别处理 |
| GC | AS 自动 GC 关闭，引用图进入 UE；禁止 UE incremental reachability。[源码][src-gc-config]、[源码][src-gc-incremental] | 保留本项目分步 GC 与屏障，不复制原始指针假设 |
| 借用失效 | 数组 reference/iterator 诊断仅 WITH_EDITOR。[源码][src-reference-config] | 契约在开发与发布都成立，原生回调按效果逐步准入 |
| 执行 | 常规 VM；生成 C++ 后重新构建并走 JIT 接口。[文档][doc-precompiled] | 保持 LLVM 仅 JIT；日后按需缓存 JIT 产物 |
| 更新与工具 | body/layout 更新分级；运行时向 LSP 投影类型。[源码][src-reload-admission]、[源码][src-debug-database] | 先方法体与小模块，再布局迁移；工具读已发布 Runtime 目录 |


## 原生绑定：声明、布局、调用各有职责

### 反射是绑定来源，原生调用有专门路径

**事实。** 官方自动绑定先遍历 UE 反射数据，再根据 Blueprint 与 Script 可见性生成 AS 绑定。可编辑性、Blueprint 可见性、Script 可见性是策略，不是每个类型天然只能供某一语言使用。[自动绑定规则][doc-auto]

更关键的是实际调用：`BindBlueprintCallable` 查找 `UClass::ASReflectedFunctionPointers`，要求有 `CallerPtr`，取得原始函数指针或成员函数指针，再注册为 AS 方法或全局函数。因此常规可绑定原生函数有直接调用路径，不要求每次经过 `ProcessEvent`。[原生函数绑定][src-native-binding]

这个能力依赖引擎改动。UHT 的代码生成器保留原始 C++ 的 const/ref 声明，并生成 `ERASE_FUNCTION_PTR` 或 `ERASE_METHOD_PTR`；CoreUObject 的 `ASAutoCaller` 为真实 C++ 签名生成调用 thunk。不能据此认为只给 AS 填一个通用反射签名就能自动得到正确 C++ ABI。[UHT 生成][src-uht]、[thunk 模板][src-thunk]

**建议。** 保持我们的“原生声明生成薄接线 + 权威类型定义 + 编译器执行”方向。函数调用记录需要真实参数、返回与效果契约；引入必要的 ABI 适配是合理的，但不要发展出独立的脚本镜像对象。

字段访问也有直接路径：普通 property 用 UE 真实 offset 注册；带原生 setter/getter、对象解析或特殊语义的 property 则选择 accessor。源码中的 `ActorLocation` 一类属性语法还可能实际调用 `GetActorLocation()`，不能仅根据点号语法推断为直接内存读取。[直接字段登记][src-property-offset]、[accessor 语义][doc-accessors]

### 值布局取真实原生 size 与 alignment

**事实。** `ValueClass<T>` 使用 `sizeof(T)`、`alignof(T)` 与 C++ 类型 traits；反射结构体使用 `ICppStructOps` 的 size 或 `UScriptStruct` 的 properties size，并使用 `GetMinAlignment()`。注册为 AS 值类型之后，插件把这些布局值写入 AS 类型信息。[值类型注册][src-value-layout]、[注册实现][src-value-registration]

原生 `USTRUCT` 对应的 `FUStructType` 将 AS TypeInfo 关联到 `UScriptStruct`；复制、构造、析构继续使用 `CopyScriptStruct`、`InitializeStruct`、`DestroyStruct` 等 UE 操作。这是把真实布局与生命周期导入语言，而不是只建立同名的字段清单。[结构体关联][src-struct-binding]、[结构体生命周期][src-struct-lifecycle]

**建议。** 我们已实现权威布局。下一步的价值在于让 owning local 和返回值使用对应的构造、复制/移动、析构契约。不要以“字段看起来相同”替代布局和生命周期一致；也不要用字段 memcpy 复制含 String、Array 或对象引用的记录。

## FString 与 TArray：原生表示成立，零复制有明确边界

### FString 就是 UE FString

**事实。** `Bind_FString.cpp` 直接注册 `ValueClass<FString>`，在值地址上 placement-new `FString`，复制构造调用 C++ `FString(Other)`，析构调用原生析构，赋值、追加和字符串方法绑定原生实现。索引绑定返回 `TCHAR&` 或 `const TCHAR&`，并做边界检查。没有另一种 AS 字符串再在每次宿主边界转换成 FString。[FString 注册与操作][src-string]

共享表示不意味着所有字符串操作没有分配或复制：复制构造、字符串相加与按值结果仍有正常值语义。格式化绑定也能通过 generic 参数地址处理输入并构造新的返回 FString。[格式化调用][src-string-format]

**建议。** 借鉴拥有值的生命周期接线以及 const/ref 的可用性，不照搬编码和字节尺寸。我们的 String 是 UTF-8 + SSO；UE 绑定直接暴露 TCHAR 字符索引。名称相近不能让这两种字符串成为布局或索引语义兼容的值。

### TArray 使用 UE 的 FScriptArray，而不是上游 AS 脚本数组包装

**事实。** UE 集成把 `TArray<class T>` 注册成 `ValueClass<FScriptArray>`；类型的 size/alignment 就是 FScriptArray 的 size/alignment。元素操作记录缓存元素大小、对齐、是否需要构造/复制/析构及元素类型。索引直接返回 `GetData() + index * stride` 的地址。[TArray 注册][src-array-registration]、[原生容器布局][src-array-layout]、[元素操作与索引][src-array-operations]

当元素存在合法 C++ 形式时，插件能生成 `TArray<CppType>` 的 native form；泛型运行形态仍是 FScriptArray。这里证明的是 UE 默认反射容器的原生存储路线，不能泛化为任意自定义 allocator 的 TArray 都自动兼容。[容器 C++ 形式][src-array-cpp]

Array 的值语义也完整存在：POD 元素复制用整段 memcpy，非 POD 元素逐个构造、复制、析构；Add 和 Append 会扩容并复制输入；按值返回到 UE 侧时 `GetReturnValue` 调用 `CopyValue`。[数组复制][src-array-copy]、[增加元素][src-array-add]、[参数与返回边界][src-array-boundary]

**建议。** 我们已经在借用边界实现零元素复制；下一步要在同一个 Array 类型上增加拥有型局部值和受控扩容。容器操作应继续从元素类型的真实生命周期取得能力，并保留 POD 批量路径。既不要所有操作都强制反射逐元素调用，也不要把含生命周期的元素当 POD。

### 地址共享会带来失效问题

**事实。** UE 绑定区分 const 和可写的元素 reference。扩容、赋值等结构修改之前，源码有数组迭代检测与旧数据区间 reference 失效处理；这些路径由 `AS_ITERATOR_DEBUGGING` / `AS_REFERENCE_DEBUGGING` 控制。[reference 失效入口][src-array-invalidation]

**建议。** 我们当前借用期间禁止扩容，能够保住元素地址。以后允许 Array 扩容、原生回调或重入时，必须给“哪些操作会使哪些地址失效”一个统一规则。仅增加 `Reserve` 方法会漏掉这项契约。是否需要版本检查、作用域约束或调用效果准入，应按真实调用链选择最小机制。

## const/ref/out 与按值调用

### 简洁语法建立在不同于 C++ 的参数语义上

**事实。** 官方文档明确：脚本函数的 struct 参数默认内部变成 const reference；写 `FVector` 和写 `const FVector&` 在其脚本函数中具有相同语义和性能。修改调用者值需要显式 reference；`&out` 还用于 Blueprint 输出参数。builder 也有对应自动转换。这个规则描述脚本函数，不意味着原生 C++ 的按值签名被改成了按引用签名。[脚本参数语义][doc-structs]、[编译转换][src-auto-reference]

C++ 参数绑定使用 `CPF_ConstParm` 和专门保存的 `APF_CppConst` / `APF_CppRef`。源码说明某些 UE 的 `CPF_ReferenceParm` 不准确，因此原生参数需要根据真实 C++ 声明判定 reference；运行时生成的参数则使用 UE reference 标记。[原生 const/ref 判定][src-native-qualifiers]

**建议。** 保留已经选定的显式参数契约：`T` 表达按值，`const T&` 表达只读借用，`T&` 表达可写借用；聚合按值传递目前尚未实现。UE 的隐式 const reference 是不同的语言选择，本轮不建议改变我们既定语义。语言规范须明确快照、共享读取、别名更新和方法可写性。`out`、可写借用和拥有返回也需要分别定义，不以“都传地址”合并语义。

### 原生 ABI 仍通过 thunk 和 native form 接线

**事实。** 正常 VM 路径 `CallFunctionCaller` 建立参数地址表，区分对象/reference/primitive，并确定返回地址，然后调用绑定 caller。`ASAutoCaller` 按 C++ 模板 `ParamTypes` 解包并调用真实函数。因此 native `T` 按值参数依然执行其 C++ 按值语义；传一个地址到 caller 不会把 native 复制取消。[VM 调用适配][src-call-context]、[thunk 参数与返回][src-thunk]

同一 thunk 的非 reference 返回会经 `GetAddressIfReference(const T&)` 再 placement-new 返回类型。这是存在复制构造的源码路径，不能承诺所有聚合返回零中间值或零复制。UE 调用脚本的反射边界也有明确的临时值和结果复制：FString 的 property helper 对值结果做 C++ 赋值，Array 结果做逐类型复制，USTRUCT 结果调用 `CopyScriptStruct`。[值类型边界][src-cpp-type-boundary]、[数组边界][src-array-boundary]、[结构体边界][src-struct-boundary]

转译代码还有 native form：绑定宏在生成配置下记录原生函数名与是否 trivial；UFunction 的 native form 在可见性和可链接性允许时生成 `Class::Function(...)` 或 `Object->Function(...)`。因此编译代码可以绕过部分通用 caller，但它仍依赖绑定信息以及真实原生签名。[绑定宏][src-bind-macros]、[native 调用生成][src-jit-native]

**建议。** 分开实现本机 ABI lowering、值生命周期和调用效果。我们的 LLVM JIT 可以生成直接 native call；必要时由生成的 adapter 处理复杂 C++ ABI。所谓“薄绑定”应表示适配服务同一类型契约，不表示 thunk 必须绝对不存在。聚合按值/返回应在有真实用例时端到端核实复制和销毁次数，而不是以函数签名外观推断性能。

## 类型与对象统一到什么程度

**事实。** UE 原生类绑定创建 AS reference type，创建插件 `FUObjectType`，并在 AS TypeInfo 中记录 UClass；原生结构体绑定创建 AS value type 和 `FUStructType`，再记录 UScriptStruct。UE 的 UClass/UScriptStruct、AS TypeInfo 以及插件类型映射都存在。[原生类元数据][src-class-binding]、[原生结构体元数据][src-struct-binding]

同时，脚本 class 对业务使用者表现为真正的 UObject，进入 UE 对象体系与 GC；脚本 struct 的字段即使没有显式 UPROPERTY，也有内部登记。官方文档和源码支持“对象身份与存储整合”，不支持“AS 编译器与 UE 只剩一个元数据数据结构”的说法。[脚本对象语义][doc-objects]、[脚本字段默认策略][doc-struct-fields]

源码中 UObject 与 `asIScriptObject` 直接互转，脚本对象在同一地址上完成 native 构造和 AS script object 初始化。这直接证明共享对象地址，不是为每个 UObject 建立另一份业务包装对象。[地址转换][src-object-address]、[同地址构造][src-object-construction]

**判断。** FrostFoxAtelier 应继续保持一个权威 Runtime 类型身份，原生注册和语言定义都是它的生产者；编译器、JIT、反射、调试与编辑器各有派生信息和索引。这不要求把所有编译数据塞进一个对象，也不要求所有执行都动态反射。需要防止的是第二套可独立定义业务类型身份、对象身份和生命周期的入口。

当前 Runtime 的首次 create 会封闭注册。推进源码类型定义时，应先收集原生和语言声明、校验并发布，再实例化 Runtime；不能为了动态增加类型而直接解除这个封闭条件，让已编译代码和实例面对未经发布的布局。

## 值生命周期、GC 与借用安全

### 拥有值确实有局部析构和异常清理

**事实。** UE fork 会为包含非 POD 值成员的脚本类型生成默认析构。VM 的 FREE 路径调用值析构；异常 `CleanStackFrame` 销毁仍存活的局部值。FString 和 TArray 的绑定提供原生构析，UASStruct 的 ICppStructOps 也把构造、析构和复制接到脚本函数。[析构生成][src-value-destructor]、[正常值释放][src-value-free]、[异常值清理][src-value-unwind]、[结构体生命周期桥接][src-asstruct-ops]

这说明它支持真实拥有值与 scope 清理。UObject 仍由对象生命周期管理；脚本对象析构在 UE 的 RuntimeDestroyObject 路径触发，不能由局部 value RAII 推导出局部 UObject 随离开作用域销毁。[对象销毁][src-object-destruction]

**建议。** 把现有类型描述器的构造、复制、析构和 trace 接到源码拥有型局部值，并按实际转移需求补齐移动契约。正常离开作用域、提前返回和错误都需要同一清理规则；聚合值进入协程帧后，已有取消协议也必须销毁这些值。若这些操作由 JIT 代码提供，执行帧与拥有值还要保留相关代码版本，直到最后一次生命周期操作结束；这是我们的设计要求，不是本文已证明的 UE 等价机制。

### GC 统一的是对象回收与引用图

**事实。** UE fork 默认 script object 注册使用 `asOBJ_REF | asOBJ_NOCOUNT`，AS 的 AddRef/Release 和自动 GC 行为登记被注释，Manager 关闭 AS 自动垃圾收集。脚本类的对象引用由 UE 收集器处理。[AS 对象登记][src-as-object-gc]、[引擎设置][src-gc-config]

类生成器对没有生成 UE property 的脚本字段递归补 GC schema。数组的 UObject 元素登记为 ReferenceArray，含对象引用的 struct 元素登记为 StructArray；原生结构体也通过 property 与 AddReferencedObjects 信息追踪引用。这才使“没有 UPROPERTY 注解的脚本引用也能参与 GC”成立。[字段 trace][src-gc-fields]、[容器 trace][src-gc-array]

**关键限制。** Manager 明确 `checkf` 要求关闭 UE incremental reachability，并说明当前 AS 集成不支持它。因此不能把其原始对象指针写入路径照搬到我们的分步 GC，再假定帧末统一回收就会安全。[增量 GC 限制][src-gc-incremental]

共享对象地址和成员引用图也没有自动证明所有宿主 raw pointer、局部临时引用和原生调用期间都被 root 或 pin。源码中的测试原生函数会在脚本调用内主动触发 `ConditionalCollectGarbage`，调试代码还承认测试可以在 Tick 中途收集；不存在由这些证据支持的“GC 永远等到 Tick 结束”承诺。[主动收集][src-force-gc]、[中途 GC 注释][src-debug-mid-gc]

**建议。** 保留我们的 identity + generation、强弱根与分步 GC 屏障。编译器写入对象引用时必须遵守同一 GC 契约；聚合值的 trace 与 Array 元素追踪也必须补齐。先把 GC 安全点放在当前 borrow 区间之外，再按真实业务开放跨调用与跨帧持有。

### const reference 不提供完整 lifetime 证明

**事实。** 数组引用失效和迭代修改诊断仅在 WITH_EDITOR 启用。修改绑定会把 active context 中指向旧数据区间的 tracked reference 置空；发布配置没有同等诊断。原生函数如果直接修改容器，也不一定经过这些数组绑定。[配置][src-reference-config]、[reference 置空][src-reference-invalidate]

普通 struct 的 const reference 限制这个别名的写入，却不能阻止 owner 被其他别名或原生 callback 修改。官方 UE fork 的 `Suspend()` 直接返回 `asERROR`；定时器、latent command 和回调也不能被当成栈协程支持。[Suspend 实现][src-suspend]

**建议。** 当前纯借用区间禁止 native callback、对象操作、挂起并暂缓 GC，在现有存储合同下合理。长期把所有 native 函数永久禁止会阻碍游戏算法组合；更合适的增量是允许效果已知、不会 resize/reenter/GC/suspend 的函数。未知函数继续受控。以后需要可写容器扩容、原生重入或跨帧借用时，再增加对应 owner/lease 与失效机制。

## 执行、热更新与语言工具

### 保持 LLVM JIT，吸收产物兼容性纪律

**事实。** Hazelight 文档的“JIT”包含生成 C++ 文件、重新构建游戏二进制，再通过 AS 的 JIT 接口替换 VM 执行的路线。`StaticJIT` 在生成 precompiled data 时创建，编辑器配置跳过 jitted code；不能把它描述成编辑器中的通用 LLVM 在线 JIT。[预编译路线][doc-precompiled]、[初始化][src-manager-jit]、[编辑器配置][src-jit-config]

预编译缓存含原生字段偏移与结构尺寸，文档要求与使用它的二进制匹配。源码的 `GetCurrentBuildIdentifier()` 仅区分 Debug/Development/Test/Shipping 配置；另用 `PrecompiledDataGuid` 与生成 C++ 的 CompiledInfo GUID 比较，不匹配时清除 native functions。**这不是完整原生二进制或布局 hash 校验**；“同二进制”首先是官方操作合同，不能夸大自动检查覆盖任意布局变化。[缓存兼容][doc-precompiled]、[配置标识][src-cache-build-id]、[GUID 检查][src-manager-cache]

**建议。** 继续我们的 LLVM 执行路线。将来真的需要缓存时，把原生布局、函数服务 ABI、目标、编译配置和代码版本纳入 fingerprint，并使不兼容产物明确失效。现在增加 AS VM 或 C++ 转译器会扩大执行与诊断面，不会补齐眼前的拥有值和业务调用能力。

### 方法体更新与布局迁移是两个阶段

**事实。** UE 集成区分结构依赖与 hard-value 依赖，布局变化会向依赖方传播；编译失败不交换模块。它根据 SoftReload、FullReloadSuggested/Required 更新，PIE 中无法完整 reload 时保留旧版本并排队。类生成器检查父类、字段类型、定义与 metadata；完整 reload 还涉及 Blueprint 重新实例化与对象迁移。[模块依赖][src-reload-dependencies]、[失败与交换][src-reload-failure]、[更新准入][src-reload-admission]、[结构检查][src-reload-schema]、[编辑器迁移][src-reload-editor]

**建议。** 先让兼容方法体更新、旧执行帧和代码租约形成完整路径；多函数模块出现后建立必要的依赖闭包。结构 schema 发布、活实例迁移、默认值和编辑器资源重建应另有验收阶段。共享类型身份不会免除布局变更的迁移成本。

### 工具应读取已发布目录

**事实。** UE 调试服务器从 AS Engine 的类型和方法提取数据，包括参数类型、const、`&in` / `&out`，并发送 debug database 与设置。官方 LSP 请求这份数据，再更新类型数据库、基本类型宽度和模块解析。派生工具数据库存在，但它从运行引擎取得宿主类型信息。[调试数据生产][src-debug-database]、[LSP 数据接收][src-lsp]、[LSP 请求][src-lsp-request]

**建议。** 编译器、调试器和语言工具都从已发布 Runtime 类型目录投影原生事实；源码索引可以独立存在，但不能另设权威原生布局。源码位置、诊断、引用索引、断点与变量位置仍需专门接线；本机 unwind 能回溯机器栈，不等于已具备源码语言调试。

### 游戏与编辑器策略留在调用者

**事实。** UE 脚本默认属性可编辑和 Blueprint 可读写；对象引用不需要 UPROPERTY 注解才能参与 GC；脚本的 `float` 默认为 float64。它使用属性默认值与 `default` 语句，减少热更新期间不稳定的构造时机。[与 C++ 的差异][doc-cpp-differences]

**建议。** 我们的 float32、UTF-8 和构造规则应保持自身契约。Actor/component、默认组件、网络复制和编辑器属性可见性可以是 Runtime 的调用者与元数据消费者；核心提供类型、生命周期和阶段准入原语。不要把 Blueprint 的默认访问策略或 UE 的类型名当成通用语言必需行为。

## 值得做、按需做、不要直接照搬

### 现在值得做

1. **让当前 Array/String 成为可在源语言局部拥有的值。** 优先接线构造、析构、返回与错误清理，使用已有权威布局和生命周期。
2. **逐步开放效果已知的原生函数。** 当前纯借用入口全部禁止 callback 是可靠的第一片。长期应允许不会结构失效、触发 GC、挂起或重入的已知函数；未知原生函数仍走受控边界。先做少数真实 leaf 算法，不引入完整 borrow checker。
3. **编译器定义和原生定义共同发布。** 在 Runtime create 之前完成收集、校验、目录发布，然后实例化；小模块的多函数与多类型是可观察的下个增量。
4. **将 native 服务、字段快路径、反射和工具接在同一契约上。** 类型映射和工具索引可以派生；性能快路径也必须保留边界、const 和生命周期语义。

### 待具体用例再做

- **新增聚合值的跨帧持有、await 与并发。** 本项目现有 `async/yield`、对象根、合作取消和旧代码帧保活继续沿原契约。按真实业务把新增拥有型值接入帧的清理与代码租约；借用跨挂起、等待协议和多线程准入仍需独立定义。
- **多模块依赖图。** 等多函数、小模块已经跑通，再用真实变更传播构建闭包。
- **结构热迁移。** 在确实需要修改活实例布局时，把 migration、默认值、引用与编辑器资源作为一个可验收流程。
- **JIT 产物缓存。** 在启动或重复编译成本成为实际问题时做，并核对原生布局与服务 ABI。UE 的 C++ AOT 作为执行路线对照；本项目继续采用仅 JIT 的已定契约。
- **针对性测量。** 真实场景中区分值复制、容器分配、动态类型操作和代码执行；不能从已发布游戏数量或行数推导我们的性能。

### 不要直接照搬

- 不增加第二套脚本业务对象身份、拥有权或 GC 世界。
- 不以 UE 与 AS 的多个绑定登记结构替代我们的统一权威类型目录。
- 不把所有原生调用强制收敛到动态反射调用，也不把必要的 ABI adapter 当成架构失败。
- 不把所有容器操作都宣传成零复制；拥有值赋值、按值返回与 Add/Append 有真实成本。
- 不照搬 UE `float`、FString 字符索引、Blueprint 默认可见性与 UObject 构造策略。
- 不把开发配置的失效诊断当成发布配置的内存安全保证；我们的契约需要在两种配置都成立。
- 不为了模仿其完整编辑器生态，在第一项业务用例之前铺设全套模块、网络、AOT 与结构迁移设施。

## 一手来源

以下源码链接固定到本次读取的 commit。Unreal 引擎仓库链接需要相应 Epic/GitHub 访问权限。

[repo-head]: https://github.com/Hazelight/UnrealEngine-Angelscript/commit/a7890667d1abb289bc2cec16b3da075faf836f2d
[docs-head]: https://github.com/Hazelight/Docs-UnrealEngine-Angelscript/commit/3864c72fa3c1ef67413dfaad24417bad01879b4d
[doc-install]: https://angelscript.hazelight.se/getting-started/installation/
[doc-auto]: https://github.com/Hazelight/Docs-UnrealEngine-Angelscript/blob/3864c72fa3c1ef67413dfaad24417bad01879b4d/content/cpp-bindings/automatic-bindings.md#L7-L16
[doc-structs]: https://angelscript.hazelight.se/scripting/structs-refs/#automatic-references-for-function-parameters
[doc-objects]: https://github.com/Hazelight/Docs-UnrealEngine-Angelscript/blob/3864c72fa3c1ef67413dfaad24417bad01879b4d/content/scripting/structs-refs.md#L6-L9
[doc-struct-fields]: https://github.com/Hazelight/Docs-UnrealEngine-Angelscript/blob/3864c72fa3c1ef67413dfaad24417bad01879b4d/content/scripting/structs-refs.md#L47-L53
[doc-accessors]: https://angelscript.hazelight.se/scripting/properties-and-accessors/#property-accessors-in-c-binds
[doc-precompiled]: https://angelscript.hazelight.se/cpp-bindings/precompiled-data/
[doc-cpp-differences]: https://angelscript.hazelight.se/scripting/cpp-differences/
[src-string]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_FString.cpp#L155-L221
[src-string-format]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_FString.cpp#L140-L152
[src-array-registration]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_TArray.cpp#L1331-L1378
[src-array-layout]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_TArray.cpp#L203-L210
[src-array-copy]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_TArray.cpp#L119-L167
[src-array-add]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_TArray.cpp#L613-L679
[src-array-boundary]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_TArray.cpp#L214-L247
[src-array-cpp]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_TArray.cpp#L418-L438
[src-array-operations]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Public/AngelscriptBinds/Bind_TArray_Functions.h#L14-L34
[src-array-invalidation]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_TArray.cpp#L22-L45
[src-class-binding]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_BlueprintType.cpp#L632-L645
[src-struct-binding]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_UStruct.cpp#L888-L896
[src-struct-lifecycle]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_UStruct.cpp#L256-L312
[src-struct-boundary]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_UStruct.cpp#L327-L373
[src-value-layout]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Public/AngelscriptBinds.h#L207-L226
[src-value-registration]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptBinds.cpp#L59-L100
[src-native-binding]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_BlueprintCallable.cpp#L41-L117
[src-native-qualifiers]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptType.cpp#L239-L268
[src-auto-reference]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L1414-L1430
[src-property-offset]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_BlueprintType.cpp#L1285-L1289
[src-cpp-type-boundary]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Helper_CppType.h#L157-L192
[src-thunk]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Source/Runtime/CoreUObject/Public/UObject/CoreNative.h#L36-L151
[src-uht]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Source/Programs/Shared/EpicGames.UHT/Exporters/CodeGen/UhtHeaderCodeGeneratorCppFile.cs#L2894-L3027
[src-call-context]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/ThirdParty/source/as_context.cpp#L5159-L5278
[src-bind-macros]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Public/AngelscriptBinds.h#L43-L71
[src-jit-native]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/StaticJIT/StaticJITBinds.cpp#L450-L505
[src-manager-jit]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L363-L382
[src-jit-config]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Public/StaticJIT/StaticJITConfig.h#L8-L10
[src-manager-cache]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L453-L477
[src-reload-dependencies]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L2048-L2117
[src-reload-failure]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L2560-L2574
[src-reload-admission]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L2625-L2692
[src-reload-schema]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L1079-L1135
[src-reload-editor]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptEditor/Private/ClassReloadHelper.cpp#L141-L187
[src-debug-database]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Debugging/AngelscriptDebugServer.cpp#L1368-L1445
[src-lsp]: https://github.com/Hazelight/vscode-unreal-angelscript/blob/ffc7ab2948589abb04200b7dcedd326eaeaf7a4d/language-server/src/server.ts#L161-L181
[src-lsp-request]: https://github.com/Hazelight/vscode-unreal-angelscript/blob/ffc7ab2948589abb04200b7dcedd326eaeaf7a4d/language-server/src/server.ts#L276-L284
[src-object-address]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Public/AngelscriptManager.h#L149-L158
[src-object-construction]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/ASClass.cpp#L1330-L1340
[src-object-destruction]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/ASClass.cpp#L938-L945
[src-value-destructor]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L885-L975
[src-value-free]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/ThirdParty/source/as_context.cpp#L2520-L2555
[src-value-unwind]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/ThirdParty/source/as_context.cpp#L4850-L4923
[src-asstruct-ops]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/ASStruct.cpp#L134-L177
[src-as-object-gc]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/ThirdParty/source/as_scriptobject.cpp#L294-L331
[src-gc-config]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L325-L338
[src-gc-incremental]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L260-L265
[src-gc-fields]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L4784-L4850
[src-gc-array]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_TArray.cpp#L55-L79
[src-force-gc]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Testing/UnitTest.cpp#L157-L169
[src-debug-mid-gc]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Binds/Bind_BlueprintType.cpp#L342-L347
[src-reference-config]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Public/AngelscriptManager.h#L19-L20
[src-reference-invalidate]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/ThirdParty/source/as_context.cpp#L5875-L5891
[src-suspend]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/ThirdParty/source/as_context.cpp#L875-L879
[src-cache-build-id]: https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/StaticJIT/PrecompiledData.cpp#L2594-L2612
