# 类型与对象运行时

本文维护运行时协议，除明确标记的 W0 目标合同外均依据当前源码。业务可观察语义以[语言契约](language-contract.md)和 [LANGUAGE.md](../LANGUAGE.md) 为准，能力范围与构建入口见 [README](../README.md)，模块归属、执行链及已知成本见[执行结构说明](runtime-structure.md)。现行取舍的原始依据见本页末尾。

| 状态 | 内容 |
| --- | --- |
| 已实现 | 成组类型发布、原生与描述型对象/记录、对象单继承、定型引用与强引用序列、受保护原生借用、成组引用修改、外部对象根和值根、观察取得、隐藏字段与嵌套引用遍历、对象写代号与变化游标、非移动分步 GC；共享函数签名、同步/协程 JIT 执行、协作安全点、单入口及项目快照的兼容方法体发布与旧版本保活 |
| 尚未实现 | 引擎 Tick 调度、任意值执行栈、跨发布单元调用与热更新、对象布局迁移 |
| 按需设计 | GC 的参与线程、并发/分代与堆布局；统一在[能力边界](runtime-structure.md#已知成本与能力边界)记录，不因外部算法存在而安排替换 |

项目快照通过同一 Program 接入多文件、原生派生类和具名同步入口，见[项目快照](source-function-modules.md#项目快照与多个公开入口)；下文单入口协议同样适用于单源码消费者。

## 无 C++ 异常的目标合同

**W0 冻结、W2 分片实施：NativeLayout 结果回调、原生成员 Result 注册/反射与定型 thunk 已实现。** 全局错误分类、fatal 与构造工厂遵循[工程指南](engineering.md#错误处理)。这不代表全仓去异常完成：其他运行服务回调、LLVM 错误适配及上层业务仍按对应片迁移，最终编译开关尚未翻转。Runtime 类型身份、单线程准入、根与代码寿命不因错误通道改变；World 检查仍不排期，MaterialProgram 的独立并发合同不变。

### 原生值生命周期目标

`NativeLayout` 中可失败的回调现使用以下完整函数指针类型；`Result` 为现有 `atelier::Result`，不是新的错误体系。可选复制能力仍可为空。此签名变更与 String 的新存储表示同批升级，生产者、注册适配、反射生成代码和全部调用方必须一起重编译、重启，不能混用旧类型目录或 JIT 版本：

```cpp
Result<void> (*construct)(void*) noexcept;
Result<void> (*copy_construct)(void*, const void*) noexcept;
Result<void> (*copy_assign)(void*, const void*) noexcept;
Result<void> (*sequence_mutate)(SequenceMutation, void*, const void*, std::size_t) noexcept;
```

`destroy`、`swap`、`base_address`、`sequence_view` 保持现有 void/指针返回值及 `noexcept` 签名；后两者继续只是受信任的非分配投影，不引入失败协议。无失败分支的 C++ 构造/复制/赋值由注册器调用后返回成功；确实有业务失败的类型提供结果化操作，不再用可抛的 C++ 特殊成员函数表达它。destroy/swap 不能报告可恢复失败，不得吞掉清理错误继续；不变量破坏按工程指南终止。

| 回调或操作 | 成功 | 返回失败时的存储与清理责任 |
| --- | --- | --- |
| `construct` / `copy_construct` | 目标从未初始化存储成为一个完整活值；复制不修改源 | 原生回调在返回前清理自己建立的全部子资源，目标没有活值，Runtime **不得**再对该目标调用 destroy；源保持有效且不变 |
| 描述型构造 / 复制构造 | 只有字段/原生基段成功后才登记为已构造 | Runtime 以现有预留工作栈逆序销毁已成功部分，不销毁刚失败的原生目标、不重复清理；不发布对象身份或根 |
| `copy_assign` | 原目标仍为活值并取得源内容 | 保留基本不变量和可销毁状态；允许前面字段已改变，不承诺整体回滚，源不变；调用方之后只正常销毁一次 |
| `sequence_mutate` | 完成指定序列操作 | 序列仍为可销毁活值，回调清理本次未提交元素/缓冲；上层若原有未改动保证，则必须继续以预检、暂存、成功后交换履行，不能降成部分成功 |

C++ 原生回调不能依赖异常自动展开：若先构造完整对象再做可失败初始化，失败时由回调销毁该对象；若逐子对象构造，则自己记录并逆序清理。描述型父记录只管理已经返回成功的子值。带默认文本的 String 字段仍在完整构造成功后才入清理栈，不先登记一个未完成目标。清理不分配、不失败；宿主不可恢复终止不提供上述正常返回保证。

已有公共值操作的错误分类保持：构造/复制构造失败为 `construction_failed`，赋值失败为 `assignment_failed`，无效存储为 `invalid_storage`，能力缺失为 `unsupported_operation`；适配时保留原诊断原因，不能把输入验证误码为构造失败。`sequence_mutate` 沿所属操作的原错误分类返回。整值字段写入、数组结构编辑及成组引用修改原有的失败保旧语义保持，不能因底层赋值只提供基本保证就直接修改已发布目标。外部业务副作用不由值操作自动撤销。

### 原生调用与 evaluate 目标

可恢复的原生业务错误通过 `atelier::Result<R>` 返回。原生成员方法已支持 `Result<R>` 返回值的注册/反射，const 与 noexcept 的接收者含义不变；已有 contextual 方法 `Result<R>(Runtime&, ObjectId, C&, A...) noexcept` 继续走同一生产者和执行通道。R 仍限于现有 void、标量/枚举、数学值或 plain record；不可借此开放拥有容器、引用结果或新的语言 Result 类型。不可失败方法仍可直接返回 R；这是有无失败能力的区别，不是两套错误模式。

原生程序化节点迁移后的可失败签名为 `atelier::Result<void> evaluate(...) const noexcept`，原有 Geometry/Context 引用参数、参数名及 BorrowSafe 约束保持。注册器把它发布为 **业务结果 void** 的同一个 `evaluate`，错误不是额外输出端口。生产者只剥离一层 runtime Result；不把 `std::expected` 的布局注册成业务类型，不让 Runtime 理解 Geometry、列名或节点错误。程序化操作自己的 Result 由节点在本模块边界转换为带原原因的 runtime 错误（节点执行失败为 `invocation_failed`），不通过 throw、线程局部“最后错误”或往 Context 塞错误旁路传递。

动态 Invoker 与定型 native thunk 从同一个原生结果取值/取错：失败先保留首因，不读取未产生的结果、不覆盖调用方原输出；成功仍需通过既有结果类型与存储检查。两种回调均为 noexcept，Runtime::call 不再捕获异常。进入 JIT 的状态/诊断保持固定 ABI，宿主拥有 Error，生成代码只走状态分支并显式释放已构造值、根和借用。`runtime.cpp` 的值构造现已在清理后返回失败，不再重抛；这不是 JIT 展开协议。Material 的采样、方向与编译期默认值捕获均使用显式 Result；evaluate 和 compile_material 只在宿主保存普通 Error，不再延迟保存或重抛异常。默认值构造失败不发布预览类型或部分 MaterialProgram；已构造临时值沿 Runtime 生命周期清理。Source 运行服务、调用观察器、调试处理器及 Runtime 引用访问者、纯值校验器、with_native 均直接消费 Result；LLVM 的 Error/Expected 在 DLL 内显式传播，到边界转成原有状态与诊断，不抛宿主异常。前端诊断合同见[语言执行与诊断](../LANGUAGE.md#执行诊断与保护)。

失败只保证声明过的输出暂存不提交，不撤销原生回调此前通过可写引用造成的业务修改。程序化求值器仍拥有节点候选输出和发布决定：任一绑定/调用失败不发布部分 Geometry，旧输出保留，修正输入后可重新求值。原生与 Source 列节点现已共用这条结果通道和绑定计划，当前字段、列与组选择合同见[原生与自有语言列节点](modules/procedural/README.md#原生与自有语言列节点)。

W2 以既有 Runtime/Source/反射/程序化的构造失败、部分赋值、写暂存后失败与恢复序列验收该合同；保留清理计数、旧输出及再次执行结果的独立断言，去掉“必须抛异常”的夹具写法，不新建拒绝矩阵。

## 结构与职责

| 职责 | 当前实现 | 当前边界 |
| --- | --- | --- |
| 发布类型定义 | 一组 `TypeDefinition` → `Runtime::register_types` → `Impl::Type` | 解析全部依赖、验证并完成布局后整体发布；类型目录只追加，在类型安全点发布，已发布定义不可变 |
| 查询类型信息 | `TypeInfo`、`TypeId`、`is_a` | 标量、枚举、记录、对象共用类型表；对象以 `core.Object` 为根单继承；没有定义版本切换 |
| 标识对象实例 | `ObjectId`、`Impl::Slot`、实例存储 | 运行时上下文与代际校验；显式销毁或 GC 提交死亡后拒绝访问，GC 不搬迁存活对象 |
| 描述和定位字段 | `FieldInfo::value`、`ValueDescription`、`FieldStorage`、`ReferenceField` | 内联值、`ReferenceSlot` 与对象字段中的 `ReferenceSequence` 均可操作；隐藏字段通过受保护原生访问定位，仍参与引用遍历 |
| 操作值的生命周期与内容 | `initialize_value`、`copy_construct_value`、`assign_value`、`destroy_value` | 调用方提供存储；标量和含引用记录共用四个入口，引用复制保持目标身份 |
| 描述和执行函数 | `SignatureDefinition`、`FunctionSignature`、`MethodInfo`、`Invoker`、`CallContext` | 原生与源码共用接收者/参数/结果描述及解析；标量/枚举支持值与借用，原生方法与源码入口均支持无 GC 引用的原生聚合借用；源码入口的 String 参数限于只读；结果为 void、标量/枚举值或同步数学值/plain struct，执行实现仍分开 |
| 执行准入与根槽 | `ExecutionScope`、`ExecutionFrame` | 活动调用计数、借用/遍历禁入与稳定根槽；挂起保留存储，但不保持活动执行作用域 |
| 协程帧拥有者 | `Coroutine`、`CoroutineFrame` | 同线程状态、恢复、合作取消和结果；具体编译帧负责根、存储和代码版本的释放 |
| 发布原生类型 | `NativeRegistration<T>`，以及调用它的生成代码 | `build()` 产出同一个 `TypeDefinition`，推导字段、参数、返回及原生接收者 const 信息；[自动反射](reflection.md)从 C++ 声明生成这些调用，类型依赖仍在 Runtime 发布时解析 |
| 表达对象引用 | `ObjectReference`、`ReferenceSlot` | 前者是定型传递值，后者是紧凑字段存储；二者不自动创建外部根 |
| 登记外部持有 | `ObjectRoot`、`ValueRoot` | 对象根支持复制/移动，值根可移动且借用调用方存储；撤根不等于销毁 |
| 枚举引用边 | `visit_roots`、`visit_references` | 区分持有/观察，递归内联记录而不递归对象图；GC 使用同一字段定义派生的持有边追踪计划 |
| 分步回收对象 | `begin_collection`、`collect_step`、`CollectionProgress` | 宿主显式安全点推进根处理、标记和清扫；统一死亡提交，析构按步执行，时间预算为软上限 |

源码依据：[公开定义](../modules/execution/runtime/include/runtime/runtime.hpp)、[注册与运行时实现](../modules/execution/runtime/src/runtime.cpp)、[原生定义生产者](../modules/execution/runtime/include/runtime/native.hpp)。

`TypeDefinition` 是发布输入，`TypeInfo` 是发布后元数据的查询面；它们指向同一类型身份，不能因结构体不同就判定为两套类型世界。`Runtime::call` 仍只执行原生 `Invoker`；源码编译与 JIT 经独立执行作用域使用同一 Runtime，不把机器码地址塞进原生借用通路。仅 JIT 的决定见[编译与执行契约](language-contract.md#编译与执行)。

**类型目录只追加。** 创建对象后仍可发布新的类型组：新类型可以派生或内联已发布类型，也可以引用之后的类型。准备阶段只读取已发布目录；提交需要类型安全点，即 `check_code_publication()` 的条件（没有回调、借用、遍历或活动的生成代码调用）加上 GC 空闲，否则返回 `busy` 且保留候选，宿主在下一个安全点重试。挂起的协程不阻止发布。已发布类型不重定义、不注销、不增删成员，同名仍返回 `duplicate_name`；`TypeId`、`TypeInfo`/`FieldInfo` 地址与内容在 Runtime 存续期内保持有效。`find_type` 与原生表示查找使用发布时维护的索引。目录消费者缓存 `type_count()`，之后用 `types(first)` 只扫描新增类型；Runtime 不发送增长通知。

## 并行任务体禁入诊断

Foundation 的 `parallel_for` / `JoinGroup` body 只处理宿主预先准备的普通值，不调用 Runtime 或它的 Source 执行入口；同一规则也适用于 caller 实际参与执行、inline/P=1、嵌套 body 和失败返回前的局部析构。宿主先准备输入、容量与独占输出，join 后才恢复 Runtime 操作。`.source` 语言级 `parallel_for` 遵守同一边界：分派前在串行线程一次性完成序列投影、别名校验与 `self` 值字段快照，轮廓化块函数在 worker 内只读写普通栈值与预投影缓冲，经 `tasks::Failure` 将 `fail(...)` 或算术/越界错误带回 join 后的 Runtime 线程统一清理。任务的完整借用、捕获与错误载荷前提见[基础库任务协议](foundation.md)。

开发构建通过内部 `assert_serial_access()` 读取唯一的 `tasks::in_parallel_body()` TLS。命中时以固定诊断调用唯一 fatal 出口，不返回 busy、不抛可捕获异常、不进入 Runtime 日志或用户回调。release（`NDEBUG`）编译掉检查，不读取此 TLS；它仍须遵守相同的禁入契约，不因此获得并发 Runtime 能力。

诊断集中守住会执行代码、修改共享状态或释放资源的路径：

- 执行准入与退出、类型准备/发布、对象创建/销毁、原生调用与借用、根登记/复制/撤销、回调计数、写代号、GC 与关停；值构造/复制/赋值/析构及结构修改。
- Program 请求/准备/发布/关闭，Entry/Lease/Typed 调用或绑定，候选与代码拥有句柄的释放/替换，Version 退休，以及协程创建/恢复/取消。保留原有所有权及代码退休链，不新增后台 prepare 许可。
- ExecutionFrame 的槽读取、持有与清理，在 `reference()` / `empty()` 首次查看槽之前检查，不只依赖深层执行准入。

纯元数据查询（如 describe/types/type_count/native_type/object_type）和仅验证身份的 `resolve_object` 不新增检查。部分名字看似只读的访问并非纯读：字段路径解析、NativeContext 接收者解析及校验/引用遍历会进入 `CallbackScope` 修改共享计数；它们由该计数边界检查，防止多个 body 同时写计数或遍历暂存。未命中诊断不表示调用合法，也不授权直接使用预先取得的对象内部地址。

对象的两级纯偏移字段路径在所属 Runtime 内使用一个有 16 个槽的直接映射缓存。缓存键包含实际根类型和两个完整 FieldId，结果只包含偏移、叶类型和硬范围。每次访问仍通过对象槽验证代际并从本次存储基址寻址。数组选择和需要原生回调的路径继续走逐段解析。读取这份缓存同样遵守串行准入。[编辑器实测](research/runtime-jit-study/README.md)

这是任务体误用诊断，不是任意线程检测、锁或捕获安全证明；提交前及 join 后的闭包构析不一定在 TLS 标记内，任务 Failure 则只有平凡普通值。World 入口尚未覆盖且本片不排期；Runtime 检查不能保证 World 在读写自身队列前已拒绝。MaterialProgram 的独立纯值并发合同保持不变，不在通用 JIT DLL 中增加禁令。

代表性清理验收沿既有 Source shutdown：挂起帧存续时执行普通值嵌套任务并显式转发 Failure，完整 join 后继续源码恢复，再取消、关闭及释放根/代码；dev/release 都执行该合法序列。故意在 P=1 caller body 中撤根的开发版终止探针仅作临时验证，不加入常驻拒绝矩阵。

## 共享函数签名与结果

对象继承的成员查找、布局与引用规则见下面的[正式对象单继承](#正式对象单继承)。

`SignatureDefinition` 使用既有 `TypeReference` / `ParameterDefinition` 表达参数，`ValueDefinition` 表达可选结果和可选接收者；`Runtime::resolve_signature` 返回拥有其描述数组的 `FunctionSignature`。它只解析已发布类型，不登记函数、不拥有 Runtime 或代码，不需要类型安全点。成组方法发布复用同一私有验证实现，并在事务内处理暂存类型；布局及引用闭包完成后再核对完整方法签名，防止同批前向记录隐藏 GC 引用；成员接收者默认取所属对象类型，显式提供者必须匹配，不让临时身份逃逸。空结果表示 `void`，不是伪造的类型 ID 或零标量。

原生成员的权威签名归 `MethodInfo::signature`，源码入口的权威签名归既有程序所有者的不可变声明。AST 分类、LLVM 类型和私有 DLL 参数描述是经核对的转换表示，不是另立业务类型身份或独立手填的声明。调用暂存归当前作用域，不拥有程序或执行代码；没有新增函数注册表。

参数名、参数扩展元数据与尾部默认实参是签名旁的元数据，不在 `FunctionSignature` 里，所以不参与底层 `FunctionSignature` 比较。`MethodDefinition::parameter_names` 为空或每个参数一个，空字符串表示未命名；非空名字在同一方法内不得重复（`duplicate_name`），数量不符返回 `invalid_definition`。`MethodDefinition::parameter_metadata` 为空或每个参数一个 `MetadataList`，与字段扩展元数据共用同一套键值格式、排序与同键拒绝校验。`MethodDefinition::parameter_defaults` 为空或每个参数一个可选 `Scalar`，只允许覆盖按值或 `read_only` 标量/枚举参数的连续尾部后缀（浮点须有限，枚举默认值可提交底层整数以便同批前向引用，发布时归一为精确 `EnumValue`）。发布后 `MethodInfo::parameter_names`、`parameter_metadata` 与 `parameter_defaults` 总是与参数一一对应；继承的方法一并复制。动态 `Runtime::call` 在实参少于形参且存在对应尾部默认实参时自动补齐；源码调用在编译期静态展开为常量实参。描述型对象类型（如 `.source` class）还可通过 `TypeDefinition::described_methods` 发布其声明方法的只读契约（`DescribedMethodInfo{name, signature, parameter_names, parameter_metadata}`）至 `TypeInfo::described_methods`，供图拓扑等宿主只凭 `Runtime&` 检查方法签名与参数元数据，而不将其混入原生可调用 `TypeInfo::methods`。源码入口视图见[源码函数模块](source-function-modules.md)的 `EntryInfo`。

`NativeRegistration::method` 推导实际成员指针的签名。支持 bool/float/double/u32/i32 及底层 i32/u32 的独立枚举，参数使用 `ParameterPassing::value/read_only/reference`。参数模式、接收者只读性和 `ValueForm` 分别表达。C++ 记录/Array/String 还支持同步借用；原生成员适配要求实际 C++ 表示，共享签名解析器也接受不含 GC 引用的描述型记录。非 void 结果按值返回；内建数学记录与 plain struct 支持同步按值调用；资源记录与容器按值调用、对象引用、右值/volatile 引用和借用返回尚不支持。

`Scalar` 保存 bool/float/double/u32/i32 或带 TypeId 与 32 位数值的 `EnumValue`。`TypeInfo::enumeration` 保留底层类型、名称/数值、`flags` 位掩码标记（要求 `core.u32` 底层）以及与枚举项等长的 `EnumeratorMetadata`（`label` 与归一化 `description`），允许数值别名和未命名值，拒绝类型混用。`Runtime::types()` 返回当前类型 ID 快照；源码捕获枚举目录（含 `flags`）并记录依赖。

每个 Runtime 自动发布独立标量 `core.bool`，对应 C++ bool，大小和对齐均为 1 字节，初始化为 false。原生/描述型字段、值操作、反射、源码 bool 与 `Scalar{true/false}` 使用同一 TypeId；bool 不与 u32 的 0/1 值混用。bool 参数支持按值、只读引用和可写引用，async 入口只接受按值参数；结果沿同一成功后提交协议。生成代码的 SSA 布尔量用 `i1`，对象和借用存储用 1 字节表示；私有按值调用槽只允许 0/1，非法槽值或错误类型的成功结果按调用契约违约处理，不泄漏到业务值。

`Argument{Scalar{...}}` 拥有值，`Argument::borrow(value)` 借用原生变量，`Argument::field(object, path)` 借用字段及其路径。既有 Scalar 数组和新增 Argument 数组通过同一 `call`、`Entry::invoke`、`Lease::invoke` 入口校验。按值复制，const 引用可绑定临时存储，可写引用必须绑定可写地址。同一存储多次传参或与接收者字段重合时保留别名，不逐参数复制再写回。

`PreparedArguments` 持有定型暂存和 `ArgumentView`，参数视图前八项内联；标量、数学值与 plain struct 快照共用 128 字节、32 字节对齐的紧凑暂存区，各值按自身对齐排布，超出容量或要求更高对齐时按实际布局扩展。平凡析构快照不登记销毁回调，也不保留 Runtime 依赖。视图不能越过目标存储、调用及暂存拥有者的有效期。`Runtime::prepare_arguments` 为源码入口适配校验调用者自有存储，拒绝对象字段借用；字段地址仅在原生调用的生命周期/GC 保护范围内解析，离开后才执行源码后续步骤。可信原生代码不得捕获地址，或把逃逸对象内部指针伪装成宿主自有存储。

原生方法与独立源码签名接受不含 GC 引用的原生/描述型记录、Array 和 String 借用，`Runtime::prepare_arguments` 核对其 Runtime 类型身份、实际对齐和可写性，直接交付原地址。聚合值不进入 Scalar 暂存，源码 String 参数只允许只读。原生成员借用同样经过共享签名验证。

直接访问的权威信息归 `TypeInfo`：`native_storage` 标识原生表示，大小/对齐和数组 `element` 决定布局，`FieldInfo::offset` 表示相对声明类型的字段偏移，`TypeInfo::base_offset` 表示非虚原生基类子对象在派生类中的字节偏移。生成器从目标 ABI 的 `ASTRecordLayout` 为自然对齐字段与非虚基类提交字节偏移，Runtime 发布前校验范围和对齐；没有偏移的绑定保留既有访问器。`NativeLayout::sequence_view` 发布读取连续缓冲和长度的原语，基础库适配器实现 Array/String 的投影；它不得分配、修改存储、调用用户代码或重入 Runtime。

源码编译从上述元数据捕获可投影的值目录，数组按元素可用性接入；没有投影的类型，以及参数或声明结果使用这些类型的方法，保留为原生能力，不阻断无关源码。声明了结果但没有投影不等于 void，不能进入源码方法目录。目录绑定代码版本并在发布时核对依赖。生成代码按字段偏移/元素步长直接访问原生存储，序列访问先检查长度、对齐和边界。含聚合值借用、拥有型局部或 String 字面量的源码只允许同步执行，原生调用限于 `self` 上声明 `MethodInfo::preserves_borrows` 的方法，对象访问限于按值读取 `self` 字段；其他对象操作被拒绝，GC 留给返回后的宿主；清理与结果提交仍走既有入口。调用方负责存储存续期和结构稳定，不允许将 Runtime 对象内部地址伪装成自有存储。完整边界见[原生值借用](native-value-borrows.md)和[语言规则](../LANGUAGE.md#原生值借用)。

源码一般拥有型记录/String/Array 的值本体放在生成栈上，构造、复制、赋值、销毁使用同一 Runtime 值操作。`NativeLayout::value_operations_preserve_borrows` 表达操作不保留输入地址、不重入源码、不使其他仍使用的借用失效；平凡原生类型自动具备此能力，Array 依元素、描述型记录依字段递归组合。`sequence_mutate` 提供结构修改原语，`Runtime::mutate_sequence` 统一校验目标、输入类型和操作，再在原生回调保护下执行。前端只向本函数拥有的序列开放结构修改；按值调用和跨挂起生命周期不在此能力内。详见[拥有型基础值](source-owned-values.md)和[源码值字段组合](source-type-declarations.md#值与借用)。

async 参数只允许值传递；async 体可以在一次同步原生调用期间借用帧内局部。源码不提供保存借用或借用返回语法。只读约束只作用于该访问路径，可观察其他别名的写入，不授予独占访问。

`Runtime::call(object, method, arguments, Scalar* output = nullptr)` 保留 `Result<void>` 错误通道；非 void 方法缺少输出存储时在进入用户代码前返回 `invalid_storage`。Invoker 是 noexcept `Result<void>(CallContext&)` 回调，只把业务值写入初始为空的 `CallContext::result`。运行时先保留调用错误，再核对成功值与声明，最后提交 output；缺值、错类型或 void 方法伪造结果均失败。void 不修改可选的 output，任意失败保留调用方原值，但不回滚已经发生的业务修改。原生业务失败必须显式返回，不捕获异常；生命周期借用和准入保护保持原规则。

源码可以使用原生标量结果参与计算，也可作为语句丢弃成功值；丢弃不吞掉错误。嵌套实参按顺序求值并保留 SSA 值或独立聚合快照，后续调用不能覆盖先前实参或结果。引用字段的身份先求值保活，全部实参求值完成后才解析实际地址并盖必要的写代号。

源码的 script→native 路径从同一 C++ 签名生成定型 thunk，编译/发布核对签名、布局、借用效果与入口地址；逐次调用不再构造 Argument/Scalar 或检查可变结果标签。定型结果地址由调用方固定，thunk 成功时写入真实类型，失败沿固定布局状态/错误信息传播，已有宿主首因优先。数学/plain record 按值实参经独立对齐活值指针跨窄 ABI，由 C++ thunk 完成真实按值调用；结果暂存沿已有成功后提交与清理规则。私有 DLL 不交换 STL 所有权或 C++ 异常。动态 Invoker 的缺值/错类型检查保留在 Runtime::call，不是 JIT 的并行回退路径。

Invocation 在有效执行段内复用已解析身份、存储及对象索引；不缓存可扩容槽表的地址，也不缓存字段值。引用身份变化和协程恢复都要求后续访问重新验证；同步非调试函数在入口解析一次不可重绑的 `self` 存储。字段使用已发布偏移或固定取址/基类调整函数，不再逐次解析 FieldId 路径。原生调用仍持回调保护，禁止生命周期操作、GC 和生成代码重入；可写字段/借用/接收者的 stamp 时机不变。Runtime 元数据和提供入口的原生模块须在代码使用期间存活。

### 数学值调用与字段

Runtime 构造时通过同一注册机制发布内建数学记录，验证原生布局后设置只读的 `TypeInfo::math` 分类。构造是 noexcept：固定内建注册或布局失配、全进程 Runtime 身份耗尽属于不能继续的不变量失败，直接 fatal；外部类型注册仍返回 Result。普通 `TypeDefinition` 无法伪造这个标志；编译器根据权威身份与布局分类处理数学值，不凭名称猜测结构。类型、默认值和精度约定见[数学合同](math.md)。

`Scalar` 保持紧凑的标量/枚举表示。数学结果通过同一 `Runtime::call` 或 `Program::Entry/Lease::invoke` 的 `ValueOutput{TypeId, void*}` 重载提交；调用方提供该类型的活值及正确对齐存储。参数的数学值快照在调用前完成，返回值在受保护暂存区形成，完整成功后才写调用方结果。失败不提交结果，但已经发生的业务修改不回滚；如果输出故意与可变参数别名，参数修改仍然可见。

`Runtime::read(object, path, ValueOutput)` 和 `write(object, path, ValueInput)` 校验身份、路径、数学类型及对齐，整块复制数学值。源码对象字段与借用记录/数组中的数学值按已验证布局直接访问；对象字段另保留执行段身份验证、写代号和硬边界约束。数学临时值不建立对象或 GC 根，也不调用容器的构造/析构服务。

### 普通记录按值调用

`TypeInfo::plain_value` 表示记录递归只包含标量、枚举、数学值或其他 plain struct，复制与析构均平凡，不包含 GC 引用或资源。原生布局通过 `NativeLayout::plain_value` 声明自身平凡复制资格，再由 Runtime 验证字段；描述型记录由字段递归推导。它与 `value_operations_preserve_borrows` 独立：后者只承诺值操作不破坏借用，不能据此开放资源记录按值调用。

同步 plain struct 参数沿现有 `Argument`、`PreparedArguments` 和共享签名入口在执行前冻结快照；原生成员的定型 C++ thunk 根据真实参数与返回类型处理平台 ABI，JIT 不自行猜测寄存器分类。返回沿同一 `Runtime::call` 或 `Program::Entry/Lease::invoke` 的 `ValueOutput` 提交，要求调用方提供精确类型的活值和正确对齐。参数与结果暂存共用 `ValueStorage` 的内联容量和对齐扩展实现。源码内部返回值直接复制到独立结果槽，Program 的结果暂存仅保存返回字节；两者都不额外执行默认构造或析构。原生 Invoker 的输出仍沿已构造活值契约。完整成功后才复制到宿主输出，失败保留原输出，已发生的可变参数或业务修改仍可见。

`Runtime::read/write` 的 `ValueOutput/ValueInput` 也接受 plain struct，复用既有字段路径与类型身份。源码对象字段继续投影标量及数学值叶字段，不提供普通记录字段的整体读写语法。容器按值参数/结果和异步聚合边界仍拒绝含 String/Array、GC 引用或其他资源的记录。

## 正式对象单继承

`Runtime::object_type()` 返回本 Runtime 内建的 `core.Object`。它是可创建的空对象根，不要求 C++ 对象增加公共头部。对象定义未填写 `TypeDefinition::base` 时默认派生此根；显式基类用同一 `TypeReference` 解析，允许同批前向声明。`TypeInfo::base` 保存解析后的父类型。记录和标量不参与继承。

`NativeRegistration<Derived>::base<Base>()` 显式登记基类，并生成 `NativeLayout::base_address` 的 C++ 地址转换。未登记的 C++ 继承不会自动成为 Runtime 祖先。循环基类、非法父类型及祖先成员重名（含隐藏字段）在整批发布前拒绝，重名错误写出派生类型和声明该成员的祖先，例如 `Member hides an inherited declaration: game.Crate.value hides world.Base.value`；不实现成员遮蔽或覆盖。

| 派生 / 基类表示 | 布局与生命周期 |
| --- | --- |
| 原生 / 原生 | 完整派生对象由 C++ 构析一次，访问祖先时使用编译器生成的地址转换，不假定基段偏移为零 |
| 描述型 / 描述型 | 基段在前，自身字段随后按对齐追加；先基后派生构造，逆序清理 |
| 描述型 / 原生 | 实际构造原生基对象，在其完整 `sizeof` 范围外追加描述字段；不复用基类尾部填充，也不宣称具有 C++ 派生类 RTTI 或虚表 |
| 原生 / 描述型 | 除无负载的 `core.Object` 根外拒绝；不能把描述字段臆测为 C++ 基段 |

公开 `TypeInfo::fields/methods` 按祖先、本层的顺序给出有效成员；继承成员保留原 `FieldId/MethodId` 和原方法声明。`find_field/find_method` 返回这些声明身份。`read/write/call` 验证实际对象能否作为声明类型，再在 Runtime 内调整存储地址。进入内联记录后仍按记录类型访问，字段路径不跨对象引用。成员不适用返回 `invalid_member`，陈旧对象返回 `invalid_object`，失败发生在写入或方法执行前。

`MethodId` 固定选择已注册 `Invoker`；`CallContext::self` 是原对象身份，`storage` 是调整后的声明基段。原生实现内部的 C++ 调用不代表已经提供语言虚方法覆盖。GC 和引用枚举从对象槽中的实际类型追踪基类及派生的全部已登记边，包括隐藏字段；基类引用视图不会截断派生对象图。内部基段复用现有布局、构析和追踪遍历，不建立另一套对象存储。

### 类型关系与引用声明视图

- `is_a(actual, expected)` 对有效对象类型判断自身或祖先关系；无关类型返回 `false`，无效、跨 Runtime 或非对象类型返回 `invalid_type`。
- 对象槽的 `TypeId` 始终表示实际类型；`ObjectReference::description().type` 表示声明视图。`cast_reference` 保留 `ObjectId` 和强弱形式，不登记根。上转可保留陈旧身份；非空下转要求活对象满足目标类型，陈旧返回 `invalid_object`，不相容返回 `type_mismatch`。相关类型的空引用可转换，无关类型不能借空值绕过检查。
- `hold(id, declared_type)` 在存活和祖先校验后登记指定视图的根。原有 `hold(id)` 仍返回实际类型视图，`hold(reference)` 仍可保留合法空/陈旧强引用。`observe/try_acquire` 保持声明类型；弱引用提升无目标时仍返回空 optional。
- 引用字段允许派生声明视图赋给祖先目标，读取时恢复字段声明视图；下转必须显式转换。强字段不接受弱引用，弱字段可以接受兼容的强或弱引用。错误不修改原字段；所有强边写入仍经过原有屏障。
- `ObjectReference` 默认相等比较仍包含声明视图；跨视图判断同一对象使用 `id()`。`std::hash<TypeId/ObjectId>` 与各自完整身份相等规则一致，不暴露布局，也不能作为持久 ID。
- `belongs_to_context(id)` 只查询身份归属：本 Runtime 的存活或陈旧身份均返回 `true`，其他 Runtime 的身份及默认 `ObjectId{}` 返回 `false`。查询不分配、不保活且为 `noexcept`；存活与类型仍由 `type_of` 等访问入口验证。消费者可据此区分本地失效和跨 Runtime 输入，不读取身份私有布局或解析错误消息。

计划中的 Source 显式 cast 将使用 `cast_reference` 转换引用声明视图，当前尚未接入。

源码宿主可以将基类 `Program` 绑定到派生接收者。同步与协程共用接收者检查，`ExecutionFrame::hold(slot, id, declared_type)` 建立绑定类型视图的 `self` 槽；同步槽在执行区间内保存定型身份，协程槽保留根登记。成员目录及发布依赖包含祖先，元数据快照包含父类型。源码只能看绑定视图可见的成员，`new T()` 仍构造绑定类型，旧 `Lease` 和挂起帧继续持有各自代码版本。语法边界以 [LANGUAGE.md](../LANGUAGE.md) 为准。

此处不提供多继承、源码类声明、通用虚方法覆盖、布局热迁移或世界成员资格。继承目前只有 [JIT 验收](../acceptance/jit/business.cpp)中通过派生对象调用基类原生方法的一个代表。

## 引用序列与受保护原生访问

### 存储与字段定位

对象字段可用 `ValueForm::strong_sequence` 声明强引用序列，`ValueDescription::type` 是元素的声明类型，允许兼容的派生对象。原生成员使用不可复制、不可移动的 `ReferenceSequence`，通过既有 `field/hidden_field` 登记；描述型对象由 Runtime 构析同一存储。序列紧凑保存 `ObjectId`，不为每个元素登记外部根。记录字段、函数参数/结果及独立 `ObjectReference` 不接受此形式。

`reference_field(object, path)` 建立公开字段定位器；`reference_field(object, native_slot_or_sequence)` 在 `with_native` 期间按实际存储匹配已登记元数据，可定位隐藏字段、内联记录及原生基段。引用强弱和元素类型来自字段定义，调用方不能重贴标签。`ReferenceField` 拥有字段路径，不保活对象或保存载荷地址；每次访问重新验证对象身份。原生存储定位器仅可在受保护借用期间使用，离开借用后返回 `busy`。

`read_reference`、`sequence_size` 和 `sequence_read` 使用同一定位器。元素读取恢复字段声明视图；空值及显式销毁后的失效身份沿用普通强引用规则。

### 同步原生借用

`with_native<T>(object, callback)` 验证实际对象与已注册原生类型的继承关系，在 Runtime 内调整基段地址，再传入 `T&`。描述型对象的原生基段也可访问。回调返回 `Result<R>`；`R` 必须拥有返回数据，直接返回指针、引用、`span`、`string_view` 或 `reference_wrapper` 会被模板约束拒绝。

借用期间可嵌套借用及执行合法字段修改，创建、销毁、GC、同步 JIT 进入和协程启动被拒绝。`with_native` 是 noexcept 入口，回调不得抛异常；业务失败原样返回 Result，不映射为 `invocation_failed`。成功或失败返回时均释放借用保护，不回滚回调已发生的副作用。受信任 C++ 调用方仍须保证 Runtime 覆盖整个调用，并且不通过捕获或包装类型带出借用地址；模板约束不构成原生代码沙箱。可变的 `T` 在借用开始时给对象盖上[写代号](#写代号与变化游标)；`const T` 不盖章，回调不得通过它写入对象存储，`const_cast` 也不行，否则这次写入不会被消费者看到。`const T` 不承诺整个对象图不可变：经它调用的其他入口照常按自己的规则盖章。

### 成组修改与 GC

`apply_reference_changes` 是同步成组入口，接受 `SequenceSplice`（位置、删除数量、插入引用）和 `ReferenceWrite`。同一目标序列最多一个 splice，同一引用槽最多一次写入；可在一批中修改多个对象。它先验证全部身份、字段、类型、强弱、范围及目标重复，再复制插入引用、准备全部容量，最后提交逻辑修改。非法末项也不会留下前面已生效的写入。准备阶段的分配失败可向调用方传播，逻辑内容保持不变，已准备的容量可能保留；提交阶段不分配、不调用用户代码，也没有普通失败出口。它不回滚回调中的其他业务副作用。

单槽写入与成组写入共用类型/强弱校验和强边写屏障。成组操作不开放长期 prepared 句柄；原生借用保护生命周期，成组准备期间另拒绝修改重入。

GC 暂停时仅保存稳定序列头和元素下标，不保存可扩容数组中的元素指针；每个元素占一个追踪工作单位。序列修改会取消所属对象的活动追踪游标，并把已标记对象显式重新加入已有灰队列，确保删除移位后未扫描的元素和后续字段仍会被访问；新增强边也执行屏障。重新排队不增加对象标记计数或分配节点。时间预算仍是既有软上限，单个原生析构和序列存储释放不因此成为可抢占操作。

上述接口只提供 Runtime 引用图与借用机制。成员资格、owner 的强弱、世界关闭和离开世界后的写入规则由后续消费者确定；源码尚未开放对象引用序列操作，无 GC 引用的 Array 值已可同步使用。引用序列目前没有专项端到端验收。

## 字段契约

### 整值读写与调用方存储

`read/write(…, ValueOutput/ValueInput)` 接受 plain 值，以及可复制构造与赋值、值操作保持借用、不含 GC 引用的值，即 String、Array 和由它们组成的记录。含 GC 引用的值仍只能经 `read_reference/write_reference` 写引用字段。

- plain 值整块复制。其他值先复制构造到临时值，在临时值上完成硬边界夹值，再与目标做 noexcept 交换：复制、分配或夹值前的 NaN 检查失败时，字段保持原值。读出时同样先复制再交换，失败时输出保持原值。交换来自 `NativeLayout::swap`（`NativeRegistration` 在类型 nothrow 可交换时自动提供），描述型记录由字段的字节交换和原生交换组合而成。没有 noexcept 交换的类型退回复制赋值，只有基本保证。
- 对象目标上替换非 plain 值会释放旧缓冲，所以在回调、原生借用和引用遍历期间返回 `busy`；plain 值和标量写入不受影响。
- `read/write(TypeId, storage, path, Scalar)` 与 `read/write(TypeId, storage, path, ValueOutput/ValueInput)` 对调用方持有的活值做同样的访问，与 `read_reference/write_reference(TypeId, storage, path)` 对称。存储规则与[值操作入口](#已实现的值操作入口)相同：调用方提供已构造的活值，Runtime 检查类型、容量与对齐。路径解析、精确类型、硬边界和元素选择子与对象目标共用同一份实现；写入只在引用遍历或批量引用准备期间返回 `busy`，因为存储归调用方所有。
- `restore(object, path, ValueOutput previous)` 撤销调用方自己刚做的写入。前提由调用方保证，Runtime 无法检查：`previous` 是调用方在本次操作中、写入之前从同一字段读出的值。它把字段与 `previous` 交换，不复制、不分配；之后 `previous` 里是被替换掉的值，由调用方销毁。它跳过硬边界、有限性检查和类型校验器，所以被 `with_native` 或生成代码写成越界值或 NaN 的旧值按位放回。路径（包括数学向量分量和数组元素选择子）、精确类型、对象存活和 `busy` 规则与整值写入相同，标量叶用叶的原生类型。含 GC 引用的值返回 `type_mismatch`；非 plain 且没有 noexcept 交换的类型返回 `unsupported_operation`，因为退回赋值就不能保证不失败。任何失败都不改动字段和 `previous`。`restore` 按一次写入处理，同样给对象盖[写代号](#写代号与变化游标)。

### 字段展示元数据

`FieldDefinition` 和 `FieldInfo` 的 `label/category` 是可选的拥有型字符串。`register_types` 将公开字段的文本移入 `TypeInfo::fields`；原生和描述型定义共用此路径，继承时随字段描述复制并保留原声明的 `FieldId`。描述副本独立拥有文本，输入定义销毁后查询仍有效；开销归类型元数据，不添加对象实例字段。隐藏字段仍不进入公开目录。

缺省均为空，由消费者决定显示字段名和未分类。实际字段名继续负责查找、源码绑定和序列化键；展示名称可以重复，类别不创建类型或改变命名空间。两项不参与字段偏移、大小、对齐、值操作或 Source 的 ABI 依赖比较，修改展示文本不会被视为值布局变化。元数据仍通过既有类型发布入口建立，不新增编辑器字段表或活动类型原位更新接口。

C++ 生产者使用 `AFIELD(Label = "Position", Category = "Transform")`，规则见[自动反射](reflection.md#声明写法)。描述型生产者直接填写 `FieldDefinition`；`.source` 类字段用同名属性填写，见[语言规则](../LANGUAGE.md#项目快照与原生派生类)。

### 字段扩展元数据

`TypeDefinition::metadata` / `TypeInfo::metadata`、`FieldDefinition::metadata` / `FieldInfo::metadata` 以及方法参数 `parameter_metadata` 均为拥有型字符串键值列表（`MetadataList`）。键由至少两段 ASCII 标识符以点分隔组成（每段首字符为字母或下划线，其后可带数字）；值为解码后的有效 UTF-8，允许空值、控制字符和内嵌零字节，按完整长度保存。发布按键排序、拒绝同一载体上的重复键；输入销毁及 `TypeInfo` 复制不使文本悬空。继承字段的复制保留原 `FieldId` 和字段元数据，隐藏字段不进入公开目录；类型自身的 `TypeInfo::metadata` 不合并到派生类型，消费者按需沿 `base` 链查询。同一描述型对象类型上的可调用 `methods` 与契约声明 `described_methods` 之间同样检查成员重名；`TypeInfo::described_methods` 只保存首次注册时冻结的契约方法声明，兼容热更新增或改参数名的可执行入口以代码宿主（`source::Program::entries()`）为准。

Runtime 只拥有和校验载体，不解释消费者的命名空间、绑定对象或算法。元数据不增加实例字段、不改变存储布局、不写入场景格式，但属于不可变的类型定义，进入 Source 发布依赖快照。Source 类字段的 Meta 内容变化要求重建上下文，排序和格式空白变化不要求重建；原生定义仍通过重新构建和注册建立。原生与源码语法分别见[字段 Meta](reflection.md#字段-meta)和[原生派生类](../LANGUAGE.md#项目快照与原生派生类)。

### 字段默认值

`FieldDefinition::default_leaves` 只用于描述型对象的内联标量、枚举和 f32/f64 数学向量字段：按存储顺序每个分量一个叶，类型精确等于叶标量，向量必须给齐全部分量，枚举给底层 i32/u32 整数（同批暂存的枚举也可）。`Runtime::create` 在字段自身构造之后按分量偏移写入这些叶；复制构造复制源对象，原生字段由 C++ 构造函数负责。`FieldInfo::default_leaves` 发布同一组叶，枚举叶改为 `EnumValue`。数量或类型不符、原生字段、记录及四元数、矩阵字段带默认值时发布失败；浮点叶无论字段有没有边界都必须有限，有硬边界时每个叶还须落在边界内。默认值属于已发布定义。

`default_text` 是仅用于描述型对象内联 String 字段的拥有型文本默认值，同时保存在 `FieldDefinition`、`FieldInfo` 和内部构造数据。省略时规范化为空文本；发布验证 UTF-8，不进入 `Scalar/default_leaves`。构造使用一次真实引擎 String 文本构造，完成后才计入字段清理栈；失败只逆序销毁已完成字段，不能在未登记的空 String 上再做可抛异常赋值。复制路径复制原值而非重新应用默认值。原生字段仍由 C++ 构造函数负责，其他字段或原生定义带 `default_text` 会被拒绝。文本默认值进入 Source 发布依赖快照，改变它要求重建，不改动旧对象。

### 成员选择子与数组元素

`FieldId` 是成员选择子，表示所属类型的第 index 个成员：记录和对象的成员是字段，`Array` 类型的成员是元素。因此字段路径可以穿过数组元素，例如 `[volumes, element(VolumeArray, 2), extent, x]`。`element(TypeId array, index)` 构造元素选择子；`element_index(FieldId)` 对元素选择子返回下标，对字段选择子返回空。

- 数组类型没有字段，`find_field` 对它返回 `invalid_member`。Runtime 内部按 `FieldId` 取字段元数据的位置（路径解析、引用字段定位、校验路径命名）都先区分两种选择子：数组值只接受本数组类型的元素选择子，元素选择子只能用于该数组类型的值，越界下标在解析时返回 `invalid_member`，不改动数据。Runtime 没有按 `FieldId` 直接取 `FieldInfo` 的公开接口；消费者按 `TypeInfo::fields` 比较身份时，元素选择子不会匹配任何字段，应先用 `element_index` 判断。
- `read/write`（`Scalar` 与 `ValueInput/ValueOutput`）和 `constrain` 都接受元素路径；`constrain` 不需要对象，因此不检查下标。
- 元素选择子是位置性的，只在数组至少有 index + 1 个元素时有效。插入、删除或移动后，指向受影响位置的路径会指向别的元素或失效。编辑器撤销按逆操作的相反顺序执行即可还原：添加对应删除，删除对应带值插入，移动对应反向移动。
- 元素选择子的下标是 32 位。数组超过 2^32 个元素时，超出部分不能按路径读写，只能整体访问；校验和硬边界检查仍逐元素进行，失败报告指到数组本身。
- 结构操作：`element_count`、`insert_element(object, path, index, ValueInput)`、`remove_elements(object, path, first, count)` 和 `move_element(object, path, from, to)`。所有检查先于改动，失败时数组不变，包括元素复制抛出异常的情况；元素的移动或交换可能抛出时，操作在副本上完成后再交换。插入值用元素类型的复制构造，不要求 plain；空 `ValueInput` 默认构造元素，默认构造视同原生构造，不夹值。插入的副本按硬边界夹值，NaN/±Inf 在改动前拒绝。回调、原生借用和引用遍历期间返回 `busy`，借用中的元素地址因此不会因结构变化而悬空。
- 源码前端不绑定对象的数组字段，元素路径不进入源码绑定目录，也不影响生成代码 ABI。
- 按路径写入和元素操作的失败消息与校验失败一样以“类型名.路径: 原因”命名所在值：越界下标写出下标和元素数，结构操作越界写出操作、位置和元素数，硬边界拒绝 NaN/±Inf 时指向输入里第一个违规叶，例如 `arrays.Owner.points[1].y`。只有失败才构造路径，成功路径不分配。
- Array 类型发布时必须提供元素投影 `sequence_view`（`array_definition` 已提供）；缺少 `sequence_mutate` 的数组可以按元素读写，结构操作返回 `unsupported_operation`。

计划中的 Parameters 数组编辑器（R5-6b）将使用 `insert_element` 添加元素及撤销删除。`remove_elements` 计划用于删除元素及撤销添加。`move_element` 计划用于调整元素顺序及逆向撤销。`element_index` 计划用于解释元素路径；这些消费者当前尚未接入。

### 字段范围与写入约束

`FieldDefinition::range` 与 `FieldInfo::range` 是 `FieldRange{ui_min, ui_max, hard_min, hard_max}`，四个可选 `Scalar`。UI 范围只供编辑器滑条和数值阶梯使用，不约束写入；硬边界由 Runtime 在自己的写入路径上强制执行。C++ 写法见[自动反射](reflection.md#字段范围与硬边界)。

- 发布：范围只允许出现在 f32/f64/i32/u32 内联字段、f32/f64 数学向量字段以及这些类型的 Array 字段上，向量按分量、数组按元素生效。边界必须有限，且是叶标量的精确类型；`ui_min ≤ ui_max`、`hard_min ≤ hard_max`，每个 UI 边界落在硬边界内，描述型默认值的每个叶满足硬边界（默认值叶本身必须有限，见[字段默认值](#字段默认值)）。任一项失败时整组发布失败。继承字段保留声明的范围。范围属于已发布定义，同类型不能改范围。
- 写入：`write(Scalar)` 和 `write(ValueInput)` 只看硬边界。越界值夹到闭区间边界；只要存在任一硬边界，NaN/±Inf 就以 `argument_mismatch` 拒绝。整值写入先检查输入中所有受约束叶，再改动任何字节，然后夹值（非 plain 值在交换前的副本上夹值），输入本身不变。覆盖范围包括标量字段、数学向量整值与分量、数组元素、带范围数组的整值，以及内含受约束字段的记录整值（发布时为类型预计算受约束叶路径）；`insert_element` 的副本和调用方存储的写入遵循同一规则。场景反序列化使用这些入口；源码字段写入根据已绑定范围生成等价的边界检查和夹值，不再逐次查路径。
- 预演：`constrain(TypeId, path, Scalar)` 不需要对象，返回同一次写入会存储的值，或同样的拒绝。编辑器预显示与场景加载诊断共用它。
- 借用：对受约束字段、内含受约束叶的记录字段以及元素受约束的 Array 字段，`Argument::field` 的可写引用实参以 `argument_mismatch` 拒绝，只读引用和按值仍可使用。源码前端在编译期给出同样的诊断，绑定目录为每个字段记录 `constrained`，数学分量继承所在向量的标记。
- 可信直接访问不检查：`with_native`、`borrow_native`、原生方法的 `this` 和公开偏移的直接存储。编写作者数据的宿主路径应改用 Runtime 写入，或先调用 `constrain`。

字段锁只处理单个标量的边界。四元数归一化等跨字段规则、形状规则、"唯一 Sky" 这类跨对象规则，以及写入后的派生缓存失效，都不属于字段锁：跨字段规则由下面的类型校验器负责，跨对象规则留在拥有这些对象的系统中。派生缓存用[写代号](#写代号与变化游标)找出输入对象中被写过的那些，只对它们重新读取和校验；输入不在对象上（例如调用方持有的值）时，仍携带输入键并在使用处校验。

### 写代号与变化游标

Runtime 为每个对象记录最近一次写入时的代号。消费者用游标查询"上次以来哪些对象被写过"，不必每帧比较所有对象的字节。

- 盖章的入口：`write`（Scalar 和 ValueInput）、`restore`、按对象路径的 `write_reference`、`apply_reference_changes`（盖在每个被改的所属对象上）、`insert_element/remove_elements/move_element`、可变 `with_native`（整个对象或按路径借字段）、接收者不是 read_only 的原生方法、可写的 `Argument::field`（盖在字段所属的对象上，不盖接收者），以及 `create`。交出可写访问就算写入，Runtime 不比较字节；路径解析成功之后被拒绝的写入也可能盖章。可信原生槽上的 `write_reference(ValueDescription, ReferenceSlot&)` 和调用方存储上的值操作没有对象身份，由取得这块存储的可变借用盖章。
- 不盖章：`with_native<const T>`、各种 `read`、`read_reference`、`element_count`、`validate`、read_only 接收者的方法和只读字段实参。只读访问应使用这些入口；const 借用不得写入，见[同步原生借用](#同步原生借用)。
- 查询：`change_cursor()` 返回当前代号并开始下一个代号，之后的写入代号都比它大。`changed_since(object, cursor)` 在对象于游标之后被写过，或者它已经不是这个存活对象时（销毁、回收、槽位复用、外来或空身份）返回 true。`changed_objects(cursor, out)` 按槽位顺序列出游标之后被写过的存活对象：每 4096 个槽位设一级超页最大代号摘要，每 64 个槽位设记录最近两轮精确 64 位脏掩码的二级写页摘要，未变化的超页与页直接跳过，近两轮游标内的稀疏修改按精确置位直达槽位；已销毁的对象不在列表里，按身份查询时报告为变化。
- 游标协议：消费者先取游标，再读取。同时是写者的消费者（例如物理先回写位姿，再在下一步查询外部改动）可以在自己的写入结束之后取游标，前提是从读取到取游标之间没有别的写者。回调、原生借用、引用遍历和生成代码执行期间取游标返回 `busy`：借用和方法调用在开始时盖章，期间的写入沿用这个代号，游标不能把它们切开。
- 粒度是整个对象：任何字段的写入，包括隐藏字段、引用和数组结构，都会标记整个对象；无关字段造成的误报由消费者比较后过滤。成员和关系的变化仍由消费者自己的版本（例如 `World::revision`）负责。
- 归属：代号是运行期状态，不进入 `ObjectId`，不持久化，源码不可见；每个 Runtime 独立，没有跨实例的全局状态。每个对象槽位占 8 字节写代号，每 64 个槽位占 40 字节二级页摘要，每 4096 个槽位占 8 字节超页摘要；读取路径不变。
- 不变量：所有对象可写访问路径，包括生成代码直接存储对象字段、批量执行和源码借用对象字段，都必须盖章。生成代码在首次写入或交出可写借用前通过 `NativeContext::stamp` 为对象盖章。同步非调试代码可复用同一执行段内同一 `self` 的标记。执行期间无法推进变化游标，因此同一段内重复写入对观察者属于同一个代号。
- 验收：写代号目前没有专项端到端验收；不再展开写入口、拒绝或跨页规模矩阵。

### 类型校验

`TypeDefinition::validate` 是记录或对象类型可选的 noexcept 校验函数 `Result<void>(const void* storage)`，原生注册写作 `NativeRegistration<T>::validate<&F>()`，`F` 是接受 `const T&` 的自由函数或 const 成员函数。校验器是纯函数：只读一个值的字节，不调用 Runtime，在回调保护下执行；拒绝必须显式返回，不得抛异常。Runtime 保留原校验报告合同，将拒绝文本附上字段路径并报告 `argument_mismatch`，不把它当作一次方法调用失败。校验器属于已发布定义，枚举、标量和容器类型不能声明。

`validate(ObjectId)` 与 `validate_value(TypeId, storage)` 只检查，不修改，也从不在写入时自动运行。调用方是呈现提取、物理协调、编辑器事务提交和场景加载；对象上的输入用[写代号](#写代号与变化游标)找出需要重新校验的那些。检查顺序为：

1. 所有受约束叶的硬边界，包括每个数组元素的叶，只报告、不夹值，用来发现可信 C++ 直接写入造成的越界；
2. 类型校验器按后序执行：基类段视为第一个组成部分，然后是按声明顺序的内联记录和数组字段，各自由内到外，数组元素按下标升序，类型自身的校验器最后。因此元素先于所在记录，继承链从基类到派生类，最派生类型的校验器最后执行。

这样每个校验器都可以假定它的组成部分已经通过，例如限制元素数的记录校验器运行时，每个元素已经通过自身检查。引用序列和引用目标不遍历。发布时为每个类型预计算受约束叶和校验步骤，数组只在元素类型有规则时登记一项并在校验时展开；没有规则的类型直接返回。通过的值只走一遍不带路径的检查，不分配；失败时再带路径重走一遍来报告，所以失败值的校验器会执行两次，这也是校验器必须是纯函数的原因之一。

外层 `Result` 只表示对象、类型或存储无效；内层 `optional<ValidationFailure>` 为空表示通过，否则只报告第一个失败：

- `field`：硬边界失败给出叶路径，类型校验器失败给出该记录或元素的路径；对象自身或其基类的校验器失败时为空。继承字段使用声明类型的 `FieldId`，数学分量使用向量类型的分量 `FieldId`，数组元素使用元素选择子。
- `names`：同一路径的名称，元素写作 `[2]`，可以直接填入诊断。
- `value`：硬边界失败时的越界叶值。
- `error`：可读消息，形如 `validation.Derived.settings.gain: 5 is above the hard maximum 1` 或 `arrays.Owner.probes[1].gain: 7 is above the hard maximum 1`。

### 类型与值的使用方式

同一个对象类型可以被持有引用或观察引用指向，字段分别描述目标类型和保存方式。字段与函数参数/结果元数据复用以下值描述；实际参数及结果范围统一见[共享函数签名](#共享函数签名与结果)。值形式不等同于传参借用方式：

```cpp
enum class ValueForm { inline_value, strong_reference, weak_reference, strong_sequence };

struct ValueDescription {
    TypeId type;
    ValueForm form{ValueForm::inline_value};
};
```

合法组合：标量、枚举和记录使用 `inline_value`，对象目标使用两种单引用形式；对象字段还可使用 `strong_sequence` 保存定型强引用序列。这里的引用仅指对象引用，值的借用由 ParameterPassing 独立描述。引用字段存储引用值或序列，其大小不等于目标对象实例的大小；布局由值描述及对应的存储规则决定。

字段负责名称、所属类型、值描述及定位信息。值描述存于共享元数据，不要求未来每个字段值附带运行时类型标签。描述型字段保留计算偏移，原生字段保留成员地址适配。参数复用值描述，但参数布局和调用 ABI 不因此变成字段布局。

### 已实现的成组类型发布

`TypeReference` 是定义输入中的类型选择器，可以持有本运行时已发布的 `TypeId`、完整声明名称，或由 `TypeReference::native<T>()` 生成的 C++ 表示标记。名称按精确字符串匹配同批和已发布定义，不做短名回退或语言作用域解析。`ValueDefinition` 组合该选择器与 `ValueForm`；发布后的 `ValueDescription` 只保留已解析的 `TypeId` 与值形式，不创建第二套业务类型身份。

```cpp
std::vector<TypeDefinition> definitions(2);
definitions[0].name = "sample.A";
definitions[0].fields = {
    {"next", {TypeReference{"sample.B"}, ValueForm::strong_reference}}
};
definitions[1].name = "sample.B";
definitions[1].fields = {
    {"previous", {TypeReference{"sample.A"}, ValueForm::weak_reference}}
};
auto published = runtime.register_types(std::move(definitions));
```

`register_types` 返回按输入顺序排列的类型标识；空批次拒绝。先在事务内解析全部类型身份，校验字段、方法与原生表示，再按内联依赖完成布局和能力汇总。内联边与容器元素依赖参与环检查，持有/观察引用边不递归展开目标实例。所有可能分配存储的准备工作完成后才提交；失败不公开临时身份、名称或 C++ 表示映射，已发布的元数据保持有效。单条 `register_type` 复用同一个事务入口。

`prepare_types` 在这个唯一注册边界验证类型名、字段名、方法名、参数名、枚举项名以及字段 label/category 的 UTF-8，失败返回 `invalid_definition`。合法 Unicode 名称、原空值和重名政策不变；已发布的这些文本可由显示层直接使用 Platform 已验证转换入口，不在消费方重复校验。

`NativeRegistration<T>::build()` 消费定义生产者并交出 `TypeDefinition`，可与描述型定义一起提交；默认 `field` 通过 C++ 表示延迟解析，支持同批尚未发布的原生记录。显式 `field<成员指针>(名称, ValueDefinition)` 可指定同批目标和值形式，发布时仍校验成员的实际 C++ 表示。原生构造和用户方法不在发布过程中执行。

引用字段按 `ReferenceSlot` 的尺寸和对齐布局，原生引用字段也必须提供相同的 C++ 表示，裸 `ObjectId` 成员不是引用槽。`TypeInfo::contains_references` 从所有已声明字段及内联依赖传播，包括 `hidden_field`，用于引用遍历；它不限制实例化或复制能力。原生代码仍须完整报告引用，运行时不会从任意 C++ 裸指针推断持有关系。

### 拥有容器值

`TypeKind::string/array` 已接入原生值生命周期；数组的 `TypeInfo::element` 指向权威元素类型。元素必须可复制构造且无 GC 引用，包含隐藏引用的记录、递归容器依赖及不匹配的 C++ 元素表示在发布事务中拒绝。原生与描述型对象都可以内联存放这些拥有值，GC 回收对象时析构其缓冲与元素。

`with_native<T>(object, path, callback)` 复用同一原生借用保护来访问内联值字段，不改变标量 read/write 或 Runtime::call 的参数范围。StringView/Span 为借用描述，不能作为持久反射字段或直接从借用回调返回。完整合同见[基础库](foundation.md)。

### 已实现的定型引用与外部根

| 表示 | 职责 | 不承担的职责 |
| --- | --- | --- |
| `ObjectId` | 保存运行时内的对象身份与代际 | 不证明静态类型，不登记持有 |
| `ReferenceSlot` | 保存字段中的目标身份，默认空；目标类型与形式在共享元数据中 | 不逐字段注册外部根，不给每个字段附加类型标签 |
| `ObjectReference` | 在接口间传递目标类型、持有/观察形式和身份；允许复制失效身份 | 单独保存在 C++ 变量中不会自动登记外部持有 |
| `ObjectRoot` | RAII 外部对象根；复制增加独立登记，移动转移登记 | 不阻止显式销毁，不延长 Runtime 本身的生命 |
| `ValueRoot` | 在明确存续期内借用并登记调用方的活值存储，支持移动 | 不拥有存储，不自动析构该值 |

`create(TypeId)` 返回 `Result<ObjectRoot>`。对象与首个根在交付前一起完成发布；创建失败不遗留本次创建的对象或根，原生回调对其他对象的外部副作用仍不自动回滚。`hold(ObjectId)` 检查活对象并建立定型根；`hold(ObjectReference)` 只接收持有形式，允许保留已经失效的身份而不复活目标。

`observe` 将定型引用转换为观察形式，不建立根。`try_acquire` 只接收观察形式，返回 `Result<std::optional<ObjectRoot>>`：空或失效目标产生空结果，活目标交付已经登记的根；错误类型或跨运行时引用报告错误。弱引用不能直接写入持有字段，须先成功取得根。持有引用可写入同类型观察字段，或先通过 `observe` 转换。普通空/失效取得不抛异常；根登记分配失败仍遵守现有内存耗尽边界。

`read_reference/write_reference` 支持对象字段路径和调用方的记录存储。路径只穿过内联记录，不隐式解引用对象；已有定型引用的同形式复制不要求目标仍然存活。可信原生代码可用 `ValueDescription + ReferenceSlot` 重载访问未公开成员，但必须提供该槽真实的声明描述并保持宿主存储有效。引用槽不能通过任意原生赋值绕过 Runtime；整条原生记录复制通过四个值操作入口执行。

`root_value(TypeId, storage)` 登记已经构造且容量、对齐正确的标量或记录存储；只使用类型大小对应的字节。调用方须保持存储地址及活值状态有效，撤销全部重叠登记后才能重建或销毁该值。赋值允许在登记期间更新内容，根始终指向同一份存储。此接口仍是可信原生接口，不验证任意字节是否真的处于构造状态，也不接管运行时对象内部存储的所有权。

存储重叠检查只遍历紧凑的活跃 `ValueRoot` 索引，复杂度随当前活跃值根数量变化，不随对象根数量或历史根登记峰值增长。撤根使用末项交换移除并更新反向位置，不移动借用的值存储，不改变根句柄身份。GC 的根阶段按工作单位扫描登记项。

`ObjectRoot` 的 C++ 复制是 noexcept，不提供可恢复失败通道；在引用遍历中复制或复制时根身份表示已耗尽，直接 fatal。需要处理登记拒绝时使用 `hold`；`create/hold/try_acquire/root_value` 保留原有 Result 准入与容量检查。根表自身不再抛异常。

根句柄持有独立的根登记表，Runtime 结束时使该表失效并照常销毁对象；迟于 Runtime 释放根句柄不会访问已销毁的 Runtime。这里的内部共享所有权只保护登记表，不是用 `shared_ptr` 管理业务对象。`ObjectRoot::registered()` 仅表示登记所属 Runtime 仍存在，不能替代目标有效性检查：对象可能已被显式销毁，失效身份也可以继续登记。根撤销不立即销毁目标；宿主继续推进收集后，不再可达的对象才进入回收。

`visit_roots` 枚举外部入口；`visit_references` 枚举单个对象或外部值中的引用，递归内联记录而不继续遍历所指对象。访问者收到非空的持有和观察引用，也可能收到已失效身份；收集器应据形式与代际判断后续处理。`hidden_field` 只控制公共反射字段的暴露，不是语言访问权限机制；它仍参与引用遍历。

遍历期间拒绝 Runtime 的对象/值/字段修改、方法执行和新增根。`ReferenceVisitor::visit` 是 noexcept Result 回调，失败保留原错误并立即结束枚举，正常成功或失败返回后都解除保护；不捕获异常。C++ 根复制会新增登记，因此不能在访问者中执行；Root 的撤销不销毁目标，允许解除登记，但访问者必须始终保持借用的值存储有效，不能绕过接口释放或改写该存储。普通方法回调中的合法引用写入、根登记和观察取得仍可执行。

计划中的根泄漏排障将使用 `visit_roots` 观察外部根入口，当前尚未接入产品排障入口。

### 值操作

| 操作 | 必须表达的行为 |
| --- | --- |
| 初始化 | 在未构造的存储中建立合法值；不预定所有类型都能默认构造 |
| 复制构造 | 在未构造的存储中建立一个值副本 |
| 赋值 | 替换已构造值，维护原值与新值的引用关系；不隐式克隆目标对象 |
| 销毁 | 结束该值的生命周期及持有关系；销毁引用值不等于显式销毁目标对象 |
| 枚举对象引用 | 给引用管理提供该值包含的对象关系及其持有/观察性质 |

这些是行为契约，不要求逐字段存储一套函数指针或每次操作动态分派。操作可按类型与值形式共享，平凡值保留直接处理路径。复制失效句柄仍遵守已确认的值语义，不能借复制将其重新解析成另一个对象。

原生对象仍由其 C++ 构造和析构负责自身字段，描述型对象由运行时执行字段操作，避免对同一字段重复构造或释放。原生对象中的托管引用仍须提供完整的引用信息，即使字段不对编辑器公开。原生裸指针本身不足以说明是否保活目标。

### 已实现的值操作入口

当前接口服务引擎内部和可信原生调用方，操作标量及记录，包括含持有/观察引用的记录。调用方提供存储，运行时不为值分配存储或创建对象身份；需要作为外部持有入口的活值另外登记 `ValueRoot`：

```cpp
Result<void> initialize_value(TypeId, std::span<std::byte> destination);
Result<void> copy_construct_value(TypeId, std::span<std::byte> destination,
                                  std::span<const std::byte> source);
Result<void> assign_value(TypeId, std::span<std::byte> destination,
                         std::span<const std::byte> source);
Result<void> destroy_value(TypeId, std::span<std::byte> storage);
```

运行时在修改前检查类型、容量、对齐、操作能力及重叠范围。每个缓冲区只使用 `TypeInfo::size` 字节；复制构造禁止重叠，支持赋值的类型允许原址自赋值并直接成功，部分重叠则拒绝。调用方保证源值已构造、目标处于操作要求的构造状态、实际 C++ 表示正确，以及存储有效；这些条件无法仅凭字节视图验证。调用方须在所属运行时结束前销毁值，运行时不会自动清理外部值。已登记的外部值禁止重建和销毁，须先撤销其根登记；复制引用值不自动新增根或克隆对象。

`TypeInfo::copy_constructible` 与 `copy_assignable` 分别表达两种能力。原生记录依据其 C++ 操作生成适配，描述型记录在发布时汇总全部内联依赖的能力，引用槽本身支持身份复制。缺少能力时，整个操作在开始前拒绝，不以字节复制替代其语义。当前已注册值均可默认构造和无异常销毁，这不是未来所有类型的永久要求。

失败目标状态、清理责任和错误分类统一见[原生值生命周期目标](#原生值生命周期目标)。原生操作仍处于现有回调保护范围内，不能创建、销毁运行时对象或注册类型。值操作本身不关闭后续类型注册。部分赋值失败后仍对目标内已写入的持有引用执行写屏障，不能因返回错误漏掉本轮 GC 保活。

描述型构造、复制、赋值、销毁及同步引用遍历使用显式可复用工作栈，不以 C++ 调用栈承载内联布局深度。发布前按执行计划预留最大遍历深度；回调重入的公开值操作在修改前预留新增深度，并以独立栈区间隔离。原生回调可能扩容工作栈，因此循环不跨回调保留栈元素引用。构造失败复用已有容量逆序清理已完成部分，不重复清理返回失败的原生目标。

Runtime 私有类型表、对象槽、写代号页、字段/调用/遍历计划、类型发布候选目录及引用批事务缓冲使用 Array。Type 仍独立分配，容器增长不移动类型对象；对象槽保留代际和自由链。引用批修改仍在全部验证和候选复制后才预留目标容量并提交，预算拒绝返回 Result。公开类型定义、诊断路径、ReferenceSequence 与 PreparedArguments 的现有容器和借用接口不因此改变。

发布时派生需要析构的字段计划：内建标量、枚举、`NativeRegistration` 自动识别的 C++ 平凡析构类型（`NativeLayout::trivially_destructible`）及仅由它们组成的描述型子树无需逐字段调用空析构；非平凡 C++ 类型与任何未标记平凡析构的显式原生 `destroy` 仍在原地址、原顺序逆序执行一次，不能以“不含引用”代替平凡性判断。持有引用追踪与析构计划相互独立。

无填充同构标量描述型布局走标量块快路。由标量、枚举、数学值、可复用默认值的 plain 记录和 `ReferenceSlot` 组成的描述型对象或记录，可以在发布时生成对齐的默认值原型（含 `default_leaves`），供默认构造、复制构造和非重叠赋值整块复制。含引用的复制仍执行标记期写屏障。原生字段只有在 `NativeLayout::prototype_constructor` 与当前构造回调一致、且类型满足平凡复制与析构条件时才能进入原型。`NativeRegistration` 自动识别平凡默认构造，内建数学类型显式确认其固定默认值。替换构造回调后按实例调用新回调；值操作的借用资格独立判断。快路的对象生存期依据见[取舍依据](#取舍依据)。

## 已实现的分步回收

采用单线程、非移动的增量标记—清扫，复用统一类型表、对象代际身份、`ObjectRoot`/`ValueRoot` 和引用写入入口，不建立另一套对象堆或持有体系。宿主只在当前运行时线程的安全点推进；回调期间或同一 Runtime 的任一 `ExecutionScope` 活动期间，启动和推进 GC 均返回 `busy`。这暂停整个收集状态机，包括标记结束时的死亡提交，不只推迟物理释放。引擎阶段与 GC 阶段的区别见 [CONTEXT](../CONTEXT.md#引用追踪与执行协调)，单线程轮询不授权其他线程同时改动对象图。

### 入口与阶段

| 入口/阶段 | 行为 |
| --- | --- |
| `begin_collection()` | 只允许从 `idle` 开始；先预留嵌套记录游标与位图容量，再清空标记位开启新轮次。容量准备失败不启动收集 |
| `roots` | 按占用位图分步检查开始时已有的活跃对象根与值根：无强引用的叶子对象直接完成标记，含强引用字段的对象和外部值进入对应追踪队列；期间新增根即时补记 |
| `mark` | 按类型派生的持有字段计划推进，隐藏字段与嵌套记录同样参与；观察字段不保活。嵌套记录游标跨步保存，末字段完成后即时弹出空帧，不要求一个大对象在单步内扫完 |
| 标记结束 | 队列及当前游标均完成后，用已占用数和已标记数一次提交整轮死亡判定。有待回收对象时进入清扫，否则直接回到 `idle`。未标记对象立即失效，析构随后执行 |
| `sweep` | 分批读取占用位图与标记位图的差集，逐个析构死亡对象、更新代际并释放槽位。清扫期间创建的对象记为本轮存活，最后一个死亡对象释放后立即回到 `idle` |
| `collect_step(CollectionBudget)` | 执行限定工作后返回 `CollectionProgress`；阶段回到 `idle` 表示本轮完成，空闲时调用返回零工作，不自动开始下一轮 |
| 状态查询 | `collection_phase()` 报告阶段；`live_objects()` 排除已判死对象；`pending_reclamation()` 只计已判死但尚未释放的对象 |

分配、普通值操作、撤销根和退出执行区间都不隐式开始或推进收集。宿主决定何时调用 `begin_collection`，之后在各安全点重复 `collect_step` 直至空闲。长执行区间会延迟回收并累积不可达对象；没有分配压力绕过执行保护的收集路径，也不由此增加后台线程或隐式任务调度。

### 跨步进修改与生命周期

- 标记期间新增或复制对象根、成功取得弱引用以及发布新对象都会对活目标补标。宿主取得的对象由返回的根持有，同步源码取得的对象由执行区间保护；即使临时持有随即释放，成功取得的对象也会活过本轮收集。
- 三种引用写入路径——对象字段、外部值字段、可信原生槽——共用写入保护；持有边的新目标补标，观察边不补标。目标已经扫过也不会漏掉后来写入的持有边。
- 值初始化/复制构造成功后，以及赋值成功或部分失败后，追踪结果值中的持有边。原生部分更新沿用既有基本失败保证，不能因返回错误而漏掉已经写入的引用。
- 写入保护是保守的：已经标记的对象即使在本轮内撤根或删边，也可能保留到下一轮；对未登记外部值的本轮引用写入/复制也会补标，但不会把该值变成永久根。
- 新登记的外部值进入追踪队列；撤销值根移除排队项，并取消正在借用该登记存储的游标。若撤销发生在原生字段定位回调中，回调返回后先检查取消状态，不读取其返回的引用槽，也不重新压入内联子游标。调用方仍须先撤销全部重叠登记，且等当前同步借用返回后，再销毁或释放存储；普通访问者对借用存储的保护约定不变。
- 显式销毁取消该对象的排队标记或活动游标，释放存储并更新代际；它仍可使持有引用失效。Runtime 在任一收集阶段退出都会释放剩余对象，脱离运行时的根句柄仍可安全复制、移动和释放。

死亡提交后，`try_acquire` 对已判死目标返回空；对象访问、调用和 `hold(ObjectId)` 拒绝该身份。`hold(ObjectReference)` 仍可复制定型的失效持有身份，但不能使目标复活。引用不会因为 GC 自动清空；槽位复用不能把旧引用绑定到新对象。原生析构看到的同轮已判死对象均已失效，不允许因内存尚未释放而访问它们，也不允许重入收集。

### 工作量与时间预算

`CollectionBudget::max_work` 必须为正；`max_time` 为零表示关闭时间限制，负数拒绝。`CollectionProgress` 返回本步阶段、`work_done`、`reclaimed` 和 `pending_reclamation`；只有设置时间上限时才读取时钟。

一个工作单位是一次活跃根登记项检查、一次队列取项、一个持有字段或内联记录的遍历、一个清扫批次，或一次阶段转换。每个清扫批次最多查看 64 个位图字（4096 个槽位），最多完整析构并释放一个死亡对象。标记队列使用紧凑外部灰链与值根登记项链接，嵌套游标在开始前预留容量。

时间限制只在工作单位之间检查；非空闲步骤至少执行一个单位，所以是软上限，单个长原生析构不可中断。开始时的容量准备、业务执行中的引用写入保护、整值复制/赋值及其追踪不属于某次 `collect_step` 的预算。不能由工作单位数量推导固定毫秒数，也不承诺任意分配速度下都能在固定帧预算内完成回收。编辑器与 Play 宿主每帧对文档 Runtime 推进一步收集（必要时先 begin_collection），这段墙钟记为[帧阶段计时](foundation.md#帧阶段计时)的 `app.gc` 阶段，显示在 Performance 表、Control 与 Play 报告中；UI Runtime 的收集步和代码发布前的排空收集不计入该阶段。尚未据此评估真实游戏负载下的吞吐或尾延迟。

## JIT 执行

源码只经 JIT 编译后执行，没有解释回退。范围和命令见 [README](../README.md#jit-业务执行闭环)。下文是执行、协程、发布与关停协议，最后列出仍未开放的能力。

### 业务执行协议

核心执行协议位于 [execution.hpp](../modules/execution/runtime/include/runtime/execution.hpp) / [execution.cpp](../modules/execution/runtime/src/execution.cpp)，不依赖 LLVM。正式 [source::Program](../modules/execution/source/include/source/program.hpp) 管理源码调用与发布，[内部桥接](../modules/execution/source/src/detail/bridge.cpp)连接共享 Runtime，[源码前端](../modules/execution/language/include/language/source.hpp)产出定型 AST，[LLVM 后端](../modules/execution/jit/src/source_backend.cpp)编译并拥有机器码。端到端验收作为这些模块的消费者；支持范围见[最小源码子集](language-contract.md#已实现的最小源码子集)。

1. **准入与身份**：宿主在参数准备、登记新根和业务副作用之前取得执行准入（`ExecutionScope::enter(runtime)` 或无诊断/调试同步快路 `DirectScope`），保持到结果交接和帧清理结束。准入检查关停、原生借用和只读遍历，不要求 GC 周期已结束；同一 Runtime 的活动执行统一计数，直到最外层退出才允许 GC 或代码发布。已验证发布版本复用不可变准备，仍逐次检查实际参数及需要/提供的接收者活身份与类型。不能以执行保护、根登记或静态签名核对替代活身份验证与代码保活。
2. **帧与存储**：`ExecutionFrame` 借用调用方已构造、地址稳定且独占使用的槽。同步形式接受 `ExecutionScope&` 与 `ObjectReference` 槽，在整个区间内保存定型身份而不逐局部登记持久根；持久形式接受 `Runtime&` 与 `ObjectRoot` 槽，供协程跨挂起保活。作用域、Runtime、槽存储和上下文必须保持到相应帧清理完毕。Frame 不复制、不移动，自身不分配堆，槽编号不是对象身份。编译产物报告强/弱槽需求；每个版本保留自己的槽需求，每次 Invocation 独占引用槽、活值计数和段内接收者缓存。拥有值存储与活值标记由生成代码按需分配并在语句间复用临时槽。原生实参使用生成代码的定型值或独立聚合暂存，不设通用实参缓冲。生成帧只预留接收者槽，不需要接收者的入口不填入它。尚无通用含引用记录帧或跨调用共享 scratch 池。
3. **更新与交接**：`hold(ObjectId)` 验证入场活对象；`hold(ObjectReference)` 与 `reference` 保留类型描述，支持复制合法的空值/过期身份而不重新要求存活。替换失败保留原槽；持久槽成功取得新根后才撤销旧关系。`create` 复用 Runtime 原子的对象与首根发布，同步槽随后保存身份并释放临时根。`object` 只交付身份，后续 Runtime 访问仍检查代际；执行保护与根都不阻止获准的显式销毁。`take` 提供宿主根交接：验证活身份后，同步槽先登记结果根再清槽，持久槽移交原登记。此接口不等于开放一般源码引用返回。`clear` 和析构清空相应存储，持久槽继续复用原有撤根/游标取消协议。
4. **宿主收集边界**：源码入口和循环回边不插入宿主轮询或收集回调，活动执行期间不开始或推进 GC，也不携带收集预算。`Program::Entry/Lease` 不接受逐调用 `CollectionBudget`；宿主在最外层执行结束后直接调用 Runtime 的收集入口。退出不自动开始或排尽周期。语言规范中的[同步值区间](../LANGUAGE.md#原生值借用)保留原有借用和对象操作限制，不是另一套 GC 调度模式。
5. **原生借用**：整个 JIT 入口不包装成 `Runtime::call` 的 Invoker。原生成员使用编译时核定的定型入口，字段使用已绑定定位；借用保护、段内存储地址复用与失效、参数及结果的权威合同见[共享函数签名](#共享函数签名与结果)。弱引用不隐式增强或绕过借用。借用或遍历内重入业务仍在首次副作用前返回 `busy`，不临时减掉保护计数。C++ 自有记录/Array/String 经 `Argument::borrow` 或同步 `Program::Typed` 引用绑定直接进入同步源码是另一种参数存储，仍使用同一顶层准入和共享签名；它不开放 Runtime 对象字段跨入口借用。
6. **错误与结果**：主机运行服务将 `Result` 转换为整数状态和固定容量诊断，保留首个错误的码与文本，不捕获 C++ 异常；不支持抛异常传递业务拒绝。源码 void、标量/枚举或同步数学值/plain struct 结果与该状态分离：定型参数先校验，返回值先暂存，全部生成清理成功后才写调用方结果；任意失败不覆盖结果。完整表达式和块退出撤销相应引用，循环块局部在每次迭代末尾清理。显式 return 与正常落尾共用结束清理：先清理用户资源，再释放接收者。所有错误边幂等清理全部强/弱槽及已构造的拥有值，宿主在 Frame 析构前检查根、观察存储和拥有值存活计数，RAII 仅作兜底。已发生的业务修改不回滚，不扩展既有 Runtime 的 OOM 恢复保证。
7. **代码与 ABI 寿命**：LLVM/标准库对象和代码删除留在隔离 DLL；Runtime、Frame、元数据与根留在宿主，边界只有不透明上下文、固定宽度标量和调用方存储。一次调用通过执行区间与退休链保护版本代码和展开信息，即使原生回调销毁外层代码句柄，代码也保留到最外层执行退出；这不保护被调用方擅自销毁的 Runtime/Frame，不授权并发调用/销毁或活动 DLL 卸载。

[业务验收](../acceptance/jit/business.cpp)保留四项最小代表：

- 定型标量与枚举参数的寄存器/栈位置及数值位表示（K5）；紧凑 4 字节对齐结构体中的 `Matrix4`/`Quaternion` SIMD 运算与 12 字节 `Vector3` 边界守卫不越界覆盖相邻字段。
- bool 只读/可写借用别名（动态 `invoke` 与 `Typed` 引用绑定）可见且不覆盖相邻字节。
- 派生接收者基类视图、强/弱引用跨 `yield` 与完整 GC 周期保持身份，协程完成后释放接收者根。
- 原生方法返回双精度 `Vector3d` 聚合结果（sret）。

### 协程核心协议

[execution.hpp](../modules/execution/runtime/include/runtime/execution.hpp) 中的 `Coroutine` 是同线程、可移动且不可复制的帧拥有者；`CoroutineFrame::resume()` 为 noexcept `Result<CoroutineStep>`，失败直接传递，不以 C++ 异常或 catch 适配。正常失败、取消与完成沿同一不抛异常的终态通知和帧清理；`Coroutine::resume()` 同样为 noexcept Result。核心不依赖 LLVM，也不拥有业务调度器。源码规则集中在 [LANGUAGE.md](../LANGUAGE.md#显式协程)。

- **所有权与状态**：稳定控制块区分 `ready/running/suspended/completed/failed/cancelled`，持有帧及完成值/错误。每次恢复先保留控制块，再进入已有 `ExecutionScope`；回调销毁外层拥有者不会使本次恢复访问已释放对象。同一实例重入返回 `busy`，终态恢复拒绝。
- **准入**：原生借用或只读遍历内恢复被拒，帧保持原状态，可在外层退出后重试。创建准备和每次恢复均受执行区间保护；挂起本身不占活动执行计数，但必须等同 Runtime 的所有外层执行退出后，宿主才可推进 GC 或发布兼容代码。
- **取消**：未启动/挂起实例取消时立即释放帧；运行中仅标记请求，在本次恢复返回后处理。错误首因优先，否则取消胜过挂起或完成结果。活动恢复的终结清理在当前执行作用域退出前完成。析构相当于取消并释放拥有者；取消不会执行源代码续体，也不回滚副作用。
- **寿命前提**：Runtime、原生回调和 DLL 必须覆盖所有非终态帧。宿主关停时先取消/销毁这些帧，再销毁 Runtime 和卸载代码。当前不自动追踪并关闭宿主所有协程，不授权在协程运行中销毁 Runtime。终态已释放帧，之后查询结果不访问 Runtime。
- **帧与 GC**：宿主适配器在稳定存储中持有 `Invocation`、原有根槽与观察槽。源码局部标量由 LLVM 持久帧保存；持有引用仍登记于原 Runtime，不另建对象堆。原生借用在返回后结束，挂起点不保存可跨期使用的裸对象地址。
- **代码与发布**：共享 `FunctionSignature::execution` 区分同步和协程，普通原生 Invoker 只接受同步声明。正式 `source::Program::Entry/Lease` 通过 `start` 创建协程，通过 `invoke` 同步调用，误用明确拒绝；仍由同一程序所有者管理声明和版本。每个协程固定持有创建时 Version，恢复和销毁代码、绑定信息及展开表一起保活；完成或取消即释放，结果对象不额外保留代码。
- **编译后端**：现有 AST → LLVM IR → LLJIT 链增加 `coro.id/begin/suspend/end/free` 与显式 CoroEarly、CoroSplit、CoroCleanup 转换，并在转换前后验证模块。初始挂起在源码体前，最终挂起与取消清理分开。DLL 内成对分配/释放满足对齐的帧存储，CPU 分配失败直接 fatal，不生成空帧恢复分支；宿主仅持不透明句柄，LLVM/STL/CRT 所有权不跨边界。

当前每实例有五个固定分配调用点：核心控制块、宿主帧、DLL 状态、不透明句柄和 LLVM 帧；非空参数保留副本，根/实参扩展仍按实际需求分配。固定调用点数量不等于实际总分配次数。同步入口不执行协程转换。

[JIT 业务验收](../acceptance/jit/business.cpp)保留一个强弱引用跨 GC 挂起后恢复、完成后不再持有接收者的代表；[关停消费者](../acceptance/source/shutdown.cpp)保留活动回调阻止 Runtime 退役、挂起任务取消及代码与根退休的序列。不重复标量、位模式或准入拒绝矩阵。没有 `await` 组合、跨线程恢复、原生深层挂起、帧序列化或迁移。

### 单入口兼容方法体发布

[source::Program](../modules/execution/source/include/source/program.hpp) 管理一个完整源码入口/LLJIT 资源单元。Runtime 提供共同签名解析、执行作用域及发布准入；源码请求、候选、稳定入口与租约归该正式宿主模块。通用公共调用使用 Scalar 值或 Argument 借用，同步标量/枚举及同步引用参数还可绑定固定或跟随版本的定型句柄；私有 DLL 参数布局不进入业务接口。每个版本都可以随代码提交新的源码 enum/struct 类型；兼容更新保持已发布类型的身份和布局，不重建对象，也不向原生 Invoker 表塞入裸 JIT 地址。

- **创建与绑定**：`Program::create(Runtime&, TypeId = {}) noexcept` 返回 `Result<std::unique_ptr<Program>>`，先完成目录绑定再交付拥有者；错误不留下半初始化程序。非空接收者必须是当前 Runtime 的对象类型，空 TypeId 建立无接收者目录。Program 不可移动，地址稳定；独占拥有者可交给宿主已有的共享所有权，不额外建立脚本对象世界。
- **声明与依赖**：创建程序时只绑定 Runtime 和对象目录，不重复手写源码的函数名或参数/结果声明。首次成功发布从真实编译产物通过 Runtime 解析建立不可变 `FunctionSignature`；后续请求捕获该声明及类型/字段/方法、布局和服务表快照。依赖复核使用该程序捕获的目录，其他模块追加无关类型不改变已有索引。`describe()` 交付拥有描述数组的副本；动态 `Entry/Lease::invoke` 核对值或借用实参，`start` 核对协程实参并创建固定版本实例，同步/协程错用拒绝。
- **定型绑定**：`Lease::bind<R, Args...>()` 和 `Entry::bind<R, Args...>()` 核对同一共享签名并返回 `Program::Typed<R, Args...>`，分别固定版本和跟随发布，不增加模式参数。支持同步按值 int32/uint32/float/double/bool 和 32 位底层枚举，以及已支持标量、枚举、数学值、plain struct、原生记录/Array 和只读 String 的同步 `const T&` / `T&` 引用；结果为 void 或其中一个按值标量/枚举；按值聚合参数/结果与协程继续使用动态接口。`invoke(ObjectId, Args..., Diagnostics* = nullptr, Debugger* = nullptr)` 返回 `Result<R>`，不构造 Scalar/BusinessValue 实参数组，也不静默回落。签名不合返回错误，空句柄不能执行。固定句柄持有其 Version，热发布不改绑，Program 关闭后仍可调用。跟随句柄持有控制状态与稳定入口索引；发布后同一句柄选新版本，关闭后拒绝调用，不保活旧代码。每次只选一次 Version/定型地址，不查名字或重复核签名，执行区间保护该版本至错误定位、诊断和清理结束，不逐调用复制版本 shared_ptr；回调之后不访问可能已销毁的句柄或控制状态。两者都不保活接收者或 Runtime，仍需合法活身份、同线程准入及完整诊断/调试/显式清理；失败不交付结果，不回滚已发生的业务副作用。具名 `entry(name)` / `pin(name)` 同样可绑定，导出重排保持已发布索引。
- **源码类型发布**：enum/struct 的定义来自源码，身份、布局、字段与值操作来自同一 Runtime。发布在类型安全点完成代码编译、签名解析、依赖核验与全部分配，再提交新类型和代码；失败不占用类型名。已发布的定义随后冻结，后续版本可以追加新声明；关闭代码不注销类型，具体见[源码类型声明](source-type-declarations.md)。
- **接收者固定**：首次声明确定是否需要接收者；目录中有对象类型并不表示必须提供 self。无接收者声明不能在兼容更新中新增 self；已有接收者即使新函数体不再读取 self，调用方仍须提供正确活对象。`BusinessCodeInfo::requires_receiver` 只是当前代码使用标志，不能重新决定已发布声明。
- **候选与准备**：请求自有完整源名、模块源码、入口名称、声明快照和单调序号，并通过 Runtime 内部批次准备新类型布局；候选走真实解析、定型、链接及物化，只读捕获输入，不读取 live Runtime 或执行初始化。后端拥有签名/目录/名字及实际引用槽、拥有值和暂存需求。内部函数按定型参数直接互调；公开入口固定，私有助手随整个候选编译与发布。发布前保留完整兼容及环境核验；已发布 Version 缓存自身 info/layout，热调用不重复比较整份目录。
- **诊断与身份**：过长源名在编译诊断和 COFF 文件标签中使用有界首尾显示，保留尾部请求号与 UTF-8 边界，为位置、阶段和原因留出空间；请求快照及 `BusinessCodeInfo::source_name` 仍保存完整名字，兼容性检查不使用缩略标签。短名格式不变，显示标签不是身份键。
- **显式发布**：`check_code_publication()` 要求整个 Runtime 的活动执行计数为零，并拒绝原生借用、遍历和执行中的 GC 步。忙时保留候选，外层调用结束后由宿主重试；暂停的一轮 GC 不强制排尽。提交前检查最新请求、所属程序和原依赖；成功消费候选，失败保持旧入口。最新请求失败不能使旧候选重新获得发布资格。
- **版本持有**：`Entry` 保存弱程序身份，每次调用只读取并持有一次当前版本；工具事件另外保存接收者 `ObjectId`，不保活对象。`Lease` 明确保留某个版本，通过同一受检查入口执行。幂等的 `Program::close()` 关闭请求、准备、发布及稳定入口，并释放 current；析构复用 close。活动调用、已有租约和协程仍保留各自代码，租约仍可调用；执行后不访问可能已被回调销毁的入口对象。
- **退休**：Version 的最后共享持有者释放时，若 Runtime 仍有活动执行，则在析构前进入无分配侵入退休链；最外层执行退出才释放代码、Contract、映射及展开表等依赖，嵌套退出不提前释放。模块容器可立即销毁，选定的 Version 自持完整执行依赖；不建立第二条模块退休链。无活动调用时立即释放。项目快照里输入未变的单元由新旧 Version 共同持有；跨挂起协程仍明确持有 Version，不能只靠活动计数。历史源码、诊断和元数据快照不拥有机器码；调用方有意保留的未提交候选/租约则是实际持有者。以系统函数表查询和页查询验证保留与撤销，避免先重新分配代码造成地址复用假象；不把这些检查宣称为完整堆泄漏或资源故障审计。

[关停与版本代表](../acceptance/source/shutdown.cpp)保留活动拥有者销毁后租约执行、挂起取消及代码与展开表退休；项目、World owner 与交错版本由 Source 代表覆盖，JIT 不再重复诊断文本、兼容拒绝、需求规模或多轮发布矩阵。

模块内的无环同步调用图随整个编译单元发布和退休，旧执行链不会混用新助手。跨发布单元调用与内联仍未实现，整体替换一个公开入口不等于通用依赖失效。Runtime 元数据、原生回调和 ABI 不可原位变化；不兼容源码拒绝，结构修改须另行定义受控重建/迁移。Source 模块不自建后台编译调度或文件监视；编辑器已通过[源码工作流](modules/editor/source-editor.md)接入磁盘检测和兼容发布。[源码调试](source-debugger.md)使用固定版本与原始文件映射，活动帧迁移尚未实现；Runtime、宿主回调和 DLL 必须覆盖调用/租约的寿命。本节维护发布合同，后端取舍见[结构说明](runtime-structure.md)。

### 宿主关停闭环

宿主集中拥有任务与入口，按两步关停。第一步停止接受工作，调用 `Program::close()`，释放宿主持有的租约，并取消所有非终态协程。运行中取消只登记请求；可以从原生回调发起这一步，不能在回调中销毁宿主或 Runtime。

第二步先调用 `Runtime::check_code_publication()` 检查执行、原生借用和遍历已静止。`busy` 时保留 Runtime 与所有拥有者，等外层调用返回后重试；仅检查自己持有的协程状态不足以排除同步调用。通过后释放任务拥有者、未提交候选、请求、Program 和外部根，最后销毁 Runtime。关闭程序不会消费被拒绝的候选，候选仍是代码拥有者。

[公共关停消费者](../acceptance/source/shutdown.cpp)实现这一顺序，覆盖 ready/suspended/completed/failed 混合状态、兼容更新后的旧帧、原生回调中的延迟取消、同步回调中的忙状态、重复关闭，以及最终根、对象、代码页和展开表释放。核心不增加全局任务登记或调度器；宿主须保证没有遗漏的外部租约、任务或调用者。

### 尚未开放的能力

- 调用：对象引用参数与结果、资源记录与容器按值调用、借用返回及非平凡值结果的构造交接。`Invoker` 仍是原生函数指针，不拥有 JIT 代码；`std::variant`/`std::expected` 布局不是跨 JIT 协议。`Runtime::call` 用 `CallbackScope` 保护整个原生 Invoker，语言方法没有并入通用方法分派，不能把机器码指针塞进 Invoker 绕过这一差异。
- 执行帧：任意含引用记录或值临时量的自动帧描述、源码级引用活跃集分析、直接字段写屏障与描述型直接字段代码生成。GC 安全点不是代码发布点，取消不保证无挂起长循环的响应时延。
- 代码版本：跨发布单元调用与内联、一般方法身份与依赖失效、代码相关值操作，以及多声明/多单元下的依赖闭包与发布单位。类型目录只追加，已有类型的布局变化需要重建上下文；后台编译若引入，只处理安全快照。
- 调度与工具：引擎 Tick 调度、永久通用 ABI、编辑器调试面板与 Control 调试指令、PDB/原生调试器集成。

后端取舍与原始来源见[执行结构说明](runtime-structure.md#当前取舍)；技术债的处理时机统一见[能力边界](runtime-structure.md#已知成本与能力边界)。

## 取舍依据

以下只保留支撑现行实现的机制与差异；外部系统的性能数字、对象模型和线程保证不成为本项目承诺。

- **不移动不等于免除引用协议。** [LLVM GC 接口](https://llvm.org/docs/GarbageCollection.html)提供编译器接线而不提供 collector；[Epic 增量 GC](https://dev.epicgames.com/documentation/unreal-engine/incremental-garbage-collection-in-unreal-engine)与 [Go 写屏障](https://go.dev/src/runtime/mbarrier.go)说明跨切片修改必须保护可达性。宿主 stage 是调度机会，不能替代根发布、写屏障和终止确认。
- **预算仍是软上限。** [Metronome 的实时调度模型](https://research.ibm.com/publications/a-real-time-garbage-collector-with-low-overhead-and-consistent-utilization--2)要求可调度操作具有上界；任意原生调用、析构和系统调度不满足此证明，调用一次 Tick 不构成硬实时保证。[ZGC 的搬迁与读屏障](https://dev.java/learn/jvm/tool/garbage-collection/zgc-deepdive/)服务另一种布局约束，本项目不为非移动对象预置这些机制。
- **候选算法的组合不能继承论文收益。** [Nofl](https://arxiv.org/html/2503.16971)的 lazy sweep 会转移成本到分配路径；[LXR](https://www.steveblackburn.org/pubs/papers/lxr-pldi-2022.pdf)包含存活对象复制；[CDPT](https://doi.org/10.1145/3808310)的阶段协议也不是游戏 stage。剥除搬迁或替换根协议后是新组合，不能引用原实验推算本项目速度，当前没有采纳它们。
- **根、图内边和析构分别负责不同事情。** [cppgc 的 Member/Trace、Persistent、弱处理与清扫说明](https://chromium.googlesource.com/v8/v8.git/%2B/HEAD/include/cppgc/)提供职责对照，但其保守原生栈、可选压缩、弱引用自动置空和线程堆不是本项目合同；V8 的 Unified Heap 协作也不等于两种语言共用类型身份。
- **登记身份不等于持有。** UE 的 `NewObject` 与 `TStrongObjectPtr` 是不同交付路径，见 [UObjectGlobals.cpp](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp#L4803-L4874)及[对象指针](https://dev.epicgames.com/documentation/unreal-engine/object-pointers-in-unreal-engine)。本项目由 create 一起交付对象与首根是自身加强的保证；强持有不阻止已获准的显式销毁，旧身份保留不等于可以复活。
- **身份验证不提供持续借用。** [Godot ObjectDB](https://github.com/godotengine/godot/blob/ed1daf0bf001b61586d9930840f2f1394092c079/core/object/object.cpp#L2284-L2318)的 validator 是代际身份对照；其 [GDScriptInstance](https://github.com/godotengine/godot/blob/ed1daf0bf001b61586d9930840f2f1394092c079/modules/gdscript/gdscript.cpp#L154-L253)另外附接脚本字段，不能证明统一原生布局。[RefCounted](https://docs.godotengine.org/en/stable/classes/class_refcounted.html)的 RC 也不代替本项目追踪不可达环。
- **分阶段准备不等于整组原子发布。** [UE 的 Class 注册/Link](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/CoreUObject/Private/UObject/Class.cpp#L127-L155)及 [Godot 的接口/函数体解析](https://github.com/godotengine/godot/blob/ed1daf0bf001b61586d9930840f2f1394092c079/modules/gdscript/gdscript_analyzer.cpp#L1268-L1405)仅提供阶段分离的对照；本项目的私有 staging、内联环拒绝和失败无可见半成品由自身发布事务保证。UE 固定源码链接需要相应访问权限。
- **平凡快路不改变生命周期。** [cppgc FinalizerTrait](https://chromium.googlesource.com/v8/v8.git/+/3e6373bb9f915f75f8e5d1d0d64e4d5a7968d1fa/include/cppgc/internal/finalizer-trait.h)与 [MarkingWorklists](https://chromium.googlesource.com/v8/v8.git/+/3e6373bb9f915f75f8e5d1d0d64e4d5a7968d1fa/src/heap/cppgc-internal/marking-worklists.h)支持按实际工作分类和显式遍历；[P0593R6](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0593r6.html)与 [cstring.syn](https://eel.is/c++draft/cstring.syn)给出 memcpy 的对象生存期依据。它们不授权省略原生回调、把独立对象当数组或改变清理顺序；活跃 ValueRoot 索引是本项目实现，不声称 cppgc 使用相同结构。
