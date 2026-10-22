# 精悍自有 STL：依据与后续方向

研究日期：2026-10-04。Map/Set 与两个业务消费者已落地；第二、三段是候选方向，没有排期。本文保存设计依据，现行接口以[基础库文档](../foundation.md)为准，语言能力以 [LANGUAGE.md](../../LANGUAGE.md) 为准。

研究覆盖工业容器、哈希表、本地消费者与共享语言契约，并经 gpt-6.1-sol 专家复核。外部事实以一手文档、固定源码为依据；UE 完整官方源码未读取，所述 UE 行为来自公开官方文档/API。下文明确区分已交付范围与后续建议，外部基准不作为本项目性能证据；补充事实与取舍见[工业依据记录](foundation-stl-industrial-research.md)。

## 方向与完成尺度

将现有 `atelier_foundation` 演进为引擎及自有语言的基础库权威入口。目标是引擎自有代码中的业务数据、模块接口、文本与容器默认使用项目控制的类型；类型的所有权、失效、错误、分配及语言接线由项目维护。底层可复用成熟工业实现，供应商类型和私有布局留在实现内。

长期逐步覆盖引擎内部、编辑器、工具与编译前端的实际需要。LLVM、图形库、filesystem、JSON 和 Windows 的外部接口遵循各自契约，转换集中在相应模块接入处。`std::atomic`、类型特征、构造/析构原语、数值限制与成熟无状态算法可继续直接使用；只有真实成本或语义收益才增加本地设施。

完成度按真实接管衡量：正式模块接口与调用链是否使用 Foundation；业务是否仍需重复处理容量、复制、编码、失效和哈希；旧的拥有路径是否在迁移后退出。迁移不以减少 `std::` 文本数量为验收。

Epic 官方规范同样按设施选择标准库，明确推荐 atomic、标准 type_traits、initializer_list、limits 等。UE 的自有容器体现引擎控制数据契约的方向，完整 ISO C++ 标准库重写并非其官方要求。[Epic C++ 规范](https://dev.epicgames.com/documentation/unreal-engine/epic-cplusplus-coding-standard-for-unreal-engine)

## 现有基础与初期范围

当前能力由[基础库合同](../foundation.md)、[源码拥有值](../source-owned-values.md)及 [LANGUAGE](../../LANGUAGE.md)维护，本报告不另立现行规范。

| 家族 | 当前基础 | 建议推进 |
| --- | --- | --- |
| 连续拥有存储 | `Array<T>`，深复制、移动、批量平凡路径、自身追加、过对齐、显式容量管理 | 接管完整 producer/consumer 链；已有能力直接复用 |
| 连续借用 | `Span<T>`，只读由元素 const 表达 | 成为数组类模块接口的共同借用形式 |
| 文本拥有存储 | UTF-8 `String`，32 字节对象、15 字节 SSO、尾零、嵌入零、显式校验 | 接管真实 UTF-8 文本与身份键；保留现有表示与错误合同 |
| 文本借用 | `StringView`，字节范围，可含未验证输入 | 同步文本参数和异构键查询；查询不构造 String |
| 唯一键关联 | 原生 `Map<K,V>` 与同一 Hash/Equal 合同已实现 | 已接管 AssetBrowser 的资源键字典，后续按完整模块扩大使用 |
| 唯一成员关联 | 原生 `Set<T>` 已实现，与 Map 共用哈希内核 | 已接管 World 临时重复身份检测 |

初期围绕这六个类型收敛；Hash/Equal 是它们的共同操作合同。这里的 `Map` 定义为不承诺遍历顺序的唯一键字典。需要排序、范围查询或地址稳定时，根据真实消费者另选有明确合同的存储；不能由某个底层实现悄悄决定公共语义。

UE 的 TArray 为拥有值容器，TMap/TSet 使用稀疏数组及哈希，增长也不能保证元素地址稳定。可借鉴显式区分保序和交换删除，以及容器内部控制存储；项目不需要随之新增稀疏数组或照搬全部 allocator 参数。[TArray](https://dev.epicgames.com/documentation/en-us/unreal-engine/array-containers-in-unreal-engine)、[TMap](https://dev.epicgames.com/documentation/en-us/unreal-engine/map-containers-in-unreal-engine)、[TSet](https://dev.epicgames.com/documentation/en-us/unreal-engine/set-containers-in-unreal-engine)

## Map/Set 底层选择与依据

**首片已采用 ankerl::unordered_dense，通过依赖锁文件固定版本。** 研究核实 `v5.3.0` 的 tag 对应 commit `0d34cfeb5dea7fe7681a61473f8cea334ddf0a8c`，发布于 10 月 3 日。5.x 改为分组指纹索引，已经不能使用旧 4.x Robin Hood/backward-shift 描述。新版本的工业成熟度仍需与旧系列分开评价，正式依赖版本只由[锁文件](../../cmake/dependencies-lock.json)维护。[固定发布](https://github.com/martinus/unordered_dense/releases/tag/v5.3.0)、[5.x 迁移说明](https://github.com/martinus/unordered_dense/blob/v5.3.0/doc/upgrading-to-5.md)

| 候选 | 适用特征 | 本项目取舍 |
| --- | --- | --- |
| ankerl dense | 连续值区加分组索引；遍历与值局部性；透明查询和缺失时构造 | 首片已采用；删除搬末尾值，按明确失效合同使用 |
| Boost unordered_flat_map | 值直接处于槽，分组过滤；删除不搬其他值，rehash 搬移 | 默认候选在真实查询/小表场景明显吃亏时的对照 |
| Boost unordered_node_map | 独立节点保持元素地址，带节点分配与间接访问 | 有真实跨结构修改地址稳定需求后再评估 |
| Abseil SwissTable | 成熟分组探测、直接槽值；flat/node 分开 | 参考算法；固定头文件声明无异常安全保证，不能直接承担现项目的失败合同 |
| Folly F14 | Value/Vector/Node 存储，探测机制共用 | 参考存储取舍；当前单为 Map 引入完整依赖收益不足 |

这些是布局及合同对照。dense 会增加一次值索引访问，direct-slot 也有稀疏载荷、遍历和扩容成本；哪个更合算要由查询命中率、条目大小、表规模与删除行为共同决定。[ankerl 固定设计](https://github.com/martinus/unordered_dense/blob/v5.3.0/doc/design.md)、[Boost flat 合同](https://www.boost.org/doc/libs/1_88_0/libs/unordered/doc/html/unordered/reference/unordered_flat_map.html)、[Boost 存储设计](https://github.com/boostorg/unordered/blob/boost-1.88.0/doc/modules/ROOT/pages/structures.adoc)、[Abseil 固定头](https://github.com/abseil/abseil-cpp/blob/20250814.1/absl/container/flat_hash_map.h)、[F14 固定设计](https://github.com/facebook/folly/blob/v2026.09.28.00/folly/container/F14.md)

初版只维护一种正式 Map 实现。上游按现有 CMake 依赖锁定方式接入，保存版本、来源、许可证与必要修改；不暴露 `native_handle`、供应商 iterator 或桶布局。EASTL 沿既有选择只作工业参照，本轮不整体接入另一套基础库。

上游默认 std::allocator/vector 已能沿全局 new/delete 使用 mimalloc。先复用该成熟实现，避免为了改变私有存储名称就让现 Array 承担整套 STL 兼容协议。ankerl 支持自定义 value container；后续若接现有 Array，所需适配留在 Map 的实现内，维护同一容器语义，不给业务增加 ArraySTL 等入口。是否值得替换内部存储由实际复杂度与收益决定。[自定义容器协议](https://github.com/martinus/unordered_dense/blob/v5.3.0/doc/usage.md#custom-container-types)

## 公共接口要承担的真实不变量

下列设计考虑已落实到首片；具体函数、约束和错误行为由[当前合同](../foundation.md#mapset-与哈希)维护，本研究记录不形成第二套规范。

1. **拥有与键稳定。** Map 拥有 K/V，Set 拥有 K；默认持久文本键使用 String。遍历不允许修改 Map 键或 Set 元素；修改参与哈希/相等的状态必须通过能维护索引的操作。值可通过受限访问修改。表内不保存临时 StringView 作为拥有键。
2. **小而完整的接口。** 已提供状态查询、`reserve/clear/release`、`find/contains` 与 `remove`；Map 的 `try_emplace/insert_or_assign` 返回值指针和是否新增，Set 的 `add` 返回是否新增。没有开放全套 bucket/load-factor/vendor API，尚未增加 multimap、有序和稳定节点变体。Set 使用真实 set 存储，共用 hash 内核，不存一列虚假 bool 值。
3. **容量和清理。** reserve 按可容纳的总条目数定义，不能混成追加数量；clear 销毁元素并保留容量，release 释放动态存储；删除不隐式收紧存储。mimalloc 继续是现行默认分配路径。
4. **借用和遍历。** 用户已接受迭代器不稳定；当前合同明确结构操作后重新取得指针、引用和迭代状态，不承诺节点地址稳定。删除可能把另一个值搬到原地址，地址/下标不成为业务身份。普通遍历仅进行不改键和结构的操作；后续消费者的边遍历删除需有明确的继续位置合同。
5. **哈希等价。** 同字节 String/StringView 使用同一哈希与相等入口，包含长度和嵌入零，不增加大小写折叠或 Unicode 规范化。StringView 查询不要求有效 UTF-8、不分配；相等键在同一算法/种子下必须同哈希。哈希结果与表顺序不用于存档身份、网络身份或确定性游戏逻辑，确定输出显式排序。
6. **元素和错误能力。** dense 首版要求 Hash/Equal 不抛，K/V 具备不抛移动构造、移动赋值与析构；拒绝不满足搬移条件的类型并给明确诊断。分配、键/值构造及复制仍按逐操作合同处理，可能抛异常的操作不统一标 noexcept。C++ 与 Runtime 的既有错误边界保持；不借新容器重写全局 OOM 政策。

ankerl v5.3 的 erase 在移除旧索引后移动末尾值、重新哈希修索引，所以任意抛异常的搬移/hash 不能得到通用安全保证。源码还支持透明 try_emplace：命中直接返回，只有缺失才构造拥有键。因此 `Map<String,ControlKey>` 可直接从 StringView 查询或尝试插入，避免先 find 再 insert 和每次查找创建 String；传入的 mapped 参数表达式仍按 C++ 规则提前求值。[删除实现](https://github.com/martinus/unordered_dense/blob/v5.3.0/include/ankerl/unordered_dense.h#L2929)、[透明插入](https://github.com/martinus/unordered_dense/blob/v5.3.0/include/ankerl/unordered_dense.h#L4151)、[查询至构造路径](https://github.com/martinus/unordered_dense/blob/v5.3.0/include/ankerl/unordered_dense.h#L3410)

Hash 的首片可复用已选后端的成熟字节算法，通过项目的 String/StringView 入口保持一致；只在算法确有质量依据时向后端声明高质量哈希。普通快速哈希不自动具有抗对抗性碰撞能力；未来网络输入应在实际入口选择对应策略，不提前建全局哈希策略框架。结构体不得默认对含 padding 的整块表示做语义哈希。

## 三段推进与真实交付

### 第一段：Map/Set 成为真实业务基础（已交付）

**交付包 A 已随 a57cb0c 发布：Foundation Map/Set + AssetBrowser 键字典 + World 临时重复身份 Set。**

- AssetBrowser 首个 Map：[keys_](../../src/editor/asset_browser.hpp) 保存拥有键到 ControlKey，[插入路径](../../src/editor/asset_browser.cpp)立即按值读取；无长期节点借用，也不靠 Map 遍历决定 UI 顺序。插入键来自 ASCII 前缀、已验证 UTF-8 相对路径或 canonical GUID，适合 String 所有权。
- 保持既有 nav/item/row/type/size 身份域、字节区分大小写、reset 清表及 next_key_ 单调递增。搜索、过滤、目录刷新不应更换已有 ControlKey。`control(std::string_view)` 仍允许任意字节查询且缺失返回 0，用不校验 StringView 保持行为；首片不必同时改整个 Project 的公开字串接口。
- World 临时 seen Set：[接入检查](../../src/world/world.cpp)检测重复对象身份，结果不依赖 Set 顺序；复用现有对象标识的 Hash/Equal，拥有集合不自行变成 GC 根。
- Map/Set 的容量、删除、资源与失败行为通过现有公开 Foundation 消费入口加必要端到端场景覆盖；不用单元/mock 框架。受影响目标及 Foundation 在 dev/release 均验收。

真实 [Project 工作区消费者](../../acceptance/editor/project_workspace.inl)生成 2600 文件目录并验证虚拟化、滚动及末尾资源，首片已加入中文/非 BMP 键、隐藏/恢复后身份、reset 和任意字节 missing 查询断言。查询本身不构造拥有键，但 AssetBrowser 仍先拼接资源前缀，不能宣称整条调用零分配。后续若做性能采样，须滚遍中间 viewport，让 keys_ 实际积累完整工作集；现有头/尾验收不足以证明已测 2600 项 Map。比较同一端到端路径的查询分配、内存、耗时及尾部情况，区分字符串构造、哈希、探测与 UI 工作。

生命周期复核已修复四类问题：完整检查比较结果到 bool 的异常转换；非透明查询只接受实际键类型；有状态 Hash/Equal 移动后为源恢复默认策略；Array/Map/Set 移动赋值先接管来源再释放旧内容，避免来源由目标间接拥有时提前析构。[资源目录验收](../../acceptance/foundation/associative.cpp)与[生命周期验收](../../acceptance/foundation/lifecycle.cpp)保留了反例及幂等、边界和失败恢复场景。

ankerl v5 最小索引为 4 组、352 字节，空表首插入才分配；大量很小的表需要观察实际内存。该数字为固定源码推导，尚未在本项目测量。只有首选候选表现明显不足才增加有针对性的 Boost flat 对照，不例行建立多后端基准工程。[最小索引源码](https://github.com/martinus/unordered_dense/blob/v5.3.0/include/ankerl/unordered_dense.h#L1942)

### 第二段：逐个完整模块接管（候选）

| 完整迁移片 | 接管内容 | 保留的关键行为 |
| --- | --- | --- |
| immutable Mesh | `src/geometry/mesh.hpp:24` 的顶点拥有存储及只读范围，连同 OBJ、primitive/cube 生产者改用 Array/Span | Mesh 校验/归一化、MeshRef 支撑视图寿命；gfx 边界仍 data/size |
| Project 资源目录 | `src/editor/project.cpp:152` 起目录快照、UTF-8 key、索引与 mesh 缓存，String/Array/Map/Set 一并按责任接管 | 准备完成再 swap 发布、revision、rename 复用及 import 失败回滚 |
| World 成员索引 | `src/world/detail.hpp:58` 的 ObjectId→索引表及其调用链 | members 的行为顺序、发布前可能失败的分配、回滚、生命周期与回调重入保护 |
| Renderer 缓存 | `src/renderer/renderer.cpp:180` 的 geometry/image 缓存 | erase 前 GPU retire、统计和 CPU owner 释放；处理 dense erase 搬入的项及关闭失败重试 |

每片完成接口、存储和本地调用方迁移，清理替代路径；转换集中在外部库接入处。逐步继续进入 Runtime/Source/编译前端内部时，先核实元数据地址、调用暂存和版本存续的真实约束，不能对所有 std::unordered_map 机械换成 dense。

Windows/DirectWrite 的 UTF-16、caret/IME 编辑单位及 ui_layout 的专用 BufferAllocator 有各自合同。现 `std::wstring` 与专用布局缓冲按独立完整片处理；本轮普通 String/Array 迁移不能改这些行为。

### 第三段：只增加付得起固定成本的设施（后续方向）

| 设施 | 明确触发 |
| --- | --- |
| SmallArray / FixedArray | 高频小序列确有分配成本或编译期固定长需求；按实际分布选择容量 |
| BitSet | 大量布尔标志/集合运算有实际密度需求 |
| RingBuffer / Queue | 已有 FIFO、高频 push/pop 或固定容量需求；并发队列另按线程合同处理 |
| FunctionRef / UniqueFunction | 同步不逃逸回调与拥有回调需要分清，现 std::function 有实际分配/复制或表达问题 |
| 文本 Builder、数值转换、格式化 | 已见重复文本分配或业务表达缺口；复用 String/View 和成熟算法 |
| 有序/稳定字典 | 范围查询、确定顺序或跨结构变化地址稳定成为真实不变量 |

SmallArray 用同一数组行为和清楚的内联存储机制；跨接口继续用 Span。内联空间是每个对象的固定成本，N 会改变类型与布局，内联态 move/swap 也不再是单纯转交堆指针。LLVM SmallVector/ArrayRef 和 EASTL fixed_vector 可作实现参考；不同时发展两套数组 API。[LLVM 经验](https://llvm.org/docs/ProgrammersManual.html#llvm-adt-smallvector-h)、[EASTL fixed_vector 固定源码](https://raw.githubusercontent.com/electronicarts/EASTL/3.27.01/include/EASTL/fixed_vector.h)

Optional、Result、智能指针与无状态算法按实际语义选择。现 Runtime Result 与对象根已有权威入口，不能为名称统一新增竞争身份/错误路径。FName 的名字驻留与 FText 的本地化 history 属于对应语义系统，按真实需求分别规划。[UE 字串类型](https://dev.epicgames.com/documentation/unreal-engine/string-handling-in-unreal-engine?lang=en-US)、[本地化语义](https://dev.epicgames.com/documentation/en-us/unreal-engine/text-localization-in-unreal-engine)

## 与自有语言的统一演进

C++ Foundation 完备度与源码开放程度分别交付，底层类型及生命周期共同使用 Runtime。

```text
C++ Foundation 直接使用 ─────────────────────────────┐
                                                    │ 同一实际存储/构析
原生定义生产者 → Runtime 同一类型事务 → Program/JIT ──┘
```

Array/String 的构析、借用、序列修改已经沿这条路径；本次已发布的 Map/Set 仅服务原生 C++，没有 Source 语法、反射定义或全部实例的自动注册。出现真实源码/反射需求后，再与 Runtime 负责人协作补 K/V 权威类型关系、查询的命中/缺失、值访问与失效合同及必要修改原语。保持原定义事务、值生命周期、错误与 GC 机制，Map 不套用连续 sequence_view，不建第二套容器目录。[现统一链](../source-owned-values.md)

`Array<源码 struct>` 是中期的重要候选：现 array_definition<T> 依赖编译期 C++ T，而源码记录在 Runtime 才产生布局和操作。要支持它，需要一套能接收 Runtime 元素布局/操作的实际容器生产能力，并与原生数组共享权威数组语义；布局、类型实例化与快路径须形成独立可审查方案。增加别名不能解决这个缺口，当前不承诺与 C++ Array<T> 私有表示相同。[源码现行边界](../../LANGUAGE.md)

含 GC 引用的 Array/Map/Set 更需接回元素引用遍历、根与写屏障。ReferenceSequence 当前承担对象字段内强引用集合的身份、屏障和扫描责任；普通 `Map<K,ObjectId>` 不自动保活对象。保留现有边界，待真实引用集合需求单独协作。[Runtime 协议](../runtime-model.md)

Position 源码 UI 标签的已交接需求是：原生方法同步借 `const String&`，完成按长度的 UTF-8→UTF-16 转换，结果由构造上下文/Control 拥有，不保存源码局部的 view。中文、非 BMP、嵌入零和无效目标输入沿既有 UI 文字合同处理；静态文字构造时转换，动态文字改变时转换。转换服务放在适当编码/平台接入层，不给 String 添加 UI 专用方法。该 UI 接线不属于 a57cb0c；bool、多具名导出、通用值方法和 Reload 的阶段进展由应用/Runtime 在最终集成文档中核定，本研究稿不把其他分支成果判定为未实现。

Rust 的 Vec/String/HashMap 与 Swift/C++ 互操作同样依赖明确的拥有、等价、借用有效期合同。可借鉴不变量，不复制它们的对象系统或把静态借用规则当成本项目已经具备的能力。[Rust Vec](https://doc.rust-lang.org/std/vec/struct.Vec.html#guarantees)、[Rust String](https://doc.rust-lang.org/std/string/struct.String.html#representation)、[Rust HashMap](https://doc.rust-lang.org/std/collections/struct.HashMap.html)、[Apple 互操作](https://developer.apple.com/videos/play/wwdc2025/311/?time=501)

## 前沿机制与限制

分组指纹探测值得借用成熟实现；可重定位机制值得跟踪。WG21 的 2026-04 P4197 明确记录 trivial relocation 从 C++26 移除，并整理 C++29 的设计问题。因此当前 C++23 下保持 Array 的操作级平凡快路径与非平凡构析；String、智能指针等不能仅靠一个项目 trait 获得任意 memcpy 搬迁许可。[P4197](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p4197r0.html)、[现 Array 依据](array-storage-performance.md)

默认 mimalloc、SSO、容量复用与批量复制都是现有资产，不再建立容器分配器注册表、全局 arena 或通用容器虚表。需要新存储策略时，由实际资源生命周期与工作负载触发。

首片已验证项目工具链、拥有资源、过对齐元素、失败清理、生命周期及两个实际业务接入；没有测量新 Map 的性能收益、最小表实际占用或扩容峰值，不能宣称比 std、UE 或其他候选更快。交付包 A 已完成；其余阶段仍需按真实需求独立确定范围，不构成本次文档归档的生产授权。
