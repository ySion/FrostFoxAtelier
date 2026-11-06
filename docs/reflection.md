# C++ 自动反射

`atelier_reflect` 是使用锁定 Clang LibTooling 的构建期 C++ 工具。它读取真实翻译单元中的宏与 AST，生成现有 `NativeRegistration<T>` 调用。生成结果仍是 `TypeDefinition`，由原有模块入口交给 `Runtime::register_types`。工具不进入游戏进程，没有额外类型表、对象身份、GC 或注册静态初始化。

## 声明写法

```cpp
#pragma once
#include "runtime/reflection.hpp"
#include "models/counter.gen.hpp"

namespace models {
ACLASS(Name = "models.Counter")
class Counter {
    AGENERATED_BODY(Counter)
public:
    AFIELD() float value{};
    AFUNCTION() float read() const noexcept { return value; }
};
}
```

| 宏 | 合同 |
| --- | --- |
| `ACLASS(Name = "...", DESC = "...")` | 声明 Runtime 对象；无原生基类时沿用 `core.Object` |
| `ASTRUCT(Name = "...", DESC = "...")` | 声明 Runtime 记录值，允许内联使用 |
| `AENUM(Name = "...", DESC = "...")` | 声明命名空间级 enum class；底层支持 int32_t/uint32_t，不需要 AGENERATED_BODY |
| `AFIELD(Name = "...", Label = "...", Category = "...", DESC = "...", Hidden, Strong = "...", Min = "...", Max = "...", LockMin, LockMax, HardMin = "...", HardMax = "...")` | 声明字段；展示名称、分组、说明与范围均可省略，引用形式和范围见下文 |
| `AFUNCTION(Name = "...", DESC = "...")` | 声明成员方法；名称与说明可省略，重载须使用不同的反射名称 |
| `AGENERATED_BODY(TypeName)` | 在类型体内出现一次，仅授权对应元数据生产者访问私有成员；不改变访问段或对象布局 |

`ACLASS/ASTRUCT` 表达引擎语义，C++ 声明本身可以使用 `class` 或 `struct`。类型的 `Name` 默认是完整 C++ 名字，并把 `::` 换成 `.`；成员名字默认取 C++ 名字。稳定的既有名字应明确指定。宏值为普通字符串字面量，`Hidden` 是无值标记。未知或重复参数直接报错；字段的 `Meta` 可重复声明不同键，见下文。

`AFIELD` 的 `Label` 和 `Category` 是可选展示文本，例如：

```cpp
AFIELD(Label = "Position", Category = "Transform") math::Vector3 position;
```

该字段仍以 `position` 查找和保存；`Label` 不改变字段名称，`Category` 不创建类型或命名空间。两项支持普通 UTF-8 字符串及 C++ 转义，允许显式空字符串；省略或为空时，由消费者回退到字段名或未分类。控制字符、重复参数和未知参数仍报错，只接受标准拼写 `Category`。

生成器沿 `NativeRegistration` 把展示文本写入 `FieldDefinition::label/category`，发布后由同一个 `FieldInfo` 持有。原生和描述型定义共享这套元数据，继承字段保留声明身份和展示文本；不存在编辑器专属字段表。两项不参与字段存储布局或 Source 的 ABI 依赖比较，详见[Runtime 展示元数据](runtime-model.md#字段展示元数据)。源码类字段的 `[Label = "...", Category = "..."]` 属性也填写同一套展示元数据。

标记紧邻一个声明，可隔空白和注释；每个字段独立标记、独立声明。标记必须直接写在源码中，不再包在另一层宏内。不支持模板、局部类、匿名命名空间和嵌套类型。前向声明可以存在，最终必须在本生成组中找到完整定义；重复标注必须一致。

### 字段 Meta

`AFIELD` 另接受 `Meta("namespace.key", "value")`，每项恰好两个普通字符串字面量；可重复使用 Meta 声明不同键。例如 `AFIELD(Meta("procedural.bind", "evaluate.values"), Meta("procedural.layer", "point")) String attribute;`。生成器保留解码后的完整字节和长度，沿 `NativeRegistration::metadata({{"namespace.key", "value"}})` 写入最近声明的字段；手写生产者也可直接填写 `FieldDefinition::metadata`。

键格式、排序、同键拒绝、拥有关系和定义兼容性由 [Runtime 字段扩展元数据](runtime-model.md#字段扩展元数据)统一规定。消费者解释自己的命名空间，生成器与 Runtime 不解释几何列或其他业务绑定。Source 类字段使用相同的 `Meta("key", "value")` 写在属性列表中，不借用 `DESC` 或展示名称承载机器语义。

### 类型与成员说明

`ACLASS / ASTRUCT / AENUM / AFIELD / AFUNCTION` 都接受可选的 `DESC`，例如：

```cpp
AFIELD(Label = "Position", Category = "Transform",
    DESC = "相对于当前 Space Reference 的位置，单位 m。\n改变它会移动 Entity。")
math::Vector3 position;

AFUNCTION(DESC = "读取当前 Position，不改变 Entity。")
math::Vector3 read_position() const;
```

说明是 UTF-8 纯文本，可中英文混排，支持 `\n`、`\t`、引号和反斜杠等普通 C++ 转义。省略或显式空字符串表示没有说明，不自动填充字段名。发布时将 CRLF 和 CR 统一为 LF；NUL、其他 C0 控制字符、DEL、C1 控制字符及无效 UTF-8 会拒绝。只接受 `DESC`，不提供其他大小写或缩写。`Name / Label / Category` 原有的单行校验保持不变。说明不是 Markdown，也不解释链接或执行指令。

生成器将文字写入现有 `TypeDefinition / FieldDefinition / MethodDefinition::description`；Runtime 的同一批次发布入口完成校验和换行归一，然后由 `TypeInfo / FieldInfo / MethodInfo::description` 拥有。手写原生注册和描述型生产者遵守同一规则，失败不会发布部分类型。`.source` 类字段用同名的 `DESC` 属性填写同一字段，见[语言规则](../LANGUAGE.md#项目快照与原生派生类)；没有编辑器专用说明注册表。

手写 `NativeRegistration<T>` 的构造器在 `kind` 后接受说明；自动推导类型的 `field(name, description)` 与 `hidden_field(name, description)` 直接传入说明，完整字段重载则在 `label, category` 后接受说明。`method` 在 `preserves_borrows` 后接受说明。也可以直接填写 `TypeDefinition` 及其成员的 `description`。

类型说明解释类型本身，字段说明解释该字段在声明处的具体含义，两者不互相覆盖。继承字段和方法保留原声明 ID、签名及说明；派生类型的空说明不会自动变成基类说明。文本按类型保存，不复制到每个对象，不进入 Scene 数据，也不参与类型/成员身份、对象存储布局或调用 ABI。拷贝的 `TypeInfo` 自行拥有文本；借用 `Info` 的消费者仍须遵守原 Runtime 生命周期。

### 字段范围与硬边界

```cpp
AFIELD(Min = "0", Max = "10", LockMin, HardMax = "65504") float emissive_strength{1.0f};
AFIELD(Min = "0", Max = "1", LockMin, LockMax) math::Vector3 albedo{1.0f, 1.0f, 1.0f};
AFIELD(Min = "1", Max = "64", LockMin) std::uint32_t samples{8};
```

- `Min / Max` 是 UI 范围，供滑条和数值阶梯使用；单独出现时不约束写入，编辑器允许输入范围外的值。
- `LockMin / LockMax` 把同侧 UI 边界同时作为硬边界（Houdini strict range），需要同侧的 `Min / Max`。
- `HardMin / HardMax` 单独指定同侧硬边界并隐含锁定，用于 UI 范围远小于合法范围的字段；同侧不能再写 `LockX`。只有硬边界、没有 UI 边界的一侧合法，滑条范围由 UI 自行决定。
- 适用于 float、double、int32、uint32 字段、float/double 数学向量以及这些类型的 `Array<T>`；向量的范围逐分量生效，数组的范围逐元素生效。bool、枚举、四元数、矩阵、记录和引用字段报错。值按字段叶标量的精确类型解析：浮点接受有限的十进制写法，整数拒绝小数、负 uint 和溢出。生成代码用十六进制浮点保持位精确。
- 大小顺序、UI 边界须落在硬边界内、描述型默认值须满足硬边界等语义检查由 Runtime 发布统一执行，规则见[字段范围与写入约束](runtime-model.md#字段范围与写入约束)。

生成器在字段调用后输出 `.range({.ui_min = ..., .hard_max = ...})`，手写注册使用同一个 `NativeRegistration::range(FieldRange)`，也可直接填写 `FieldDefinition::range`。范围属于已发布的类型定义。`.source` 类字段用同名属性 `[Min = 0.0, LockMin, HardMax = 10.0]` 写在字段前，边界是字段叶类型的常量表达式而不是字符串，其余规则相同，见[语言规则](../LANGUAGE.md#项目快照与原生派生类)。

跨字段规则由类型校验器表达，目前只能通过手写注册的 `.validate<&F>()` 或 `TypeDefinition::validate` 声明，生成器还没有对应的宏。生成的类型可以在发布前，对 `Definition<T>::build` 返回的定义设置 `validate`，例如 `definition.validate = [](const void* p) { return check(*static_cast<const T*>(p)); };`。语义见[类型校验](runtime-model.md#类型校验)。

### 字段与方法

- 内联字段支持 bool、float、double、int/int32_t、uint32_t、本生成组中的反射枚举与记录，以及内建数学值、引擎 String 和无 GC 引用的 Array<T>。Array 元素使用同一已注册值类型；StringView/Span 不允许作为持久字段。字段必须可写、可取成员地址；位域、const/volatile 字段及其他存储形式报错。
- `ReferenceSlot` 必须指定 `Strong = "类型名"` 或 `Weak = "类型名"`；`ReferenceSequence` 必须指定 `Sequence = "元素类型名"`，且只能属于对象。目标可以是本组的反射对象，也可以是组外任意对象类型的限定名（如 `world.Entity`）：生成代码按名称引用，由 Runtime 在发布时解析，所以组外目标必须先发布或在同一批中，缺失时发布错误写明字段和所引用的名称。目标指向本组的非对象类型时生成器报错。这些选项不能用于普通内联值。
- `Hidden` 只控制 Runtime 公共字段目录；字段仍参与追踪。C++ 的 public/private 与此独立。
- 漏标直接引用槽、序列或内嵌包含引用存储的记录/数组会报错，避免遗漏 GC 边。未标记的普通原生状态不自动成为反射字段；任意外部资源的所有权仍由原生实现负责。
- 对象支持一个 public、非 virtual 的原生反射基类；记录不支持继承。基类可以在本组中，也可以来自另一个生成组：生成器凭基类的 `AGENERATED_BODY` 识别（类模板特化不在此列），使用方必须在基类所在组之后注册。组内基类的反射成员重名由生成器报错；跨组时，基类是否为对象类型、成员是否重名，由 Runtime 注册时检查（`Object base must resolve to an object type`、`Member hides an inherited declaration`），不在生成时报。生成 `.base<Base>()`，实际地址调整由 C++ 编译器处理。
- 方法支持 void、标量（含 bool）/枚举、内建数学值或 plain struct 结果，标量/枚举参数支持按值、`const T&` 和 `T&`；无 GC 引用的原生记录、Array、String 支持后两者。支持 const/noexcept 成员函数。静态方法、变参、模板、指针、右值/volatile 引用、引用返回、引用限定方法和运算符方法报错。内建数学值与递归 plain struct 支持同步按值参数/结果；资源记录与容器按值参数/结果与借用视图参数尚未开放。生成的定型 C++ thunk 负责实际平台 ABI。生成器使用完整成员指针类型和完全限定的原生类型名消除重载与命名空间歧义。
- `AFUNCTION(BorrowSafe)` 发布 `MethodInfo::preserves_borrows`，允许同步原生值区间调用该方法。它是开发者对完整调用链的保证，不是自动效果分析；具体约束见[语言规则](../LANGUAGE.md#拥有型-stringarray-与原生算法)。普通方法无需此标记，已有调用行为保持。
- 方法的参数名取自带 `AFUNCTION` 的那个 C++ 声明，生成为 `.method<...>(...)` 之后的 `.parameter_names({...})`，发布到 `MethodInfo::parameter_names`；未命名的 C++ 参数得到空字符串。手写注册在 `method` 后调用同一个 `NativeRegistration::parameter_names`，规则见[运行时模型](runtime-model.md)。
- 满足自然对齐的标准布局类型中，公开反射字段额外生成 `offsetof`，由同一 Runtime 发布并校验，供源码借用直接访问。隐藏、非标准布局或不能保证自然对齐的字段继续使用既有成员访问器，不推测偏移；这一限制不影响原有反射能力。详见[原生值借用](native-value-borrows.md)。
- 构造、析构和记录复制能力沿用 `NativeRegistration`。生成代码带原头文件的 `#line`，编译器继续验证默认构造及不抛异常析构等约束。

bool 反射字段和方法使用 Runtime 的 `core.bool` 身份与当前目标的 1 字节 C++ bool 存储。bool/const bool&/bool& 及 bool 结果沿既有 `NativeRegistration`、共享签名与结果提交入口处理，保留真实地址和别名；不将 bool 转成 uint 另建调用路径。动态调用及源码调用的错误类型或缺失结果仍按[共享函数契约](runtime-model.md#共享函数签名与结果)拒绝。

### 枚举与引用示例

```cpp
AENUM(Name = "models.Mode")
enum class Mode : std::int32_t { idle = -1, active = 2, running = active };

// 位于带 ACLASS/AGENERATED_BODY 的类中：
AFIELD() Mode mode{Mode::idle};
AFUNCTION() void accumulate(int amount, const int& bias, int& total);
```

枚举必须与引用它的类型处于同一生成组，并由模块入口一起发布。生成器保留独立 TypeId 及名字/数值，不将枚举合并为底层整数；别名和未命名底层值均有效。当前不生成 uint8/int16 等底层枚举。借用合同见[共享函数契约](runtime-model.md#共享函数签名与结果)，源码写法见 [LANGUAGE](../LANGUAGE.md#参数传递与枚举)。

## 生成文件与使用入口

每个输入头产生同路径的 `.gen.hpp`，一个目标产生一份 `reflection.gen.cpp`。头中只有前向声明与元数据构建特化声明；实现包含普通成员指针和既有注册代码。生成文件位于 `out/<配置>/generated/<目标>/`，不提交，也不手工编辑。

```cpp
auto definitions = std::vector<atelier::TypeDefinition>{
    atelier::reflection::Definition<models::Counter>::build(runtime)
};
auto types = runtime.register_types(std::move(definitions));
```

调用方仍决定发布的类型集合及生命周期。每次构建定义显式传入 Runtime，不缓存全局 `TypeId`。已迁移类型删除重复手写描述；`NativeRegistration` 本身及其他未迁移消费者保留。

[World 的类型声明](../modules/engine/world/include/world/world.hpp)由生成器发布 Material 等作者值类型与 Entity、StaticMeshEntity、Component、PrimitiveComponent、StaticMeshComponent、CollisionComponent、LightComponent 等真实对象类型；空间状态内联于 Entity，不注册 SceneComponent。数学记录由 Runtime 的基础初始化统一发布，World 复用其权威身份；`world::register_types` 保留成组业务发布和字段路径绑定，`detail::Access` 保留业务访问职责。Counter 及其方法仅属于验收业务类型。详见[数学合同](math.md)。

拥有容器的类型定义由 `runtime/foundation.hpp` 提供，与生成类型一起提交到同一发布入口；完整示例、字段借用与生命周期合同见[基础库](foundation.md#共享反射与-runtime)。

## CMake 接入

目标按正常方式声明后调用：

```cmake
atelier_reflect(atelier_world
    SOURCE modules/engine/world/src/objects.cpp
    BASE_DIR modules/engine/world/include
    HEADERS modules/engine/world/include/world/world.hpp)
```

`SOURCE` 必须在 `compile_commands.json` 中对应唯一真实编译命令，并包含全部待生成头。`BASE_DIR` 必须显式指定为模块的公开 include 根或验收头根；头文件必须是其下的 `.hpp`。生成头的 include 路径相对此目录，例如 `world/world.gen.hpp`。生成 include 目录通过目标传播给消费者。模块头与实现的归属遵循[源码布局与依赖](engineering.md#源码布局与依赖)。

配置时会清理该目标生成目录中已不属于本次声明的 `.gen.hpp`，保留仍声明的输出。头文件迁移到另一个目标后，旧目标的公开 include 目录不能继续提供同路径的过期生成头；清理只限该目标拥有的生成目录，不扫描或删除源码头。

执行链如下：

```text
目标编译命令 + 真实 SOURCE
  → Clang 预处理回调记录标记和 include 依赖
  → AST 确认类型、成员、基类、签名和标记归属
  → 校验完整输入，形成 C++ 输出
  → 仅替换内容变化的生成文件，并写 depfile
  → 正常编译生成的 C++，沿既有模块入口发布
```

扫描复用 SOURCE 的宏、include、目标平台和标准选项，使用锁定 SDK 的资源头；反射头和一般公共 C++ 头一样，影响类型布局的宏须在目标与消费者之间保持一致。宏标记由 `PPCallbacks` 观察，普通 C++ 编译无需加载插件。

Clang/LLVM 包在独立变量作用域中加载，SDK 的静态 CRT 默认值不会传播到后续引擎或第三方目标。`atelier_reflect` 通过 `atelier_llvm_target` 显式使用 SDK ABI，正式消费者继续使用各自配置的动态 CRT。

`reflection.hpp` 提供固定的 `Definition<T>::build` 声明。扫描期间用内存虚拟头屏蔽本组旧 `.gen.hpp`，因此首次生成不需要旧文件，也不会消费过期特化。正常编译必须先完成 CMake 生成依赖，不会把缺失生成文件静默忽略。

编译参数、配置、工具或 include 依赖变化会触发生成；内容不变时保留输出时间戳。输出不含时间戳、绝对路径或 AST 地址。源码/标注验证失败返回非零并保留先前输出，构建仍然失败，不继续使用旧输出完成本次构建。

## 验收与边界

`reflection_acceptance` 保留真实生成器、Runtime 和 JIT 路径的 ABI 与生命周期代表，不包含诊断/拒绝矩阵或独立 CLI 验收。

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target reflection_acceptance world_acceptance
cmake --preset release
cmake --build --preset release --target reflection_acceptance world_acceptance
```

[生成消费者](../acceptance/reflection/main.cpp)检查 64 字节对齐且大于 128 字节的记录按值快照/返回、失败后的借用写入与输出不发布及恢复、自动生成构造/析构钩子，以及定义和 Runtime 销毁后的元数据说明寿命。期望来自输入记录、注入故障前的输出和显式 live 计数。[调用链消费者](../acceptance/reflection/calls.cpp)各保留一次 Host→JIT→原生整数别名调用和嵌套调用前按值操作数快照，期望分别为夹具语句直接推导的 20 与 8；不再展开类型、协程或签名更新矩阵。

内联记录与枚举字段的类型须在本生成组中解析；反射基类和引用目标可以来自其他生成组，分别按上文的 `AGENERATED_BODY` 识别和限定名解析，其余跨生成组的语义导入尚未开放。工具升级随工具链锁文件进行；源码实现不承诺跨 Clang AST API 版本二进制兼容。

选型依据：[LibTooling](https://clang.llvm.org/docs/LibTooling.html)使用实际 TU 的[编译数据库](https://clang.llvm.org/docs/JSONCompilationDatabase.html)，[PPCallbacks::MacroExpands](https://clang.llvm.org/doxygen/classclang_1_1PPCallbacks.html)记录声明前标记，再由 AST 确认归属、类型和访问规则。未知 attribute 不能假定保留为有效元数据，见 [Clang unknown-attributes](https://clang.llvm.org/docs/DiagnosticsReference.html#wunknown-attributes)。当前生产链直接复用锁定 C++ SDK，不维护 C#/ClangSharp shim 或另一套参数转换；生成结果仍进入同一 NativeRegistration 与 Runtime 类型发布。
