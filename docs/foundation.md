# 引擎 C++ 基础库

当前提供 `atelier::Array<T>`、`SharedArray<T>`、`Span<T>`、`String`、`StringView`、`Map<K,V>`、`Set<T>` 和 `Hash/Equal`。它们服务于连续数据、共享不可变数据、文本、唯一键索引与共享 Runtime 值存储。独立目标为 `atelier_foundation`，不依赖 Runtime、World、UI 或 JIT；反射适配位于 `runtime/foundation.hpp`。Map/Set 当前服务原生 C++ 消费者，尚未开放反射或源码容器定义。完整 STL 兼容与 Unicode 排版尚未提供。源码已能同步借用原生 String/Array 并访问同一缓冲，也能创建有作用域生命周期的拥有型局部，规则见[原生值借用](native-value-borrows.md)。

产品代码中的拥有型 STL 容器按 A1 决定逐模块迁移到自有容器；这不要求替换成熟原子、类型特征、构析原语和算法。容器、字符串等基础函数库提供语义清楚、经过代表验收的完整常用操作，可以为明确的功能类别预留，不必等首个业务消费者才补齐；不建立兼容转发层或重复实现。第三方与平台类型的转换留在各模块接入边界，性能结论仍以真实工作负载测量为准。选型依据见[工业基础库研究](research/foundation-stl-industrial-research.md)。

## 类型与所有权

| 类型 | 存储与复制 | 入口 |
| --- | --- | --- |
| `Array<T>` | 独占连续缓冲；复制拥有独立元素，平凡操作可批量完成；移动转移缓冲，源变为空；析构剩余元素并释放缓冲 | [array.hpp](../modules/core/foundation/include/foundation/array.hpp) |
| `InlineArray<T,N>` | 默认使用 N 个内联槽，超出后上堆；移动内联值逐元素，移动堆值接管缓冲 | [inline_array.hpp](../modules/core/foundation/include/foundation/inline_array.hpp) |
| `FixedArray<T,N>` | 固定容量、可变长度，只构造活前缀；容器自身永不分配堆存储 | [fixed_array.hpp](../modules/core/foundation/include/foundation/fixed_array.hpp) |
| `BitVector` | 独占压缩位集合；最低位对应最低下标，尾部未使用位清零，不提供 bool 引用代理 | [bit_vector.hpp](../modules/core/foundation/include/foundation/bit_vector.hpp) |
| `SparseArray<T>` | 独占稀疏槽；活下标稳定，扩容可搬移地址，删除后的下标可复用 | [sparse_array.hpp](../modules/core/foundation/include/foundation/sparse_array.hpp) |
| `SharedArray<T>` | 计数句柄，共享一个不可变 Array；复制只增加计数，写入前先取得独占；移动后源为空 | [shared_array.hpp](../modules/core/foundation/include/foundation/shared_array.hpp) |
| `Span<T>` | 指针与元素数量；复制只复制借用描述 | [span.hpp](../modules/core/foundation/include/foundation/span.hpp) |
| `String` | 有效 UTF-8，保证尾部零字符；拥有缓冲复制独立，literal 复制借用固定字节，移动后源为空 | [string.hpp](../modules/core/foundation/include/foundation/string.hpp) |
| `StringView` | 指针与字节数量；允许外部未验证 UTF-8 与局部字节切片，不保证尾部零字符 | 同上 |
| `Map<K,V>` | 独占键和值；复制独立拥有，移动转移存储；键只读、值可修改，无遍历顺序保证 | [map.hpp](../modules/core/foundation/include/foundation/map.hpp) |
| `Set<T>` | 独占唯一成员；复制独立拥有，移动转移存储；成员只读，无遍历顺序保证 | [set.hpp](../modules/core/foundation/include/foundation/set.hpp) |
| `Optional<T>` | 内联值或缺席；移动后源仍 engaged，保存合法的 moved-from T；reset 才变空 | [optional.hpp](../modules/core/foundation/include/foundation/optional.hpp) |

String 使用 32 字节对象、8 字节对齐及 30 字节短字符串内联存储（SSO）；长文本由独占堆缓冲或固定字面量持有。字节复制直接搬移连续缓冲，不构造随后会被覆盖的字符。Array 保持 data/size/capacity 的 24 字节、8 字节对齐布局。Array 与 InlineArray 的堆块统一经 `mi_malloc_aligned/mi_realloc_aligned/mi_free`，保持相同 `alignof(T)` 与零 offset；String 仍沿全局 new/delete 接入 mimalloc，无额外分配器注册表。包含数组模板的目标必须同源重编译，不能把旧分配入口的块交给新实现。[字符串研究与测量](research/string-storage-performance.md)记录旧表示的历史测量，不代表新布局性能。

原生容器可持有普通 C++ 资源；复制指针或句柄不复制其指向对象。Runtime 对象引用的存活仍由根、字段与写屏障管理。普通 `Array<ObjectId>` 不会自动成为 GC 根。

Array/String/Map/Set 自复制、自移动和自交换保持原值；移动后的源为空且可复用。重复 clear/release、重复删除缺失键不改变逻辑内容；reserve 不减少容量。Array/Map/Set 的移动赋值先接管来源，再释放旧内容，因此来源即使由目标内某个资源间接拥有，也不会在转移前被析构。元素构析、Hash/Equal 或谓词不得重入并结构修改正在执行操作的同一容器。

## Array 常用操作

```cpp
atelier::Array<atelier::String> names;
names.reserve(16);
names.emplace(atelier::String::literal<"sword">());
names.emplace(atelier::String::literal<"shield">());
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
- `reserve(n)` 只准备容量；`resize(n)` 对新增元素做值初始化，`resize(n, value)` 从 value 复制构造新增元素，value 可以是本数组的元素；两者都对移除元素做析构。`emplace` 直接构造新增元素。`release()` 清空并释放缓冲。删除不自动缩容。
- 扩容为几何增长；只构造实际元素，不构造预留槽位。元素要求不抛异常析构，并提供不抛异常的移动构造或复制构造。支持 move-only 元素，但此时复制 Array 的接口不可用。
- 扩容优先不抛异常移动，否则复制。复制、追加与扩容只在类型 trivially-copyable 且实际选中的构造也平凡时批量复制；有序删除另外要求平凡移动赋值，使用可重叠的 memmove。类型标签不会跳过选中模板构造或赋值的副作用。
- 复制赋值在容量足够、元素可平凡复制构造且平凡复制赋值时复用缓冲，包括空值、变短和重新增长；其他情况先构造完整副本再替换。可复制但不可赋值的元素仍然可用。String、智能指针和其他资源类型保留逐元素构析；不抛异常的非平凡赋值不自动取得字节搬运或容量复用资格。
- 预留容量中的单元素 emplace 直接构造并在成功后增加大小。符合 realloc 条件时，增长前先保存候选值或自视图区间偏移，增长后重新定位，保留自身元素、子对象或视图作为来源的合同；其余增长先构造新增后缀，再处理旧前缀。remove_if 仍逐元素交替调用谓词与搬移，谓词可以观察之前已经压缩的位置。
- 普通容器操作均为 `noexcept`，元素构造、复制、赋值和谓词不得以异常表达业务失败；CPU OOM 为 fatal。旧异常回滚路径已删除。`emplace/add` 仍允许移动自身元素，来源随后处于合法 moved-from 状态。
- `find/find_last/find_if/find_last_if` 返回索引或 `npos`；`contains/contains_if` 查询存在性；`index_of(pointer)` 将活元素地址转成索引，外部地址或尾后地址返回 `npos`。`add_unique` 返回既有或新增索引。`remove/remove_last/remove_swap/remove_last_swap` 删除首个或最后一个匹配并返回是否删除；`remove_all/remove_all_swap` 返回删除数，按值保存比较键以支持键来自自身元素。谓词版本为 `remove_first_if/remove_last_if/remove_swap_if/remove_all_swap_if`；既有 `remove_if` 保持删除全部匹配且保序的语义。swap 删除不保序。
- 三种数组自身的非法索引/删除范围、不可表示的内部容量及 CPU 分配失败在两种配置均走唯一 fatal 出口；借用视图的 `Span::slice` 越界同样为合同 fatal。Runtime 适配已采用 `noexcept` 结果回调，并在调用容器前检查业务范围与容量；不再捕获 C++ 异常转成 `Result`，异常越过回调会终止进程。可恢复的元素业务失败必须由显式结果回调及相应容器适配处理，按操作合同清理未发布候选、保留旧值或处理部分赋值；不能假设普通 C++ Array 会调用元素的结果回调。

### 内联数组与受限 realloc

谓词接口仅在调用期间借用谓词，不复制或保存它；支持 move-only 闭包，并按 `std::invoke` 调用（包括成员投影）。

三种数组共用元素构造/销毁/搬移、增长容量计算、自视图区间识别及先后缀后前缀的扩展实现。`InlineArray<T,N>` 与 `FixedArray<T,N>` 共用内联序列实现，提供上述序列操作及 `back/pop`。两者默认 size 为 0、capacity 为 N；N=0 合法。FixedArray 不是恰好 N 个活元素的 `std::array`，超过固定容量属于调用前置条件违例。业务输入应先检查预算，不把 fatal 当作输入拒绝。

- InlineArray 的 `clear` 保留堆容量；`release` 清空并返回内联；`squeeze` 在 size≤N 时搬回内联，否则收紧堆块。FixedArray 的 `clear/release` 均析构活前缀，`squeeze` 无操作，`reserve` 仅检查是否超过 N。
- 内联移动不保存来源对象的内联地址，源变空且可复用。移动赋值先分离来源，允许来源由目标元素间接拥有。移动、交换以及使元素搬移的结构变化终止相关借用；临时数组不能取得 view。与 Array 一样，普通元素要求不抛析构，提供不抛移动或可复制构造；移动和交换均为 noexcept，不提供异常恢复。
- FixedArray 保存 size 和按 T 对齐的原始内联区；InlineArray 另存堆指针/容量。两种新类型不承诺跨 T/N 的固定 ABI，不注册为 Array 的相同表示，暂不开放 Source/反射实例化。后续真实消费者是程序化 BVH 的构建栈和有界查询栈，产品迁移归 W4/A1。
- realloc 只用于平凡可复制且属于隐式生存期的元素。锁定工具链缺少 `is_implicit_lifetime_v`，具名 concept `ReallocatableArrayElement` 使用已批准的充分条件：平凡可复制，且 aggregate 或默认/复制/移动构造之一平凡；依据为 [c.malloc] 的隐式对象创建。工具链提供标准 trait 后直接替换此条件，不保留回退矩阵。String、智能指针等资源元素仍逐元素构析，没有业务自定义的重定位 trait。
- 实际 realloc 还要求所选搬移构造平凡，复用 `byte_relocate` 的判定；仅有平凡默认构造不能跳过非平凡搬移的副作用。只有配对分配入口的堆块进入 realloc；内联区从不传给它。零容量显式 free，不以 realloc(p,0) 判断 OOM。只保证正确性和减少可避免的逐元素搬移，尚无本轮性能提升测量。

## 位集合与稀疏槽

`BitVector` 是独立类型，不特化 `Array<bool>`，不改变 SharedArray<bool> 或程序化 group 列的表示。Windows x64 上为 32B/align8（Array<uint64_t> 加位数）；每个 word 的低位对应较低下标，尾 word 未使用位始终为零，缩小再扩大不复活旧位。

- `size/empty/reserve/resize(bits,value=false)/clear/release/swap` 管理存储；resize 的 value 只填新增区间，clear 保留容量，release 释放。复制独立拥有，移动后源为空。字数用除法加余数计算，不先做 bits+63。
- `test/operator[]` 返回 bool，不借出可写引用；`contains(index)` 查询集合成员，未置位或不存在的下标返回 false，由位图入口一次判界，供 SparseArray 占用查询使用；test 的越界 fatal 合同不变。`set(index,value=true)/reset/flip/set_range(first,count,value=true)` 显式修改。`count/any/none` 查询集合，`find_first/find_next` 返回下标或 npos；find_next 严格从给定下标之后查找，npos 或超出末尾时返回 npos。`set_bits()` 是升序、零分配 forward range，禁止从临时对象借出。
- `&=/|=/^=` 要求位数相等，按 word 计算；test/operator[] 与修改操作的索引/范围越界，以及不等位数运算为 contract-fatal。CPU OOM 和内部容量错误沿 Array 唯一出口。没有原子位、平台分派或第二套分配器。修改内容或结构后重新取得迭代器。

`SparseArray<T>` 的 `size` 是活值数，`extent` 是槽位上界，`capacity` 是槽存储容量。`emplace` 返回下标，先 LIFO 复用空槽，空闲链为空才延伸 extent；`erase` 对空洞或不存在下标返回 false。`contains_index/try_at` 查询正常缺项，`operator[]` 对缺项 contract-fatal。查询指针/引用和 `entries()` 禁止从临时对象借出；entries 按下标升序返回 `{index,value引用}`，const 版本只读。结构修改后重新取得迭代器。

- Windows x64 对象为 64B/align8：槽指针、capacity、BitVector、free_head、活值数；extent 直接取占用位图长度，不重复保存。Slot 是 T 与 size_t next 的 union，对齐取两者较大值。空洞没有活 T，不要求 T 默认构造。
- `reserve` 保留下标和空闲链，只构造/搬移活 T；槽分配复用 ArrayElements 的对齐分配/释放与增长算术，不把带空洞的 Slot 整块交给 realloc。增长时先保存 emplace 候选，支持参数借用既有值。普通构析要求与 Array 相同：析构不抛，移动不抛或可复制；操作 noexcept，业务失败不能由元素抛异常表达。
- 复制从 const 元素来源构造，保留下标、空洞和下次复用顺序，不选用可变来源的复制重载；元素地址统一用 std::addressof，支持重载或删除 operator& 的 T。移动接管全部存储，源为空。移动赋值先分离来源再释放旧资源，支持资源子树提升。`clear` 析构全部活值并令 size/extent 为0，保留容量；`release` 另释放槽和位图。无 compact/重排/连续 T 视图。
- 稳定下标不是稳定地址或永久身份：增长可使全部元素引用失效，删除的数字可被重新分配。不提供代际、GC、弱引用、并发或持久化格式。两类型均未新增 Runtime/Source/反射/JIT 注册，不改变已有容器 ABI。

本片真实基础消费者为 SparseArray 自身的占用位图和 foundation 的资源槽消费序列。首批产品迁移候选是 `procedural/volume.cpp` 的 seen_corner/seen_point（BitVector）与 `runtime.cpp` 的 RootState.entries（SparseArray）；这些替换归 W4/A1，尚未实施。RootState 迁移须保留 value_entries、移除回调、trace 队列与身份上限；Runtime 对象槽/UI 带代际槽不能直接按此替换。

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

- 句柄 8 字节，指向堆上的一个块：原子计数加一个 Array。从 `Array<T>&&` 构造只分配这个块并接管缓冲，分配失败为 fatal。复制句柄只增加计数；最后一个句柄释放时析构元素、释放缓冲和块。默认构造与移动后的源是没有块的空句柄，不分配，`array()` 返回共用的空 Array；读取接口不区分空句柄与块中 Array 为空，`same()` 区分二者。
- 只读访问为 `array()`、`view()`、`size()`、`empty()`；临时句柄不能取 `array()/view()/write()`。`write()` 在本句柄是块的唯一持有者时原地返回块中的 Array，否则先把内容复制进新块再返回；空句柄的 `write()` 新建一个块。普通操作均 noexcept。`same(a, b)` 判断两者是否共享同一个块，两个空句柄也算相同。没有弱引用、计数查询或可作版本键的地址。
- 块的地址在任何句柄存活期间不变，移动句柄不移动块，因此可以把 `array()` 只读借出、把 `write()` 的结果可写借出，包括经 Runtime 借给源码。`array()/view()` 的结果只在该句柄没有被赋值、移动、`write()` 或析构期间有效；`write()` 返回的引用还要求该句柄没有再被复制，借出期间复制句柄，副本会看到之后的写入，跨线程即数据竞争。借给源码时，调用方在调用期间持有出借的句柄，不对它做上述操作。源码不能给借来的 Array 扩容，借出期间元素缓冲不会重新分配。
- 计数递增为 relaxed，递减为 release，最后的持有者析构前取 acquire 栅栏；`write()` 以 acquire 判断唯一性。指向同一块的不同句柄可以在任意线程同时复制、析构和读取；同一个句柄的只读操作（读取、从它复制）可以并发，修改它（赋值、移动、`write()`、析构）时不能有其他线程同时使用它。线程独占一个句柄时，在任何线程调用 `write()` 都安全。元素在释放最后一个句柄的线程上析构。源码只在 Runtime 线程执行：工作线程可以持有、读取和 `write()`，不能进入源码。
- 元素须是不带 const/volatile 的可复制类型，并且是平凡可复制类型或 String；声明了 `is_runtime_reference` 的 Runtime 引用值（ObjectReference、ReferenceSlot）被拒绝，ObjectRoot 等非平凡根句柄不满足前一条。嵌在平凡结构体中的引用值无法在编译期识别；与 `Array<ObjectId>` 一样，SharedArray 中的身份值不是 GC 根，指针、Span 与 StringView 元素也不延长所指对象的寿命。
- SharedArray 不注册为 Runtime 类型，不能作 Runtime 对象字段，也不是源码类型。

## Map、Set 与哈希

```cpp
atelier::Map<atelier::String, unsigned> resources;
resources.reserve(128);                    // 可容纳的总条目数
auto [value, inserted] = resources.try_emplace(atelier::String::literal<"材质/stone">(), 7u);
if (const auto* found = resources.find(atelier::StringView{"材质/stone"})) {
    // found 借用表中的值；本次查询不创建拥有型 String。
}
resources.insert_or_assign(atelier::String::literal<"材质/stone">(), 9u);
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
- `reserve(n)` 按总条目数准备存储，不构造元素，声明 noexcept。删除和 clear 不自动缩容，release 释放存储。超过底层 max_size 的内部容量为合同 fatal，不静默截断；外部输入的容量与预算拒绝仍由调用方显式返回错误。默认空表不动态分配索引；沿既有全局 new/delete 使用 mimalloc，CPU OOM 为 fatal。
- 不提供并发保护；同一容器不能在读/遍历时被另一线程修改。复制指针或身份值不复制其目标，也不自动登记 Runtime 根。

### Hash/Equal 与失败合同

[Hash/Equal](../modules/core/foundation/include/foundation/hash.hpp) 是共同入口。整数、枚举和指针使用锁定后端的整数哈希，即一次 64 位乘法混合，不读取这些类型的 `std::hash` 特化；MSVC STL 的 `std::hash` 对整数逐字节做 FNV-1a，因此不经过它。其他类型的哈希复用现有 `std::hash<T>`；相等仍使用 `operator==`（String/StringView 除外）。结果已充分混合的哈希声明 `is_avalanching`，表直接使用结果而不再混合一次；`std::hash<T>` 声明了该标记时，`Hash<T>` 随之声明。TypeId 与 ObjectId 的 `std::hash` 由 `hash_fields` 逐字段混合 Runtime 上下文、槽位以及 ObjectId 的代际，与各自的完整相等规则一致；`std::unordered_*` 与 Map/Set 因此得到同一哈希。自定义策略作为 Map/Set 的模板参数提供，当前由容器默认构造；策略本身的默认/复制/移动/交换/析构及 Hash/Equal 调用必须不抛，检查包含结果到 size_t/bool 的转换。移动容器时源获得新默认策略，不要求其已移空的旧策略仍可调用。元素必须具备不抛移动构造、移动赋值和析构，编译期拒绝不满足的类型。标准类型的相等运算若未声明 noexcept，也需提供满足该合同的比较策略。

非透明策略只接受实际键类型，必要的类型转换在调用之前显式完成，例如 `Set<unsigned>::contains(1u)`。异构查询要求 Hash/Equal 同时声明 `is_transparent`，并且实际 Q→哈希结果、Q/K→相等结果的完整调用均不抛；不以策略中存在某个重载来推断后端会跳过键构造。这样查询不会在 noexcept 包装内部隐式执行可抛键转换。

Equal 必须是键域上的等价关系，相等键必须同哈希；键留在表内期间，这些结果必须稳定，包括策略访问的间接状态。普通浮点相等对 NaN 不满足该要求，使用浮点键时须排除 NaN 或提供成对一致的 Hash/Equal。编译期 noexcept 检查不证明这些语义规律。

String/StringView 共用同一按长度的字节哈希和比较，包含嵌入零，不做大小写折叠或 Unicode 规范化；零结尾字符指针作为便利用法，在首个零处结束，空指针按空串处理。透明查询不创建拥有存储，也不验证 UTF-8；插入 String 键时才建立其 UTF-8 不变量。持久文本键使用 String，StringView 作为键则仍由调用方保证外部字节存续且内容不变。自定义透明策略必须使查询表示与拥有键的相等和哈希一致；结构体不能默认哈希含 padding 的原始表示。

Map/Set 的元素构造和复制入口尚未整体迁移到禁异常合同，现有 C++ 元素异常仍沿这些入口传播：插入失败不发布新成员；复制赋值或替换值构造失败保留目标的原值，部分构造的资源会清理。这不包含 CPU OOM 或 reserve 内部容量违例，两者为 fatal，不承诺回滚。调用者主动移动表内资源或构造函数修改实参属于用户副作用，不由容器回滚。Hash/Equal 不保存输入借用、不修改表、不重入结构操作；不能通过错误的 noexcept 声明掩盖会抛异常的实现。

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
auto label = atelier::String::literal<"你好">();
label.append(atelier::String::literal<" / loot">());
atelier::StringView input = label.view();
auto suffix = input.slice(6);       // 字节偏移；“你好”为 6 个 UTF-8 字节
bool found = input.contains("loot");
```

String 的 `size/capacity` 不含末尾零字符，所有索引、查找和切片位置均为字节。带长度的 StringView 保留嵌入零字节；零结尾构造只扫描首个零。`c_str()` 可用于需要零结尾的接口，但对方可能在嵌入零处停止。

`String::from_utf8(StringView)` 返回 `std::expected<String, StringError>`；`assign(StringView)` 与 `append(StringView)` 返回 `std::expected<void, StringError>`。三个入口均 noexcept，拒绝截断序列、非法续字节、过长编码、代理区和超出 U+10FFFF 的编码，返回唯一编码错误 `StringError::invalid_utf8`，失败不更改原字符串。旧 StringView 构造函数已删除。支持从自身视图追加或赋值。String 不提供可写字符地址。复制有效 String、`assign(const String&)` 或 `append(const String&)` 直接复制已验证字节；任意 StringView，包括来自有效 String 的字节切片，仍需校验。ASCII 校验按完整 8 字节块跳过，残余及非 ASCII 序列继续按 UTF-8 规则处理，不越过视图边界。

容量不包含末尾零字符。空 String 的内联容量为 30；不超过此字节数的拥有值创建与复制不分配堆缓冲。`reserve` 只准备容量，堆增长采用需求值与约 1.5 倍旧容量的较大值；复制长文本不继承源对象预留的大容量。拥有缓冲容量足够时赋值复用缓冲，变短不会自动缩容。堆值的 `clear` 保留容量，`squeeze` 将堆容量收紧到长度，短值可退回内联存储；`release` 释放堆缓冲并回到空内联表示。业务长度上限在上层预检并返回 Result；容器内部不可表示容量及 CPU OOM 走唯一 fatal 出口，不作为 StringError。

`String::literal<"text">()` 编译期验证完整 UTF-8，借用具有静态存储期的模板参数字节，任意长度构造和复制均不分配。字面量所在原生模块须活到全部副本结束，不能借可卸载模块或 JIT 常量池；Source 文本仍复制。写入先分离，不能写/释放静态区。literal 的 capacity 等于字节长度，不保证下一次写入零分配；reserve 不超过该长度时不分离，clear/release 回空内联，squeeze 不改变 literal。literal 拷贝可共享字节，其他 String 拷贝独占缓冲。

String 的末字节 tag 为 0–30（内联长度）、0xFE（堆）、0xFF（literal）；长描述符通过字节复制读写，不读取未激活 union 成员。虽然大小/对齐未变，表示 ABI 已改变；必须与 Runtime NativeLayout 结果化联合重编译、重建反射/JIT 并重启进程，不能混入旧对象和旧代码。

StringView 的 `slice/find/starts_with/ends_with/contains` 都按字节工作，切片可能截断码点；`is_valid_utf8()` 可明确检查。转成 String 时重新校验。`std_view()` 是对标准库消费者的显式借用适配，不创造另一份拥有存储。Unicode 大小写折叠、规范化、字符/字素索引和本地化文本未实现。

- `String::replace(first,count,replacement)` 返回 `expected<void,StringError>`；范围越界为 contract-fatal，切断码点或 replacement 非法返回 invalid_utf8，失败保旧。`replace_all(search,replacement)` 返回替换数，按从左到右非重叠匹配；空 search 不插入、返回 0。两个参数即使未命中也须有效 UTF-8。候选成功才交换，支持参数引用自身内容；实际替换后重新取得视图。
- `String::to_ascii_lower/to_ascii_upper` 原地只改 A–Z/a–z，保留所有非 ASCII 字节和嵌入 NUL；literal 只有实际改变时才分离。StringView 的 `equals_ascii_case_insensitive/starts_with_ascii_case_insensitive/ends_with_ascii_case_insensitive` 使用同一 ASCII 规则。`starts_with/ends_with` 另接受单字节，`starts_with_any/ends_with_any` 接受 `initializer_list<StringView>`，空集合返回 false，包含空串时匹配成功。
- `StringView::parse_number<T>` 返回 `expected<T,std::errc>`，必须消费完整输入，不自动 trim，不接受开头加号或整数基数前缀。整数接受 charconv 支持的非 bool 整数类型及显式 base（默认 10，范围 2–36）；浮点使用 general 格式，包括 charconv 的 inf/nan。非法格式、尾部残余和非法 base 返回 invalid_argument，数值溢出返回 result_out_of_range。
- `String::from_number(integer,base)` 返回 `expected<String,std::errc>`，唯一可恢复错误是非法 base；`from_number(float/double/long double)` 直接返回 String，使用最短 general 表示，无 locale、精度或格式选项。固定栈缓冲覆盖类型最大表示，输出已经是有效 ASCII，不重复 UTF-8 校验。普通有限值往返保值，不承诺 NaN payload 保留。CPU OOM 仍为 fatal。

这些文本函数补齐 C++ 常用函数库，供后续 A1 文本消费者迁移，不新增 Runtime 注册、Source 语法或 JIT ABI；String 的布局不变。UTF-8↔UTF-16 和 `to_wide` 归统一 IO 的 platform 接口，不在此处另建实现。

`trim/trim_start/trim_end` 默认剪除六种 ASCII 空白（空格、tab、CR、LF、VT、FF），也接受显式字节集合，不按 Unicode 空白分类。`split(delimiter)` 返回零分配惰性输入范围，按完整字节串分隔并保留首尾及相邻分隔符之间的空片段；空输入仍返回一个空片段，空 delimiter 返回整个输入一次。`partition/rpartition` 返回 `{before, separator, after}`；未命中或 delimiter 为空时，前向返回 `{whole, empty, empty}`，反向返回 `{empty, empty, whole}`。所有片段和 split 的 delimiter 都借用原字节，迭代期间不能改变或销毁来源。

`code_points()` 先验证整个视图，返回 `expected<CodePointRange, StringError>`，不以替换字符吞掉坏输入；成功范围逐项给出 `{char32_t value, byte_offset, byte_count}`，偏移与长度均为 UTF-8 字节，不是 UTF-16 单元或字素。空文本产生空范围，嵌入 NUL 是一个正常码点。校验和迭代分两遍且不分配；取得范围后必须保持来源内容不变。新增范围不注册为 Runtime 值，也不新增 Source 字符串遍历语法。

Span 的 `slice(first,count)` 按元素切片；`Span<T>` 可转 `Span<const T>`，反向不允许。`const Span<T>` 只冻结描述本身，不冻结元素。只读元素必须写 `Span<const T>`。

Span 构造、索引和切片均 constexpr/noexcept。空指针配非零数量、超过可表示的元素范围、越界索引或切片在 dev/release 都为合同 fatal；外部输入先由所属业务边界验证。合法的空 Span 与尾后空切片可用。切片复用来源的合法存储不变量，不重复验证构造条件。

**借用有效期由拥有者和修改行为决定。** 销毁、释放、替换存储及实际扩容使原地址失效；删除和压缩可能让相同地址对应不同元素；clear 结束全部元素生命周期。Array 和堆 String 移动转移缓冲后，原地址若仍使用便依赖新拥有者；内联 String 移动复制字节到目标对象，旧视图不能跟随该值。调用者统一按移动后重新获取 String 视图处理。视图不冻结容器，不提供并发保护，也不能无条件跨协程挂起保存。直接对临时 Array/String 取 `view()` 及从临时 String 构造 StringView 被禁止；C++ 调用者仍须保证其他借用路径有效。

## Windows 文本、路径与文件摘要

[platform/utf.hpp](../modules/core/platform/include/platform/utf.hpp) 统一负责严格 UTF-8↔UTF-16 转换，不提供有损模式。`utf8_to_utf16` 返回 `expected<std::wstring, UtfError>`，`utf16_to_utf8` 返回 `expected<std::string, UtfError>`；空输入成功，嵌入 NUL 按长度保留，字符串另有尾 NUL。非法编码、Win32 长度不可表示、输出预算超限和转换 API 失败显式返回错误。UTF-16 方向的 `maximum_units` 不计尾 NUL，在分配前检查；UI 仍先查 16384 UTF-8 字节，再查 4096 UTF-16 单元。当前 std 返回类型直接衔接 Win32、filesystem 和现有文本存储，后续 A1 统一迁移自有类型。

`utf8_to_utf16_validated` 仅接受所属边界已验证且长度可表示的文本，违约为 contract-fatal；String、已解析 JSON、Runtime 注册文本和字面量可复用其保证，任意字节切片不能沿用。OS 路径、用户 UTF-16 输入及原始文件/管道/二进制文本走严格 Result。旧 Assets/Control `wide/utf8` 入口已删除；已有失败的诊断显示回退由 Application 的 `diagnostic_text` 集中负责，不替代业务 Result 或严格输入验证。

[platform/windows_path.hpp](../modules/core/platform/include/platform/windows_path.hpp) 的唯一 `windows_file_path` 返回 `expected<filesystem::path, error_code>`：绝对化失败显式返回，成功后规范化并保留已有扩展前缀、转换 UNC 或添加长路径前缀。它只处理拼写，不承担存在性、包含关系和链接验证。[platform/error.hpp](../modules/core/platform/include/platform/error.hpp) 的 `error_message` 对系统错误使用 FormatMessageW 和严格 UTF-8 输出，避免 filesystem 异常 `what()` 中的 ANSI 路径；非系统类别保留原类别消息，未知系统码保留数字身份。

[foundation/sha256.hpp](../modules/core/foundation/include/foundation/sha256.hpp) 的 `Sha256` 构造、`add`、`finish` 和 `sha256(bytes)` 均 noexcept；内存哈希没有可恢复的输入失败，CNG 原语失败直接 fatal。流式对象在最后一次 `add` 后只调用一次 `finish`。仅 `sha256_file` 返回 `expected<std::string, std::string>`，报告路径、打开和读取失败；上层自行映射业务码。内存哈希复用 CNG SHA-256 伪句柄，不逐次打开算法 provider。

## 同步借用与作用域工具

- [FunctionRef](../modules/core/foundation/include/foundation/function_ref.hpp) 的 `FunctionRef<R(Args...)>` 和 `FunctionRef<R(Args...) noexcept>` 是两字借用，不复制闭包、不分配、不提供空态；空函数指针为 contract-fatal。支持左值和右值 callable、普通函数指针、move-only 捕获及 `std::invoke` 语义。noexcept 特化仅接受调用及结果转换不抛的 callable。只作同步参数使用，不能存为成员或跨调用保存；临时闭包只保证活到整个调用表达式结束。`Window::poll` 已采用此接口，现有四处临时 lambda 无需命名或复制，窗口不保存回调。
- [ScopeExit](../modules/core/foundation/include/foundation/scope_exit.hpp) 拥有一个无参、不抛的清理动作，禁止复制；移动转移清理义务，`release()` 放弃自动执行。正常 return（包括 Result 失败）析构执行一次，fatal 不保证清理。构造、移动和析构的 callable 操作都须不抛。此为 C 类预留：首个计划消费者是 W4 CONTROL 的结果化 Security；当前无提前返回的 Security 不作无效迁移。
- [flags](../modules/core/foundation/include/foundation/flags.hpp) 以 `enable_enum_flags<E> = true` 显式启用枚举，提供 `flags_or/flags_and/flags_xor/flags_without/has_any/has_all`，均 constexpr/noexcept，不添加全局运算符，不屏蔽未知位。零 mask 的 any 为 false、all 为 true。OperatorFlag 与 StyleState 已显式启用并直接消费共用函数；操作标记使用 any，样式选择器使用 all。旧 has_flag、StyleState operator| 和手写集合运算已删除；非法外部样式状态仍由解析/创建边界返回错误，flags 不替业务校验。

## Optional 的原生合同

Optional 不承载错误；空值不构造 T，也不要求 T 默认可构造。payload 从偏移零开始，占 sizeof(T)，随后是 1 字节 engaged（0/1），总大小按 alignof(T) 向上补齐；不使用 niche 或 padding 压缩。复制、移动、赋值、emplace 和 swap 按 T 对应的 nothrow 能力提供。emplace 先销毁旧值，参数不能借用该旧值；reset/emplace/赋值使 payload 借用失效。

`has_value/bool` 查询状态；`try_get() &/const&` 缺席时返回 nullptr。`operator*/operator->` 空访问为合同 fatal，所有这些借用接口禁止临时对象。移动保留源 engaged；Optional<String> 的源含一个空 String，而不是空 Optional。不提供抛 bad_optional_access 的 value()。Runtime/Source 的接线由 Runtime 负责：通过回调访问，不读取 engaged 私有偏移，也不能自动把 Optional 中的引用作为 GC 根。

原生操作集为默认/`nullopt`/值/`in_place` 构造、复制/移动构造和赋值、`nullopt` 赋值、`reset/emplace/swap`，以及上述查询和借用接口。普通操作均 noexcept；`emplace` 返回新值引用，不能保存临时 Optional 的该引用到完整表达式之后。该类型表示缺席而不是可恢复失败；业务失败使用 Result。原生寿命验收覆盖 move-only 资源、两种对齐布局、移动保留源 engaged、空/有值交换、复制保活及 reset 释放。本轮收口不改其布局或 API，也不声称已完成 Runtime/Source/反射/JIT 注册。

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

[tasks.hpp](../modules/core/foundation/include/foundation/tasks.hpp) 提供 `atelier::tasks::parallel_for` 和单次使用的 `JoinGroup`，用于普通 C++ 值上的同步计算；当前产品消费者是 [Scatter](../modules/engine/procedural/src/operations.cpp) 和 [VolumeScatter](../modules/engine/procedural/src/volume.cpp)。公共头不暴露 oneTBB 类型，后端版本与源码校验值只在[依赖锁文件](../cmake/dependencies-lock.json)。构建仅部署对应配置的一份 TBB 调度 DLL 与许可证，不构建或分发 tbbmalloc；通用分配器仍按项目既有 mimalloc 入口接管。

任务体与 join 已使用下列 noexcept expected 接口，不保留 void body 或异常传输兼容入口。分块、线程、借用、取消、浮点环境与计时合同保持不变。全局错误分类和 fatal 只由[工程指南](engineering.md#错误处理)定义。

```cpp
auto candidate = snapshot;
const auto input = snapshot.view();
const auto output = candidate.write().view(); // 发起前完成 COW、容量及元素构造
const auto result = atelier::tasks::parallel_for(
    {0, input.size()},
    [input, output](atelier::tasks::Range range) noexcept
        -> std::expected<void, atelier::tasks::Failure> {
        for (auto i = range.begin; i < range.end; ++i)
            output[i] = input[i] * 2;
        return {};
    },
    1024); // grain 提示，不是每块的最大长度
// 仅 result && *result == Completion::completed 时发布 candidate。
```

### 任务失败目标合同

`tasks::Failure` 是平凡复制、平凡析构的两字段普通值，不分配、不拥有指针、字符串、异常对象或 Runtime 资源：

```cpp
struct Failure {
    std::uint32_t code; // 非零；含义由本次操作约定，host 翻译为业务错误
    std::size_t item{SIZE_MAX}; // 可选逻辑工作项；SIZE_MAX 表示未提供
};
```

成功只由 expected 的值分支表示，code=0 不是另一种成功。item 不表示 worker 编号或完成顺序；需要完整 size_t 域的消费者可以省略位置，在自身固定失败槽中保留细节。嵌套任务转发须保持同一操作的码语义，或由 body 显式换码，不建立跨模块通用错误枚举或任意 payload 通道。

- parallel_for body 精确返回 `std::expected<void, Failure>`，以 `const Body&(Range)` 调用；组 body 以 `const Body&()` 调用。两者必须 `noexcept`，模板同时检查精确返回类型与 `is_nothrow_invocable`，擦除回调也保留 noexcept。不失败的 body 显式 `return {}`，不自动适配 void。
- `parallel_for` 与 `JoinGroup::join` 返回 `std::expected<Completion, Failure>`，并声明 noexcept，表示不以异常作为接口；它不证明禁 EH 编译时能截获外部异常。`JoinGroup::run` 保留 void，只接纳闭包，异步业务失败由 join 观察；不创造已经没有可恢复来源的“提交分配失败”Result。构造、析构与闭包复制/移动/析构不抛异常。
- 倒置范围、grain=0、跨线程操作组、终态后 run 或重复 join 是调用前置条件违例，dev/release 均 contract-fatal，不占用 Failure 的业务码。终态后的 cancel 仍无作用；取消后的 run 可以接纳闭包并在 join 后释放，但不保证执行 body。

### 范围、完成与拥有者

- `Range{begin,end}` 是 size_t 半开区间。completed 时每个索引恰好处理一次；空范围不调用 body，按 stop 状态返回 completed/cancelled。块长、顺序和执行线程不是业务结果；grain 交给自适应分块器，不引入持久 affinity 状态。
- caller 进入共享 arena 参与执行；arena 满时允许委派，不能以 caller 每次实际执行至少一块作为正确性前提。嵌套仍用同一池并继承父取消；已处于任务 body 的嵌套调用不再次 execute 切换默认 context。禁止用普通 future、兄弟任务屏障或业务锁等待池内同伴；依赖只通过结构化子任务 join 表达。
- `parallel_for` 借用 const-invocable 的 body，只向后端复制借用描述。`JoinGroup` 不可复制/移动，只有建立它的线程调用 `run/cancel/join`，body 可建立自己的子组。组拥有普通值闭包，存储与释放在提交线程；所有 body 执行结束后才释放。一次 join 后终态，不提供 detach、跨帧复用或后台任务。
- 每次 parallel_for 在进入 arena 前捕获 caller 的浮点设置；组在构造时捕获，供该组全部任务使用。P=1、单块及委派路径仍由后端应用、恢复相同设置。body 不修改浮点环境，不以无序浮点归约改变元素内原计算次序。
- foundation 内部唯一进程拥有者在首用时完成初始化，多个 Runtime/World 共用；不在 Runtime 中各建池。默认并行度 P=min(8,max(1,H/2))，H 为 hardware_concurrency 提示，P 包含 caller。它限制本 arena 并发与 TBB worker 预算，不限制外部应用线程，也不接管 Jolt、RHI 或解码线程。
- 进程退出前须停止发起任务，全部调用线程和组均已终态。禁止静态寿命 JoinGroup，禁止静态析构期再提交。拥有者在任务外先 terminate 本方 arena，再 finalize 调度器；arena 析构不是 join。finalize 返回失败会报告并终止进程，不伪装成已清空 worker。以后增加其他 TBB arena/handle 时须共同履行关停合同。

### 取消、失败与借用边界

- stop_token 与 `JoinGroup::cancel()` 是合作取消，不强杀已经执行的 body；长 body 自行选择合理分块。业务失败只发布一个代表 Failure，再请求取消；所有已接纳工作终止执行后才返回，优先级为 Failure、cancelled、completed。不保证最小索引、最早时间或发现所有未执行任务的错误，不收集全量错误；部分输出不自动回滚。不同顶层调用的取消隔离。
- 嵌套调用返回 Failure 后，父 body 必须显式传播或转换才能成为父失败；忽略 Result 不能依赖 TBB 替代传播。已请求取消时，已经执行的 body 仍可能报告失败，该失败不能被取消状态覆盖。
- parallel_for 和组任务的 stop 回调只在已经绑定 context 的后端任务执行栈上存活，避免首次绑定覆盖提前设置的取消位；进入用户 body 前再次检查取消。所有正常返回路径先注销并等待回调结束，之后任务才完成，因此 group wait 重置 context 前没有残留回调。组的显式取消另保留单调状态，首次绑定与 wait 重置都不能将其清除。
- 正常路径显式 join 消费错误；放弃组时析构取消并 drain，只在清理中丢弃任务 Failure。借用拥有者先于组建立，直到 join/析构清理后才能销毁。可失败的闭包准备先在外部取得并检查 Result，再提交闭包；`run([v=make()] ...)` 中 make 的执行发生在 run 之前，不受 run 管理。完整 join 保证只适用于正常返回的成功、失败与取消，不适用于 fatal；后端不能完成 drain 时不释放仍可能被读取的闭包再继续运行。
- 输入快照、COW 独占、增列/resize 与容量准备在发起前完成。worker 只借固定存储，不结构修改共享容器；输出不仅彼此不重叠，也不得覆盖其他分块的只读输入。固定拥有者覆盖整个 fork→join，借用在 join 返回时结束，不能逃逸后续任务。借出可写列期间不得复制其 SharedArray/Geometry 句柄、移动或重分配存储。
- Runtime 所属线程可同步发起普通值任务，但 body（包括 caller 执行、P=1、嵌套）不能调用 Runtime、其 Source/VM/JIT、GC、根操作或 World 命令，不能捕获 Runtime 对象内部地址；等待不泵 UI/World/Runtime 回调。普通值自定义构析、闭包和跨 join 的错误载荷都不得携带 ObjectRoot/ValueRoot/Program/Coroutine 等 Runtime 依赖拥有者。不能以任务最终在 caller 上析构为理由放宽此限制。
- `tasks::in_parallel_body() noexcept` 在 dev/release 均可用，标记可嵌套的 body 执行，直到局部对象析构结束才恢复外层状态。它不包住调用前的闭包构造、join 后的闭包销毁或所有错误载荷转运，不证明 C++ 捕获安全。Runtime/Source 已接入开发版动态禁入诊断，具体范围见[任务体禁入范围](runtime-model.md#并行任务体禁入诊断)；纯查询不铺满检查，World 尚未覆盖，release 不做检查，也不是安全沙箱。
- 本入口不新增 Program 后台许可：Program 操作仍按现有 Runtime 线程与关闭/发布合同执行。无 Runtime 依赖的 MaterialProgram 既有并发求值合同不因本片而收紧，本片也不新增其任务消费者。Source worker 执行须另审，不能由 C++ 任务入口自动放行。

### oneTBB 二进制边界

只在包含 TBB 头的自有使用方统一定义 `TBB_USE_EXCEPTIONS=0`；runtime 目标保持上游异常设置和原样源码，不打禁异常补丁、不新增自有 EH 隔离岛。TBB 仍是每配置一个调度 DLL；不将它的源码或内联对象混入使用方的链接优化域。使用方不交换 TBB 异常对象、STL 拥有权或任意回调结果；Failure 由本方调度适配记录，不作为 TBB 异常传递。

上游 Feature Macros 明确允许 headers 禁异常而 runtime 仍抛异常。对当前锁定版本的使用面，task_group/task_arena/parallel_for 和 `try_call_proxy` 没有该宏分叉；global_control 只条件定义单参 throwing finalize，既用的双参 nothrow finalize 与句柄布局不变。因此未发现此配置导致的已用内联定义冲突。这不是整套 TBB 的 ODR 保证；新增直接 include（特别是并发容器、memory_pool、scalable_allocator）或依赖升级时重新核对，不在同一自有链接模块混用不同宏值。

唯一 Process 拥有 `global_control{terminate_on_exception, 1}`，先于其他需要调度器的成员建立，保持到 arena 终止和 scheduler finalize 之后。该公开开关使上游覆盖的 throw_exception/handle_perror 与调度 catch 路径在 DLL 内终止，而非把故障转换为本方 Failure；它不覆盖控制对象自身建立之前的失败。共享 CRT 域的 SIGABRT 接线和诊断覆盖例外以[统一 fatal 出口](engineering.md#唯一-fatal-出口)为准，不要求逐线程安装，也不能单凭 `noexcept` 推导终止保证。

### 计时与 Scatter

所有 body 进入同一 RAII TLS 区间并暂停所在线程的 Recorder，包括 caller、嵌套及返回 Failure 的路径；worker 不写发起者 Recorder。发起阶段包住 fork→join 的墙钟，已经包含等待及 worker 工作，不再累加各 worker 耗时。parallel_for 不复制用户闭包，但后端调度可以分配；JoinGroup 的拥有存储也可分配，不能宣称任务零分配。

Scatter 先按权威 TriangleView 的原图元 p、子三角 j 顺序算候选数，保留每子三角的点前缀和每原图元的子三角前缀；零计数也保留槽位，不缓存三角几何、法线或种子。分配输出后按连续输出区间并行填充，因此一个大三角形也能分块；fill 从固定输入 span 重读几何，以 p/j/k 恢复原种子和来源，k 在每子三角重新计数。连续 n 角多边形贡献 n−2 个扇形三角，所以 p 的首角下标可由其三角前缀加 2p 恢复。纯三角仍为原六列，只有含多边形输入追加 sourcetri。不存在按规模切换到旧串行实现的双路径。每元素内 double/float 运算顺序不变，最小间距在全部候选原序上串行淘汰、各列同步压紧。成功 join 后才构成返回 Geometry，失败不发布部分候选；不在 worker 上调用 Runtime 或操作 Geometry 结构。

Volume 的块级计数槽只累计普通数值；位置不能表示的业务失败返回 code=1 与候选 ordinal，join 后 host 翻译为 procedural 错误，不在每候选上分配 Error/String，也不保留块内 failed 标记这条旧错误通道。代表性 tasks 验收以 Failure 注入保留取消后的失败优先、完整 join、提交线程销毁、再次调用和 TLS 恢复的同一序列，不扩展矩阵。

## 帧阶段计时

[frame_timing.hpp](../modules/core/foundation/include/foundation/frame_timing.hpp) 提供常开的 CPU 帧阶段计时机制，dev 与 release 行为一致。foundation 只拥有根标记 `timing::frame`；各模块在自己的 `timing.hpp` 中以 `inline constinit timing::Marker` 声明阶段，给出键名、显示标题和父标记，例如 [renderer/timing.hpp](../modules/render/renderer/include/renderer/timing.hpp) 的 `render` 以 `timing::frame` 为父项。父标记只引用本模块或其依赖模块的标记，依赖方向与链接一致。

宿主在启动时、任何线程记录之前调用一次 noexcept 的 `timing::install({module_a::markers, module_b::markers})`：先登记 `frame`，再按参数顺序登记各模块标记，由此确定进程内唯一的目录、下标和显示行序。输入是编译进产品的 constinit 固定表，不是运行时业务输入；重复调用、空标记、同一标记重复登记、键名重复、父项未先登记或超过 64 项均为固定注册损坏，在 dev/release 都走唯一 fatal 出口，不回滚、不重试。有标记的错误以栈上有界诊断带出 key，不分配诊断字符串。没有静态自动注册，不依赖静态初始化顺序。`catalog()` 按下标返回已登记标记，`depth()` 给出层级深度。

宿主线程每帧用 `Recorder::Frame` 绑定一个 `Recorder` 并打开 `frame`，`finish()` 或析构时关闭并解绑；`last()` 保留最近一个完成帧的 `FrameRecord`。`Scope` 只读取标记预先分配的下标和父下标，与当前打开的阶段比较后读两次 `steady_clock`，不分配、不查找、不加锁。计时为包含式：父阶段包含子阶段；同一帧多次进入同一阶段时累加时长并增加 `entries`。`record(marker, total_duration, entries = 1) noexcept` 把其他时钟测得的分段（例如 RHI 帧调用内部的提交与等待）记为当前阶段的子项，遵守同一父项规则。总时长只加一次，不乘 `entries`；后者是本帧事件／进入次数，不是帧样本数，不改变消费者按帧计算 mean/p95 的口径。

批量事件由宿主在相关任务全部 join 后、声明的直接父阶段仍打开时提交；worker 只累计自己的普通整数，不写 Recorder 或累加 worker 时长。零事件不调用 `record`，不引入“0 次但有时长”；调用方须保证汇总值和同帧累计值可由 uint32 表示，不得静默窄化。`record` 与 `Scope` 入口共用注册、父项和容量检查：零次数、累计溢出、未注册或错父项均只记一次 `structure_error`，不增加时长和次数；满计数的 `Scope` 在改变当前父项或读钟前拒绝。未绑定或 `Suspend` 时仍不记录。

Recorder 已绑定时，未登记的标记、未在声明父项下打开的标记以及嵌套的 `Frame` 计入 `structure_errors`，`first_error` 指向第一个出错的标记，这些条目不记录时长。`Suspend` 在作用域内解绑当前线程，被挂起的工作整体计入外层已打开的阶段，用于辅助表面等不属于本帧阶段结构的嵌套调用；在 `Suspend` 内结束的 `Frame` 不会在它退出时被重新绑定。

没有绑定 Recorder 的线程只读取一次线程局部变量，不做记录。物理工作线程等辅助线程的耗时体现在发起它们的宿主阶段的墙钟时间中。Recorder 不是线程安全的，只能由绑定它的线程写入。产品宿主绑定 Recorder：编辑器的每次 `Application::step`、Play 的每次运行循环各是一帧。进程入口调用 [application/timing.hpp](../modules/editor/application/include/application/timing.hpp) 的 `install_frame_timing()`，依次登记宿主阶段 `app.*`、World、物理和渲染阶段；`Application` 与 `run_project` 启动时检查目录，未安装时以 `std::logic_error` 拒绝启动，不会把每个阶段都记成结构错误。宿主把每一步的记录交给编辑器的帧性能统计，供 Performance 表、Control 和 Play 报告使用。计时值只用于诊断报告，不作为验收的通过条件；验收只检查阶段是否存在、层级、进入次数和 `structure_errors`。单个作用域的开销由 `atelier_editor_document_acceptance --profile-scene-cpu` 输出的 `timing_scope_ns` 在同一机器上标定。
## 验收

构建与运行方式遵循 [README 的必要性验收](../README.md#构建与端到端验收)，按改动选择以下消费者；这里描述覆盖范围，不记录某次交付的通过状态。

`foundation_acceptance` 保留资源所有权和失败清理的代表序列，检查在 dev/release 中均有效，不依赖 assert：

- [资源目录消费者](../acceptance/foundation/associative.cpp)：Map 部分复制失败后，已有目标和源资源仍存活，退出作用域后全部释放。
- [生命周期消费者](../acceptance/foundation/lifecycle.cpp)：资源子树提升时的移动赋值顺序，以及 SharedArray 多线程复制、写时分离和线程结束后的共享缓冲释放。原子内存序在 x64 上不能由这项验收区分。
- [拥有资源消费者](../acceptance/foundation/main.cpp)：Array 自身别名追加/填充、构造与复制失败后的清理，以及宏生成原生 Inventory、描述型 Array 字段、独立快照、Runtime 值复制与 GC 析构资源的链路。
- [原生任务消费者](../acceptance/foundation/tasks.cpp)：不可变输入与 COW 输出的非对称邻接计算、非零起点和尾块、嵌套 join；取消后已开始任务返回 Failure 的优先传播、用户闭包在提交线程释放及失败后再次调用。原地正向计算反例说明仅写区间互不重叠不足以安全并行，不执行有数据竞争的反例。只检查结果与生命周期，不以耗时或参与线程数判 PASS。
- [Array 缓冲寿命](../acceptance/foundation/array_lifetime.cpp)与 [String 缓冲寿命](../acceptance/foundation/string_lifetime.cpp)：分别执行一次 Array 拷贝/销毁和一次 String 目录清空，严格比较前后 mimalloc 活分配字节；不是元素 live 计数，也不是性能采样。两个可执行文件随同一个 `foundation_acceptance` 入口运行，不输出计时或性能报告。

[帧阶段计时消费者](../acceptance/foundation/frame_timing.cpp)检查目录组装、层级与记录序列，入口仍为 `foundation_acceptance`。

[源码借用消费者](../acceptance/source/native_values.cpp)通过 `source_acceptance` 保留共享布局、原地写入与别名、失败借用及拥有者析构；[模块消费者](../acceptance/source/modules.cpp)和[组合字段消费者](../acceptance/source/value_fields.cpp)保留 String/Array/记录的深层失败、部分构造与赋值清理。重复 owned-values 入口已移除；Source 目标仍包含在 `jit_acceptance` 中。

设计依据与 UE/Qt 的取舍见[研究记录](research/shared-foundation-types.md#qtue-接口取舍)。

历史采样及条件见[字符串研究](research/string-storage-performance.md)和 [Array 研究](research/array-storage-performance.md)；当前没有独立的 Array/String 性能测量入口。
