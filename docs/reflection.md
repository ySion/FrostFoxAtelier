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
| `ACLASS(Name = "...")` | 声明 Runtime 对象；无原生基类时沿用 `core.Object` |
| `ASTRUCT(Name = "...")` | 声明 Runtime 记录值，允许内联使用 |
| `AENUM(Name = "...")` | 声明命名空间级 enum class；底层支持 int32_t/uint32_t，不需要 AGENERATED_BODY |
| `AFIELD(Name = "...", Hidden, Strong = "...")` | 声明字段；各选项按需要填写，引用形式见下文 |
| `AFUNCTION(Name = "...")` | 声明成员方法；名称可省略，重载须使用不同的反射名称 |
| `AGENERATED_BODY(TypeName)` | 在类型体内出现一次，仅授权对应元数据生产者访问私有成员；不改变访问段或对象布局 |

`ACLASS/ASTRUCT` 表达引擎语义，C++ 声明本身可以使用 `class` 或 `struct`。类型的 `Name` 默认是完整 C++ 名字，并把 `::` 换成 `.`；成员名字默认取 C++ 名字。稳定的既有名字应明确指定。宏值为普通字符串字面量，`Hidden` 是无值标记。未知或重复参数直接报错。

标记紧邻一个声明，可隔空白和注释；每个字段独立标记、独立声明。标记必须直接写在源码中，不再包在另一层宏内。模板、局部类、匿名命名空间和嵌套类型不在首片支持范围。前向声明可以存在，最终必须在本生成组中找到完整定义；重复标注必须一致。

### 字段与方法

- 内联字段支持 float、int/int32_t、uint32_t、本生成组中的反射枚举与记录，以及引擎 String 和无 GC 引用的 Array<T>。Array 元素使用同一已注册值类型；StringView/Span 不允许作为持久字段。字段必须可写、可取成员地址；位域、const/volatile 字段及其他存储形式报错。
- `ReferenceSlot` 必须指定 `Strong = "类型名"` 或 `Weak = "类型名"`；`ReferenceSequence` 必须指定 `Sequence = "元素类型名"`，且只能属于对象。目标须为本组反射对象或 `core.Object`。这些选项不能用于普通内联值。
- `Hidden` 只控制 Runtime 公共字段目录；字段仍参与追踪。C++ 的 public/private 与此独立。
- 漏标直接引用槽、序列或内嵌包含引用存储的记录/数组会报错，避免遗漏 GC 边。未标记的普通原生状态不自动成为反射字段；任意外部资源的所有权仍由原生实现负责。
- 对象支持一个 public、非 virtual 的原生反射基类，基类须在本组中；记录不支持继承。继承反射成员重名会报错。生成 `.base<Base>()`，实际地址调整由 C++ 编译器处理。
- 方法支持 void 或标量/枚举值结果，标量/枚举参数支持按值、`const T&` 和 `T&`；无 GC 引用的原生记录、Array、String 支持后两者。支持 const/noexcept 成员函数。静态方法、变参、模板、指针、右值/volatile 引用、引用返回、引用限定方法和运算符方法报错。聚合值按值参数/结果与借用视图参数尚未开放。生成器使用完整成员指针类型和完全限定的原生类型名消除重载与命名空间歧义。
- `AFUNCTION(BorrowSafe)` 发布 `MethodInfo::preserves_borrows`，允许同步原生值区间调用该方法。它是开发者对完整调用链的保证，不是自动效果分析；具体约束见[语言规则](../LANGUAGE.md#拥有型-stringarray-与原生算法)。普通方法无需此标记，已有调用行为保持。
- 满足自然对齐的标准布局类型中，公开反射字段额外生成 `offsetof`，由同一 Runtime 发布并校验，供源码借用直接访问。隐藏、非标准布局或不能保证自然对齐的字段继续使用既有成员访问器，不推测偏移；这一限制不影响原有反射能力。详见[原生值借用](native-value-borrows.md)。
- 构造、析构和记录复制能力沿用 `NativeRegistration`。生成代码带原头文件的 `#line`，编译器继续验证默认构造及不抛异常析构等约束。

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

首个真实消费者是 [World 的类型声明](../src/world/world.hpp)和[数学记录](../src/world/math.hpp)：七种类型、十五个字段及 `Counter::advance` 已迁移。`world::register_types` 保留原顺序、成组发布和字段路径绑定；`detail::Access` 保留业务访问职责。

拥有容器的类型定义由 `runtime/foundation.hpp` 提供，与生成类型一起提交到同一发布入口；完整示例、字段借用与生命周期合同见[基础库](foundation.md#共享反射与-runtime)。

## CMake 接入

目标按正常方式声明后调用：

```cmake
atelier_reflect(atelier_world
    SOURCE src/world/objects.cpp
    HEADERS src/world/math.hpp src/world/world.hpp)
```

`SOURCE` 必须在 `compile_commands.json` 中对应唯一真实编译命令，并包含全部待生成头。`BASE_DIR` 默认是项目 `src/`，可显式指定；头文件必须是其下的 `.hpp`。生成头的 include 路径相对此目录，例如 `world/world.gen.hpp`。生成 include 目录通过目标传播给消费者。

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

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target reflection_acceptance world_acceptance
cmake --preset release
cmake --build --preset release --target reflection_acceptance world_acceptance
```

[生成消费者](../acceptance/reflection/main.cpp)实际编译并调用生成方法，检查私有记录、重载别名、const/noexcept、配置宏和构析。[调用链消费者](../acceptance/reflection/calls.cpp)覆盖 i32、枚举、三种传递方式、原生/描述型存储、JIT 双向调用、别名、错误、协程与签名更新。[命令行消费者](../acceptance/reflection/cli.ps1)检查旧输出隔离、可重现写入、include 依赖及带位置的非法输入诊断。World 验收继续覆盖实际对象图、字段/方法、GC、JIT、协程和业务生命周期。

当前生成组要求共同解析其反射基类、记录与引用目标；跨生成组的语义导入尚未开放。工具升级随工具链锁文件进行；源码实现不承诺跨 Clang AST API 版本二进制兼容。初始 C#/ClangSharp、Attribute 路线对照见[选型记录](research/attribute-reflection-feasibility.md)。
