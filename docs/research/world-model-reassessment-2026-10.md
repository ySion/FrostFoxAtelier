# 游戏世界与对象模型：比较研究后的改良判断

实现同步：2026-10-10。SceneComponent、显式空间树、Actor/Component 有序生命周期及 Primitive 物理组成均已落地。本文保留行业研究，并更新第 1、5、7 节的项目结论；当前接口见 [World](../modules/world/README.md)、[物理](../modules/physics/README.md)与[项目场景](../modules/world/project-scenes.md)。

研究日期：2026-10-01。对照代码为 `70e08ded193fcf33a2bcf414cf03bc4b7e105569`，源码树与 main `8d774cf82b30d320a246246853e3359a4f6c83e9` 相同。本轮组织了五个 gpt-6.1-sol 研究席：UE、Unity、Godot/CryEngine、Frostbite/大厂分享、当前实现；各席完成了一轮针对其他席结论的质询。主持补充了 ECS 存储及并发论文。

行业资料来自[综合研究稿](world-object-paradigms-2026-10.md)及本次交叉核验。外部来源以原研究日期为界；下文第 6 节保留当时的讨论取舍，不作为当前实施排期。

## 1. 判断：值得改良，优先完善能力接入

**共同 Runtime 类型、ObjectId 和 Actor/Component 的方向已保留，能力接入已沿同一 World 扩展。** Actor 与 Component 共用有序 Begin/Tick/End；StaticMesh、Light 和 Primitive 分别接入真实渲染、灯光与物理消费者，旧 Counter/Cube 专用执行模型已退出。

本轮后续讨论明确了项目定位：**Actor/Component 是游戏世界与业务建模的主范式；ECS 可按具体系统的需要作为子范式使用。** 能力可以有行为与对象身份；批量执行或专用数据布局服务这些业务语义。本文的批处理建议不构成将全部 Actor/Component 改为纯数据 Entity/Fragment 的建议。

后续继续以真实游戏对象验证类型、实例化、编辑、保存与退出的完整链路。核心 Class 引用、通用对象图与跨 Actor 挂接仍有缺口；这些需求不要求同时改变强 owner 或引入通用并行调度器。

这是对现有实现和业务扩展成本的判断，不是宣称当前 Runtime 已在大规模游戏负载上优于其他布局，也不是用行业案例证明现有 GC 合同最优。

## 2. 行业公开机制：各自解决什么问题

研究中的“目前”指截至研究日可核验的公开资料。版本和年份写在来源旁；闭源引擎的历史演讲只证明当时所述系统。我们没有获得 Frostbite、当前商业 CryEngine 或其他大厂完整源码。

| 引擎与核验范围 | 世界和对象的组织 | 值得借鉴的机制 | 不能直接套用的合同 |
| --- | --- | --- | --- |
| UE：主要固定 5.6 官方文档，Destroy API 为 5.5 | UObject 类型基础；Actor/Component 组成；SceneComponent 表达空间；Mass 另有实体、fragment 和 processor | 注册、执行、呈现与资源回收分别处理；多空间部件；按负载批处理和表示分级 | Outer、网络 Owner、组成和 GC 强边含义不同；Mass 与 UObject 的双模型不等于本项目的共同业务身份 |
| Unity：Unity 6 资料，Entities 1.3.15；World/cleanup/transform/ECB 再核验 1.4.8 | GameObject/Component 与 Entity/World 并存；作者内容可 baking 为运行数据 | 内容转换、chunk 批处理、启用位、结构命令缓冲及异步内容发布 | Entity 身份受 World 管理；cleanup 可让 destroy 后实体继续存在，和本项目立即失效合同不同 |
| CryEngine：公共 V 文档与公共 5.7.1 发行证据 | Entity 组成对象组件；反射连接代码、Sandbox、Schematyc；空间还包含 component/slot 等表达 | 类型声明接作者工具；按事件掩码订阅；多个空间表现部件 | 公开资料不足以证明通用依赖求解或并行访问安全；RemoveEntity 的 remove 不是保身份的 detach |
| Godot：4.7 文档，归档核验 4.7.2-stable | Object 基础；Node 场景树；RefCounted/Resource 使用不同寿命机制；Servers 有低层 RID | 场景组合与保存范围分开；处理资格与入树分开；高层对象之外允许专用批量消费者 | Node parent、保存 owner、空间 parent 不能合成一个关系；RID 不保 Resource；引用计数环和本项目追踪 GC 不同 |
| Frostbite：公开 2011、2012、2017、2018 技术分享 | 可证实作者、持久、运行 schema 的分工，以及剔除、地形、渲染图的专用组织 | 依消费者设计数据表示、内容转换、使用区间和资源驻留 | 局部数组、handle、FrameGraph 不能证明整个引擎使用哪种 gameplay GC 或统一对象模型 |

UE 的对象组件和 Mass 机制见 [Components](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-in-unreal-engine?application_version=5.6)、[MassEntity](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine?application_version=5.6)、[MassGameplay](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine?application_version=5.6)。Unity 的并存取舍还有开发者实例：V Rising 将 gameplay 放在 ECS，动画、粒子和 UI 使用 GameObjects；这是该产品的选择，不是 Unity 全部产品的强制架构。[Unity 官方圆桌问答，2024](https://unity.com/blog/engine-platform/dots-webinar-roundtable-q-a)

CryEngine 依据 [Components](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756813/pages/26871489)、[IEntityComponent](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/28704770/pages/29797021)、[IEntitySystem](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/28704770/pages/29797278)；公开发行与源码访问边界见 [5.7.1 公告](https://www.cryengine.com/news/view/hotfix-today-cryengine-5-7-1-lts-is-here)、[官方仓库说明](https://github.com/CRYTEK/CRYENGINE_ReadMe)。Godot 依据 [Node](https://docs.godotengine.org/en/4.7/classes/class_node.html)、[RefCounted](https://docs.godotengine.org/en/4.7/classes/class_refcounted.html)、[Servers](https://docs.godotengine.org/en/4.7/tutorials/performance/using_servers.html)，版本依据[官方归档](https://godotengine.org/download/archive/)。

这些案例支持分别回答以下问题：谁拥有业务身份；能力怎样组成；什么引用保活；谁参与本帧计算；什么数据适合连续访问；何时加载资源。它们不能推出一条所有引擎都必须遵循的架构升级顺序。

## 3. 会影响本项目决策的反例

### 对象有效、参与世界和资源驻留不能互相代替

UE EndPlay 覆盖多种退出情形，文档还给出快速重载时恢复同一 Actor 的情形；注册组件则建立实际场景参与。这能支持本项目区分退出和对象回收，但不能证明双方销毁细节相同。UE 的 Destroy API 说明 tick 末延迟销毁；本项目的显式 destroy 是立即使 ObjectId 失效的更强合同。[Actor Lifecycle](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-actor-lifecycle?application_version=5.6)、[AActor::Destroy](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/GameFramework/AActor/Destroy?application_version=5.5)

Unity Entities cleanup 会保留待清理实体，直到 cleanup 组件移除。该机制适合让系统收束外部资源，但不应该直接变成本项目“已 destroy 的 Actor 继续有效”。如需异步清理，可以由资源系统保有完成清理所需的记录；它不必继续保留可调用的业务身份。[Cleanup 1.4.8](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/components-cleanup-introducing.html)

Godot remove_child 不释放节点，保存 owner 在不再满足祖先条件时却可能被清空；PackedScene 只保存符合归属规则的节点。这说明脱离结构之后哪些关系保留，必须由本项目自己写清；组成强边也不能自动规定存档范围。[Node](https://docs.godotengine.org/en/4.7/classes/class_node.html)、[PackedScene](https://docs.godotengine.org/en/4.7/classes/class_packedscene.html)

### 统一业务身份允许内部资源句柄和多种表示

Mass 可让同一个模拟主体切换高精度 Actor、低精度 Actor、实例网格或无表现。这证明“表现对象身份”与“模拟主体身份”可以不同；本项目可借其表示策略，由现有 ObjectId 标识业务主体，再映射零个或多个 GPU instance、physics body 或资源句柄。[MassGameplay](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine?application_version=5.6)

真正需要避免的是第二个体系又独立决定同一业务对象的类型、存亡和字段值。GPU 槽位、资产内容 ID、物理句柄都有各自用途，不应为了追求“只有一种 ID”把它们塞进 Actor 生命周期。

推荐的可解释链路是：负责模拟的阶段写权威状态，在确定的提交点生成只读呈现数据；renderer 使用自己的句柄消费这些值。若物理系统生产位置，就明确它在哪个阶段拥有写权、如何提交到共同 Transform。当前 World 的编辑写要求停止状态，因此运行中写回还需明确模拟写入合同，不能假装已有编辑入口可以直接复用。该合同可以利用已有受保护的原生/语言直接访问，不预设所有数值写入都要经过动态反射或新增 setter。两个可修改表示若没有生产者和发布规则，就会争夺同一字段的权威。

Unity LocalToWorld 在模拟期间可能尚未更新，也可能带有图形平滑偏移。这是实际反例：缓存名叫“世界矩阵”，不保证它就是任意时刻的精确模拟状态。[Transform 1.4.8](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/transforms-concepts.html)

### 数据导向和并行各有自己的前提

Unity archetype/chunk 给同类组件提供密集布局，增删组成会产生迁移；enableable 则服务于高频参与变化。ECB 延后结构命令，但仍需回放和同步。可以借这些机制解决具体成本，不能把它们视为免费的事务、连续存储和并行保证。[Archetypes](https://docs.unity3d.com/Packages/com.unity.entities@1.3/manual/concepts-archetypes.html)、[Enableable](https://docs.unity3d.com/Packages/com.unity.entities@1.3/manual/components-enableable-intro.html)、[ECB 1.4.8](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/systems-entity-command-buffers.html)

ECS 也有不同存储选择。EnTT 使用稀疏集，并提供可选稳定地址；保留删除洞的策略与部分 group 优化不兼容。Flecs 关系组合会增加 table 数和查询、索引维护成本，其清理策略也不规定唯一清理顺序。因此不能用“ECS”一词替代具体布局、访问和终结合同。[EnTT 官方说明：Storage、Pointer stability、Multithreading](https://github.com/skypjack/entt/wiki/Entity-Component-System)、[Flecs Relationships：Fragmentation](https://www.flecs.dev/flecs/Relationships.html)、[Flecs Cleanup order](https://www.flecs.dev/flecs/ComponentTraits.html)

## 4. GDC、大厂讲义与论文带来的补充

| 一手资料及实际读取范围 | 能支持的判断 | 对 FrostFox 的约束 |
| --- | --- | --- |
| Frostbite / Chabant，GDC 2018，完整 PPTX，重点第 27–39、59–61、83–98 页 | 作者、存储与运行 schema 可以不同，live edit 需要相应转换 | 一套运行类型不要求所有内容表示逐字段相同；转换和可逆性须显式定义 |
| Frostbite / Collin，GDC 2011，剔除讲义第 10–20、27 页；Widmark，2012，地形讲义 | 密集剔除可用数组和分块工作；地形 LOD、驻留仍使用空间树 | 为消费者选择结构；局部优化不能决定整个对象模型 |
| Frostbite / O’Donnell，GDC 2017，FrameGraph 作者幻灯片 | 声明资源使用区间才能管理别名、同步和异步执行 | 添加任务并不自动缩短关键路径，增加重叠还可能增加驻留 |
| Guerrilla / Fahner，GDC 2023，HFW packaging 完整 76 页讲义，重点第 21–22、35–60、73 页 | 完整内容依赖图可按 root 集合打包、去重并重写链接 | 资产图中的 root 不是 gameplay GC 根；千万级内容对象不等于同时运行的千万 Actor |
| Bungie / Butcher，GDC 2015，Destiny 核心架构完整 PDF，重点第 6–7、27、42–46 页 | 强大的组件系统仍可能难用，内容和脚本迭代成本会妨碍生产 | 新注册机制必须通过真实能力制作来验收，不能只证明底层接口存在 |
| Naughty Dog / Gyrling，GDC 2015，完整 94 页讲义，重点第 5–38、84–92 页 | 对象更新可以任务化，fiber 与跨帧临时数据仍有明确约束 | 对象组件模型不排除并行；共同 ObjectId/GC 也不自动允许并行访问 |

原始资料：[Frostbite Three Data Schemas](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc18-tools-tutorial-day-a-tale-of-three-data-schemas.pptx)、[Culling the Battlefield 作者讲义镜像](https://www.gamedevs.org/uploads/culling-the-battlefield-battlefield3.pdf)、[Battlefield 3 Terrain](https://media.gdcvault.com/gdc2012/slides/Programming%20Track/Widmark_Mattias_Terrain_in_Battlefield3a.pdf)、[EA/DICE 上传的 FrameGraph 幻灯片](https://www.slideshare.net/slideshow/framegraph-extensible-rendering-architecture-in-frostbite/72795495)、[Horizon Forbidden West Packaging](https://d3d3g8mu99pzk9.cloudfront.net/JanJaap/Space%20Efficient%20Packaging%20for%20Horizon%20Forbidden%20West.pptx)、[Destiny Core Architecture](https://media.gdcvault.com/gdc2015/presentations/Butcher_DestinyEngine_GDC2015_final.pdf)、[Naughty Dog Fibers](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf)。这里不将演讲的局部性能数字外推到本项目。

论文进一步帮助划清语义与实现：

- **OOPSLA 2025，Core ECS。** 论文形式化查询、改动和调度，并对满足其安全条件的程序证明调度确定性。它给未来语言和执行模型提供研究方向；该证明不自动覆盖本项目原生回调、任意外部副作用和 GC/借用合同。实际采用仍需把这些行为纳入模型。[作者全文，§3–5](https://arxiv.org/html/2508.15264v2)
- **SIGGRAPH 2023，Madrona。** 大量独立学习环境通过 GPU 批执行摊薄开销，执行图在节点间提供安全的数据整理时点。这是多环境高吞吐负载，不能用其加速数字预测单个复杂编辑器世界。值得借鉴的是明确数据和执行范围，而不是立即迁移 GPU。[完整论文，§5–6](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- **SAC 2026，The Essence of Entity Component System。** 形式化 archetype 转换和访问条件，并用 Scala 塔防实现比较布局；实验的实体规模、程序和运行环境有限，不能证明任意 C++ 对象模型都落后。本文只将它作为结构变化与读写冲突需要显式规则的补证。[作者全文，§4–7](https://boyang.cs.uwm.edu/publication/sac2026_ECS.pdf)
- **GAS 2025，ECS mapping study。** 该研究筛选出 25 篇论文，并指出现有学术结果偏向积极发现。它支持谨慎解释“ECS 更快”等概括，不作为任何特定引擎实现的证据。[研究全文，研究选择与有效性威胁](https://assets.ptidej.net/Publications/Documents/GAS25.doc.pdf)

## 5. 已落地的项目合同与剩余缺口

`TypeId` 是所属 Runtime 内的类型身份；原生反射和语言定义生产同一 TypeDefinition，实例分别使用 ObjectId。World 用已有类型链做创建、兼容性检查与行为查找，类型身份不充当执行优先级或存档键。[Runtime](../../src/runtime/runtime.hpp)、[World 类型](../../src/world/objects.cpp)

### 生命周期与执行

Actor 组成使用有序强引用序列。`bind_behavior` 为 Actor 或 Component 绑定可选 Begin/Tick/End，实际类型沿基类链选择最近的完整登记。Begin/Tick 按 Actor 成员顺序、每组 Actor 在前而组件按组成顺序；End 按当前成员和组成逆序，只结束成功 Begin 的对象。enabled 控制 Tick 和任务取消，Begin/End 按运行周期配对。

正常停止、异常停机和关闭共用取消任务、逆序 End 与退出通知；重入被拒绝，重复退出不重新通知。LifecycleObserver 仍描述成员与参与变化。执行合同不依赖同类型池的枚举顺序；真正的合批和并行需要另外满足顺序与访问约束。[生命周期](../modules/world/README.md#生命周期与执行顺序)

### 空间、呈现与物理

SceneComponent 持有局部 TRS 和空间父引用，Actor 指定显式根；当前空间树限于同一 Actor。StaticMeshComponent 通过 PrimitiveComponent 继承空间能力，网格与碰撞配置同属一个对象；CollisionComponent 表达独立空间碰撞。方块是网格资源，纯 pivot 使用 SceneComponent，不再强制创建单独 Transform 对象。

换父的 keep-world 操作完整验证可表示的 TRS，失败保持原状态。物理以组件世界位姿创建 body，求解后父先子后回写；同 Actor 可有多个独立 body，挂接不自动产生焊接。Mesh、Light 和 Physics 各自维护消费者所需的派生数据，不改变共同对象身份。[空间合同](../modules/world/README.md#空间树与网格)、[物理边界](../modules/physics/README.md)

### 所有权与持久化

共同 Runtime 负责类型、对象身份、字段和 GC；World 负责成员、组成和运行参与。组件的强 owner 保持既定保组语义；detach 结束参与并保留对象图，destroy 明确终结。若以后真实轨迹证明少量长期持有的组件导致过量 CPU 数据驻留，再共同审视 owner 强度及引用消费者；本轮没有相关测量。

Scene v2 保存真实类型名、组成、空间关系、资源来源及通用标量/枚举/内联 record，物理配置使用同一 `properties.body`。公开业务引用和容器仍未接入一般归档；核心 Class 引用驱动的默认组件图、跨 Actor 挂接、材质槽实例覆盖仍需完善。现状与候选见 [Class/Actor 评估](ue-class-actor-attachment-2026-10.md)。

### 补充：UE 组件无序与 Tick 调度

UE 的“组件无序”应理解为所属组件集合没有可依赖的枚举顺序。5.5 官方 GetComponents 无参数重载返回 `const TSet<UActorComponent*>&`；填充数组的重载也未承诺添加顺序。它不表示生命周期或执行完全无序。[GetComponents](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/GameFramework/AActor/GetComponents?application_version=5.5)、[TSet 容器合同](https://dev.epicgames.com/documentation/unreal-engine/tset?application_version=4.27)

普通 Actor/Component 更新的公共模型是分别注册 TickFunction 到 Level，再由 TickTaskManager 为 World 的一帧组织阶段与先决条件。不能把它概括为默认按组件类型建立连续池，也不能把组成关系当作 Actor::Tick 每帧遍历所有组件。标准组件设置接口另有“在 Actor 后执行”的约定，说明独立登记仍可有执行依赖；本轮没有核验其函数体中的具体实现方式，也不把它外推到全部自定义 Tick。[Actor Ticking 5.6](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine?application_version=5.6)、[SetupActorComponentTickFunction 5.5](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/Components/UActorComponent/SetupActorComponentTickFunction?application_version=5.5)、[TickTaskManager](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FTickTaskManagerInterface)、[RegisterTickFunction](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FTickFunction/RegisterTickFunction?lang=en-US)

本次默认 TickFunction API 页标为 5.8，包含合批、间隔、线程资格和先决条件。Tick 合批不等于组件载荷池化，所有 Tick 也不自动获得并行安全。Actor Ticking 概述保留了旧式逐 Actor 驱动的历史措辞，本文依据注册、目标和管理器接口交叉核对，不用该句概述推导全部内部路径。[FTickFunction](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FTickFunction?lang=en-US)

FrostFox 已明确把 Actor 成员和组件组成顺序用于默认 Begin/Tick 与反向 End；SceneComponent 空间树独立决定变换关系，物理按空间依赖回写。未来合批不能无声重排已承诺的回调顺序；跨系统执行依赖需要显式表达，不因 UE 使用 TSet 就改变现有组成容器。

## 6. 原研究阶段的分歧与取舍

| 被挑战的建议 | 质询结果 | 本轮判断 |
| --- | --- | --- |
| 一套 ObjectId 是否排斥资产/GPU/物理 handle？ | 这些句柄管理不同内容或资源；问题是另造同一业务实例的独立权威 | 保留统一业务身份，允许有明确寿命和映射的内部句柄 |
| 注册处理器是否已经完成扩展？ | 本地席指出 Session/应用仍有专用逻辑；AAA 席以 Destiny 说明作者体验不能由注册表代替 | 以一个完整能力交付为终点，而非“类型注册成功” |
| 批次接口是否应该立即附带效应图？ | Unity 席要求避免固化 N 次逐对象调度；本地席指出当前串行合同已经成立 | 采用批次消费形态，当前明确顺序；真实并行出现后再做访问分析 |
| 资源可卸载是否意味着强 owner 没有问题？ | UE 席指出兄弟 CPU 数据仍驻留；AAA 席指出存活图和资源图根的用途不同 | 保留合同，按根来源、保留字节和持续时间决定是否重审 |
| 应不应该先通用化全部依赖和空间关系？ | 研究基线只有一项显式依赖；Godot/Cry 席确认外部 parent/owner/删除合同不能机械套用 | 优先补真实能力接线；空间与依赖按新消费者分别演进 |
| 所有寿命和可见性问题能否等性能数据？ | AAA 席反对：同帧字段可见、结构提交、destroy/close 写入资格是现在就必须明确的语义 | 语义先明确；版本、双缓冲、栅栏等设施由真实异步消费者触发 |

这轮没有找到推翻共同 Runtime、detach 保身份或显式 destroy 的必要证据。也没有证据证明当前按类型索引已经足够应付大量同构模拟。两种判断可以同时成立。

## 7. 后续候选

基础空间、生命周期及普通物理组成已贯通正式类型、参数编辑、场景保存、模拟和退出；当前验收见各模块文档。下一增量应由真实游戏需求选择，例如核心 Class 引用实例化完整组件图，或独立 Actor 在挂点与世界空间之间切换。

这些能力需要同步对象引用、加载修复与失败边界，不能只放宽一个类型或 parent 检查。Compound、材质槽覆盖、批执行与并行各有自己的需求和成本，不作为当前模型必须一次完成的清单。性能结论仍需实际负载测量。

原研究过程材料位于工作树忽略的 `out/research-world-2026-10/`，只供来源与讨论追溯。
