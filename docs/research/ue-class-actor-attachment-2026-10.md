# UE 对照与 Actor/Component 核心模型评估

日期：2026-10-10。第 1–4 节保留 Epic 官方资料对照；第 5–7 节已同步本轮 PrimitiveComponent 实现，研究起始基线为 `5d24e4a6ab10b954c3bda03ffdfea674e939b12d`。当前接口以 [World](../modules/world/README.md)、[物理](../modules/physics/README.md)与[项目场景](../modules/world/project-scenes.md)为准。所引无版本参数官方页面在调研时显示为 UE 5.8。

用户后续澄清：这里所需的 Class 是核心类型系统中可引用、可实例化并与 JIT/Runtime 深度结合的类描述，不要求它必须成为独立资产。下文“Class 资产”只讨论一种可选作者与持久化形式；类引用、类型身份和实例化语义的完善不以资产化为前提。

## 1. “可实例化的 Class”需要区分什么

| 概念 | UE 中的职责 | 官方依据 |
| --- | --- | --- |
| 原生反射类 | C++ 的 UObject 派生类通过 `UCLASS` 进入对象类型系统；对象可通过 `GetClass()` 获取对应类描述。 | [Objects](https://dev.epicgames.com/documentation/unreal-engine/objects-in-unreal-engine) |
| `UClass` | 对象类型的运行时描述，包含类型相关信息和 CDO。`ClassGeneratedBy` 对原生编译类为空，对生成类指向生成它的 Blueprint。因而“有 UClass”不能推出“有一个用户创作的类资产”。 | [UClass](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UClass) |
| `UBlueprint` | 用户在编辑器中创作的资产，可以表达新 Actor 类型和行为；其 `GetBlueprintClass()` 返回编译生成的类。作者资产与生成类是不同对象。 | [UBlueprint](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UBlueprint) |
| `UBlueprintGeneratedClass` | Blueprint 的生成类，是 `UClass` 的派生类型，接入同一对象类型体系。它不要求业务实例另有一套脚本对象身份。 | [UBlueprintGeneratedClass](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UBlueprintGeneratedClass) |
| CDO | 每个类的默认对象，是该类的对象实例，用于默认初始化及差量序列化；与类描述本身不同。通常应按只读默认模板使用。 | [UClass](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UClass)、[GetDefault](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/GetDefault) |
| default subobjects | 构造函数中声明的默认子对象，可为组件；`CreateDefaultSubobject` 的契约是让所有类实例都实例化相应子对象。它们既不是类描述，也不能简单理解为所有 Actor 共享 CDO 中的同一组件实例。 | [CreateDefaultSubobject](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UObject/CreateDefaultSubobject)、[Object Replication：Default Subobject Replication](https://dev.epicgames.com/documentation/unreal-engine/replicating-uobjects-in-unreal-engine) |

运行时创建也分两层：一般 UObject 使用 `NewObject`；进入 World 的 Actor 使用 `UWorld::SpawnActor`。后者接受 `UClass`、世界变换及 `FActorSpawnParameters`，其中模板 Actor、Owner、目标 Level、出生碰撞处理等是分别表达的参数。未指定模板 Actor 时，类默认对象提供默认初始化。这说明“能按类型分配对象”与“完整地出生到 World”是不同职责。[Creating Gameplay：Instantiation](https://dev.epicgames.com/documentation/unreal-engine/creating-gameplay-in-unreal-engine-for-unity-developers)、[SpawnActor API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UWorld/SpawnActor)、[FActorSpawnParameters](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FActorSpawnParameters)、[Spawning Actors](https://dev.epicgames.com/documentation/unreal-engine/spawning-actors-in-unreal-engine)

类引用同样不是对象实例引用：`TSubclassOf<T>` 对所选 `UClass` 施加基类约束，既服务编辑器选择，也服务 C++ 类型检查；`TSoftClassPtr<T>` 表达可延迟加载的类引用，载入后再取得可用类。官方资产引用文档明确说，它引用的是资产所对应的 UClass，而不是该类的业务实例。[TSubclassOf](https://dev.epicgames.com/documentation/unreal-engine/typed-object-pointer-properties-in-unreal-engine)、[Referencing Assets](https://dev.epicgames.com/documentation/unreal-engine/referencing-assets-in-unreal-engine)

**可借鉴判断：** 类资产可以作为“持久作者定义 → 权威运行时类型 → 默认值与默认组件构造 → 实例出生”的入口。已有运行时类型注册与按 TypeId 创建对象，只覆盖这条链路的一部分。是否采用 CDO 这种对象模板实现，则应由自身默认值、构造和序列化机制决定；不必为了“像 UE”增加一套平行类型身份。

## 2. Actor 的世界成员关系与父子关系

`ULevel` 持有 Actor 列表，World 可包含多个 Level；Actor 的世界成员关系因此不能准确概括为“一个 World 只有一张平面数组”。不过，Level 成员列表本身确实不同于空间挂接树。**据此推论：** 平面成员集合与可选父子挂接可以同时存在，二者没有架构冲突。[ULevel](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/ULevel)

Actor 的位置、旋转和缩放来自 RootComponent。SceneComponent 有变换并支持层级挂接；普通 ActorComponent 表达不需要空间位置的功能。[Actors：Components](https://dev.epicgames.com/documentation/en-us/unreal-engine/actors-in-unreal-engine)

| 被称为“父子”的关系 | 实际契约 | 官方依据 |
| --- | --- | --- |
| `AttachToActor` | 将当前 Actor 的 RootComponent 挂到目标 Actor 的 RootComponent，可选 socket；不是把 Actor 的 World 成员身份换成父 Actor 的组件身份。 | [AActor：AttachToActor](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/AActor) |
| `AttachToComponent` | 将当前 Actor 的 RootComponent 挂到指定 SceneComponent；目标可以是另一 Actor 的组件。 | [AttachToComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/AActor/AttachToComponent) |
| Actor `Owner` | 主要用于网络复制相关性和可见性。`AActor::Children` 是 Owner 指向该 Actor 的对象集合，并不等于所有空间附着子 Actor。 | [AActor：Owner 与 Children](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/AActor)、[Actor Owner and Owning Connection](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-owner-and-owning-connection-in-unreal-engine) |
| UObject `Outer` | 对象包含关系参与对象路径；`GetPathName` 输出包含 Outer 链的全限定路径。Actor 的出生参数另有 `OverrideLevel`，文档将此 Level 描述为 Actor 的 Outer。它与空间变换父节点不同。 | [GetPathName](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UObjectBaseUtility/GetPathName)、[FActorSpawnParameters](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FActorSpawnParameters) |
| `ChildActorComponent` | 注册时生成 Actor，注销时销毁它。这是组件所管理的生成与销毁责任，不只是设置一个空间父节点。 | [UChildActorComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UChildActorComponent) |

挂接规则明确表达位置、旋转、缩放的处理方式及是否焊接模拟刚体，说明父子关系不只是一个 `parent` 指针。[K2_AttachToComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/AActor/K2_AttachToComponent)

**待核实边界：** 本轮官方文档没有充分证明“销毁普通附着父 Actor 是否自动销毁所有附着子 Actor”，也没有充分证明 Owner 或 Outer 单独意味着级联销毁。因此本文不作这类断言。已确认的明确生命周期契约限于 ChildActorComponent 的注册生成、注销销毁。若项目决定加入 Actor 父子，销毁、脱离、保留世界变换、跨 World 限制及物理驱动权都需要自身给出明确契约。

**可借鉴判断：** 保留 World/Level 成员责任，把空间挂接接到现有空间节点机制；Actor 级便利入口应落到同一挂接链路。“跟随父变换”“由某对象创建并负责销毁”“网络 Owner”“持久包含”必须能分别表达。无需把所有关系压入一个 Parent 字段，也无需马上照搬 UE 的全部关系种类。

## 3. StaticMeshComponent 的核心职责

UE 的继承链为 `UActorComponent → USceneComponent → UPrimitiveComponent → UMeshComponent → UStaticMeshComponent`。StaticMeshComponent 创建的是 UStaticMesh 的一个场景实例，`SetStaticMesh` 更换该实例使用的网格资源。[UStaticMeshComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UStaticMeshComponent)

| 责任 | UE 的表达 | 官方依据 |
| --- | --- | --- |
| 空间变换与挂接 | 由 SceneComponent 提供；Actor 变换沿 RootComponent 入口访问。 | [Actors](https://dev.epicgames.com/documentation/en-us/unreal-engine/actors-in-unreal-engine) |
| 网格资源与场景实例 | StaticMeshComponent 使用 UStaticMesh 资源，组件是放入场景的实例。 | [UStaticMeshComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UStaticMeshComponent) |
| 材质槽与实例覆盖 | MeshComponent 有逐组件 `OverrideMaterials`；StaticMeshComponent 按元素索引读取材质及材质数量。材质覆盖不是只有一个全网格颜色字段。 | [UMeshComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UMeshComponent)、[UStaticMeshComponent：GetMaterial / GetNumMaterials](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UStaticMeshComponent) |
| 碰撞与物理 | PrimitiveComponent 提供碰撞启用模式、对象通道等接口；StaticMeshComponent 可让 BodyInstance 使用网格的 body setup。网格资产可创作碰撞形状，组件/场景实例可覆盖碰撞配置。 | [UPrimitiveComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UPrimitiveComponent)、[UStaticMeshComponent：UpdateCollisionFromStaticMesh](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UStaticMeshComponent)、[Setting Up Collisions](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-collisions-with-static-meshes-in-unreal-engine) |
| 可移动性 | Mobility 控制是否在游戏期间移动。Static Mesh Actor 设置为 Movable 后可以移动、旋转和缩放。“StaticMesh”名称不意味着实例永远不动。 | [Static Mesh Actors](https://dev.epicgames.com/documentation/en-us/unreal-engine/static-mesh-actors-in-unreal-engine)、[Actor Mobility](https://dev.epicgames.com/documentation/unreal-engine/actor-mobility-in-unreal-engine) |

**可借鉴判断：** 精悍与玩具的区别应由真实业务链路判断：网格资源能否稳定解析并进入渲染；部件与材质槽是否完整保留；实例覆盖是否有明确规则；空间变换、世界注册和移除是否闭环。需要碰撞的业务应能接入项目现有物理体系；共享网格不应让一个实例的材质覆盖意外修改其他实例。

UE 的类继承链体现其责任分配，但这些资料没有证明五层继承是唯一方案，也没有证明 UE 的对象成本适合本项目。若引擎采用独立 Body、碰撞形状和网格资源机制，应让渲染组件接入这些责任明确的机制。不能以“组件类很短”证明完整，也不能以“没有复制 UE 全部渲染、导航、编辑器功能”证明它是玩具。

## 4. 对本次重新审视的结论

1. 应检查完整的可创作、可持久保存、可引用、可实例化 Class 链路；UClass 只是其中运行时类型概念的对照。
2. 默认值与默认组件构造属于类定义的实例化语义，需要与普通运行时添加组件兼容；CDO 是 UE 的实现选择。
3. Actor 的世界成员列表与空间父子层级可以共存，Actor 挂接应复用空间节点链路。
4. Attachment、Owner、Outer、生成销毁责任的职责不同；对外可以简洁，但底层契约不能混淆。
5. 普通附着父销毁后的子 Actor 行为仍待源码核实，本项目必须自行明确该契约。
6. StaticMeshComponent 应完整连接资源、变换、材质槽、渲染生命周期，并按业务接入碰撞；精简外形不能掩盖缺失链路。
7. UE 的材质逐槽覆盖值得借鉴，“StaticMesh 可移动”需要在命名理解中保持清楚。
8. 本次依据只足以支持上述机制对照，不支持性能优劣、内存成本或现有项目完成度的测量结论。

## 5. 本仓库已实现的能力

### Class 与实例创建

- `TypeDefinition`、`TypeInfo`、`TypeId` 已表达共享类型、继承、字段和方法。描述型标量字段可声明默认值，原生构造负责其默认值；这部分具备类描述与按类型构造能力，但不是 UE UClass 的等价实现。[Runtime 定义](../../src/runtime/runtime.hpp)
- `Runtime::create(TypeId)`、`create_actor(Runtime&, TypeId)`、`create_component` 创建真实类型实例。`World::attach` 可以将准备好的图加入运行中 World 并启动 Begin。[对象创建](../../src/world/objects.cpp)、[注册](../../src/world/world.cpp)
- `source::ClassInfo` 仅有名称、实际 TypeId 与基类 TypeId。项目 Source 可生成真实 Actor/Component 派生类型；普通业务类引用与完整 SpawnFromClass 工作流尚未接通。[ClassInfo](../../src/source/snapshot.hpp)、[语言边界](../../LANGUAGE.md)
- `Class Asset`、`Component Template` 已有术语定义；资产目录实际只有 folder/mesh/scene/source/file，未实现可持久引用的类资产、组件模板图、实例覆盖及资产实例化入口。Source 文件是作者输入，不能据此声称完整类资产功能已经存在。[术语](../../CONTEXT.md)、[资产目录](../../src/editor/project.hpp)
- StaticMeshActor 的默认组成由专用工厂组装。只按该 TypeId 创建对象并不会自动得到网格组件。场景加载从低层创建再恢复组成，避免重复构造组件。[工厂](../../src/world/objects.cpp)、[场景恢复](../../src/editor/scene.cpp)

### 正式引擎组件清单

以下是引擎类型，排除验收夹具和用户 Source 派生类；共 15 种，其中 Component/SceneComponent/PrimitiveComponent 为共享基础类型。

| 类型 | 直接基类 | 当前用途 |
| --- | --- | --- |
| Component | Runtime 对象语义 | 非空间能力、归属和生命周期 |
| SceneComponent | Component | 局部 TRS 与同 Actor 空间树 |
| PrimitiveComponent | SceneComponent | 反射的碰撞/物理配置及统一的物理参与合同 |
| StaticMeshComponent | PrimitiveComponent | 不可变网格资源、来源键、实例 Material，可启用物理 |
| CollisionComponent | PrimitiveComponent | 独立空间碰撞体与触发区域，默认启用 body |
| LightComponent | SceneComponent | Point、Rectangle、Directional、Sky |
| CharacterComponent | Component | Jolt 虚拟角色配置和输入 |
| RagdollComponent | Component | 布娃娃控制 |
| RagdollPartComponent | StaticMeshComponent | 布娃娃部件、局部静止位置、形状参数 |
| VehicleComponent | Component | 车辆配置和输入 |
| VehicleWheelComponent | StaticMeshComponent | 可视车轮及序号 |
| DistanceConstraintComponent | Component | 目标 Actor 的弱引用及距离范围 |
| ForceComponent | Component | 力和初始冲量 |
| KinematicMotionComponent | Component | 运动学速度和周期运动 |
| SceneQueryComponent | SceneComponent | 射线、球扫掠、球重叠与暂态查询结果 |

声明见 [World](../../src/world/world.hpp)、[Physics](../../src/physics/physics.hpp)。World 组件、BodySettings 与 PhysicsMaterial 使用头文件生成反射；专用物理组件由 NativeRegistration 发布到同一 Runtime。物理配置沿通用 `properties.body` 保存，运行时不注册旧 BodyComponent。[物理类型登记](../../src/physics/components.cpp)

原生 Actor 类型为 Actor 和 StaticMeshActor；项目 Source 可以扩展。当前没有正式 CameraComponent、AudioComponent、SkeletalMeshComponent。Box/Sphere/Capsule 使用同一个 CollisionComponent，由 BodySettings.shape 选择形状；无需为每种图元建立独立类型。

## 6. 具体缺口与设计风险

### 6.1 默认组成、存档与复制缺少共同的对象图机制

当前通用属性处理覆盖标量、枚举和嵌套 record，包括 BodySettings；公开业务引用被明确拒绝，容器也没有一般存档编码。TRS、网格与约束目标仍有专有恢复路径。这些路径尚不足以支撑通用 Class 默认组件图及内部引用重映射。[属性遍历](../../src/editor/fields.hpp)、[存档与复制](../../src/editor/scene.cpp)

一个具体缺陷：DistanceConstraintComponent.target 是隐藏弱引用；场景保存/恢复显式处理 constraint_target，但 duplicate_selected 只复制公开字段、空间关系、网格与 Body，没有恢复该目标。因此复制此组件会失去约束目标。此结论来自静态调用链，本轮未运行复现或修复。继续为每类组件加复制特判会放大维护成本，应由共同的引用编码、克隆和重映射机制解决。

### 6.2 Actor 空间挂接尚不可表达

Actor 当前只有成员关系、组件序列与根引用。`validate_parents` 要求根无父；`reparent` 要求同 Actor、禁止根换父且要求 World 停止。这意味着独立武器 Actor 挂接角色、拾取后再脱离，无法直接沿现有空间入口表达。[空间验证](../../src/world/objects.cpp)、[reparent](../../src/world/world.cpp)

只放宽校验也不足够：场景 parent 当前只编码 Actor 内部组件 ID。PhysicsScene 已按 Primitive 的世界位姿创建和回写刚体，但跨 Actor 根挂接仍须同时处理全图循环检测、World 归属、引用存档及父节点离场/销毁。成员和 Tick 顺序可以保持现有有序列表；空间依赖单独处理。

### 6.3 普通物理组成已统一，复合碰撞仍待需求驱动

PrimitiveComponent 承载空间关系与 BodySettings，StaticMeshComponent 和 CollisionComponent 共用物理注册、命令、查询及生命周期。PhysicsScene 按组件身份登记，一个 Actor 可拥有多个独立刚体；三角网格来自该 StaticMeshComponent 自身。历史事件携带 BodyHandle，旧代际无法操作重建后的刚体。[组件声明](../../src/world/world.hpp)、[PhysicsScene](../../src/physics/physics.cpp)

空间挂接不自动产生焊接或多形状刚体。动态子体独立模拟；移动物理祖先下的静态/运动学子体（含 Sensor）目前明确拒绝。复合形状、焊接及专用角色/布娃娃的通用查询映射仍未接入，不能把多 Primitive 等同于完整的多形状刚体模型。

### 6.4 StaticMesh 的资源能力领先于实例作者能力

`geometry::Mesh` 已有 UV、MeshPart、多个导入 Material 及图像引用，基础 renderer 按 part 使用资源材质。StaticMeshComponent 自己仍只有一个 `world::Material`，缺少可持久保存的逐槽实例覆盖；因此不能概括为“完全没有多材质”，也不能概括为“已有完整材质实例模型”。[网格资源](../../src/geometry/mesh.hpp)、[组件](../../src/world/world.hpp)、[renderer](../../src/renderer/renderer.cpp)

网格来源键与已解析不可变 MeshRef 的分工合理，应继续保留。需要补齐的是资产材质默认值、实例覆盖、稳定槽标识、反射编辑及持久化的一致规则。碰撞配置已接入 Primitive；任何合法空间位置上的 StaticMeshComponent 都可配置自己的物理能力。

### 6.5 enabled 的跨系统含义需要定清

World enabled 控制 Tick 和任务取消，Physics 据此退出，Light 提取会过滤禁用 Actor/组件；网格 render_items 则没有同样的 enabled 过滤。这可能包含编辑呈现的意图，但目前一个标志承载不同系统行为，不能让调用方凭名称猜测。[执行](../../src/world/execution.cpp)、[网格提取](../../src/world/world.cpp)、[灯提取](../../src/world/lighting.cpp)

建议明确运行启用、可见性和碰撞参与之间的关系，再决定是否引入独立控制项；不要先机械增加一组布尔开关。

## 7. 建议的最小完整方向

保留共享 Runtime 对象、Actor/Component、显式根、确定执行顺序及已经验证的生命周期。优先完善：

1. **类型驱动的对象图构造与归档。** 字段、内部对象引用、资源引用、默认组件和实例覆盖沿共同机制处理。加载/复制/类实例化共用构造、关系修复和领域验证；World 仍负责最终注册。
2. **可引用的核心 Class 与 Spawn。** 类引用解析到同一 Runtime 类型，并与 JIT 调用、默认值和组件构造结合；是否包装为资产由实际内容工作流决定。宿主和游戏执行经安全边界实例化完整对象图。构建失败不发布；发布后 Begin 错误遵守当前 Commit 和停机清理合同，不承诺回滚业务副作用。
3. **跨 Actor 挂接。** 保持独立 Actor 的身份及 World 成员资格，根 SceneComponent 可挂到另一 Actor 的空间组件。父变化、销毁、跨 World、keep-world、存档和物理位姿都明确处理；空间挂接不自动推导级联销毁或 Begin/Tick 顺序。
4. **按真实物体补齐网格与物理能力。** Primitive 已承担共享物理配置和参与合同；后续以逐槽材质覆盖、确有需求的复合碰撞为候选，避免先扩张专用演示组件。

后续可用三条真实链路评价模型：从核心 Class 引用生成两把互不污染的武器并保存重开；武器在角色挂点与地面之间切换而身份不变；门的实体、触发区和逐槽材质可编辑，复制后内部引用正确。这些仍是后续验收目标；已完成的 Primitive 增量及证据见[物理验收](../modules/physics/acceptance.md)。
