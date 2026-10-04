# 工业基础库依据补充

核对日期：2026-10-04。本文记录方案阶段独立核实的工业依据与边界，不另立现行合同；实施后的当前能力以[基础库文档](../foundation.md)为准。研究阶段没有改生产代码、构建或性能测量。UE 依据为公开官方文档/API，未读取完整引擎源码；在线文档会随版本变化。

## UE 的当前选择原则

**事实：** Epic 当前规范明确认为标准库已成熟，不愿为所有设施维护包装或重写；旧自有实现失去价值时可以弃用并迁向标准库。新代码推荐 `std::atomic`，重叠部分推荐标准 `type_traits`；新 trait 应使用小写 `value/type`，以支持 `std::conjunction` 等组合。`initializer_list`、`numeric_limits`、`cmath` 也有明确使用许可，同时要求一个 API 不混用两套惯用形式。[Epic C++ 规范](https://dev.epicgames.com/documentation/en-us/unreal-engine/epic-cplusplus-coding-standard-for-unreal-engine#useofstandardlibraries)

**判断：** 自有 Foundation 的收益来自拥有、分配、错误与语言接线的统一合同。用成熟原子、编译器识别的特征和底层算法，并不削弱这个目标；为了消除 `std::` 字样另建透传入口，缺少上述工业依据。

## 不能直接移植的 UE 假设

**事实：** `TArray` 拥有元素、深复制，allocator 控制布局与增长；官方文档明确假设元素可直接按字节重定位。`TMap/TSet` 的稀疏数组存储不承诺顺序或连续布局，扩容仍可能使指针/引用失效；Set allocator 同时决定桶与元素存储。[TArray](https://dev.epicgames.com/documentation/en-us/unreal-engine/array-containers-in-unreal-engine)、[TMap](https://dev.epicgames.com/documentation/en-us/unreal-engine/map-containers-in-unreal-engine)、[TSet](https://dev.epicgames.com/documentation/en-us/unreal-engine/set-containers-in-unreal-engine)

**判断：** 这些是 UE 自身的类型前提，不能变成项目任意 C++ 类型可 `memcpy` 的依据。保持现有 Array 的非平凡构析路径；不以容器地址代替 Runtime 对象身份，也不为模仿 UE 预建整套 sparse/allocator 家族。

## 字串语义与编码证据质量

**事实：** UE 的 `FString` 是可修改字符串，`FName` 是不可修改、忽略大小写的驻留标识，引用全局名字表；`FText` 承担本地化及文本生成。三者对应不同语义。[字符串分类](https://dev.epicgames.com/documentation/en-us/unreal-engine/string-handling-in-unreal-engine)

编码概览仍包含 2009/2010、旧平台和 UCS-2 描述，不能把它当现代 UE 全部文本能力的完整清单；当前 API 已明确列出 UTF-8 view/literal，以及 UTF8/UTF32 等输入。`StringCast` 要求正确的源编码类型，普通 `char*` 的非 ASCII 字节不能自动被当成 UTF-8；按长度重载也不要求尾零。[历史编码概览](https://dev.epicgames.com/documentation/en-us/unreal-engine/character-encoding-in-unreal-engine)、[当前 FString API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FString)、[StringCast](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/StringCast)

**判断：** 源码 String 标签同步按长度转成 UI 拥有的 UTF-16，集中于文本/平台边界；不保留局部 StringView，不把 UI 转码塞进 String。接线优先集中项目已有平台转换能力，短标签需求不足以证明应立即引入新的 SIMD 转码依赖。名字驻留和本地化系统分别等待实际业务，不新增竞争的全局身份表。

## EASTL 与 LLVM 的实际启示

**事实：** EASTL 的固定版本设计承认 STL 大部分设计良好，主要区别是分配控制、扩展容器及游戏优化；其中仍有早期标准库限制和速度宣称，不能代替 C++23 下的项目测量。3.27.01 的正式 CMake 编译 `.cpp` 并依赖 EABase，并非完全无依赖的头文件库。FAQ 明确 allocator 与 std 不直接兼容；仓库采用 BSD-3-Clause，README 记录持续维护者。[固定设计](https://github.com/electronicarts/EASTL/blob/3.27.01/doc/Design.md)、[固定构建](https://github.com/electronicarts/EASTL/blob/3.27.01/CMakeLists.txt)、[FAQ](https://github.com/electronicarts/EASTL/blob/master/doc/FAQ.md)、[许可证](https://github.com/electronicarts/EASTL/blob/master/LICENSE)、[维护说明](https://github.com/electronicarts/EASTL#credits-and-maintainers)

LLVM 的 `SmallVector/SmallString` 用对象内空间减少小负载分配，也增加每个实例体积；文档更推荐临时栈缓冲。`StringRef` 借用指针与长度，不延长存储生命；只读连续输入用 `ArrayRef`，不让接口绑定内联容量。[LLVM 手册](https://llvm.org/docs/ProgrammersManual.html#picking-the-right-data-structure-for-a-task)

**判断：** 初期围绕现有四类加拟新增的 Map/Set 六类收敛，EASTL 作分配/容器机制参照、LLVM ADT 作选择性实现参照；SmallArray、Builder 等按真实分配热点引入。整体接入第二套基础库或扩大对象内缓存，并无本项目已测收益。

## 主负责人补充判断

本轮另由 gpt-6.1-sol 分别核实哈希实现和本地容器接线。以下对已有方案补充决策理由和限制，仍是待实施建议，不修改现行 Foundation/Runtime 合同。

### 接管尺度与当前资产

长期目标应覆盖自有模块的业务存储与接口：拥有数据默认使用 Foundation，连续借用默认使用 Span/StringView，第三方与平台转换集中在接入处。标准原子、特征、构造析构原语和成熟算法继续作为实现材料。Optional、回调、原生资源智能指针若要进入自有公共接口，应说明实际能力与契约收益；现有 Runtime Result、对象根和引用身份仍只有一个权威入口。

当前工作树的文本计数中，src 内 std::vector 与 std::span 出现分别为 324、219 次，std::unordered_map/set 为 15、5 次。这只是范围线索，不是容器实例数、热度或迁移 KPI；它说明 Map 补齐后，更大的工作仍是让现有 Array/String/视图接管完整生产消费链。

Array/String 已有合理快路径，不应从零重写。现有 Array 测量还揭示一个真实取舍：Array<String> 复制赋值为了保持失败时原值不变，采用构造副本再替换，可能比复用目标元素容量的标准容器产生更多分配。这不能通过不经讨论地改为逐项部分提交来“优化”。未来确有热点时，先选择复制整个拥有值是否必要，再评估独立明确的更新操作。[既有 Array 测量](array-storage-performance.md#本轮实现与同机采样)

### ankerl 的候选条件

已核实 v5.3.0 于 2026-10-03 发布，README 的 5.2.0 字样可能滞后。5.x 已换成分组指纹索引；不能沿用旧版 Robin Hood/backward-shift 介绍。v5.3.0 可作为具体接入候选，但一次刚发布的版本与长期项目验证是不同证据。[固定 release](https://github.com/martinus/unordered_dense/releases/tag/v5.3.0)、[设计](https://github.com/martinus/unordered_dense/blob/v5.3.0/doc/design.md)、[升级说明](https://github.com/martinus/unordered_dense/blob/v5.3.0/doc/upgrading-to-5.md)

- 连续值存储适合遍历，查询还需通过索引访问值；erase 会用尾元素填洞。Map 的键必须只读，结构修改后重新取得借用，不把条目地址当身份。
- 本项目首版可要求 Hash/Equal、元素移动构造/赋值及析构不抛。这是为了限制接入风险而提出的项目合同，不是声称上游对所有类型都有这样的静态检查。上游已有若干分配/复制异常保护，但 erase 和重建索引仍不能据此宣称任意抛异常的 hash/move 都可恢复。[固定实现](https://github.com/martinus/unordered_dense/blob/v5.3.0/include/ankerl/unordered_dense.h)
- 初次非空最少 4 组、每组 88 字节，即约 352 字节索引，尚不含值缓冲；空表使用共享 sentinel。大量小表是单独的成本轴。Folly F14 官方设计对 tiny table 单独优化，证明这属于实际工业取舍；并不因此建议引入整个 Folly。[F14 设计](https://github.com/facebook/folly/blob/v2026.09.28.00/folly/container/F14.md)
- 默认只接一种正式实现。若真实负载显露查询、删除或小表成本问题，再选择 Boost flat 等做针对性对照；不先建立可切换后端框架。模板内部复用 vendor 不要求业务直接看到 vendor 接口，也不等于已经获得跨版本二进制布局稳定性；不为隐藏实现给每个 Map 增加堆分配的 PImpl。

选择性对照中，Boost unordered_flat_map 直接把元素放在槽内，删除不搬其他元素，rehash 仍会搬迁；Abseil 明确区分 flat 与保持节点地址的 node 方案，且 flat_hash_map 头文件声明不提供异常安全。不能只比较查找延迟而忽略项目正在使用的 C++ 异常边界。ankerl 源码为 MIT，Boost 使用 BSL-1.0，Abseil 为 Apache-2.0；正式接入按现有依赖方式固定来源与版本，保留许可和本地修改记录。[Boost flat 合同](https://www.boost.org/doc/libs/1_88_0/libs/unordered/doc/html/unordered/reference/unordered_flat_map.html)、[Abseil 指南](https://abseil.io/docs/cpp/guides/container)、[Abseil 固定头](https://github.com/abseil/abseil-cpp/blob/20260817.0/absl/container/flat_hash_map.h)、[Boost 许可](https://www.boost.org/LICENSE_1_0.txt)、[Abseil 许可](https://github.com/abseil/abseil-cpp/blob/20260817.0/LICENSE)

Hash/Equal 应先于 Map 一起定义：相等键同哈希，String/StringView 按长度及全部字节比较，查询不构造拥有键；结构体按语义字段，不直接哈希 padding。hash 与表遍历顺序不是持久身份、网络协议或确定性执行顺序。

**用户已确认的设计取舍（2026-10-04）：** 默认 Map/Set 不保证迭代器在插入、删除、扩容等结构修改后仍然有效；调用方在修改后重新取得迭代状态。稳定迭代器不作为默认实现的选型门槛，也不为其引入节点存储或额外间接访问。遍历中删除若有实际需求，由明确的遍历/删除接口表达继续位置。此次确认针对迭代器，元素指针与引用的失效规则仍须在正式接口中分别写清。本条记录方案决策，尚未发布新的生产接口。

### 首片消费者与性能措辞

[AssetBrowser::keys_](../../src/editor/asset_browser.hpp) 保存字符串到 ControlKey；插入后立即按值读取，UI 顺序来自 items_，reset 后 next_key_ 继续递增。它适合作为首个 Map；[World 临时 seen](../../src/world/world.cpp)只检查 ObjectId 重复，适合作为同一内核的 Set。ObjectId 的已有完整身份哈希必须保留，存进集合不产生 GC 保活。

AssetBrowser::control 先构造 `"item:" + resource`，key_for 的调用者也会拼前缀。因此异构 StringView 查询只保证查询本身不要求新建 String，不能宣称整条调用零分配。首片保留身份域、大小写、任意字节 missing 查询、过滤/刷新后的 ControlKey 与 reset 语义；前缀构造是否进一步优化由测量决定。

后续逐片迁移 Mesh 顶点生产消费链、Project 资源目录、World 持久索引、Renderer 缓存。每片同时迁移内部拥有存储与相关参数返回值，保留外部库适配；尤其保留失败回滚、资源退休顺序和借用期限。沿已有端到端消费者在 dev/release 验证；性能比较包含 0/1/4/16 小表、实际目录工作集、命中/未命中、增删交错、分配次数、峰值及保留内存。计数和外部基准均不代替该测量。

### 语言能力的独立里程碑

Map/Set 首版可以先服务原生业务。中期优先把 `Array<源码 struct>` 作为真实类型生产里程碑：现有 array_definition<T> 依赖 C++ 原生元素表示，源码当前只解析已注册实例。需要让原类型事务按元素 TypeId、布局与值操作生成/复用容器定义，而非增加别名。Foundation 继续不依赖 Runtime；静态元素保留编译期快路径，描述型元素由 Runtime 提供操作。每个已发布 TypeId 的实际表示须唯一，不能把布局相似的描述型记录强转为原生 T。[原生定义](../../src/runtime/foundation.hpp)、[源码类型表示约束](../source-type-declarations.md)

Map 接入语言时要表达 K/V 类型、命中/缺失、只读键和修改失效，不能套连续 sequence_view。含 GC 引用容器、跨挂起借用、一般聚合按值调用与递归容器分别涉及追踪、屏障、生命周期或布局依赖，需要与 Runtime 负责人形成对应契约后交付。尤其 `Node { Array<Node> children; }` 未来需区分内联布局边与间接存储边，不能直接移除现有循环校验。

### 文本与前沿机制

当前 src/editor/project.cpp 的 utf8/wide 与 src/ui/style_sheet.cpp 已使用 Win32 严格转码入口。Position 标签需求可先归并这类编码能力，完成转换后由 UI 持有；同时保留空文本、明确长度、非 BMP 和 UI 索引语义。通用 UTF 转换是可复用能力，UI 方法不应进入 String；非法输入拒绝与错误翻译由真实调用边界负责。[Win32 转码](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar)

simdutf 的验证/转换及 fmt 的 format_to、输出缓冲能力可作为后续工业实现候选。它们不意味着 UTF 校验可随意省略，也不要求 String 开放不受约束的可写字节；格式化到 String 仍需维护有效 UTF-8、尾零、失败和容量合同。只有本项目工作负载证明收益时才增加依赖。[simdutf](https://github.com/simdutf/simdutf)、[fmt 输出接口](https://fmt.dev/latest/api/)

2026-04 的 WG21 P4197 记录 trivial relocation 已从 C++26 移除，讨论 C++29 设计问题。当前继续使用 C++23 的已验证平凡快路径和真实构析，不把前沿提案当已经可用的语言保证。[P4197R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p4197r0.html)
