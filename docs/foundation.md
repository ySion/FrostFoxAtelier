# 引擎 C++ 基础库

当前提供 `atelier::Array<T>`、`Span<T>`、`String`、`StringView`、`Map<K,V>`、`Set<T>` 和 `Hash/Equal`。它们服务于连续数据、文本、唯一键索引与共享 Runtime 值存储。独立目标为 `atelier_foundation`，不依赖 Runtime、World、UI 或 JIT；反射适配位于 `runtime/foundation.hpp`。Map/Set 当前服务原生 C++ 消费者，尚未开放反射或源码容器定义。完整 STL 兼容与 Unicode 排版尚未提供。源码已能同步借用原生 String/Array 并访问同一缓冲，也能创建有作用域生命周期的拥有型局部，规则见[原生值借用](native-value-borrows.md)。

## 类型与所有权

| 类型 | 存储与复制 | 入口 |
| --- | --- | --- |
| `Array<T>` | 独占连续缓冲；复制拥有独立元素，平凡操作可批量完成；移动转移缓冲，源变为空；析构剩余元素并释放缓冲 | [array.hpp](../src/foundation/array.hpp) |
| `Span<T>` | 指针与元素数量；复制只复制借用描述 | [span.hpp](../src/foundation/span.hpp) |
| `String` | 独占、有效 UTF-8，保证尾部零字符；复制拥有独立缓冲，移动后源为空 | [string.hpp](../src/foundation/string.hpp) |
| `StringView` | 指针与字节数量；允许外部未验证 UTF-8 与局部字节切片，不保证尾部零字符 | 同上 |
| `Map<K,V>` | 独占键和值；复制独立拥有，移动转移存储；键只读、值可修改，无遍历顺序保证 | [map.hpp](../src/foundation/map.hpp) |
| `Set<T>` | 独占唯一成员；复制独立拥有，移动转移存储；成员只读，无遍历顺序保证 | [set.hpp](../src/foundation/set.hpp) |

String 使用 32 字节对象及 15 字节短字符串内联存储（SSO），长文本由独占堆缓冲持有。字节复制直接搬移连续缓冲，不构造随后会被覆盖的字符。Array 使用标准分配原语取得正确对齐的未初始化存储；宿主的 String 和 Array 均沿现有全局 new/delete 接入 mimalloc，无额外分配器注册表。取舍与同机采样见[字符串研究与测量](research/string-storage-performance.md)。

原生容器可持有普通 C++ 资源；复制指针或句柄不复制其指向对象。Runtime 对象引用的存活仍由根、字段与写屏障管理。普通 `Array<ObjectId>` 不会自动成为 GC 根。

Array/String/Map/Set 自复制、自移动和自交换保持原值；移动后的源为空且可复用。重复 clear/release、重复删除缺失键不改变逻辑内容；reserve 不减少容量。Array/Map/Set 的移动赋值先接管来源，再释放旧内容，因此来源即使由目标内某个资源间接拥有，也不会在转移前被析构。元素构析、Hash/Equal 或谓词不得重入并结构修改正在执行操作的同一容器。

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
- 扩容优先不抛异常移动，否则复制。复制失败时清理新存储，保留旧值。复制、追加与扩容只在类型 trivially-copyable 且实际选中的构造也平凡时批量复制；有序删除另外要求平凡移动赋值，使用可重叠的 memmove。类型标签不会跳过选中模板构造或赋值的副作用。
- 复制赋值在容量足够、元素可平凡复制构造且平凡复制赋值时复用缓冲，包括空值、变短和重新增长；其他情况先构造完整副本再替换，失败保留目标值。可复制但不可赋值的元素仍然可用。String、智能指针和其他资源类型保留逐元素构析；不抛异常的非平凡赋值不自动取得字节搬运或容量复用资格。
- 预留容量中的单元素 emplace 直接构造并在成功后增加大小；实际扩容仍先构造新增后缀，再处理旧前缀，保留自身元素或视图作为来源的合同。remove_if 仍逐元素交替调用谓词与搬移，谓词可以观察之前已经压缩的位置。
- `emplace/add` 构造新增元素可能由调用者提供任意参数；若元素构造主动修改参数，包括移动自身元素，失败时不能撤销这些用户行为。保序删除和 `remove_if` 若元素赋值或谓词抛异常，容器仍可析构，但部分元素可能已改变。
- 非法索引/切片/删除范围抛 `std::out_of_range`，容量溢出抛 `std::length_error`，分配失败沿 C++ 分配异常传播。通过 Runtime 调用的异常按原有边界转为 `Result` 错误。

## Map、Set 与哈希

```cpp
atelier::Map<atelier::String, unsigned> resources;
resources.reserve(128);                    // 可容纳的总条目数
auto [value, inserted] = resources.try_emplace("材质/stone", 7u);
if (const auto* found = resources.find(atelier::StringView{"材质/stone"})) {
    // found 借用表中的值；本次查询不创建拥有型 String。
}
resources.insert_or_assign("材质/stone", 9u);
for (auto [key, current] : resources) {      // 两个绑定都借用元素
    current += 1;                          // key 只读
}
resources.remove("材质/stone");
resources.clear();                         // 析构元素、保留容量
resources.release();                       // 释放存储

atelier::Set<unsigned> seen;
bool first = seen.add(7u);                  // 首次为 true，重复为 false
seen.remove(7u);                            // 返回是否移除了成员
```

### 接口、拥有与失效

- `size/empty` 查询状态；`find` 返回借用指针或空，查询缺失不插入。const Map 返回 `const V*`，Set 始终返回 `const T*`。Map 的 `try_emplace` 和 `insert_or_assign` 返回 `{value, inserted}`；Set 的 `add` 返回是否新增。
- `try_emplace` 先查键，命中时不构造键/值，不消费转入的资源；C++ 实参表达式本身仍先求值。`insert_or_assign` 先构造完整替换值，再以不抛移动提交。支持 move-only 元素，此时容器复制接口不可用；复制赋值先构造副本再替换，不依赖元素复制赋值。
- Map 遍历返回只读键、可写值的借用代理；推荐示例中的结构化绑定。const Map 遍历的值也只读；Set 遍历仅只读成员。普通遍历期间不得结构修改容器。迭代器不暴露上游类型，也不承诺随机访问。
- 插入、删除、预留容量、clear/release、赋值、移动和 swap 后，调用者重新取得迭代器、元素指针与引用；结构操作失败也可能已经搬动缓冲。即使部分操作在当前容量下没有搬移，也不额外保证借用稳定。删除会以尾元素填洞，遍历顺序和地址不作为业务身份。
- `reserve(n)` 按总条目数准备存储，不构造元素。删除和 clear 不自动缩容，release 释放存储。超过底层可索引范围的 reserve 抛 `std::length_error`，不静默截断。默认空表不动态分配索引；沿既有全局 new/delete 使用 mimalloc。
- 不提供并发保护；同一容器不能在读/遍历时被另一线程修改。复制指针或身份值不复制其目标，也不自动登记 Runtime 根。

### Hash/Equal 与失败合同

[Hash/Equal](../src/foundation/hash.hpp) 是共同入口。一般类型复用现有 `std::hash<T>` 与 `operator==`，因此 ObjectId 继续包含 Runtime 上下文、槽位和代际。自定义策略作为 Map/Set 的模板参数提供，当前由容器默认构造；策略本身的默认/复制/移动/交换/析构及 Hash/Equal 调用必须不抛，检查包含结果到 size_t/bool 的转换。移动容器时源获得新默认策略，不要求其已移空的旧策略仍可调用。元素必须具备不抛移动构造、移动赋值和析构，编译期拒绝不满足的类型。标准类型的相等运算若未声明 noexcept，也需提供满足该合同的比较策略。

非透明策略只接受实际键类型，必要的类型转换在调用之前显式完成，例如 `Set<unsigned>::contains(1u)`。异构查询要求 Hash/Equal 同时声明 `is_transparent`，并且实际 Q→哈希结果、Q/K→相等结果的完整调用均不抛；不以策略中存在某个重载来推断后端会跳过键构造。这样查询不会在 noexcept 包装内部隐式执行可抛键转换。

Equal 必须是键域上的等价关系，相等键必须同哈希；键留在表内期间，这些结果必须稳定，包括策略访问的间接状态。普通浮点相等对 NaN 不满足该要求，使用浮点键时须排除 NaN 或提供成对一致的 Hash/Equal。编译期 noexcept 检查不证明这些语义规律。

String/StringView 共用同一按长度的字节哈希和比较，包含嵌入零，不做大小写折叠或 Unicode 规范化；零结尾字符指针作为便利用法，在首个零处结束，空指针按空串处理。透明查询不创建拥有存储，也不验证 UTF-8；插入 String 键时才建立其 UTF-8 不变量。持久文本键使用 String，StringView 作为键则仍由调用方保证外部字节存续且内容不变。自定义透明策略必须使查询表示与拥有键的相等和哈希一致；结构体不能默认哈希含 padding 的原始表示。

构造/复制/分配失败沿 C++ 异常传播。插入失败不发布新成员；复制赋值或替换值构造失败保留目标的原值，部分构造的资源会清理。reserve 失败保留成员内容，但可能已经改变缓冲与容量。调用者主动移动表内资源或构造函数修改实参属于用户副作用，不由容器回滚。Hash/Equal 不保存输入借用、不修改表、不重入结构操作；不能通过错误的 noexcept 声明掩盖会抛异常的实现。

底层使用锁定的 `ankerl::unordered_dense` 分组索引与连续值存储，不暴露 bucket/load-factor、持久化私有布局或后端入口。版本、下载与校验值统一在[依赖锁文件](../cmake/dependencies-lock.json)，构建复制 `LICENSE.unordered_dense.txt`，当前无上游补丁。内部复用上游标准容器，不另建分配器注册表。哈希值与遍历顺序不属于持久格式或跨版本协议；确定性输出显式排序。

### 正式消费者

- AssetBrowser 的 String→ControlKey 字典维护既有 nav/item/row/type/size 身份域；刷新、过滤和临时不可见不更换已有键，reset 清表而计数继续增长。查询自身不构造 String，调用者的前缀拼接仍可能分配，不宣称整条路径零分配。
- World 接纳 Actor/组件时的临时 `Set<ObjectId>` 拒绝重复身份，沿原顺序完成准备、验证与发布；Set 不保活对象。World 持久成员索引、Renderer 缓存及其他容器不在本片迁移范围。

## 文本与视图

```cpp
atelier::String label{"你好"};
label.append(" / loot");
atelier::StringView input = label.view();
auto suffix = input.slice(6);       // 字节偏移；“你好”为 6 个 UTF-8 字节
bool found = input.contains("loot");
```

String 的 `size/capacity` 不含末尾零字符，所有索引、查找和切片位置均为字节。带长度的 StringView 保留嵌入零字节；零结尾构造只扫描首个零。`c_str()` 可用于需要零结尾的接口，但对方可能在嵌入零处停止。

从 StringView 构造、`assign` 与 `append` 验证 UTF-8：拒绝截断序列、非法续字节、过长编码、代理区和超出 U+10FFFF 的编码；失败不更改原字符串。支持从自身视图追加或赋值。String 不提供可写字符地址。复制有效 String、`assign(const String&)` 或 `append(const String&)` 直接复制已验证字节；任意 StringView，包括来自有效 String 的字节切片，仍需校验。ASCII 校验按完整 8 字节块跳过，残余及非 ASCII 序列继续按 UTF-8 规则处理，不越过视图边界。

容量不包含末尾零字符。空 String 的内联容量为 15；不超过此字节数的普通构造与复制不分配堆缓冲。`reserve` 只准备容量，堆增长采用需求值与约 1.5 倍旧容量的较大值；构造长文本只依据内容长度准备缓冲，不继承源对象预留的大容量。赋值若容量足够便复用缓冲，变短不会自动缩容。`clear` 保留容量，`squeeze` 将堆容量收紧到长度，短值可退回内联存储；`release` 释放堆缓冲并回到空内联表示。分配或 UTF-8 校验失败保留原值。

StringView 的 `slice/find/starts_with/ends_with/contains` 都按字节工作，切片可能截断码点；`is_valid_utf8()` 可明确检查。转成 String 时重新校验。`std_view()` 是对标准库消费者的显式借用适配，不创造另一份拥有存储。大小写折叠、Unicode 规范化、字符/字素索引和本地化文本未实现。

Span 的 `slice(first,count)` 按元素切片；`Span<T>` 可转 `Span<const T>`，反向不允许。`const Span<T>` 只冻结描述本身，不冻结元素。只读元素必须写 `Span<const T>`。

**借用有效期由拥有者和修改行为决定。** 销毁、释放、替换存储及实际扩容使原地址失效；删除和压缩可能让相同地址对应不同元素；clear 结束全部元素生命周期。Array 和堆 String 移动转移缓冲后，原地址若仍使用便依赖新拥有者；内联 String 移动复制字节到目标对象，旧视图不能跟随该值。调用者统一按移动后重新获取 String 视图处理。视图不冻结容器，不提供并发保护，也不能无条件跨协程挂起保存。直接对临时 Array/String 取 `view()` 及从临时 String 构造 StringView 被禁止；C++ 调用者仍须保证其他借用路径有效。

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

源码通过既有 `Program` / `Argument::borrow` 接受原生记录、Array 的只读或可写借用，以及 String 的只读借用。数组长度、元素和有直接偏移的原生记录字段由生成代码访问同一存储；String 只开放字节长度和只读索引。布局和序列视图来自同一 Runtime 元数据，不把 String/Array 塞进 `Scalar`。`AFUNCTION` 原生成员同时支持无 GC 引用的原生记录/Array/String 引用，聚合值仍不进入 Scalar。

源码拥有型局部复用原生构析操作，序列修改经 `Runtime::mutate_sequence` 分派到基础库；`sequence_mutate` 与 `value_operations_preserve_borrows` 由同一 TypeInfo 发布，数组能力还取决于元素的操作契约。当前没有数组元素编辑器、非平凡按值调用 ABI 或跨挂起借用。同步原生值区间只允许 `BorrowSafe` 原生方法，结构修改限于源码拥有的序列，并把 GC 留给区间外的宿主；原生调用方仍负责实际存续期。详细限制由 [LANGUAGE.md](../LANGUAGE.md#原生值借用)维护。

## 验收

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target foundation_acceptance reflection_acceptance
cmake --build --preset release --target foundation_acceptance reflection_acceptance
```

Map/Set 的[资源目录消费者](../acceptance/foundation/associative.cpp)包含在 foundation_acceptance：拥有资源与独立快照、64 字节对齐、move-only 资源、复制/构造失败、透明查询、UTF-8 与嵌入零、冲突密集增删和 ObjectId 跨上下文/代际及 GC 不保活。业务接入同时使用 `world_acceptance` 和 `editor_document_acceptance`；后者沿真实 Project→Workspace→UI 接受帧验证中文/非 BMP 资源键、目录刷新、虚拟列表与 reset 身份。以上均在 dev/release 执行，不依赖 assert，也不新增单元测试框架。本片未测量性能收益；未执行 GPU/原生窗口或输入法验收，原有窗口待验与剪贴板问题不由此关闭。

[生命周期消费者](../acceptance/foundation/lifecycle.cpp)进一步覆盖有状态 Hash/Equal 移动后复用、可抛比较结果与非透明键转换的拒绝、资源子树提升时的移动赋值顺序、自赋值/自移动/自交换、重复清空/释放/删除、空与末尾切片、SSO 边界和无效 UTF-8 失败保值；同样进入 foundation_acceptance。Array 生命周期变化同时由反射值与源码拥有值的既有消费者覆盖。

[消费者](../acceptance/foundation/main.cpp)实际运行原生 Inventory、宏生成元数据、描述型字段、保存独立快照、Runtime 值复制与 GC 回收资源的链路，同时覆盖自身追加、UTF-8、15/16 字节边界、内联/堆复制移动、容量复用、ASCII 块边界的合法/非法 UTF-8、过对齐元素、move-only 资源、失败清理、容量溢出及错误类型借用。Array 增量覆盖标量批量复制与自身切片追加、赋值复用中的活前缀/新尾部、空值与零初始化、重叠删除、过对齐数据包、实际选中的模板构造/赋值副作用，以及预留目标复制失败时的强保证。共享 Runtime 值赋值也确认标量数组的容量复用。生成器 CLI 覆盖持久视图和数组隐藏引用的拒绝。检查在 dev/release 中均有效；总 `acceptance` 入口包含 foundation 验收。

[源码借用消费者](../acceptance/source/native_values.cpp)通过 `source_acceptance` 额外覆盖原生缓冲直接写入、64 字节对齐记录、String、嵌套数组、别名和同步存续期；[拥有值消费者](../acceptance/source/owned_values.cpp)覆盖 String/Array 局部、原生算法、深复制、部分失败与作用域清理；该目标也包含在 `jit_acceptance` 中。

设计依据与 UE/Qt 的取舍见[研究记录](research/shared-foundation-types.md#qtue-接口取舍与原生基础库首片)。

本片 dev/release 已通过上述消费者、15 项反射 CLI 拒绝场景，以及 Runtime、源码公共消费者、JIT 和 World 端到端验收。未执行 UI/图形窗口验收，本片未改动对应模块。


## 字符串性能采样入口

```powershell
cmake --build --preset release --target atelier_string_measure
./out/release/atelier_string_measure.exe
```

该目标独立于普通验收，输出 JSON Lines：短文本构造/复制、预留容量赋值和拼接、已验证文本追加、外部 UTF-8 写入、4 KiB 复制，以及 2 万项保留名称目录的耗时、分配次数和内存。MSVC STL 对照使用相同的外部 UTF-8 校验合同；结果是具体工作负载采样，计时阈值不作为功能验收条件。

## Array 性能采样入口

```powershell
cmake --build --preset release --target atelier_array_measure
./out/release/atelier_array_measure.exe
```

独立 JSON Lines 采样覆盖 4096 个整数的复制、预留追加、复用赋值、扩容、保序删除与过滤，64 项增量构造、值初始化，512 个过对齐 64 字节数据包复制，以及 64 个拥有型 String 的追加与赋值。与 MSVC std::vector 使用相同输入、全局 mimalloc 和有效操作；复制/过滤场景包括准备工作，删除场景循环移除并补回 32 项，不混入重新构造整个批次的时间。7 次样本取中位数，记录分配次数与分配器取整字节；不以采样阈值判断功能通过。安全边界、工业实现取舍和实测结果见[Array 研究](research/array-storage-performance.md)。
