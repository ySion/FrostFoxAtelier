# Entity–Component 表达模型二审

评估日期：2026-10-12。状态：**用户已明确批准实施，Entity 模型已落地；双配置 CPU 端到端通过，GPU 窗口验收未收口，设备重置根因尚未判定。** 产品目标为单机优先、支持 16 人以内联机；对象数量尚未确定。

第 1–8 节保留当时的考据、迁移前源码结论和决策过程，其中“当前源码”、Actor/SceneComponent、原 owner/根与自动迁移风险均指迁移前快照，不代表现行实现。当前合同从 [World](../modules/world/README.md)、[Scene v6 与运行会话](../modules/world/project-scenes.md)和[物理模块](../modules/physics/README.md)进入；本报告第 9 节记录实施收口与待验收范围。

## 推荐决定

**建议采用可递归组合的 Entity–Component 作为长期作者模型：Entity 是完整场景对象，至多有一份可选的主空间状态；Component 提供能力，独立的作者空间节点使用子 Entity。** 主 Transform 优先采用普通值存储，不强制为它创建另一个 Runtime 对象。

选择的理由是对象组合、能力删除、子对象复用与引用方式更一致。**没有证据证明改名或这套作者模型本身会提高运行性能。** 多部件物体反而可能增加 Runtime 对象数；须同时消除现有重复扫描、重复空间求值及逐对象发布的明显成本。

保留现有 Runtime、TypeId、ObjectId、真实派生类型、JIT、GC、受控引用修改和 World 生命周期。ECS、按类型分块或批执行是特定运行任务的实现选择，不取代这套业务对象模型。网络复制单元也不由 Entity 层级自动决定。

这一决定可以固定作者语义；对象池、索引布局、批执行方式及网络协议仍按真实负载演进。

## 1. UE 的事实与不能推出的结论

Epic 在 2026-06-22 的官方路线图中确认 UE6 将汇合 UE5 与 UEFN，并采用 Verse Scene Graph。早期 UE6 仍保留 Actor/Blueprint，待新框架成熟后再废弃和转换；Early Access 目标为 2027 年底。这是已公开的方向，不能写成已经完成的全量替换。[Epic 路线图](https://www.unrealengine.com/news/the-road-to-ue-6)

当前可发布使用的是 UEFN Scene Graph Beta。其 entity 可以拥有子 entities 与 components，Prefab 也沿该层级组合；空间 Origin 可与组合 parent 分离。因此它统一了对象组合，但仍有树，不能直接称为消除了层级或采用了连续 ECS 存储。[Scene Graph 入门](https://dev.epicgames.com/documentation/fortnite/getting-started-in-scene-graph-in-fortnite)、[entity API](https://dev.epicgames.com/documentation/fortnite/verse-api/versedotorg/scenegraph/entity)、[Transform 与 Origin](https://dev.epicgames.com/documentation/fortnite/transforms-in-scene-graph-in-unreal-editor-for-fortnite)

传统 UE Actor 本身也不存储 Transform，而是通过可选 RootComponent 获得空间信息；一个 Actor 内可以有多个空间组件。可选 Transform 并非新 Entity 模型独有的优势。[UE Actor](https://dev.epicgames.com/documentation/unreal-engine/actors-in-unreal-engine)

本轮未找到 Epic 在相同负载下给出的 Actor 与 Scene Graph CPU、对象内存或吞吐对照。官方关于工作流与内存的定性描述，不能替代本项目测量。UE 的 MassEntity 则是另一种以 fragments、processors、archetypes/chunks 组织运行数据的机制，不能将其性能属性套给 Verse Scene Graph。[MassEntity](https://dev.epicgames.com/documentation/unreal-engine/overview-of-mass-entity-in-unreal-engine)

公开材料还存在版本边界：Scene Graph 教程与 component API 对组件唯一性的描述不完全一致；当前 UE 发布说明中的 Iris/Scene Graph 接线，也不等于传统 UE 已提供完整成熟的替代工作流。本项目的多实例、启停与迁移合同需要自行确定。[component API](https://dev.epicgames.com/documentation/fortnite/verse-api/versedotorg/scenegraph/component)、[已知问题](https://dev.epicgames.com/documentation/fortnite/scene-graph-known-issues-in-fortnite)

## 2. 当前源码已经有什么

代码审议基线为主目录 `E:/FrostFoxAtelier` 的 `72843f7a24d51542620f5bebedb83433cfca914b`，**包含其他负责人正在进行的未提交增量**。其中 World 内联光照配置、Scene v4 和 Field Label/Category 已进入源码。本报告的主目录行号是研究时定位，不等同于干净提交中的行号。

本线程的 `actor-world` 工作树仍在同一提交，研究开始时干净。这里只新增研究文档，未修改任何生产代码、未构建或执行新验收。

| 问题 | 当前源码事实 | 对决定的影响 |
| --- | --- | --- |
| 是否需要另一套对象系统 | Actor/Component 已使用同一 Runtime，真实类型、引用和 JIT 接收者均存在 | 保留。Entity 更名不引入第二个业务 EntityId。[对象定义](E:/FrostFoxAtelier/src/world/world.hpp:20) |
| 是否已有完整对象嵌套 | Actor 有组件序列与根引用，没有完整对象 children/parent | 递归组合是新增行为，不是重命名。[创建与组成](E:/FrostFoxAtelier/src/world/objects.cpp:126) |
| 是否已有唯一空间真相 | 每个 SceneComponent 保存自己的 TRS，Actor.root 仅引用它；没有另一份 Actor TRS | 当前多空间组件本身没有重复权威问题；改为主 Entity 空间是作者模型取舍。[空间类型](E:/FrostFoxAtelier/src/world/world.hpp:51) |
| 是否已有生命周期 | Actor/Component 共用正序 Begin/Tick、逆序当前组成 End，enabled 不重复配对 Begin/End | 扩展到子树，保留成功 Begin 才 End、失败继续清理的合同。[执行](E:/FrostFoxAtelier/src/world/execution.cpp:79) |
| 是否只有玩具物理 | 同 Actor 多动态 Primitive、布娃娃部件、车轮已有真实消费者 | 必须迁移跨子 Entity 的绑定与回写，不能压成一个刚体。[多 Primitive 验收定义](E:/FrostFoxAtelier/acceptance/physics/primitives.inl:1)、[专用组件](E:/FrostFoxAtelier/src/physics/components.cpp:181) |
| 是否已有完整对象图归档 | 实际派生类型已保存；通用业务引用明确拒绝；文档局部 ID 从顺序生成 | 稳定局部身份、内部引用重映射、子树复制才是新增核心能力。[字段遍历](E:/FrostFoxAtelier/src/editor/fields.hpp:13)、[场景](E:/FrostFoxAtelier/src/editor/scene.cpp:304) |
| 能否只给 setter 加 dirty | 原生可写 TRS；Source 写入进入 Runtime::write，不经过 World setter | 不能据此建立正确的跨帧缓存；先用稳定阶段的空间求值。[Source 写入](E:/FrostFoxAtelier/src/source/detail/bridge.cpp:303)、[Runtime 写入](E:/FrostFoxAtelier/src/runtime/runtime.cpp:2190) |
| 当前场景格式是什么 | 主目录已经写 Scene v4，包含 World 作者光照设置 | 新迁移必须基于实际最新格式，保留这批工作。[v4 保存](E:/FrostFoxAtelier/src/editor/scene.cpp:377) |

Class 仍指核心 Runtime 的可引用类型定义。组合文件保存作者对象图，是否包装为资产是内容工作流选择；实例化一个组合不应强制作者新增 Entity 派生类。

## 3. 对抗比较：为什么选这个方向

| 候选 | 真实收益 | 主要代价 | 判断 |
| --- | --- | --- | --- |
| 只把 Actor 更名 Entity | 术语统一 | 消费者与旧数据改名，组合缺口原样保留 | 拒绝作为独立交付 |
| 递归 Entity，保留多个 SceneComponent | 最小扩展完整对象组合，复合物体对象数较少 | 完整对象树与空间组件树都暴露给作者；Mesh 作为根时空间寿命仍耦合能力 | 技术上成立，是最有力替代方案 |
| 递归 Entity，每 Entity 至多一主 Transform | 子对象天然是同种可复用对象；删除能力不删除对象空间；对象引用入口一致 | 多独立空间部件可能增加 Entity 数；需要迁移 owner、空间和物理消费者 | **推荐长期作者模型** |

真正改变推荐方向的反例是“删除网格能力后对象位置仍存在”。当前 `set_mesh_binding` 清空资源可以保留空间，但删除作为根的 Mesh 组件是另一件事；最后根被移除会清空 Actor.root。不能用清资源冒充删除能力，也不能在读取时悄悄补根。[资源更换](E:/FrostFoxAtelier/src/world/objects.cpp:274)、[移除根](E:/FrostFoxAtelier/src/world/objects.cpp:401)

保留 SceneComponent 的合格替代方案是每个空间主体设置稳定的纯空间根，然后挂功能组件。它解决删除能力的问题，但普通单网格对象从两个 Runtime 对象变成三个。相比之下，Entity 内联主空间值可以保持 Entity＋Mesh 两个对象。复杂多部件物体则相反：保留多个空间组件更省对象。

### 对象数比较：设计推导，非性能测量

以下只计场景主体、能力和独立 Transform 对象；共享资产、后端物理状态、GC 根记录、容器和运行索引另算。推荐列允许根 Entity 同时承担第一个空间节点。

| 相同业务内容 | 当前 Mesh 可作根 | 现有模型加稳定纯空间根 | 推荐模型，TRS 为值 | 新模型，Transform 另建对象 |
| --- | ---: | ---: | ---: | ---: |
| 一个主体、一网格 | 2 | 3 | 2 | 3 |
| 一个物体、8 个网格空间节点＋2 个行为 | 11 | 12 | 18 | 26 |
| 8 个真正独立的武器，每个一网格＋2 个行为 | 32 | 40 | 32 | 40 |

这说明两件事：推荐模型并不天然更省；旧模型也并非在相同制作要求下永远更省。若再保留纯组织根，需要另计该根。仅有骨骼、碰撞形状或批实例数据的内部元素不按完整 Entity 计数。

六个研究席并非一致投票：源码席认为沿现有空间组件扩展足以解决递归组合；性能与网络席均认为其专业领域不能替唯一 Transform 决策背书；跨引擎席支持可选主空间；反方席最初偏向保留空间组件，在检验能力删除后的空间身份问题后，转而支持内联主空间的长期方案。上面的推荐是对这些成本与制作收益的取舍。

## 4. 推荐固定的表达契约

### 4.1 Entity 与 Component

- Entity 是完整场景对象，拥有有序能力组件和自有子 Entity。根对象与子对象使用同一类型，不引入 ChildEntity 包装类。
- 保留真实派生 Entity 与组件行为，默认用组件扩展能力。门可以提供直接可调用的业务接口，不要求外部按名称爬树寻找部件。
- 同类型组件默认允许多实例；只有真实需要唯一的能力才受约束。引用组件用身份，不只用 TypeId。
- Mesh、Light、普通碰撞等能力使用所属 Entity 的主空间。删除能力不会删除主空间，也不会改变 Entity 身份。
- Entity 可以没有空间；查询缺失空间返回明确结果，不创建 Transform，不伪造单位矩阵。
- 主 Transform 是一种唯一的作者能力，不要求一个额外 GC 对象。优先研究内联值布局；具体存在标记、字段访问和存储布局需与 Runtime 的反射/JIT 合同一致。

### 4.2 什么应该成为子 Entity

当一个部件需要独立的作者空间、稳定对象引用、组合复用或分离操作时，它是普通子 Entity。例如门板、可拆卸按钮、挂在角色上的武器。

骨骼姿态数组、网格顶点、粒子、碰撞形状集合、射击偏移仍是专用能力或资源中的数据。它们可以有领域内的局部变换，但不作为另一套可供任意组件挂接的通用空间节点树。不能把每个数值偏移、每个 shape 都自动升级为完整 Entity。

一个示例组合可以是：

```text
Door Entity（主空间；DoorBehavior）
├─ Frame Entity（StaticMeshComponent）
├─ Leaf Entity（StaticMeshComponent；独立物理运动）
├─ Button Entity（交互组件，直接引用本组合的 Door）
└─ Indicator Entity（LightComponent）
```

灯参数、门的开合状态、碰撞材质仍是内聚字段，不各自拆组件。保存 Door 子树不要求新增一套 Prefab 运行对象。

### 4.3 组合拥有与空间参考分开

| 关系 | 决定什么 | 不自动决定什么 |
| --- | --- | --- |
| 组合拥有 | 哪些对象属于一个完整组合，子树退出/销毁与启用传播 | 空间必须以谁为参考、联网权限 |
| 空间参考 | 局部 TRS 相对于谁求世界姿态 | 拥有、GC 强保活、独立网络复制 |
| World 成员 | 是否参与这个世界，身份索引与调度资格 | 文件归属或网络所有者 |

普通空间子 Entity 默认参考最近具有主空间的组合祖先；没有空间的组织节点不新增坐标变换。没有空间祖先时使用世界坐标系。显式空间参考可以改变这个默认关系，必须记录显式选择，不按当前值是否恰好相同推断意图。

首版可限制显式参考范围，但要明确拒绝未支持操作。目标应至少允许武器保持独立组合归属、以角色 Entity 为空间参考；骨骼 socket 由后续真实骨骼消费者定义，不先建立泛化 origin 框架。

两种关系都禁止环、跨 Runtime 和未经明确处理的跨 World 连接。空间参考失效不能回退到 identity。移除被引用空间目标前，要求显式解绑/重绑，或在同一批变更中完成可表示的 keep-world；按整批最终结构验证，允许同批解除依赖并删除目标。keep-world 在目标仍有效时求值，提交结果包含新关系与新变换，不要求消费者再读取已删除对象。不能完整完成时先拒绝，原图不变。销毁组合子树只销毁自有对象，不销毁仅以它为空间参考的外部武器。

保留当前坐标、正尺度、规范化旋转及 keep-world 数值保证。无法表达局部 TRS 的剪切、反射、奇异或溢出结果仍拒绝，不在迁移中放宽。

### 4.4 World、生命周期与 GC

- 保留一个成员目录，每个 Entity 成员只登记一次，停用不等于离场。查询、Tick、渲染提取不同时“遍历全部成员再递归 children”。组合根列表是结构视图，不是另一套成员权威。
- 子树构造在 Detached 状态完成；整批类型、引用、环与空间验证成功后发布，回调只看完整图。
- 默认 Begin/Tick 顺序建议明确为 Entity→自身组件→有序子 Entity；End 按当前已登记顺序反向，只处理成功 Begin 的对象。这个层级顺序是新增合同，需要验收；编辑器的显示排序不能暗中改写它，也不能把默认顺序当作通用依赖调度器。
- Begin/End 继续按 World 运行周期配对。有效启用为本地意图与组合祖先资格的合成；禁用取消任务、撤参与、停止 Tick，不反复 Begin/End。
- 同 World 重挂且成员与有效资格不变时，不重走 Begin/End、不重启任务、不无故重建刚体。配置替换也不等于运行状态重新初始化。
- 子树离场、显式销毁、停止和关闭继续共用既有清理责任。End/通知失败仍完成剩余清理，通过 Commit 区分已提交变化与通知失败。
- 父的 children 使用强拥有，composition parent 建议弱观察；Component→owner 保持现有强引用。叶子不因新增强父链而保住整座场景。
- Detached 对象的弱父失效是必须处理的状态：读取返回无有效父/错误，重新发布前修复并验证，不在 GC 中偷偷执行重挂、End 或子系统注册。

### 4.5 物理写入权

每个 Entity 的主空间在一个运行阶段只有一个权威写入者。动态物理、运动学、角色控制与普通游戏移动不能同时无条件覆盖它。

旧的两个独立运动 Primitive 应映射到两个具有主空间的 Entity，各自保留物理组件及查询身份；不能合成一个 body。多个 shape 构成同一刚体、骨骼布娃娃内部 bodies 与普通子 Entity 是不同能力。现有车轮/布娃娃视觉部件要改成明确的对象或部件绑定，不能继续假设所有部件在同一个 owner 的组件数组里。

组合/空间挂接不自动焊接，也不自动修复当前移动物理祖先下静态/运动学从属碰撞的限制。该限制需要独立的物理跟随/形状方案；两种对象模型都不能靠换名解决它。

## 5. 持久化、复制与 Class

这部分是完整对象组合的核心，不应等到以后“做 Prefab UI”再补。

1. 保存实际 Entity/Component 类型、子树和支持的作者字段。图内 Entity 与组件都需要稳定局部身份；重排、改名不能改变它。
2. 组合实例有自己的作用域；相同组合实例化两次，内部按钮分别引用各自的门。共享网格、材质与其他资源继续按资源身份共享。
3. 统一恢复顺序：创建全部真实类型对象→建立局部 ID 映射→写字段和恢复引用→校验完整图→注册 World→按运行状态 Begin。
4. 保存、复制和组合实例化复用同一类型/引用编码机制，避免存档能恢复引用而复制遗漏。外部业务引用采用显式保留、参数绑定或拒绝策略；无法无损支持时拒绝。
5. Runtime ObjectId、文档/组合局部 ID、资源 GUID 和未来 NetId 各有不同作用域。网络寻址映射不是第二套业务对象权威。
6. Class 是核心类型与构造语义；组合描述是内容。Prefab 不必是派生类，真实派生 Entity 也不能为了组合能力被降为基类。

工业资料支持这些分工，但不指定本项目只能照抄某种格式。Unity 保存 GUID＋fileID，并在复制中重映射内部对象引用；Godot 则将树父级、保存 owner 和空间 top-level 分开，其部分节点引用通过 NodePath 恢复。因此不能把所有引擎都说成同一种 ID 方案。[Unity 场景引用](https://docs.unity3d.com/6000.0/Documentation/Manual/yaml-prefab-serialization.html)、[Unity 复制语义](https://docs.unity3d.com/2022.3/Documentation/Manual/script-Serialization.html)、[Godot Node](https://docs.godotengine.org/en/4.5/classes/class_node.html)

本轮候选可以实现组合子树保存与实例化、必要内部引用、显式覆盖意图和一个稳定 key 的领域构造消费者；完整 Prefab 变体、深层结构覆盖合并、源资产实时更新与通用程序化图系统延期。

## 6. 性能：先修真正决定工作量的地方

下面是源码复杂度判断，**没有运行新性能基准，也不换算成帧时间**。

| 当前路径 | 源码工作量 | 推荐约束 |
| --- | --- | --- |
| PhysicsScene 每个 World 成员 reconcile 后遍历全部 bodies | A 个成员、B 个 body 时有 Θ(A×B) 基础匹配；更多子 Entity 会放大 A | 使用已有 owner→body 归属或一次全局清理遍历；稳定配置与每步姿态同步分开。[物理路径](E:/FrostFoxAtelier/src/physics/physics.cpp:558) |
| 每个消费者独立 world_matrix 求父链 | 重复求共享祖先；链式最坏累计 Θ(S²) | 在稳定阶段按空间依赖求一次快照，渲染/灯/物理复用。[矩阵求值](E:/FrostFoxAtelier/src/world/objects.cpp:323) |
| 渲染和灯分别扫描混合呈现目录 | 两种提取都预留总呈现数容量，零灯场景也可能为全部网格预留灯数组 | 沿现有派生索引和输出容量复用消除明确浪费。[网格提取](E:/FrostFoxAtelier/src/world/world.cpp:711)、[灯提取](E:/FrostFoxAtelier/src/world/lighting.cpp:181) |
| 场景逐 Entity attach，每次全量 refresh | 相似组成的 N 个对象导入可累计 Θ(N²) 索引重建 | 复用受控发布入口，增加整批子树/场景发布，一次维护索引。[成员发布](E:/FrostFoxAtelier/src/world/world.cpp:213) |

不要只在 `set_transform` 置 dirty：原生、Runtime 反射与 JIT 的直接字段写入都必须被正确观察。首版优先采用有明确有效期的阶段空间快照；回调内的即时查询仍可读取实时作者值。跨帧增量缓存需要完整失效契约后再加入。派生快照可以有不同布局，它们不能成为第二份可编辑 TRS。

静态 Entity 不注册空 Tick；大量同构行为出现时，再沿现有类型/JIT 调用边界提供明确的批执行。连续存储与结构变更成本是具体机制，不随 Entity 名称自动出现。[Unity archetype/chunk](https://docs.unity3d.com/Packages/com.unity.entities@1.3/manual/concepts-archetypes.html)、[结构变更](https://docs.unity3d.com/Packages/com.unity.entities@1.3/manual/concepts-structural-changes.html)

后续只在真实端到端场景采样：对象/组件/空间节点/刚体数，结构深度，活动 Tick 数，矩阵求值次数，临时根与分配，物理 reconcile 与后端求解分项，导入时间、GC 工作及帧时间分布。单机或 16 个玩家都不能据此推导游戏对象总量。

## 7. 小规模联机的约束

**作者 Entity 不等于独立网络对象。** 门、按钮、灯可以作为普通子 Entity，由一个复制组承载；可独立飞行的门板、交易配件则可在需要时成为独立复制单元。网络层映射同一 Runtime 对象，不维护第二份 gameplay 权威状态。UE subobjects 与 Unity 的根 NetworkObject＋子层级都是可行机制的例子。[UE 子对象复制](https://dev.epicgames.com/documentation/unreal-engine/replicating-uobjects-in-unreal-engine)、[Unity NGO 2.7 层级](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.7/manual/advanced-topics/networkobject-parenting.html)

三个必须在本地模型中避免的错误：

- 武器空间附着到角色，就自动改变拥有权、复制权限或随角色销毁。
- 组合根在远处，就认为近处按钮/门无需复制；复制相关范围不能只看根位置。
- 只记录“按钮曾按下”“门板曾断裂”的事件，后加入玩家却无法恢复当前状态和结构。

前述删除准入针对完整变更批次，未来权威端可以同批处理外部空间依赖与目标销毁。客户端因相关性过滤而暂时缺少副本，不等于对象已被权威销毁；网络适配需要处理关系和删除消息的乱序，不能让不完整的本地副本永久否决权威结果，也不能把缺失目标当作单位矩阵。这是后续网络适配约束，无需现在实现协议或复制框架。

现在保留稳定身份、保身份重挂、完整图恢复、结构受控提交及配置/运行状态分离即可。NetId 实现、传输、RPC、复制组、相关性、休眠、预测和物理校正应随第一个真实联机玩法落地，不为 16 人目标预建分布式事务或 MMO 框架。

## 8. 附件需要接受、调整和删除什么

| 处理 | 内容 |
| --- | --- |
| 接受 | 同一 Runtime，递归完整对象，真实类型与引用，组合/空间分离，子树生命周期，保存/复制内部引用重映射，稳定 key 的最小构造消费者 |
| 调整 | Actor→Entity 是推荐决定而非本轮既定授权；主 Transform 不强制独立对象；多个同类型组件按实际能力允许；网络复制不自动逐 Entity；“扁平”只描述作者角色简化，不能暗示存储或遍历收益 |
| 增补 | 当前 Scene v4/World 配置保留；能力删除后空间保持；组件迁移后的 owner/行为目标检查；一次发布子树和去除 Θ(A×B) 物理扫描；同等业务内容下的成本对照 |
| 删除或延期 | 依据 UE 路线图直接宣称更快；不经决策立即执行迁移；每 shape/bone/vertex 一个 Entity；通用生成器注册表、图数据库、全自动运行状态迁移、全面网络栈与复杂 Prefab 编辑器 |

### 最危险的自动迁移假设

旧空间组件搬到子 Entity 后，Component.owner 会从原 Actor 变为该子 Entity。调用 `translate_owner`、获取 owner 上的业务字段、收集同 owner 部件等代码都可能改变意义。当前 `translate_owner` 捕获 owner 与其根，并按同 owner 的组件验证物理限制；迁移后移动目标和检查范围都可能改变。现有 Ragdoll/Vehicle 也按同 owner 收集部件。即使变换画面看起来一样，也不等于行为无损。[移动与校验](E:/FrostFoxAtelier/src/world/world.cpp:487)、[部件收集](E:/FrostFoxAtelier/src/physics/components.cpp:181)

内置类型及官方 Source 消费者可以明确修改和验收；无法判断的用户派生类型必须给出具体诊断或要求显式迁移绑定。不能转成普通基类、增加永久影子 owner，或悄悄保留一套旧 Actor 世界以制造“成功加载”。

旧纯根/网格根的空间值可映射到原主体 Entity；非根空间节点映射为子 Entity。原功能组件保留真实类型或通过明确类型迁移规则恢复；纯空间节点不必再附一个空功能组件。旧 SceneComponent 引用如何映射到新的 Entity/空间访问、旧字段路径与 Source 基类如何调整，必须随该类型声明共同处理。

## 9. 已批准实施的收口与验收范围

用户在完成本轮研讨后明确批准迁移。实施沿用现有 Runtime 身份、引用、GC、World 生命周期和编辑事务；第 1–8 节的考据与迁移前源码诊断保留为决策依据。现行实现以模块文档为准，不把原附件或研究快照当作第二套规范。

1. **统一作者模型。** Entity 递归拥有子 Entity 和能力 Component，自身具有可选内联 TRS；组件不含独立空间。组合 parent 与空间 reference 分离，独立位置的能力使用子 Entity。完整对象图先校验、登记后发生生命周期通知，内部按组合前序执行与逆序退出。详见 [World 当前合同](../modules/world/README.md)。
2. **统一持久图与复制。** Scene v6 保存 roots、entities、共用字符串文档 ID、next_id、可选 space 和正式强弱引用字段。旧 v1–v5 仅经严格导入恢复新模型；主体保留旧 Actor.enabled，提升空间子 Entity 保持 enabled=true，能力组件保留原 enabled，不能把旧空间父组件开关扩成子树开关。同 Runtime 复制重定向图内引用并保留图外引用；独立存档拒绝图外引用。详见 [场景与运行会话](../modules/world/project-scenes.md)。
3. **迁移实际消费者。** 官方 15 个场景、Dungeons 转换工具及当前文档已采用 v6。布娃娃 parts 与车辆 wheels 显式持有目标 Entity，metadata 与网格分离，姿态整批写回实体。查询 ignore_self 只指实际 owner，旧 Actor 组的 Primitive 排除范围通过 ignored_entities 显式恢复。编辑器显示真实递归 Entity 树，Control 使用 entities 和明确的组合/空间操作。详见 [物理合同](../modules/physics/README.md)、[查询合同](../modules/physics/scene-queries.md)及 [Editor Control](../modules/editor/control.md)。
4. **用真实闭环验收。** 开发/发布受影响 CPU 端到端链已实际通过；正式编辑器窗口与 GPU 链仍未收口。闭环涵盖门—按钮—灯组合创建、独立保存、两次实例化、内部引用隔离，以及创建/组合/移动/复制/删除/撤销/保存/重载/Play/Stop；迁移内容的结构核对不能替代运行证据。

不可省略的反例包括：删 Mesh 保留空间与其他能力；两个动态部件独立运动；同有效状态重挂不重复 Begin/任务/物理注册；禁用父级不重新配对生命周期；外部对象只空间附着而不被组合销毁；组件开关不改变其原空间后代；owner 提升后查询与移动仍有明确目标；错误类型、环、失效空间目标和不可表示 keep-world 不半写；失败 Begin/End 仍完成清理；坏文件不替换当前文档。

验收状态：**实现已落地，Dev/Release CPU 通过，GPU 未收口。** 双配置全量构建、World 13/13、Physics 22/22、Source public consumer、Project Scene、editor document、viewport-edit（含 EntityComposition）、recovery、materials、legacy physics-project、10 个 physics examples 文档/模拟链、UI layout 41/41 均已通过。Dev 额外 Dungeons 详细 CPU（2332 模型、30 atlas、3 场景）、Sponza、official examples、Project integration 通过；Release Project integration 未运行。

GPU Dev edit 因 external focus loss 未完成，physics window 旧 DDGI 断言已修正并通过 Dev / Release 编译、未重跑。官方 Examples 窗口日志证实 0x887A0007 / DXGI_ERROR_DEVICE_RESET，用户报告 GPU 驱动崩溃，根因尚未判定。Dungeons/Control 被停止，不计 PASS；Dungeons 零退出无产物亦不算通过。图形进程均已停止。历史证据目录为 `out/entity-migration/`；当前验证入口见[场景模型验收](../modules/editor/README.md#场景模型验收)。

范围外扩展 UI 两配置 23/24，shared_menu_semantics 的 coverage 收集与 checkbox 边框断言不一致。对应生产与验收源码内容哈希和 b0ede07d 一致，未运行基线，不推断其 PASS/FAIL；本轮未扩展菜单整改。

 既往 Actor/Primitive/UI 的 PASS 保留为历史证据，不作为本轮 Entity 迁移通过声明。尚无新的同负载吞吐测量，作者模型的统一也不构成性能提升证明。原六席公开资料核验、源码审议及定向反证材料保留在本工作树忽略的 out/research-entity-2026-10-12/；本文件继续作为评估与实施收口的汇总入口。
