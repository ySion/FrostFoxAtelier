# 引擎 C++ 基础库

当前提供 `atelier::Array<T>`、`SharedArray<T>`、`Span<T>`、`String`、`StringView`、`Map<K,V>`、`Set<T>` 和 `Hash/Equal`。它们服务于连续数据、共享不可变数据、文本、唯一键索引与共享 Runtime 值存储。独立目标为 `atelier_foundation`，不依赖 Runtime、World、UI 或 JIT；反射适配位于 `runtime/foundation.hpp`。Map/Set 当前服务原生 C++ 消费者，尚未开放反射或源码容器定义。完整 STL 兼容与 Unicode 排版尚未提供。源码已能同步借用原生 String/Array 并访问同一缓冲，也能创建有作用域生命周期的拥有型局部，规则见[原生值借用](native-value-borrows.md)。

容器能力和迁移由真实消费者与测量驱动，不以减少 `std::` 数量为目标。成熟原子、类型特征、构析原语和算法可直接复用；第三方与平台类型的转换留在各模块接入边界。选型依据见[工业基础库研究](research/foundation-stl-industrial-research.md)。

## 类型与所有权

| 类型 | 存储与复制 | 入口 |
| --- | --- | --- |
| `Array<T>` | 独占连续缓冲；复制拥有独立元素，平凡操作可批量完成；移动转移缓冲，源变为空；析构剩余元素并释放缓冲 | [array.hpp](../src/foundation/array.hpp) |
| `SharedArray<T>` | 计数句柄，共享一个不可变 Array；复制只增加计数，写入前先取得独占；移动后源为空 | [shared_array.hpp](../src/foundation/shared_array.hpp) |
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
- `reserve(n)` 只准备容量；`resize(n)` 对新增元素做值初始化，`resize(n, value)` 从 value 复制构造新增元素，value 可以是本数组的元素；两者都对移除元素做析构，失败时数组不变。`emplace` 直接构造新增元素。`release()` 清空并释放缓冲。删除不自动缩容。
- 扩容为几何增长；只构造实际元素，不构造预留槽位。元素要求不抛异常析构，并提供不抛异常的移动构造或复制构造。支持 move-only 元素，但此时复制 Array 的接口不可用。
- 扩容优先不抛异常移动，否则复制。复制失败时清理新存储，保留旧值。复制、追加与扩容只在类型 trivially-copyable 且实际选中的构造也平凡时批量复制；有序删除另外要求平凡移动赋值，使用可重叠的 memmove。类型标签不会跳过选中模板构造或赋值的副作用。
- 复制赋值在容量足够、元素可平凡复制构造且平凡复制赋值时复用缓冲，包括空值、变短和重新增长；其他情况先构造完整副本再替换，失败保留目标值。可复制但不可赋值的元素仍然可用。String、智能指针和其他资源类型保留逐元素构析；不抛异常的非平凡赋值不自动取得字节搬运或容量复用资格。
- 预留容量中的单元素 emplace 直接构造并在成功后增加大小；实际扩容仍先构造新增后缀，再处理旧前缀，保留自身元素或视图作为来源的合同。remove_if 仍逐元素交替调用谓词与搬移，谓词可以观察之前已经压缩的位置。
- `emplace/add` 构造新增元素可能由调用者提供任意参数；若元素构造主动修改参数，包括移动自身元素，失败时不能撤销这些用户行为。保序删除和 `remove_if` 若元素赋值或谓词抛异常，容器仍可析构，但部分元素可能已改变。
- 非法索引/切片/删除范围抛 `std::out_of_range`，容量溢出抛 `std::length_error`，分配失败沿 C++ 分配异常传播。通过 Runtime 调用的异常按原有边界转为 `Result` 错误。

## 共享不可变数组

```cpp
atelier::Array<atelier::math::Vector3> positions = /* ... */;
atelier::SharedArray<atelier::math::Vector3> column{std::move(positions)}; // 接管缓冲，不复制元素
auto copy = column;                       // 共享同一个块，只增加计数
const auto& values = copy.array();        // 只读；共享内容在类型上不可写
auto& owned = copy.write();               // 共享时先复制成独占，再返回可写 Array
owned[0] = {};                            // column 仍看到原值
bool shared = same(column, copy);         // false：两者已是不同的块
```

- 句柄 8 字节，指向堆上的一个块：原子计数加一个 Array。从 `Array<T>&&` 构造只分配这个块并接管缓冲，分配失败时源 Array 不变。复制句柄只增加计数；最后一个句柄释放时析构元素、释放缓冲和块。默认构造与移动后的源是没有块的空句柄，不分配，`array()` 返回共用的空 Array；读取接口不区分空句柄与块中 Array 为空，`same()` 区分二者。
- 只读访问为 `array()`、`view()`、`size()`、`empty()`；临时句柄不能取 `array()/view()/write()`。`write()` 在本句柄是块的唯一持有者时原地返回块中的 Array，否则先把内容复制进新块再返回；复制失败时句柄不变，空句柄的 `write()` 新建一个块。`same(a, b)` 判断两者是否共享同一个块，两个空句柄也算相同。没有弱引用、计数查询或可作版本键的地址。
- 块的地址在任何句柄存活期间不变，移动句柄不移动块，因此可以把 `array()` 只读借出、把 `write()` 的结果可写借出，包括经 Runtime 借给源码。`array()/view()` 的结果只在该句柄没有被赋值、移动、`write()` 或析构期间有效；`write()` 返回的引用还要求该句柄没有再被复制，借出期间复制句柄，副本会看到之后的写入，跨线程即数据竞争。借给源码时，调用方在调用期间持有出借的句柄，不对它做上述操作。源码不能给借来的 Array 扩容，借出期间元素缓冲不会重新分配。
- 计数递增为 relaxed，递减为 release，最后的持有者析构前取 acquire 栅栏；`write()` 以 acquire 判断唯一性。指向同一块的不同句柄可以在任意线程同时复制、析构和读取；同一个句柄的只读操作（读取、从它复制）可以并发，修改它（赋值、移动、`write()`、析构）时不能有其他线程同时使用它。线程独占一个句柄时，在任何线程调用 `write()` 都安全。元素在释放最后一个句柄的线程上析构。源码只在 Runtime 线程执行：工作线程可以持有、读取和 `write()`，不能进入源码。
- 元素须是不带 const/volatile 的可复制类型，并且是平凡可复制类型或 String；声明了 `is_runtime_reference` 的 Runtime 引用值（ObjectReference、ReferenceSlot）被拒绝，ObjectRoot 等非平凡根句柄不满足前一条。嵌在平凡结构体中的引用值无法在编译期识别；与 `Array<ObjectId>` 一样，SharedArray 中的身份值不是 GC 根，指针、Span 与 StringView 元素也不延长所指对象的寿命。
- SharedArray 不注册为 Runtime 类型，不能作 Runtime 对象字段，也不是源码类型。

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

[Hash/Equal](../src/foundation/hash.hpp) 是共同入口。整数、枚举和指针使用锁定后端的整数哈希，即一次 64 位乘法混合，不读取这些类型的 `std::hash` 特化；MSVC STL 的 `std::hash` 对整数逐字节做 FNV-1a，因此不经过它。其他类型的哈希复用现有 `std::hash<T>`；相等仍使用 `operator==`（String/StringView 除外）。结果已充分混合的哈希声明 `is_avalanching`，表直接使用结果而不再混合一次；`std::hash<T>` 声明了该标记时，`Hash<T>` 随之声明。TypeId 与 ObjectId 的 `std::hash` 由 `hash_fields` 逐字段混合 Runtime 上下文、槽位以及 ObjectId 的代际，与各自的完整相等规则一致；`std::unordered_*` 与 Map/Set 因此得到同一哈希。自定义策略作为 Map/Set 的模板参数提供，当前由容器默认构造；策略本身的默认/复制/移动/交换/析构及 Hash/Equal 调用必须不抛，检查包含结果到 size_t/bool 的转换。移动容器时源获得新默认策略，不要求其已移空的旧策略仍可调用。元素必须具备不抛移动构造、移动赋值和析构，编译期拒绝不满足的类型。标准类型的相等运算若未声明 noexcept，也需提供满足该合同的比较策略。

非透明策略只接受实际键类型，必要的类型转换在调用之前显式完成，例如 `Set<unsigned>::contains(1u)`。异构查询要求 Hash/Equal 同时声明 `is_transparent`，并且实际 Q→哈希结果、Q/K→相等结果的完整调用均不抛；不以策略中存在某个重载来推断后端会跳过键构造。这样查询不会在 noexcept 包装内部隐式执行可抛键转换。

Equal 必须是键域上的等价关系，相等键必须同哈希；键留在表内期间，这些结果必须稳定，包括策略访问的间接状态。普通浮点相等对 NaN 不满足该要求，使用浮点键时须排除 NaN 或提供成对一致的 Hash/Equal。编译期 noexcept 检查不证明这些语义规律。

String/StringView 共用同一按长度的字节哈希和比较，包含嵌入零，不做大小写折叠或 Unicode 规范化；零结尾字符指针作为便利用法，在首个零处结束，空指针按空串处理。透明查询不创建拥有存储，也不验证 UTF-8；插入 String 键时才建立其 UTF-8 不变量。持久文本键使用 String，StringView 作为键则仍由调用方保证外部字节存续且内容不变。自定义透明策略必须使查询表示与拥有键的相等和哈希一致；结构体不能默认哈希含 padding 的原始表示。

构造/复制/分配失败沿 C++ 异常传播。插入失败不发布新成员；复制赋值或替换值构造失败保留目标的原值，部分构造的资源会清理。reserve 失败保留成员内容，但可能已经改变缓冲与容量。调用者主动移动表内资源或构造函数修改实参属于用户副作用，不由容器回滚。Hash/Equal 不保存输入借用、不修改表、不重入结构操作；不能通过错误的 noexcept 声明掩盖会抛异常的实现。

底层使用锁定的 `ankerl::unordered_dense` 分组索引与连续值存储，不暴露 bucket/load-factor、持久化私有布局或后端入口。版本、下载与校验值统一在[依赖锁文件](../cmake/dependencies-lock.json)，构建复制 `LICENSE.unordered_dense.txt`，当前无上游补丁。内部复用上游标准容器，不另建分配器注册表。哈希值与遍历顺序不属于持久格式或跨版本协议；确定性输出显式排序。

### 正式消费者

- AssetBrowser 的 String→ControlKey 字典维护既有 nav/item/row/type/size 身份域；刷新、过滤和临时不可见不更换已有键，reset 清表而计数继续增长。查询自身不构造 String，调用者的前缀拼接仍可能分配，不宣称整条路径零分配。
- World 接纳 Entity 子树/组件时的临时 `Set<ObjectId>` 拒绝重复身份，沿原顺序完成准备、验证与发布；Set 不保活对象。World 的成员位置索引、空间缓存（世界矩阵条目、失败记录、遍历去重）与物理姿态目标同样以 ObjectId 为键；编辑器 Workspace 用临时 `Set<ObjectId>` 收集存活和选中的对象。
- Runtime 以 C++ 表示地址和类型名索引已发布类型；名字键借用各类型内不可变的名称存储。

### 开放给源码（未实施）

Map/Set 目前没有反射或源码定义，也没有等待中的源码消费者。第一个消费者出现时单独出方案，交运行时负责人和总负责人审；下列要点是那时的契约起点：

- 与 Array 相同，由宿主在同一个 `register_types` 事务中提交具体的 Map/Set 类型；键与值不含 GC 引用。
- 键先支持整数、枚举与 String。浮点键在规定 -0 与 NaN 的等价规则之前拒绝。
- 命中与缺失用 `contains(key)` 和 `bool try_get(key, V& out)` 表达，不引入 Optional；前提是原生方法的可写引用实参能沿现有调用路径传递。
- 遍历用 `size()`、`key_at(i)`、`value_at(i)` 按下标访问连续值存储。顺序只承诺同一进程中相同的操作序列得到相同顺序；它不是插入顺序（删除以末尾元素填洞），也不取决于哈希值。按下标遍历把连续值存储的性质带进语言，以后若换用非连续后端，需要另外维护值数组。
- 借来的 Map/Set 只能查找和原地写值；插入与删除是结构修改，只用于源码拥有的局部，与借来的 Array 不能扩容是同一条规则。

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

源码拥有型局部复用原生构析操作，序列修改经 `Runtime::mutate_sequence` 分派到基础库；`sequence_mutate` 与 `value_operations_preserve_borrows` 由同一 TypeInfo 发布，数组能力还取决于元素的操作契约。当前没有数组元素编辑器、非平凡按值调用 ABI 或跨挂起借用。同步原生值区间只允许 `BorrowSafe` 原生方法和按值读取 `self` 字段，结构修改限于源码拥有的序列，并把 GC 留给区间外的宿主；原生调用方仍负责实际存续期。详细限制由 [LANGUAGE.md](../LANGUAGE.md#原生值借用)维护。

## 原生 fork-join 任务

[tasks.hpp](../src/foundation/tasks.hpp) 提供 `atelier::tasks::parallel_for` 和单次使用的 `JoinGroup`，用于普通 C++ 值上的同步计算；当前产品消费者是 [Scatter](../src/procedural/operations.cpp)。公共头不暴露 oneTBB 类型，后端版本与源码校验值只在[依赖锁文件](../cmake/dependencies-lock.json)。构建仅部署对应配置的一份 TBB 调度 DLL 与许可证，不构建或分发 tbbmalloc；通用分配器仍按项目既有 mimalloc 入口接管。

```cpp
auto candidate = snapshot;
const auto input = snapshot.view();
const auto output = candidate.write().view(); // 发起前完成 COW、容量及元素构造
const auto completion = atelier::tasks::parallel_for(
    {0, input.size()},
    [input, output](atelier::tasks::Range range) {
        for (auto i = range.begin; i < range.end; ++i)
            output[i] = input[i] * 2;
    },
    1024); // grain 提示，不是每块的最大长度
// 仅 completed 时发布 candidate；取消/失败后的部分结果不能发布。
```

### 范围、完成与拥有者

- `Range{begin,end}` 是 size_t 半开区间。成功时每个索引恰好处理一次；空范围不调用 body。倒置范围或 grain=0 抛 invalid_argument。块长、顺序和执行线程不是业务结果；grain 交给自适应分块器，不引入持久 affinity 状态。
- caller 进入共享 arena 参与执行；arena 满时允许委派，不能以 caller 每次实际执行至少一块作为正确性前提。嵌套仍用同一池并继承父取消；已处于任务 body 的嵌套调用不再次 execute 切换默认 context。禁止用普通 future、兄弟任务屏障或业务锁等待池内同伴；依赖只通过结构化子任务 join 表达。
- `parallel_for` 借用 const-invocable 的 body，只向后端复制借用描述。`JoinGroup` 不可复制/移动，只有建立它的线程调用 `run/cancel/join`，body 可建立自己的子组。组拥有普通值闭包，存储与释放在提交线程；所有 body 执行结束后才释放。一次 join 后终态，不提供 detach、跨帧复用或后台任务。
- 每次 parallel_for 在进入 arena 前捕获 caller 的浮点设置；组在构造时捕获，供该组全部任务使用。P=1、单块及委派路径仍由后端应用、恢复相同设置。body 不修改浮点环境，不以无序浮点归约改变元素内原计算次序。
- foundation 内部唯一进程拥有者在首用时完成初始化，多个 Runtime/World 共用；不在 Runtime 中各建池。默认并行度 P=min(8,max(1,H/2))，H 为 hardware_concurrency 提示，P 包含 caller。它限制本 arena 并发与 TBB worker 预算，不限制外部应用线程，也不接管 Jolt、RHI 或解码线程。
- 进程退出前须停止发起任务，全部调用线程和组均已终态。禁止静态寿命 JoinGroup，禁止静态析构期再提交。拥有者在任务外先 terminate 本方 arena，再 finalize 调度器；arena 析构不是 join。finalize 返回失败会报告并终止进程，不伪装成已清空 worker。以后增加其他 TBB arena/handle 时须共同履行关停合同。

### 取消、失败与借用边界

- stop_token 与 `JoinGroup::cancel()` 是合作取消，不强杀已经执行的 body；长 body 自行选择合理分块。所有已派发工作结束后才返回 completed/cancelled，或重抛一个代表异常。异常优先于纯取消，不保证按索引或时间选错，不收集全量错误；部分输出不自动回滚。不同顶层调用的取消隔离。
- parallel_for 和组任务的 stop 回调只在已经绑定 context 的后端任务执行栈上存活，避免首次绑定覆盖提前设置的取消位；进入用户 body 前再次检查取消。返回/退栈时先注销并等待回调结束，之后任务才完成，因此 group wait 重置 context 前没有残留回调。组的显式取消另保留单调状态，首次绑定与 wait 重置都不能将其清除。
- 正常路径显式 join 消费错误；放弃组时析构取消并 drain，只在清理中丢弃任务错误。run 内部存储/提交失败也先 drain 再抛出。实参求值不在函数内，例如 `run([v=make()] {})` 的 make 抛错，须退出组作用域后在外层 catch。借用拥有者先于组建立，直到 join/退栈清理后才能销毁。若连进入 arena 完成 drain 都失败，报告并终止，不释放仍可能被任务读取的闭包后继续运行。
- 输入快照、COW 独占、增列/resize 与容量准备在发起前完成。worker 只借固定存储，不结构修改共享容器；输出不仅彼此不重叠，也不得覆盖其他分块的只读输入。固定拥有者覆盖整个 fork→join，借用在 join 返回时结束，不能逃逸后续任务。借出可写列期间不得复制其 SharedArray/Geometry 句柄、移动或重分配存储。
- Runtime 所属线程可同步发起普通值任务，但 body（包括 caller 执行、P=1、嵌套）不能调用 Runtime、其 Source/VM/JIT、GC、根操作或 World 命令，不能捕获 Runtime 对象内部地址；等待不泵 UI/World/Runtime 回调。普通值自定义构析、闭包和跨 join 的异常/错误载荷都不得携带 ObjectRoot/ValueRoot/Program/Coroutine 等 Runtime 依赖拥有者。不能以任务最终在 caller 上析构为理由放宽此限制。
- `tasks::in_parallel_body() noexcept` 在 dev/release 均可用，标记可嵌套的 body 执行及其局部对象异常退栈。它不包住调用前的闭包构造、join 后的闭包销毁或所有异常载荷转运，不证明 C++ 捕获安全。Runtime/Source 已接入开发版动态禁入诊断，具体范围见[任务体禁入范围](runtime-model.md#并行任务体禁入诊断)；纯查询不铺满检查，World 尚未覆盖，release 不做检查，也不是安全沙箱。
- 本入口不新增 Program 后台许可：Program 操作仍按现有 Runtime 线程与关闭/发布合同执行。无 Runtime 依赖的 MaterialProgram 既有并发求值合同不因本片而收紧，本片也不新增其任务消费者。Source worker 执行须另审，不能由 C++ 任务入口自动放行。

### 计时与 Scatter

所有 body 进入同一 RAII TLS 区间并暂停所在线程的 Recorder，包括 caller、嵌套及异常路径；worker 不写发起者 Recorder。发起阶段包住 fork→join 的墙钟，已经包含等待及 worker 工作，不再累加各 worker 耗时。parallel_for 不复制用户闭包，但后端调度可以分配；JoinGroup 的拥有存储也可分配，不能宣称任务零分配。

Scatter 先按权威 TriangleView 的原图元 p、子三角 j 顺序算候选数，保留每子三角的点前缀和每原图元的子三角前缀；零计数也保留槽位，不缓存三角几何、法线或种子。分配输出后按连续输出区间并行填充，因此一个大三角形也能分块；fill 从固定输入 span 重读几何，以 p/j/k 恢复原种子和来源，k 在每子三角重新计数。连续 n 角多边形贡献 n−2 个扇形三角，所以 p 的首角下标可由其三角前缀加 2p 恢复。纯三角仍为原六列，只有含多边形输入追加 sourcetri。不存在按规模切换到旧串行实现的双路径。每元素内 double/float 运算顺序不变，最小间距在全部候选原序上串行淘汰、各列同步压紧。成功 join 后才构成返回 Geometry，失败不发布部分候选；不在 worker 上调用 Runtime 或操作 Geometry 结构。

## 帧阶段计时

[frame_timing.hpp](../src/foundation/frame_timing.hpp) 提供常开的 CPU 帧阶段计时机制，dev 与 release 行为一致。foundation 只拥有根标记 `timing::frame`；各模块在自己的 `timing.hpp` 中以 `inline constinit timing::Marker` 声明阶段，给出键名、显示标题和父标记，例如 [renderer/timing.hpp](../src/renderer/timing.hpp) 的 `render` 以 `timing::frame` 为父项。父标记只引用本模块或其依赖模块的标记，依赖方向与链接一致。

宿主在启动时、任何线程记录之前调用一次 `timing::install({module_a::markers, module_b::markers})`：先登记 `frame`，再按参数顺序登记各模块标记，由此确定进程内唯一的目录、下标和显示行序。重复调用、空标记、同一标记重复登记、键名重复、父项未先登记或超过 64 项都会拒绝，被拒绝的调用不登记任何项。没有静态自动注册，不依赖静态初始化顺序。`catalog()` 按下标返回已登记标记，`depth()` 给出层级深度。

宿主线程每帧用 `Recorder::Frame` 绑定一个 `Recorder` 并打开 `frame`，`finish()` 或析构时关闭并解绑；`last()` 保留最近一个完成帧的 `FrameRecord`。`Scope` 只读取标记预先分配的下标和父下标，与当前打开的阶段比较后读两次 `steady_clock`，不分配、不查找、不加锁。计时为包含式：父阶段包含子阶段；同一帧多次进入同一阶段时累加时长并增加 `entries`。`record(marker, duration)` 把其他时钟测得的分段（例如 RHI 帧调用内部的提交与等待）记为当前阶段的子项，遵守同一父项规则。

Recorder 已绑定时，未登记的标记、未在声明父项下打开的标记以及嵌套的 `Frame` 计入 `structure_errors`，`first_error` 指向第一个出错的标记，这些条目不记录时长。`Suspend` 在作用域内解绑当前线程，被挂起的工作整体计入外层已打开的阶段，用于辅助表面等不属于本帧阶段结构的嵌套调用；在 `Suspend` 内结束的 `Frame` 不会在它退出时被重新绑定。

没有绑定 Recorder 的线程只读取一次线程局部变量，不做记录。物理工作线程等辅助线程的耗时体现在发起它们的宿主阶段的墙钟时间中。Recorder 不是线程安全的，只能由绑定它的线程写入。产品宿主绑定 Recorder：编辑器的每次 `Application::step`、Play 的每次运行循环各是一帧。进程入口调用 [application/timing.hpp](../src/application/timing.hpp) 的 `install_frame_timing()`，依次登记宿主阶段 `app.*`、World、物理和渲染阶段；`Application` 与 `run_project` 启动时检查目录，未安装时以 `std::logic_error` 拒绝启动，不会把每个阶段都记成结构错误。宿主把每一步的记录交给编辑器的帧性能统计，供 Performance 表、Control 和 Play 报告使用。计时值只用于诊断报告，不作为验收的通过条件；验收只检查阶段是否存在、层级、进入次数和 `structure_errors`。单个作用域的开销由 `atelier_editor_document_acceptance --profile-scene-cpu` 输出的 `timing_scope_ns` 在同一机器上标定。
## 验收

构建与运行方式遵循 [README 的必要性验收](../README.md#构建与端到端验收)，按改动选择以下消费者；这里描述覆盖范围，不记录某次交付的通过状态。

`foundation_acceptance` 保留资源所有权和失败清理的代表序列，检查在 dev/release 中均有效，不依赖 assert：

- [资源目录消费者](../acceptance/foundation/associative.cpp)：Map 部分复制失败后，已有目标和源资源仍存活，退出作用域后全部释放。
- [生命周期消费者](../acceptance/foundation/lifecycle.cpp)：资源子树提升时的移动赋值顺序，以及 SharedArray 多线程复制、写时分离和线程结束后的共享缓冲释放。原子内存序在 x64 上不能由这项验收区分。
- [拥有资源消费者](../acceptance/foundation/main.cpp)：Array 自身别名追加/填充、构造与复制失败后的清理，以及宏生成原生 Inventory、描述型 Array 字段、独立快照、Runtime 值复制与 GC 析构资源的链路。
- [原生任务消费者](../acceptance/foundation/tasks.cpp)：不可变输入与 COW 输出的非对称邻接计算、非零起点和尾块、嵌套 join；取消后已开始任务抛错的优先传播、用户闭包在提交线程释放及失败后再次调用。原地正向计算反例说明仅写区间互不重叠不足以安全并行，不执行有数据竞争的反例。只检查结果与生命周期，不以耗时或参与线程数判 PASS。
- [Array 缓冲寿命](../acceptance/foundation/array_lifetime.cpp)与 [String 缓冲寿命](../acceptance/foundation/string_lifetime.cpp)：分别执行一次 Array 拷贝/销毁和一次 String 目录清空，严格比较前后 mimalloc 活分配字节；不是元素 live 计数，也不是性能采样。两个可执行文件随同一个 `foundation_acceptance` 入口运行，不输出计时或性能报告。

[帧阶段计时消费者](../acceptance/foundation/frame_timing.cpp)检查目录组装、层级与记录序列，入口仍为 `foundation_acceptance`。

[源码借用消费者](../acceptance/source/native_values.cpp)通过 `source_acceptance` 保留共享布局、原地写入与别名、失败借用及拥有者析构；[模块消费者](../acceptance/source/modules.cpp)和[组合字段消费者](../acceptance/source/value_fields.cpp)保留 String/Array/记录的深层失败、部分构造与赋值清理。重复 owned-values 入口已移除；Source 目标仍包含在 `jit_acceptance` 中。

设计依据与 UE/Qt 的取舍见[研究记录](research/shared-foundation-types.md#qtue-接口取舍)。

历史采样及条件见[字符串研究](research/string-storage-performance.md)和 [Array 研究](research/array-storage-performance.md)；当前没有独立的 Array/String 性能测量入口。
