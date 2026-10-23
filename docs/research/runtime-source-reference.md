# 对象运行时与执行器：源码参考范围

核查日期：2026-09-21。UE5、Godot 官方源码检出在项目外的本机参考目录，用于对象生命周期与类型发布的定向核查；没有构建外部引擎或完整审计其实现，其余项目为在线参考。参考项目不是依赖或待移植清单；当前行为以[运行时协议](../runtime-model.md)为准。

本项目执行方向现已确认仅 JIT，见[编译与执行契约](../language-contract.md#编译与执行)。本文中的 VM 项目用于比较对象、调用、根和执行管理机制，不表示计划引入解释器；历史研究结果与当前接入方案分开记录。

## 本机参考副本

| 仓库 | 路径 | 固定版本 | 提交 |
| --- | --- | --- | --- |
| `EpicGames/UnrealEngine` | `E:\EngineReferences\UnrealEngine` | `5.8.2-release` | `16d75d84714512edfb744e1fd0a59e9c74d57873` |
| `godotengine/godot` | `E:\EngineReferences\Godot` | `4.7.2-stable` | `ed1daf0bf001b61586d9930840f2f1394092c079` |

两者均以 `depth=1`、`filter=blob:none` 克隆，在版本标签对应提交上保持 detached HEAD，工作区检查为干净。UE 官方私有仓库已通过本机 `gh` 授权访问；没有获取构建依赖或运行引擎安装脚本。

当前稀疏检出范围：

- UE：`Engine/Source/Runtime/Core`、`Engine/Source/Runtime/CoreUObject`，以及 `Engine/Source/Runtime/Engine/Classes/Engine/BlueprintGeneratedClass.h`、`Engine/Source/Runtime/Engine/Private/BlueprintGeneratedClass.cpp`。
- Godot：`core`、`modules/gdscript`。

可直接从 [UE Object.h](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Public/UObject/Object.h) 和 [Godot object.cpp](/E:/EngineReferences/Godot/core/object/object.cpp) 开始阅读。后续依照缺失的实际依赖扩展 sparse checkout，并保留本表中的版本依据。

## 考据结论

证据分别记录在 [UE 对象生命周期](ue-object-lifetime-source.md)、[Godot 对象生命周期](godot-object-lifetime-source.md)和[类型发布](type-publication-source.md)。以下只汇总对对象引用、根与分步回收有影响的差异；项目行为以 [runtime-model](../runtime-model.md#已实现的分步回收) 为准。

| 问题 | 已核实的机制 | 对本项目的意义 |
| --- | --- | --- |
| 创建是否自动建立持有？ | UE `NewObject` 返回裸指针；登记到对象表不等于建立外部根。`TStrongObjectPtr` 另行登记持有，并与增量标记协调 | 创建交付已受保护的结果是我们有意加强的接口保证，不是照搬 UE；当前无 GC 的单线程实现也不存在已证实的创建—登记根竞态 |
| 对象字段与外部根是否相同？ | UE 通过属性引用信息及 `AddReferencedObjects` 遍历对象内引用；外部持有另有根机制 | 应区分根与图内强边，否则字段各自成为根会阻止不可达环回收。原生隐藏引用也必须被报告，不能只遍历编辑器可见字段 |
| 弱引用取得只检查代际是否足够？ | UE 常规 `Pin` 将有效性检查与取得强持有放入生命周期协调范围；Godot ObjectDB 用 validator 阻止旧 ID 误认复用槽位 | 复用现有身份/代际机制；检查成功到建立持有必须是一个受保护操作。首版单线程不因此需要引入 UE 的锁体系 |
| Godot 是否就是我们的一套类型模型？ | GDScript 创建原生 owner，再附接独立 `GDScriptInstance`，脚本字段保存在 `Vector<Variant>`；RefCounted 与普通 Object 的持有规则也不同 | 可以参考身份、注册入口和脚本接入；不能据此认定脚本字段已进入统一原生类型布局，也不能用这套 RC 路径满足我们的循环回收语义 |
| 类型身份存在是否意味着定义已经可用？ | UE 的描述、属性 Link、默认对象创建分阶段；Godot 原生注册按类进行，GDScript 将接口和函数体解析分阶段 | 身份解析与定义完成值得分离；“一组定义全部验证后对外发布”仍是本项目的选择，所读源码不能证明 UE/Godot 提供了同等事务 |
| 不移动对象是否省掉主要 GC 协议？ | UE 的可达性处理、销毁、身份失效和释放各有职责；其根变更仍参与增量标记 | 不移动省去搬迁及其修正成本，仍需完整引用信息、跨步进写入保护和死亡判定。当前证据不能给出本项目的性能提升比例 |

本轮没有发现必须推翻已确认对象语义的证据。需要避免的混淆是：**对象表不是根表，统一创建不等于统一类型定义，分阶段解析不等于成组原子发布。**这些区别允许接口简洁，同时保留直接字段访问与可信原生快路径；不要求业务操作全部经过动态反射。

下一步收敛三组接口即可：成组类型定义的解析与提交、创建/外部根/字段引用的接线、显式 GC 步进及观察取得的失败边界。先完成同一个 `根 → A ↔ B` 场景，再测量根处理、引用扫描和清理成本；没有证据要求现在移植完整 UObject、Godot ScriptInstance 或高级并发 collector。

## 主参考与补充参考

| 参考 | 本轮最值得读的内容 | 阅读入口 | 适用边界 |
| --- | --- | --- | --- |
| UE5 | 类型描述与实例的联系、统一创建、对象登记、外部持有与字段引用、弱句柄、引用遍历 | `Engine/Source/Runtime/CoreUObject`；按依赖补 `Core` 与 `Engine` 的蓝图生成类部分 | 阅读 UObject 机制；不据此引入 UHT、完整继承层次、包系统、默认对象或 UE 全部生命周期特例 |
| Godot | 手动注册、原生对象与脚本实例连接、动态成员访问、对象身份 | `core/object`、`core/variant`、`modules/gdscript` | Object/ScriptInstance 接入方式及 RefCounted 生命周期与项目并不完全相同；其循环引用策略不能替代我们已确认的 GC 语义 |
| V8 cppgc/Oilpan | 外部根、强弱字段、原生隐藏字段的追踪、标记与清理的职责 | `include/cppgc`；本页固定提交的内部目录为 `src/heap/cppgc-internal` | 可参考引用/根机制；不照搬弱引用自动清空、保守栈扫描、线程堆或可选压缩。Unified Heap 不等于共享同一类型体系 |
| AngelScript | 引用类型的原生行为注册、脚本对象、执行上下文、字节码执行与清理 | 官方仓库的库源码；围绕 type info、script object、context、GC 定位 | 是语言/VM 参考；嵌入现成 VM 不自动获得我们的共享引擎类型模型。当前只查与引用边界有关的部分 |
| Luau | 字节码与解释执行热路径、调用、调试、GC pacing、宿主按帧推进 | `VM`、`Compiler`；性能文档 | 执行器实现参考；不把其动态对象布局、语言类型规则或 userdata 接入方式直接作为我们的对象模型 |

官方资料：

- UE：[源码获取](https://www.unrealengine.com/ue-on-github)、[对象引用指针](https://dev.epicgames.com/documentation/unreal-engine/object-pointers-in-unreal-engine)、[FReferenceCollector](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FReferenceCollector)、[StaticAllocateObject](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/StaticAllocateObject)。
- Godot：[官方仓库](https://github.com/godotengine/godot)、[Object/ScriptInstance 源码入口](https://github.com/godotengine/godot/blob/master/core/object/object.cpp)、[RefCounted](https://docs.godotengine.org/en/stable/classes/class_refcounted.html)。
- cppgc：[官方说明与接口](https://chromium.googlesource.com/v8/v8.git/+/HEAD/include/cppgc/)，细节见[核查笔记](cppgc-runtime-reference.md)。
- AngelScript：[作者维护的官方仓库](https://github.com/anjo76/angelscript)，由[官网源码入口](https://www.angelcode.com/angelscript/wip.php)确认。
- Luau：[官方仓库](https://github.com/luau-lang/luau)、[解释器与 GC 调度说明](https://luau.org/performance/)。

## 获取方式建议

源码放在项目之外的独立参考目录，固定已核实的版本 tag/commit，记录来源与提交号。UE5 和 Godot 作为主要引擎参考，cppgc 按目录获取；AngelScript 与 Luau 围绕当前需要逐步展开。

可以采用浅克隆、部分克隆和稀疏检出：`--depth` 限制历史，`--filter=blob:none` 按需获取文件内容，sparse checkout 控制工作树中的目录；这些机制职责不同，稀疏检出本身不保证减少初始下载。具体命令在确认目标仓库的实际 ref 后生成。[Git clone](https://git-scm.com/docs/git-clone)、[sparse checkout](https://git-scm.com/docs/git-sparse-checkout)

首轮以源码阅读为目标，不需要运行 UE 的依赖下载或构建脚本。UE 官方访问要求关联 Epic/GitHub 账号并取得仓库访问权。Epic FAQ 允许学习实现知识后自行开发，包括竞争引擎；直接复制源码另受其许可约束，不能把阅读参考视为任意复用授权。[Epic 官方说明](https://www.unrealengine.com/ue-on-github)

## 每次阅读的产出

沿一条具体调用链核查：类型定义发布 → 实例创建 → 建立根 → 字段引用更新/遍历 → 观察取得 → 失效/销毁。每次只记录负责模块、不变量、错误处理、热路径成本，以及与本项目契约的差异；不将整个外部框架加入项目。

已核查创建与持有的边界、外部根与对象内引用的区别，以及原生隐藏字段的报告入口；本项目接口见[定型引用与外部根](../runtime-model.md#已实现的定型引用与外部根)。其余引擎或论文在出现明确缺口时再加入。

## 平凡操作与深层遍历的实现依据

本节记录已用于本项目修复与优化的机制及边界，不把外部结果当作本机收益。公开语义以[运行时契约](../runtime-model.md)为准；不引入并发、移动或分代收集器。cppgc 源码固定在 V8 提交 `3e6373bb9f915f75f8e5d1d0d64e4d5a7968d1fa`，不声称它是最新发布版。

| 本地机制 | 一手依据 | 必须保留的边界 |
| --- | --- | --- |
| 发布时派生析构计划，跳过无工作子树 | cppgc 的 [FinalizerTrait](https://chromium.googlesource.com/v8/v8.git/+/3e6373bb9f915f75f8e5d1d0d64e4d5a7968d1fa/include/cppgc/internal/finalizer-trait.h) 区分自定义 finalizer、非平凡析构和无回调；[GCInfo](https://chromium.googlesource.com/v8/v8.git/+/3e6373bb9f915f75f8e5d1d0d64e4d5a7968d1fa/include/cppgc/internal/gc-info.h) 分开保存 finalize 与 trace 元数据 | 本项目只省略已证明平凡的内建叶及描述型子树。任何显式原生回调均保留原地址、原顺序和恰好一次；不以“不含引用”代替析构判断。 |
| 活跃值根紧凑索引 | cppgc 的 [PersistentRegionBase::Iterate](https://chromium.googlesource.com/v8/v8.git/+/3e6373bb9f915f75f8e5d1d0d64e4d5a7968d1fa/src/heap/cppgc-internal/persistent-node.cc) 独立管理持久根、回收全空块 | 该实现仍逐槽检查 `IsUsed()`，不是 packed 活跃根表。本项目的活跃 `ValueRoot` 索引是针对重叠检查的独立方案，不移动值存储、不改变 GC 根扫描计量。 |
| 显式可复用遍历状态 | cppgc 的 [MarkingWorklists](https://chromium.googlesource.com/v8/v8.git/+/3e6373bb9f915f75f8e5d1d0d64e4d5a7968d1fa/src/heap/cppgc-internal/marking-worklists.h) 保存待处理工作 | 只借鉴显式工作项，不搬入并发分段队列。本地工作栈的重入、扩容、逆序异常清理和暂停游标隔离由本项目端到端验收证明，不由 cppgc 代为保证。 |
| 同质内建标量块初始化与复制 | WG21 [P0593R6](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0593r6.html) 及标准草案 [cstring.syn](https://eel.is/c++draft/cstring.syn) 中 `memcpy` 的隐式对象创建规则 | 分类只从内建 `float`/`uint32_t` 身份传播，要求无填充、同一标量叶；不把独立对象直接当作 `T[]`。初始化按字节偏移 `construct_at`，复制只涉及已校验的类型大小；自定义原生行为、引用、空记录和异质布局不采用整块快路。 |

[V8 官方文章《High-performance garbage collection for C++》](https://v8.dev/blog/high-performance-cpp-gc)说明无 finalizer 路径和清扫职责分离；其性能结果主要来自后台清扫，不能用于量化本项目的单线程优化，也不能用其对象间终结顺序放宽本项目的字段析构顺序。

### 前沿论文的适用边界

- [Monk，arXiv:2502.20522v1](https://arxiv.org/pdf/2502.20522v1)：利用 Linux 调度策略安排 ZGC 工作，研究应用与并发 GC 的 CPU 争用。它不解决本项目的历史根扫描或无意义析构；本轮不增加线程调度器。
- [LXR，arXiv:2210.17175v1](https://arxiv.org/html/2210.17175v1)：组合引用计数、Immix、选择性复制与并发追踪。可借鉴同时衡量吞吐、尾延迟和内存成本的评估方式，不据此切换当前对象身份、非移动布局或持有语义。
- [OppZGC，arXiv:2609.15558v1](https://arxiv.org/abs/2609.15558v1)：2026-09-14 提交的预印本，利用空闲核心与反馈约束 ZGC 堆增长。仅作为后续宿主调度的研究线索，不声称已正式发表或“最新最优”，不纳入此次单线程运行时实现。

### 本机验证与性能记录

`acceptance/runtime/main.cpp` 覆盖定位回调保护、回调内撤根、活跃值根索引、10,000 层含原生资源及强弱引用的布局、重入与逆序失败清理，以及标量快路不绕过自定义原生回调。原始访问违规和栈溢出复现均变为退出码 0；三个固定种子各 50,000 次操作在两个配置中合计 300,000 次随机交错检查通过。覆盖范围不是任意原生代码正确性的证明。

以下为同机、锁定工具链的 Release 临时复现，未建立通用基准工程。根场景每个值为五次样本的中位数，均计时 2,000 组 `u32` 初始化＋销毁，场景搭建在计时外，测量时对象和根都已清空：

| 历史根登记峰值 | 修复前（ms） | 修复后（ms） |
| ---: | ---: | ---: |
| 0 | 0.0657 | 0.1011 |
| 1,000 | 2.4457 | 0.1076 |
| 10,000 | 30.1683 | 0.0869 |
| 100,000 | 724.9277 | 0.1059 |

同一 4 MiB 二叉纯 `u32` 描述型布局：构造从记录的 11.0238 ms 降到 0.9360 ms，完整清扫步骤从 7.5859 ms 降到 0.0083 ms。这里前值是修复前留存的单次运行，后值是五个独立进程结果的中位数；后五次构造范围为 0.8646–7.7848 ms，清扫为 0.0071–0.0155 ms，保留离群值，不当作严格配对统计。显式栈的首版曾使构造变慢，最终标量块路径消除了该回退。

这些数据只说明指定微场景的成本变化，不承诺游戏帧率、整体吞吐或硬暂停上限。清扫时间包含释放与收集器管理，不是纯析构净耗时；空历史 GC 登记项及对象槽扫描仍未改变。部分无瓶颈对照也有时序波动，不能把本次所有指标都概括为加速。

## JIT 接入准备依据

下列官方在线文档用于确定机制和需要验证的问题，不能代替本机验收。最初研究阶段仅核对源码、工具链安装范围和文档；后续独立 Windows 平台实验已执行，实际范围见 [README](../../README.md#windows-jit-平台验收)，尚未开放的能力见[JIT 执行](../runtime-model.md#jit-执行)。

| 一手资料 | 本项目采用的判断与验证边界 |
| --- | --- |
| [LLVM ORC 设计](https://llvm.org/docs/ORCv2.html)，尤其是 How to remove code | ORC 提供编译、符号管理及 `ResourceTracker` 资源移除机制；活动调用、直接调用依赖、版本发布和旧代码保留规则仍须由本项目定义。资源可以移除，不等于当前就可安全移除。 |
| [Garbage Collection with LLVM](https://llvm.org/docs/GarbageCollection.html) | LLVM 不提供本项目收集器；编译器需与现有运行时协作处理根、安全点和必要屏障。保持非移动对象存储，不等于可以省略执行中的局部引用报告。 |
| [LLVM 的 CMake 接入](https://llvm.org/docs/CMake.html#embedding-llvm-in-your-project) | 嵌入 LLVM 依赖开发头文件、库及对应构建配置，不只是 clang 可执行程序。已补齐 SDK，并用隔离 DLL 验证开发/发布 CRT 兼容性；版本继续以现有锁文件为权威，不在本文再维护版本号。 |
| [Debugging JIT-ed Code](https://llvm.org/docs/DebuggingJITedCode.html) | 动态生成代码需要调试信息交接；文中展示的是 MCJIT/ELF 路径，不能据此声称 Windows ORC 调试已经接通，也不能据此断言不支持。应单独验证选定平台的调试和栈回溯路径。 |

后续锁定源码核查、工业实现与论文见[JIT 工业研究](jit-industrial-research.md)及[现行执行结构](../runtime-structure.md)。这些材料记录通用 debugger helper 的 COFF 支持边界、特定异常桥与完整 COFF 平台移除路径的限制；平台探针本身不证明源码调试或通用热重载；单入口兼容发布的范围以现行运行时协议为准。
