# 游戏世界与对象模型：比较研究后的改良判断

实现状态：本轮已按用户确认迁移 SceneComponent、StaticMeshComponent、显式空间树与有序生命周期。当前 API、边界和验收入口以 [World 模块](../modules/world/README.md)为准；下文“当前实现”调查及旧符号定位保留为本次改动前的研究证据。

研究日期：2026-10-01。对照代码为 `70e08ded193fcf33a2bcf414cf03bc4b7e105569`，源码树与 main `8d774cf82b30d320a246246853e3359a4f6c83e9` 相同。本轮组织了五个 gpt-6.1-sol 研究席：UE、Unity、Godot/CryEngine、Frostbite/大厂分享、当前实现；各席完成了一轮针对其他席结论的质询。主持补充了 ECS 存储及并发论文。

工作树原有的[综合研究稿](world-object-paradigms-2026-10.md)保留原样，本轮复用来源并独立核验重要判断。本报告是这次重新评估的决策入口，当前已实现合同仍以 [World 模块文档](../modules/world/README.md)为准。以下明确区分公开机制、项目事实和设计建议；本轮没有修改生产代码、运行性能测量或重做构建验收。

## 1. 判断：值得改良，优先完善能力接入

**当前的共同类型、ObjectId、对象存储和生命周期方向值得保留。最先应该改良的是业务能力接入 World 的方式。** 现在已能注册和组成新 Component，但自动逐帧执行仍只识别 Counter，呈现仍只识别 Cube。类型可反射、对象能组成、对象已激活，并不自动意味着它有更新和呈现能力。

本轮后续讨论明确了项目定位：**Actor/Component 是游戏世界与业务建模的主范式；ECS 可按具体系统的需要作为子范式使用。** 能力可以有行为与对象身份；批量执行或专用数据布局服务这些业务语义。本文的批处理建议不构成将全部 Actor/Component 改为纯数据 Entity/Fragment 的建议。

建议下一次相关开发选择一个真实新能力，贯通正式类型、参与登记、算法、编辑与历史、停用和退出，并把 Counter 迁入同一执行机制。当前没有证据要求同时改变强 owner、全面改成 archetype ECS、建立通用关系语言或并行调度器。

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

## 5. 对当前代码的重新评估

### 现状对齐：TypeId 与生命周期入口

`TypeId` 是现有 Core 的类型身份句柄，在所属 Runtime 内标识一个已注册类型；它不是这轮研究新建的 ECS 标识、组件执行优先级或存档 ID。多个 Counter 对象共享 Counter 的 TypeId，各有自己的 ObjectId。原生反射和语言定义使用共同类型体系，World 只引用已存在的类型身份做创建、兼容性检查和查询。定义见 [runtime.hpp](../../src/runtime/runtime.hpp):45，World 使用见 [objects.cpp](../../src/world/objects.cpp):8。

当前真实行为需要与拟议接口分清：

- Actor 的 components 是有顺序的强引用序列，历史记录组成位置；它已经提供稳定的组成表达。
- 尚无每个 Component 通用的 Begin/Tick/End 方法合同。LifecycleObserver 是 World 的 attached/activated/deactivated/detached 通知；start/stop 遍历 Actor 成员通知，不会自动调用其全部组件的 Begin/End。stop 当前也不是逆序退场。
- refresh 从类型 bucket 收集启用 Counter 的 ObjectId，update 逐个调用 Counter 方法或其宿主/JIT适配，再推进已有协程。这里有按类型索引，没有同类型 payload 连续池，也没有一般 Actor.Tick 或 Component.Tick。
- 显式 destroy_actor 先按组成逆序终结非 Transform 组件，再终结 Transform，最后 Actor。这是终结清理顺序，不等于已存在反向 EndPlay。
- SceneComponent 方向已得到用户同意，代码尚未迁移；现有类型仍为 Actor/Component/Transform/Cube/Counter。

以上依据 [world.cpp](../../src/world/world.cpp):55、89，[execution.cpp](../../src/world/execution.cpp):47、66、173，[objects.cpp](../../src/world/objects.cpp):193。

用户随后提出可预测的正序 Begin/Tick、逆序 End。建议将它作为同一 Actor 内的默认业务生命周期合同：先完成整组构造与绑定，再按明确组成顺序 Begin；普通 Tick 使用明确的参与顺序；退出时按实际成功 Begin 的顺序逆向 End。动态插入、Begin 失败和中途移除须使用真实的已进入状态，而非机械倒序当前列表。End 的清理即使一项失败仍需继续，其错误政策与已提交变化分开说明。

这仍是待确认和实施的行为建议。Begin/End 究竟对应一次玩法生命周期还是每次激活周期，需要明确；它们不能自动等同构造/析构或任意 Tick 暂停。组成順序若被正式选作默认执行顺序，就成为可观察合同，后续同类型合批不得无声重排；跨 Actor 或跨系统依赖则应明确表达，不能靠列表位置猜测。

### 应保留的合同

共同 Runtime 承担类型、对象身份、字段和 GC；反射生成继续产出相同 TypeDefinition。Actor 组成、World 成员、运行启用和实际呈现各有权威事实。整批组成变更先验证，再通过 Core 引用批次提交；生命周期通知失败通过 Commit 单独报告。detach/close 结束参与，普通根保数据，destroy 明确终结。这些机制已有真实消费者，不应仅因其他引擎名称不同就替换。

代码依据：[类型注册与对象操作](../../src/world/objects.cpp)，[对象与公共合同](../../src/world/world.hpp)，[组成提交与历史恢复](../../src/world/world.cpp)，[执行与退出](../../src/world/execution.cpp)。本轮只读核对这些路径，复用此前验收事实。

### 最明确的扩展边界

| 当前事实 | 具体位置，行号相对本报告基线 | 对下一能力的影响 |
| --- | --- | --- |
| Bucket 缓存 cube/counter/transform 三种分类 | [detail.hpp](../../src/world/detail.hpp):48；[world.cpp](../../src/world/world.cpp):38 | 新业务类型无法仅靠正式类型定义获得新的参与方式 |
| refresh 只把 Cube 加入呈现，把 Counter 加入更新 | [world.cpp](../../src/world/world.cpp):55、69 | 继承 Component 可以 active、能启动任务，但不等于自动 tick |
| update 只有 CounterUpdate 和 counter_advance 路径 | [execution.cpp](../../src/world/execution.cpp):135、185 | 不相关玩法不能合理地靠继承 Counter 来接入 |
| Cube 是唯一显式空间依赖 | [world.cpp](../../src/world/world.cpp):293、370；[objects.cpp](../../src/world/objects.cpp):279 | 仅泛化 updater 不会自动得到一般依赖验证与删除语义 |
| 编辑创建只开放 Transform/Cube，字段历史只保存 TRS | [session.cpp](../../src/editor/session.cpp):73、110；[session.hpp](../../src/editor/session.hpp):44 | 注册一个新类型还不能完成作者编辑、撤销和实际应用消费 |
| RenderItem 被应用转成固定 Cube 绘制 | [application.cpp](../../src/application/application.cpp):329 | 增加呈现回调但不增加消费数据，不构成完整 Mesh/Light 支持 |

因此，当前 World 已完成少数真实能力的纵向实现，但一般能力的接入还没有闭合。这是功能扩展边界，不表示现有方块编辑消费者已经失败。

### 推荐的改良：统一执行登记，按需求选择个体或批次处理

以一个真实第二种更新能力为起点，沿现有成员、enabled、start/stop 逻辑维护实际执行参与。正式 TypeId 可以绑定默认行为和维护查询索引，但最终调度的对象是登记的执行工作；它可以是一个 Actor/Component 的更新，也可以是处理多个对象的专用系统。World.update 继续承担唯一执行入口和保护，Counter 迁入同一登记链。组件作为业务对象能够有行为，不能为了调度而被强制变成纯数据片段。

对于适合同质处理的能力，已有 bucket 可以缓存类型匹配、形成活动 ObjectId 批次，处理器选择循环原生调用、调用 JIT 或采用专用批处理。批次不承诺 payload 连续；也不要求同类型所有实例拥有相同阶段、间隔和依赖。此前“按 TypeId 登记批次处理器”的建议限定为这类系统的可选路径，不将它作为所有组件的唯一调度模型。无需先给所有 Component 增加虚 tick，也无需把 World 参与策略塞入 Core 反射元数据。

第一片必须确定以下合同：

1. 在开放、停止且没有活动回调的 World 安装或替换登记，明确登记函数及捕获资源的寿命。
2. 明确派生匹配和重叠处理，不能无意对同一对象重复更新；执行次序采用明确规则，不能依赖类型首次加入的偶然顺序。
3. 字段改动何时对后续处理器可见，结构请求何时提交，延后 destroy 从哪个时点使身份失效，都沿唯一执行链说明。
4. 必要的参与登记属于提交路径，不能靠 LifecycleObserver 事后建立；后者仍只承担通知。
5. 更新错误继续遵守当前停止运行、取消任务的政策；已提交结构仍按 Commit 报告。stop/remove/detach/close 收束同一登记和任务。
6. 类型到默认行为的映射不规定每种类型只有一种执行方式。同一对象的多个更新工作，或不同实例的阶段、间隔与依赖，须按真实需求表达；类型分组不能覆盖正确的执行依赖。

它的直接收益是新增能力不再需要扩展 World 内的每个类型分支。登记表、活动数组和间接调用都有成本；本轮不声称它已经带来性能提升，更不把它当作并行安全证明。

第二呈现能力应连同实际 renderer 输入和消费实现一起落地。通用关系/依赖描述暂不提炼：当前只有 Cube 一条显式依赖，尚无两项真实重复支撑一个依赖语言。未来扩展仍需覆盖批量组成提交、移除拒绝与 detached destroy。

### 强 owner：现在保留，明确重审条件

组件强 owner 会保住 Actor 和仍附属的兄弟，这是已承诺的保组语义。编辑器字段历史也有只持 Transform 根的消费者；但 ActorRemoval 直接持 Actor、ComponentRemoval 则已解除 owner，不能说所有历史都依赖这条反向强边。相关证据见 [session.hpp](../../src/editor/session.hpp):44、[session.cpp](../../src/editor/session.cpp):122、[World 验收](../../acceptance/world/main.cpp):204。

建议普通“记住一个对象”的需求优先选合适的弱观察引用，资源驻留由资源消费者的使用权控制。即使外部资源能卸载，兄弟自身的 CPU 载荷仍然保留，因此不能把“资源独立管理”当作无限保组没有成本的证明。

只有真实轨迹显示长尾组件强根持续保住大量不需要的 CPU 数据，超过项目预算，而且引用者确实不依赖整组完整性，才应共同重审 owner 强度。届时要一并定义 owner 已回收而组件仍有效时的访问、调用、闭包和历史行为，迁移需要保组的消费者。只改 Strong 为 Weak 会改变合同。

### 空间模型：功能需求明确后再扩，先决定失败语义

需要进一步区分数学值与空间组件。当前 `Trs` 是无独立身份的数学值，名为 `Transform` 的类则是一个独立 Component。用户已同意 SceneComponent 方向：由 `SceneComponent : Component` 承担局部 Trs、空间挂接与世界变换，`StaticMeshComponent : SceneComponent` 携带网格与材质；Actor 指定一个空间组件为根。一个方块可由 Actor 加以方块网格为内容的 StaticMeshComponent 表达，网格组件直接作为根，不必强制再创建一个独立 Transform 对象。需要单独 pivot 或挂点时，才建立纯空间组件。UE 展示了这类结构，Unity 则将 Transform 作为 GameObject 的独立组件，二者都能成立。[UE Scene Components](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-in-unreal-engine?application_version=5.6)、[Unity Transform](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Transform.html)

空间组件方向已确认，空间挂接、失败和销毁细节仍需明确，尚未迁移代码。实施时需把当前 Cube 专用类型与独立 Transform 依赖完整迁移到新的空间/网格合同，避免保留两套空间入口。共享空间能力也不要求立即复制 UE 的全部渲染/物理继承层次。

唯一 Transform 使当前根、聚焦、Cube 依赖和销毁顺序清楚，但多个 Cube 共用它，无法表达炮塔、车轮或武器挂点各自的局部空间。下一项这类玩法出现时，可以选择“唯一根加空间部件”或“多个 Transform 加显式根”；不能只删除唯一性检查。

必须同时定义循环与所属检查、局部/世界变换、重挂接、父终结、历史恢复和离场重入。组成 owner、空间 parent、保存范围和销毁级联各自回答不同问题。

**额外约束是非均匀缩放与旋转可能产生剪切。** 当前正尺度、归一化四元数的 TRS 不一定能表达任意保世界重挂接结果。可选择提前拒绝不可表示的操作、限制支持范围，或明确增加仿射表示；需要一次可审查的合同决定。不能失败后回退 identity 或默默损失变换。该结论来自矩阵组合性质与当前 [math.hpp](../../src/world/math.hpp) 合同，不是外部引擎性能结论。

### 补充：UE 组件无序与 Tick 调度

UE 的“组件无序”应理解为所属组件集合没有可依赖的枚举顺序。5.5 官方 GetComponents 无参数重载返回 `const TSet<UActorComponent*>&`；填充数组的重载也未承诺添加顺序。它不表示生命周期或执行完全无序。[GetComponents](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/GameFramework/AActor/GetComponents?application_version=5.5)、[TSet 容器合同](https://dev.epicgames.com/documentation/unreal-engine/tset?application_version=4.27)

普通 Actor/Component 更新的公共模型是分别注册 TickFunction 到 Level，再由 TickTaskManager 为 World 的一帧组织阶段与先决条件。不能把它概括为默认按组件类型建立连续池，也不能把组成关系当作 Actor::Tick 每帧遍历所有组件。标准组件设置接口另有“在 Actor 后执行”的约定，说明独立登记仍可有执行依赖；本轮没有核验其函数体中的具体实现方式，也不把它外推到全部自定义 Tick。[Actor Ticking 5.6](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine?application_version=5.6)、[SetupActorComponentTickFunction 5.5](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/Components/UActorComponent/SetupActorComponentTickFunction?application_version=5.5)、[TickTaskManager](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FTickTaskManagerInterface)、[RegisterTickFunction](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FTickFunction/RegisterTickFunction?lang=en-US)

本次默认 TickFunction API 页标为 5.8，包含合批、间隔、线程资格和先决条件。Tick 合批不等于组件载荷池化，所有 Tick 也不自动获得并行安全。Actor Ticking 概述保留了旧式逐 Actor 驱动的历史措辞，本文依据注册、目标和管理器接口交叉核对，不用该句概述推导全部内部路径。[FTickFunction](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FTickFunction?lang=en-US)

对 FrostFox，Actor 组成序列继续服务展示和历史，SceneComponent 空间关系决定变换传播，执行登记说明何时运行。排列位置不应隐式规定业务 Tick 依赖。按类型查询、按阶段调度及针对热负载合批可以共存；不必因为 UE 使用 TSet 就改掉现有组成容器。

## 6. 专家讨论中的分歧及收口

| 被挑战的建议 | 质询结果 | 本轮判断 |
| --- | --- | --- |
| 一套 ObjectId 是否排斥资产/GPU/物理 handle？ | 这些句柄管理不同内容或资源；问题是另造同一业务实例的独立权威 | 保留统一业务身份，允许有明确寿命和映射的内部句柄 |
| 注册处理器是否已经完成扩展？ | 本地席指出 Session/应用仍有专用逻辑；AAA 席以 Destiny 说明作者体验不能由注册表代替 | 以一个完整能力交付为终点，而非“类型注册成功” |
| 批次接口是否应该立即附带效应图？ | Unity 席要求避免固化 N 次逐对象调度；本地席指出当前串行合同已经成立 | 采用批次消费形态，当前明确顺序；真实并行出现后再做访问分析 |
| 资源可卸载是否意味着强 owner 没有问题？ | UE 席指出兄弟 CPU 数据仍驻留；AAA 席指出存活图和资源图根的用途不同 | 保留合同，按根来源、保留字节和持续时间决定是否重审 |
| 应不应该先通用化全部依赖和空间关系？ | 现有只一项显式依赖；Godot/Cry 席确认外部 parent/owner/删除合同不能机械套用 | 优先补真实能力接线；空间与依赖按新消费者分别演进 |
| 所有寿命和可见性问题能否等性能数据？ | AAA 席反对：同帧字段可见、结构提交、destroy/close 写入资格是现在就必须明确的语义 | 语义先明确；版本、双缓冲、栅栏等设施由真实异步消费者触发 |

这轮没有找到推翻共同 Runtime、detach 保身份或显式 destroy 的必要证据。也没有证据证明当前按类型索引已经足够应付大量同构模拟。两种判断可以同时成立。

## 7. 下一步的具体终点

**推荐下一项相关开发做一次“真实新能力的完整接入”，例如移动或旋转能力。** 名称由实际游戏需求决定，不能再增加一个只有计数的演示来替代业务压力。

完整切片至少贯通：正式类型与字段 → World 执行参与 → 真实算法及可观察结果 → 实际需要的创建、参数编辑和历史 → 停用、移除、撤销、关闭。若范围选择无窗口游戏宿主，就明确仅验收该宿主；若声称编辑器支持，就完成 Session 和 UI 的对应接线。当前停止模式和游戏运行模式怎样查看结果也应随切片明确，不能暗中加入半套 Play 状态。

进入实现时，以已有端到端消费者扩展必要情形：新能力和 Counter 共存、派生匹配没有误更新、运行资格一致、结构请求有确定可见点、错误后退出完整、历史恢复同身份。性能若属于该项任务，再测实际解析、调用、复制、GC 和驻留；本轮不新增基准工程。

后续按真实需求选择：多空间部件先补空间合同；资产与流送先补持久内容、实例发布和驻留合同；高密度模拟先测布局与批执行；实际并行再定义读写与任务依赖。当前建议可以在现有共同类型和唯一 World 操作链上逐步完成。

过程材料保存在本工作树忽略的 `out/research-world-2026-10/`。它们保留各席的一手来源定位、公开证据限制和交叉回复；当前实现规范不由这些过程稿定义。
