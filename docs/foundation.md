# 引擎 C++ 基础库

当前提供 `atelier::Array<T>`、`Span<T>`、`String`、`StringView`。它们服务于连续数据、临时计算、文本传递与共享 Runtime 值存储。独立目标为 `atelier_foundation`，不依赖 Runtime、World、UI 或 JIT；反射适配位于 `runtime/foundation.hpp`。完整 STL 兼容、关联容器、Unicode 排版及源码容器表达式不在本片范围。

## 类型与所有权

| 类型 | 存储与复制 | 入口 |
| --- | --- | --- |
| `Array<T>` | 独占连续缓冲；复制构造每个元素；移动转移缓冲，源变为空；析构剩余元素并释放缓冲 | [array.hpp](../src/foundation/array.hpp) |
| `Span<T>` | 指针与元素数量；复制只复制借用描述 | [span.hpp](../src/foundation/span.hpp) |
| `String` | 独占、有效 UTF-8，保证尾部零字符；复制拥有独立缓冲，移动后源为空 | [string.hpp](../src/foundation/string.hpp) |
| `StringView` | 指针与字节数量；允许外部未验证 UTF-8 与局部字节切片，不保证尾部零字符 | 同上 |

String 的缓冲复用 Array<char> 的拥有机制，未使用写时复制、小字符串优化或固定内联容量。Array 使用标准分配原语取得正确对齐的未初始化存储；宿主沿现有全局 new/delete 接入 mimalloc，无额外分配器注册表。当前没有测量相对 STL/Qt/UE 的性能收益。

原生容器可持有普通 C++ 资源；复制指针或句柄不复制其指向对象。Runtime 对象引用的存活仍由根、字段与写屏障管理。普通 `Array<ObjectId>` 不会自动成为 GC 根。

## Array 常用操作

```cpp
atelier::Array<atelier::String> names;
names.reserve(16);
names.emplace("sword");
names.emplace("shield");
names.add(names[0]);                 // 支持自身元素作为追加来源
names.append(names.view());          // 支持自身全部或部分视图
names.remove_at(0);                  // 保持顺序
names.remove_at_swap(0);             // 用末尾元素填洞，不保证顺序
auto removed = names.remove_if([](const atelier::String& name) {
    return name.view() == "shield";
});
names.clear();                      // 析构元素，保留容量
names.squeeze();                    // 显式收紧容量；空数组释放缓冲
```

- `size/capacity/empty` 查询状态，`data/begin/end/view` 提供连续访问。`operator[]` 在所有构建中检查索引；`try_at` 在越界时返回空指针。`find` 返回索引或 `npos`，`contains` 返回是否存在。
- `reserve(n)` 只准备容量；`resize(n)` 对新增元素做值初始化，对移除元素做析构；`emplace` 直接构造新增元素。`release()` 清空并释放缓冲。删除不自动缩容。
- 扩容为几何增长；只构造实际元素，不构造预留槽位。元素要求不抛异常析构，并提供不抛异常的移动构造或复制构造。支持 move-only 元素，但此时复制 Array 的接口不可用。
- 扩容优先不抛异常移动，否则复制。复制失败时清理新存储，保留旧值；复制赋值先构造完整副本再替换。不会对任意非平凡对象做字节搬运。
- `emplace/add` 构造新增元素可能由调用者提供任意参数；若元素构造主动修改参数，包括移动自身元素，失败时不能撤销这些用户行为。保序删除和 `remove_if` 若元素赋值或谓词抛异常，容器仍可析构，但部分元素可能已改变。
- 非法索引/切片/删除范围抛 `std::out_of_range`，容量溢出抛 `std::length_error`，分配失败沿 C++ 分配异常传播。通过 Runtime 调用的异常按原有边界转为 `Result` 错误。

## 文本与视图

```cpp
atelier::String label{"你好"};
label.append(" / loot");
atelier::StringView input = label.view();
auto suffix = input.slice(6);       // 字节偏移；“你好”为 6 个 UTF-8 字节
bool found = input.contains("loot");
```

String 的 `size/capacity` 不含末尾零字符，所有索引、查找和切片位置均为字节。带长度的 StringView 保留嵌入零字节；零结尾构造只扫描首个零。`c_str()` 可用于需要零结尾的接口，但对方可能在嵌入零处停止。

String 的构造、`assign` 与 `append` 验证 UTF-8：拒绝截断序列、非法续字节、过长编码、代理区和超出 U+10FFFF 的编码；失败不更改原字符串。支持从自身视图追加或赋值。String 不提供可写字符地址。`clear/reserve/squeeze/release` 与 Array 保持相同容量含义。

StringView 的 `slice/find/starts_with/ends_with/contains` 都按字节工作，切片可能截断码点；`is_valid_utf8()` 可明确检查。转成 String 时重新校验。`std_view()` 是对标准库消费者的显式借用适配，不创造另一份拥有存储。大小写折叠、Unicode 规范化、字符/字素索引和本地化文本未实现。

Span 的 `slice(first,count)` 按元素切片；`Span<T>` 可转 `Span<const T>`，反向不允许。`const Span<T>` 只冻结描述本身，不冻结元素。只读元素必须写 `Span<const T>`。

**借用有效期由拥有者和修改行为决定。** 销毁、释放、替换存储及实际扩容使原地址失效；删除和压缩可能让相同地址对应不同元素；clear 结束全部元素生命周期。移动转移存储后，旧视图若仍使用便依赖新拥有者。视图不冻结容器，不提供并发保护，也不能无条件跨协程挂起保存。直接对临时 Array/String 取 `view()` 及从临时 String 构造 StringView 被禁止；C++ 调用者仍须保证其他借用路径有效。

## 共享反射与 Runtime

`AFIELD()` 已接受 String 和具体的 Array<T>，包括无引用记录、枚举与嵌套数组。生成器仍产生既有 `NativeRegistration` 字段描述。模块入口把所需容器类型与普通类型提交到**同一个 `Runtime::register_types` 事务**：

```cpp
#include "runtime/foundation.hpp"

auto types = runtime.register_types({
    atelier::string_definition(runtime),
    atelier::array_definition<Item>(runtime, "inventory.Items"),
    atelier::reflection::Definition<Item>::build(runtime),
    atelier::reflection::Definition<Inventory>::build(runtime)
});
```

每个具体原生类型发布一次。模块决定数组的语义名称；同批定义顺序任意，元素类型也可以先前已发布。没有静态自动注册或独立容器注册表。String 的默认定义名称是 `core.String`；`TypeKind::string` 表示引擎 UTF-8 String；`TypeKind::array` 与 `TypeInfo::element` 表达数组及其权威元素 TypeId。

Runtime 校验原生元素表示和复制能力；当前反射数组只接受不含 GC 引用的值元素，包括隐藏字段中的引用也会导致拒绝。容器元素边参与依赖校验，当前拒绝递归值容器定义。对象引用集合继续使用现有 `ReferenceSequence`。StringView/Span 无拥有生命周期，不允许作为持久 AFIELD；漏标 Array 内的 Runtime 引用存储也会诊断。

字段可以属于原生对象，也可以属于运行时计算布局的描述型对象。构造、复制、赋值、析构复用 `initialize_value/copy_construct_value/assign_value/destroy_value`，对象销毁或 GC 回收时沿同一路径清理拥有缓冲。`with_native<T>(object, path, callback)` 是现有同步原生借用入口的字段重载，校验最终字段的类型与原生表示，借用范围内保护对象存储；`T` 可为 const。返回值必须拥有其数据，直接返回 Span/StringView 会被拒绝。

本片不提供运行时擦除类型的数组增删/元素编辑器，也未把 String/Array 变成 Scalar 参数。当前 AFUNCTION、语言参数及 JIT 调用仍限于已支持的标量/枚举与三种传递方式。非平凡值调用 ABI、源码容器表达式与跨挂起生命周期接线是下一片工作。

## 验收

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target foundation_acceptance reflection_acceptance
cmake --build --preset release --target foundation_acceptance reflection_acceptance
```

[消费者](../acceptance/foundation/main.cpp)实际运行原生 Inventory、宏生成元数据、描述型字段、保存独立快照、Runtime 值复制与 GC 回收资源的链路，同时覆盖自身追加、UTF-8、过对齐元素、move-only 资源、失败清理、容量溢出及错误类型借用。生成器 CLI 覆盖持久视图和数组隐藏引用的拒绝。检查在 dev/release 中均有效；总 `acceptance` 入口包含 foundation 验收。

设计依据与 UE/Qt 的取舍见[研究记录](research/shared-foundation-types.md#qtue-接口取舍与原生基础库首片)。

本片 dev/release 已通过上述消费者、15 项反射 CLI 拒绝场景，以及 Runtime、源码公共消费者、JIT 和 World 端到端验收。未执行 UI/图形窗口验收，本片未改动对应模块。
