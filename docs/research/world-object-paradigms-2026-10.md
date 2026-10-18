# 游戏世界与对象模型：引擎及公开技术资料比较

资料状态：保留 2026-10-01 的行业来源与研究基线；项目当前合同见 [World 模块](../modules/world/README.md)（改良判断第 5 节描述的是已被 [Entity 模型](entity-component-model-evaluation-2026-10-12.md)取代的旧状态），2026-10-17 的硬闸复核见[工业引擎对照](world-hard-gates-industrial-comparison-20261017.md)。下文对旧代码的比较不作为当前能力清单。

研究日期：2026-10-01。六个研究席均使用 gpt-6.1-sol，分别调查 UE、Unity、CryEngine、Godot、Frostbite 和跨公司技术分享，并完成一轮交叉质询。本文是汇总后的研究入口；过程材料保存在本工作树忽略的 `out/world-paradigms-research/`。FrostFox 对照基线为 `70e08ded193fcf33a2bcf414cf03bc4b7e105569`，源码树与已合并 main `8d774cf` 一致；当前合同见 [World 模块文档](../modules/world/README.md)。

**公开证据支持的主要判断是：游戏世界模型由几种可以分别选择的机制组成。** 对象类型与身份、能力组合、空间关系、生命周期、内容定义、数据布局、执行依赖和资源驻留各自回答不同问题。UE 与 Unity 同时提供对象组件和批量实体机制；Godot 在场景节点之外提供服务器接口；寒霜公开分享把作者数据、持久数据和运行数据分别设计。这里的“共存”是样本内可观察的事实，不能据此给所有商业引擎规定唯一架构，也不能把某个子系统的优化推成整个引擎的对象模型。[UE Mass](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine)、[Unity Baking](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/baking-overview.html)、[Godot Servers](https://docs.godotengine.org/en/4.7/tutorials/performance/using_servers.html)、[Frostbite 三种 schema](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc18-tools-tutorial-day-a-tale-of-three-data-schemas.pptx)。

“范式”在本文具体指这些选择及其契约。Component 可以是有身份和方法的对象，也可以是一列值；World 可以管理可离开的对象参与，也可以直接拥有全部实体存储；archetype 可以指运行组件组合，也可能只是编辑器参数模板。仅凭术语相同，无法判断两个引擎是否采用同一机制。

**先把比较的问题说清楚。** 下面各行可以协作，但不宜互相代替。

| 问题 | 需要确定的事实 | 容易混淆的概念 |
| --- | --- | --- |
| 类型与身份 | 什么定义类型，怎样识别同一运行实例，旧句柄如何失效 | 类型继承、组件组合、内容模板 |
| 能力组合 | 能力是否有独立身份，能否多实例、动态增删，依赖如何提交 | 正式基类、角色标签、archetype |
| 所有权与存活 | 谁负责终结，什么引用保活，离场后还允许什么 | owner、parent、GC 强边 |
| 空间 | 局部/世界变换如何传播，是否允许多个空间部件 | Actor 组成、销毁级联、保存范围 |
| 世界参与 | 属于哪个上下文，何时开始/停止模拟和呈现 | 对象存在、enabled、visible |
| 数据布局 | 哪些字段一起扫描，地址是否稳定，怎样搬移或派生 | 身份稳定、字段连续、权威状态 |
| 执行 | 谁更新，频率、阶段、读写依赖和结构变更时点 | event priority、Tick、并行安全 |
| 内容与驻留 | 作者内容怎样成为运行数据，何时加载/释放资源 | 存档身份、运行句柄、资源句柄 |

这些区分来自下文的具体案例。它们支持共享语义和明确接线，也要求为额外关系承担一致性成本。

**五个引擎的公开模型可以这样定位。** 表中是所读版本和材料的覆盖范围。

| 引擎与资料范围 | 作者及业务对象模型 | 处理数据与执行 | 关键取舍 |
| --- | --- | --- | --- |
| UE：官方页面本次标为 5.8；未审计许可源码 | UObject 类型基础，Actor/Component；空间由 SceneComponent 与根组件表达 | Tick 阶段及依赖，Mass fragment/archetype/chunk，Actor/实例网格表现映射 | 丰富对象与工具能力，另有实体身份、表示映射和同步成本 |
| Unity：6.0 手册、Entities 1.4.8 | GameObject/Component/Transform；GameObject 作者内容可 baking 成 Entity Scene | Entity 的版本身份，chunk 内组件列，system/query/job/ECB | 连续工作集及离线精简数据，付出结构迁移与作者/运行映射成本 |
| CryEngine：公开 V 文档；公开发行线 5.7.1，Hunt 2025 文章提到内部 5.11 | IEntity 拥有对象组件，组件反射接编辑器/Schematyc；内容 class、Prefab、参数 archetype | 事件掩码及优先级；历史批量分配和实体池 | 组合、反射与订阅接线清楚；公开资料无法证明当前全局布局和完整卸载合同 |
| Godot：4.7 文档、4.7.2-stable 源码 | Object 基础，Node 场景树，RefCounted/Resource 另有引用计数寿命 | 节点处理模式/线程组，Servers/RID，节点之外的实例工作集 | 树适合作者组合；结构、保存、空间、处理和寿命仍有分别的规则 |
| Frostbite：2007—2024 公开分享，核心对象系统闭源 | 公开证据显示编辑、存储、运行 schema 与内容转换 | 剔除密集数组/handle、地形空间树/payload、渲染快照和 FrameGraph | 按消费者组织数据及驻留；当前通用对象身份、GC 与 World 生命周期无法完整核实 |

UE 的 5.8 是所访问页面的版本标签；正文仍含旧版本与旧术语，不能据标签推定所有机制的现行成熟度。Unity 的版本是一个明确的已发布组合样本。CryEngine 的公共文档没有逐页维护时间，5.11 产品文章也不提供完整对象实现。Godot 的文档分支可以继续修改，因此源码论断固定在 tag。历史演讲只证明当年的实现。[Unity 包版本](https://docs.unity.com/en-us/engine/6000.0/manual/packages-list/packages-all/pack-safe/com-unity-entities)、[CryEngine 公共发行](https://www.cryengine.com/news/view/hotfix-today-cryengine-5-7-1-lts-is-here)、[Hunt 技术文章](https://www.huntshowdown.com/news/behind-the-storm-technical-design-of-thundershower-in-hunt-showdown)、[Godot 归档](https://godotengine.org/download/archive/)。

**UE 的可借鉴处在于关系分工和丰富表现，而其混合机制也有实际成本。** UObject 提供反射、序列化和 GC 基础；反射属性、函数仍是普通 C++ 成员，类可以保留原生专用成员，UStruct 是值类型。这说明共享类型语义与直接原生执行可以同时存在。[Objects](https://dev.epicgames.com/documentation/en-us/unreal-engine/objects-in-unreal-engine)。

普通 ActorComponent 没有空间变换，SceneComponent 才有空间挂接；Actor 的变换来自指定根组件。组件所属、空间父节点、世界注册和渲染 SceneProxy 各有职责。注册/注销建立或撤销场景参与，空间关系可以落在一个对象的多个部件上。这提供了比“每个业务对象一个位置”更丰富的表达，同时增加依赖、重挂接及资源同步规则。[Components](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-in-unreal-engine)。

Actor 的 EndPlay 可以由销毁、关卡切换、PIE 退出或流送卸载触发；资源准备与最终内存释放随后进入 GC 阶段。官方还描述快速重载且未强制 GC 时，可能恢复同一个 Actor 及其局部变量。生命周期概览与最新指针说明存在术语差异，因此这些事实只用于区分参与结束与物理回收，不据此断言所有强引用的死亡行为。[Actor Lifecycle](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-actor-lifecycle)。

World Partition 有未加载 Actor 的描述信息和按来源流送；Actor 之间的引用可能使其共同加载。OFPA 改变编辑文件粒度，cook 后组织又不同。持久 GUID、包、未加载描述和当前对象指针解决不同问题。[World Partition](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine)、[FActorDesc](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FActorDesc)、[OFPA](https://dev.epicgames.com/documentation/en-us/unreal-engine/one-file-per-actor-in-unreal-engine)。

Tick 能表达阶段、频率和 prerequisite；Mass 则按组件片段批处理，并可在 Actor、实例网格及无表现之间选择。Mass 有自己的实体身份，这不是“一套 ObjectId 已覆盖所有物理布局”的现成证明。模拟 LOD、表现 LOD 及 Actor 映射都需要维护。StateTree 提供行为状态组织，也不替代整个对象类型系统。[Actor Ticking](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine)、[MassGameplay](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine)、[StateTree](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-state-tree-in-unreal-engine)。

**Unity 展示了作者模型和运行数据可以主动采用不同表达。** GameObject 是组件容器，并默认包含 Transform；activeSelf 与 activeInHierarchy 区分自身和继承后的激活。UnityEngine.Object 又有托管壳与原生对应体：托管引用存在不保证原生对象可用，Destroy 的实际处理和 GameObject 子孙删除有规定。这与普通 tracing GC 对象的寿命不同。[GameObject](https://docs.unity3d.com/6000.0/Documentation/Manual/class-GameObject.html)、[Object](https://docs.unity3d.com/6000.0/Documentation/Manual/class-Object.html)、[Destroy](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Object.Destroy.html)。

Entities 的 Entity 由索引和代际版本辨别，身份在自己的 World 内有意义。同组成数据按组件列进入 16 KiB chunk；增删组件会搬移数据，删除洞由末尾实例补齐。因此句柄稳定、地址稳定和连续扫描是可以分别选择的性质。结构变更形成同步点，ECB 把提交推迟到规定阶段；它不会消除搬运成本，其临时创建身份也不能冒充正式实体。Enableable component 则让高频参与切换避免组成迁移，仍需遵守读写访问和 job 同步。[Entity](https://docs.unity3d.com/Packages/com.unity.entities@1.4/api/Unity.Entities.Entity.html)、[World](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/concepts-worlds.html)、[Archetypes](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/concepts-archetypes.html)、[Structural changes](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/concepts-structural-changes.html)、[ECB](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/systems-entity-command-buffers.html)、[Enableable components](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/components-enableable-intro.html)。

Baking 是作者 GameObject 到运行实体场景的不可逆转换。它可以提前计算、去除运行不需要的数据，并生成额外实体；代价是依赖跟踪、映射、调试和构建维护。LinkedEntityGroup 表达整体实例化集合，承担的职责也不同于空间父子。managed component、UnityObjectRef、blob 各有不同访问、GC 或不可变约束，接入托管对象不会自动获得 Burst 的直接数据访问。[Baking](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/baking-overview.html)、[Prefabs](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/baking-prefabs.html)、[Managed components](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/components-managed.html)、[UnityObjectRef](https://docs.unity3d.com/Packages/com.unity.entities@1.4/api/Unity.Entities.UnityObjectRef-1.html)、[Blob assets](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/blob-assets-concept.html)。

第二种运行表达可以是内容的编译产物。需要检查的是作者配置、运行状态和回传编辑各自由谁写入，不能让两个表示都悄悄成为同一字段的权威。统一类型语义可以保留，作者资产与运行实例的身份未必相同；反过来，FrostFox 的语言与原生代码操作同一个业务实例，则有更强的运行身份要求。

**CryEngine 说明“实体组件”和“archetype”并不能单凭名字归为同一种 ECS。** 公开文档中的 IEntity 管运行实例；EntityId 使用索引与 salt，GUID 用于跨保存加载追踪，组件还有类型 GUID 与实例 GUID。它们的作用域与稳定条件不同。组件的 ReflectType 连接代码创建、Sandbox 添加和 Schematyc；同一描述服务多个消费者，是可观察的统一收益。[Entity](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756813/pages/26216196)、[Components](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756813/pages/26871489)。

GameSDK archetype 是类参数资产，Prefab 是多个编辑对象的实例资产，Schematyc 提供组合和状态行为；这和 Entities 的 archetype 组件集合有不同含义。实体拥有对象组件的接口，不证明外部引用可以在实体终结后保住有效 owner。默认实体空间、可选组件空间与 entity slot 又有各自用途。[Archetype Entity](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/36870432)、[Prefab](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/36866251)、[IEntity](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/28704770/pages/29797019)、[IEntityComponent](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/28704770/pages/29797021)。

事件 mask 决定谁接收，priority 决定先后；两者不表达所有读写冲突和工作完成依赖。官方执行说明又区分主逻辑和物理阶段。历史批量 XML 分配及 Crysis 2 实体池证明可以优化特定加载、驻留场景，但不足以确认当代全局 SoA、archetype chunk 或完整池身份重置合同。[Execution Order](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756813/pages/28184910)、[5.5 Preview](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/47316993/pages/44962769)、[AI Territories & Waves](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/25535416)。

**Godot 的场景树把常见作者操作做得集中，但它也分别定义保存、空间和寿命。** ObjectID 能查验旧身份；普通 Object/Node 引用不保活。RefCounted/Resource 使用引用计数，Node 采用显式释放和父子级联；引用环与 orphan 节点需要不同处理。[Object](https://docs.godotengine.org/en/4.7/classes/class_object.html)、[RefCounted](https://docs.godotengine.org/en/4.7/classes/class_refcounted.html)、[ObjectDB 固定源码](https://github.com/godotengine/godot/blob/4.7.2-stable/core/object/object.h#L845)。

parent 组织节点树，owner 指定祖先范围内的场景保存归属。remove_child 撤树参与而不释放，queue_free 延后终结；父节点删除会删除孩子。空间又有另外的限制：Node3D 使用直接 Node3D parent，普通 Node 可以截断空间继承，top_level 可关闭继承。process_mode、可见性与成员资格分别表达行为。[Node](https://docs.godotengine.org/en/4.7/classes/class_node.html)、[PackedScene](https://docs.godotengine.org/en/4.7/classes/class_packedscene.html)、[Node PREDELETE 源码](https://github.com/godotengine/godot/blob/4.7.2-stable/scene/main/node.cpp#L259)、[Node3D 源码](https://github.com/godotengine/godot/blob/4.7.2-stable/scene/3d/node_3d.cpp#L140)。

Servers/RID 允许在节点之外管理大量实例。RID 本身不替 Resource 增加引用，服务器工作集也不能被称为同一个 ObjectID 世界。节点线程组可以做子线程处理，活动树结构与跨组访问仍有限制。这个案例支持作者接口与专用处理表示分工，同时提醒身份、寿命及线程安全不能从树的外观推出。[Using Servers](https://docs.godotengine.org/en/4.7/tutorials/performance/using_servers.html)、[Thread-safe APIs](https://docs.godotengine.org/en/4.7/tutorials/performance/thread_safe_apis.html)。

**寒霜最有力的公开证据是数据转换、消费布局和资源预算。** 2018 年 Ludovic Chabant 的 GDC 分享以编辑、存储、运行三种 schema 描述同一逻辑内容：曲线从作者/存储 AoS 转成运行 SoA，变换采用不同表示。实时修改复用转换逻辑，承担额外源字段、重新计算和舍入一致性成本。它支持“权威内容生成消费者需要的表示”，没有证明全部资产与实例保持同身份，也未提供任意运行状态回传的可逆合同。[EA 原始 PPTX，2018](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc18-tools-tutorial-day-a-tale-of-three-data-schemas.pptx)。

2007 年渲染架构分享已将编辑器、离线管线和运行时分开；Cull 阶段复制实体所需的渲染数据，后续 Build/Render 使用快照和双缓冲，避免模拟同时改写消费数据。这是局部表现边界的证据，没有交代完整业务对象寿命。[作者 GDC 2007 幻灯片](https://www.slideshare.net/slideshow/frostbite-rendering-architecture-and-realtime-procedural-shading-texturing-techniques-presentation/916077)。

2011 年剔除分享使用密集数组、handle 和分块任务；2012 年地形分享使用空间四叉树及可驻留、卸载的 payload。不同负载采用不同物理组织。渲染快照可以避免模拟同时修改消费数据，却付出复制与可见延迟；地形卸载 payload 只证明资源驻留改变，不能决定业务对象死亡。[Culling the Battlefield，正式 PDF 入口](https://media.gdcvault.com/gdc2011/slides/Daniel_Collin_Programming_Culling_The_Battlefield.pdf.pdf)、[已读完整讲义镜像](https://www.gamedevs.org/uploads/culling-the-battlefield-battlefield3.pdf)、[Terrain in Battlefield 3，2012](https://media.gdcvault.com/gdc2012/slides/Programming%20Track/Widmark_Mattias_Terrain_in_Battlefield3a.pdf)。镜像承载的是作者讲义，本轮没有把镜像站观点作为技术证据。

FrameGraph 的读写声明、使用区间和资源别名是 GPU 工作与资源生命周期的合同；它并非业务对象 GC。EA 2023 地形资料与 2024 SIGGRAPH GI 材料说明局部系统持续演进，仍不足以审计当代全引擎实体生命周期、跨语言对象身份和通用 World 分区。[FrameGraph，2017，作者幻灯片](https://www.slideshare.net/slideshow/framegraph-extensible-rendering-architecture-in-frostbite/72795495)、[EA GDC 2023](https://www.ea.com/technology/news/frostbite-presents-at-gdc-2023)、[SIGGRAPH 2024 课程](https://advances.realtimerendering.com/s2024/)。

**商业游戏演讲提供了几种不同的驱动力。** 下表同时记录可读证据范围；仅有摘要的材料只承担摘要明确陈述的事实。

| 资料 | 实际压力与机制 | 可支持的判断与限制 |
| --- | --- | --- |
| Timothy Ford，Blizzard，GDC 2017，[Overwatch Gameplay Architecture and Netcode](https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and) | 玩法组合不断增加，ECS 与联网模拟 | 正式摘要明确组合与响应性动机；不能从摘要推出具体 chunk 布局、全量回滚和性能倍数 |
| Christian Gyrling，Naughty Dog，GDC 2015，[Parallelizing the Naughty Dog Engine Using Fibers](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf) | 将既有对象更新、动画等工作迁到 worker/fiber，随后处理帧关键路径 | 已读完整 94 页 slides；对象更新能直接任务化，成本含调用栈、同步、TLS 与流水线阶段 |
| Barry Genova，Bungie，GDC 2015，[Multithreading the Entire Destiny Engine](https://www.gdcvault.com/play/1022164/Multithreading-the-Entire-Destiny) | job graph/fiber，以及读写、创建、销毁安全时点验证 | 正式摘要；执行合法性独立于是否使用 ECS，内部句柄和锁策略未核实 |
| Mike Acton，Insomniac，Game Developer 2013，[Inner Product 作者专栏](https://media.gdcvault.com/GD_Mag_Archives/GDM_JuneJuly_2013.pdf) | 按输入/输出格式和硬件数据流决定转换 | 已读正文印刷页 40–43；支持数据导向思想，不能推定其所有 gameplay 都是 ECS |
| Noel Llopis，2009，[Data-Oriented Design](https://gamesfromwithin.com/data-oriented-design/) | 同类数据批处理、访问模式、离线格式和非指针引用 | 作者全文；保留敌人等对象心智同时分布数据是可讨论路线，属于经验论证而非受控 benchmark |
| Jan-Jaap Fahner，Guerrilla，[Streaming the World of Horizon Zero Dawn](https://www.guerrilla-games.com/read/Streaming-the-World-of-Horizon-Zero-Dawn) | 资产管线、低/高层 streaming、内存和调度 | 公司摘要；页面 2023 再发布时间不等于原演讲年，世界驻留不能直接等同对象所有权 |
| Jan-Jaap Fahner，Guerrilla，GDC 2023，[Space-Efficient Content Packaging for Horizon Forbidden West](https://www.guerrilla-games.com/read/space-efficient-content-packaging-for-horizon-forbidden-west) | 内容依赖图确定性打包，加载速度与磁盘重复空间权衡 | 已读完整 76 页正式 PPTX；root 寿命由高层管理，内容图重组、去重与引用重写，不能据此决定 Actor 销毁规则 |
| Vladimir Katchadourian、Vincent Leclercq，Ubisoft，GDC 2023，[Engine Optimization Powered by Big Data](https://www.gdcvault.com/play/1028767/Engine-Optimization-Powered-by-Big) | 聚合 profiling 数据、分析热点与优化优先级 | 正式摘要；报告的改善是分析效率，不能写成游戏帧时间提升 |

Naughty Dog 的材料尤其说明，数据导向、任务调度和减少关键路径是三个问题。布局改善没有自动表达读写权限，fiber 没有自动解除关键路径，流水线又要求明确跨帧数据寿命。Overwatch 则提醒，选择 ECS 的动机也可以来自玩法组合与联网需求。这个样本中没有一条可验证的“类对象→组件对象→ECS→更先进”的统一升级链。

Guerrilla 的完整讲义给出了更具体的内容模型：高层请求 root objects 并管理其寿命，streaming 按 root 的寿命管理普通内容；编译阶段按可达 root 集合分组、内容哈希去重、合并实例并重写引用。这要求已链接的内容可重组和搬移，作者文件边界没有直接决定加载粒度。shipping 和开发期路径曾产生内存差异，随后补增量版本，说明派生管线也有一致性维护成本。这是资产内容的可达性和生命周期策略，不是对 gameplay GC 的证明。[完整正式讲义，页 21–27、34–63、73](https://d3d3g8mu99pzk9.cloudfront.net/JanJaap/Space%20Efficient%20Packaging%20for%20Horizon%20Forbidden%20West.pptx)。

**即便只比较 ECS，也不能把存储策略视为唯一答案。** EnTT 的官方说明使用稀疏集，并支持特定组件的稳定地址存储；该策略会留下删除洞，而且与其 group 优化存在限制。Flecs 的 archetype 关系可以使组合和 table 数增加，查询、建表及清理都有成本；关系清理策略也不保证唯一回调顺序。这些是具体工具的合同，不是所有 ECS 的性能定律。[EnTT 官方 ECS 文档](https://github.com/skypjack/entt/wiki/Entity-Component-System)、[Flecs Relationships](https://www.flecs.dev/flecs/Relationships.html)、[Flecs Cleanup traits](https://www.flecs.dev/flecs/ComponentTraits.html)。

**交叉质询收敛了五个判断。**

1. 一套类型语义可以服务多种处理路径。UE 原生直调、UStruct 和批量处理支持这个方向；Mass 与 Godot RID 仍有独立身份，所以它们不能直接证明 FrostFox 的单运行身份目标已经覆盖全部场景。
2. 身份稳定不要求地址稳定。可迁移数据借助句柄解析能够维持实体身份，但必须限制借用和失效时点。反过来，稳定地址也有实际价值；它对原生接入、对象图和稀疏随机访问是否更合适，应由工作负载判断。
3. 世界参与、业务对象数据和资源驻留可以分工。它们的具体寿命没有统一行业答案。UE、Unity Entities、Godot 和 FrostFox 的终结、保活和再入场合同分别成立。
4. 内容编译可以形成不同的运行表示。可追溯映射、写入权威、版本及转换方向决定它是否一致；表示数量本身不决定维护质量。多个作者入口若服务不同粒度，也不能仅因数量多而判为分裂。
5. 组合能力、数据批处理和并行调度需要分别证明。类型查询只找出对象，启用位只选择参与者，事件顺序只确定回调顺序；连续数据、访问安全和工作完成依赖仍需自己的机制。

**对 FrostFox 的判断应落在实际机制上。** 当前共用 Runtime 类型、ObjectId、GC、字段与执行接收者，以及反射生成仍进入同一注册体系，有利于语言、原生代码和编辑器操作同一个业务实例。World 管参与与注册，状态由实际关系派生；空 Actor 与独立 Transform 也避免提前把所有业务对象空间化。这些是本项目已具备的合同，[模块文档](../modules/world/README.md)和实现是事实来源，外部引擎只是对照。

当前稳定 payload 和按类型索引尚不等于组件列连续。若真实热点是大量同构数值更新，可以比较三种增量：把同一对象的工作分批执行；在规定阶段提取紧凑工作集；由 Runtime 管理热点字段的特殊存储。它们分别承担调度、复制/写回、地址借用与迁移成本。当前渲染矩阵快照已有明确消费者；可扩展同一接线思想，但不能让新工作集成为可随意编辑的另一份真状态。

强 Component→owner 保整组是明确取舍：编辑历史、外部业务引用需要同身份组时，它的语义直接；一个局部组件引用也可能使 Actor、兄弟组件和所引用数据长时间驻留。GC 能处理无外部根的环，不能回收仍有合法强根的组。未来资源预算还取决于组件如何引用资产与服务，不能把撤 World 呈现注册等同于释放全部资源。UE 的强/软指针与共同加载案例只提供风险对照，[Object Pointers](https://dev.epicgames.com/documentation/en-us/unreal-engine/object-pointers-in-unreal-engine)不能替本项目决定是否改变强度。

未来空间关系应依据具体用途定义：一个 Actor 是否可有多个部件，能否跨 Actor 挂接，解绑保持局部还是世界变换，父终结是否级联，保存和整体实例化范围怎样选择。现有 owner 属于业务组成，适合作为其中一个事实；用它同时承担全部空间、寿命和持久范围，会使这些不同需求难以表达。Godot 的额外 owner/top_level 及 Unity 的 LinkedEntityGroup 正好提供反例。

资产和流送出现时，持久内容身份应映射到运行实例，未加载描述、业务状态与资源 payload 各有可访问范围。外部引用可以需要“仍能记住某个对象”而不要求“它的网格、物理、声音一直驻留”。close 后允许读哪些数据、异步结果能否发布、旧 World 回调是否有效、重新接入要恢复什么，都应继续由唯一对象和 World 操作链维护。

游戏运行并行化则需要真实读写及副作用边界。source/JIT 共接收者和已有任务取消提供了基础合同，却没有自动产生 job graph、冲突检查或任意语言函数的并行许可。应先用一个真实系统建立可观察的阶段与依赖，再判断通用调度是否有足够共同职责。

**后续值得用真实业务回答的四个问题是：** 稀疏交互对象与密集同构对象的访问成本分别在哪里；强引用保组与资源卸载怎样同时满足需求；多空间部件的关系怎样贯通编辑历史和终结；运行状态、作者内容和派生数据谁拥有写入权威。它们对应现有机制的真实能力边界，可以决定扩展什么。当前资料没有同负载的引擎横向性能比较，也没有证明必须新增 archetype、baking、第二运行身份或完整调度框架。

本次只产生研究文档。生产代码、构建和窗口验收沿用原任务状态。闭源引擎的公开片段、可读摘要和历史源码分别承担有限证据；演讲里报告的局部性能不能预测 FrostFox 的结果。
