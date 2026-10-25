# World 硬闸复核与工业引擎对照

日期：2026-10-17。状态：研究记录。用户已在同日确定下文“已定决策”；实施由 World、运行时与编辑器负责人分别提出方案并推进，已落地部分见文末“实施状态”。现行行为仍以 [World](../modules/world/README.md)、[项目场景](../modules/world/project-scenes.md)、[物理](../modules/physics/README.md)和 [LANGUAGE.md](../../LANGUAGE.md) 为准。

## 问题

Entity 模型落地后，World、源码执行、编辑器与 Play 宿主中存在一批“硬闸”：整次拒绝、整帧失败、整个 World 停机、模式限制和严格数值规则。本轮要回答：哪些是工业引擎同样具备的安全不变量，哪些来自早期阶段截断、后来被写成永久契约，从而偏离产品意图。

对照代码为本仓 main `d2b6da2b`。

## 来源与限制

- **UE**：5.8.3 release 提交 `396c9f05`，通过本机 `gh` 稀疏检出 Core 数学、CoreUObject、Engine、UnrealEd/Kismet2、Renderer/Private，逐项读源码核实。`Build.h` 未检出，`ENABLE_NAN_DIAGNOSTIC`、`DO_BLUEPRINT_GUARD`、`WITH_METADATA` 是否在别处被覆盖未核实。UEFN Scene Graph 不在公开源码中，只能引用官方文档。
- **Godot / Bevy / Flecs**：读上游源码（Godot master，Bevy v0.20.0 与 main，Flecs master）。
- **Unity**：原生核心闭源，只能依据 UnityCsReference 的托管绑定、官方文档，以及明确标注的论坛和问题追踪记录。

## 结论

1. 现行模型的方向（递归 Entity、可选内联 TRS、组件不承担空间、组合父与空间参考分离、统一 Runtime 身份）无需推翻。
2. 多数严重问题是**失败策略**，不是不变量本身。“无效数据不交给渲染器”“Tick 中不能破坏遍历”都是正确的不变量，但实现成了整次调用失败或整个 World 停机。工业引擎在边界处便宜地检查，下游按对象降级并按对象归因。
3. 有三处来自阶段截断的硬化：
   - 严格正缩放：[对象模型研究](game-world-object-model.md)第 219 行写的是“首片”，现写在 World README 第 48 行。
   - 首个对象后封闭类型注册：[第一步记录](../first-step.md)第 34 行写的是“本阶段”，现写在 [运行时协议](../runtime-model.md)第 135 行。
   - 生成与销毁：`request/flush` 的契约存在，但 `src` 中没有任何宿主调用者。
4. UE 也有不应照搬之处：NaN 诊断默认只在 Debug 构建开启；C++ Tick 没有隔离，出错即进程故障。本项目“检查在开发与发布构建中同样有效”的规则应保留，再配合逐项降级。

## 逐项对照

| 本仓硬闸 | UE 5.8（源码） | 其他引擎 | 来历 | 判断 |
| --- | --- | --- | --- | --- |
| 单个坏数据使 `render_items` / `light_items` 整次失败，Play 宿主 `take()` 抛异常退出 | `CheckAndSanitizePrimitiveBounds` 对 NaN 包围盒 ensure 后置零继续（RendererScene.cpp:1362）；阴影阶段跳过坏图元（PrimitiveSceneInfo.cpp:2399）；物理按 body 拒绝（BodyInstance.cpp:1499） | Godot 仅 debug 构建拒绝单次服务器调用；Bevy 直接存储；均不整帧失败 | 契约（[光照数据](../modules/presentation/README.md)（当时为 world/lighting.md 第 14 行）） | 保留不变量，改为逐项排除并按对象诊断 |
| 任一 Begin/Tick/任务错误使整个 World 停机 | 蓝图访问空对象：告警、清零结果后继续（ScriptCore.cpp:3163-3247）；死循环只中止最内层函数（:1265-1295） | Unity、Godot 记录后继续；Bevy 默认 panic，但错误处理器可按错误或全局配置；Verse 停止当前 device 的 Verse 执行 | 首版设计，被写成契约 | 默认按对象隔离，策略可配置 |
| TRS 要求每轴严格正缩放 | 零、负缩放照常存储；镜像按行列式符号反转剔除（PrimitiveSceneProxy.cpp:908）；物理把零缩放夹到极小值（BodyInstance.cpp:2116） | Godot、Bevy 存储负缩放，Bevy 的 `GlobalTransform::scale` 显式处理负行列式 | “首片”后硬化 | 支持负缩放；零缩放按退化处理；保留有限性检查 |
| 首个对象创建后封闭类型注册 | 任何时候可注册新类，只在热重载之外重定义已有类时致命（UObjectBase.cpp:755-790、762） | Godot 只拒绝重复注册；Bevy、Flecs 运行期注册 | “本阶段”后硬化 | 允许追加新类型，已发布定义保持不可变 |
| 新增类或方法即 `rebuild_required`，重建 Runtime 并清空撤销 | 蓝图重编译按属性名复制到新实例并改写引用（KismetReinstanceUtilities.cpp:2409、3116） | Godot debug 构建 `reload_members` 保留属性 | 阶段限制被写成契约 | 新增类型或方法走兼容发布；已有布局变化再考虑原地重实例化 |
| 场景中一个未知字段即拒绝整个场景 | 未知属性按记录的大小跳过（Class.cpp:1956-1966）；类型不符先 `ConvertFromType`，失败告警跳过（:1821-1840）；改名用 CoreRedirects | Unity 保留 YAML 中的未知字段 | 契约（“不静默丢值”） | 原则保留，改为保留原始数据并诊断 |
| 回调中禁止结构修改，但宿主从不调用 `flush` | Tick 中 SpawnActor/DestroyActor 同步执行（LevelActor.cpp），新对象进入 `TG_NewlySpawned`（TickTaskManager.cpp:2140-2157） | Unity Destroy 延迟到帧末；Bevy 自动插入同步点；Flecs 在同步点合并 | 契约存在，产品路径缺失 | 保留重入拒绝；宿主在行为阶段后自动 flush |
| Tick 承诺按组合前序执行 | 组间有序，组内只靠前置依赖（EngineBaseTypes.h:83-110）；AttachToComponent 自动把父组件设为前置（SceneComponent.cpp:2490） | Unity 不保证实例间顺序；Godot 先看 `process_priority`，树序只打破平局；Bevy 未声明即无序；Flecs 按阶段与声明顺序 | 契约 | 收窄为“阶段 + 显式依赖” |
| 运行中禁止编辑成员 | PIE 复制世界，允许编辑，结束后还原 | Unity Play 模式同理 | 契约 | 产品决策；Play 已在独立进程，当前影响有限 |
| 字段范围只靠 World 中手写的类型分支 | `ClampMin`/`UIMin` 只是 `WITH_METADATA` 元数据（Field.h:916-919），运行时写入不受限制 | Houdini 参数模板区分范围与每侧 strict | 未设计 | 引入“可编辑范围 + 每侧锁”，锁由 Runtime 在所有写入路径执行 |

规模类问题不属于闸门，但同样来自截断，UE 对应做法如下：

- 组件逐个注册，图元批量并行加入，没有全局重建（ActorComponent.cpp:1967-2052、149-232）。
- 缓存 `ComponentToWorld`，变化时立即向子级传播，渲染在帧末统一同步（SceneComponent.cpp:760-827；ActorComponent.cpp:2713；LevelTick.cpp:1322-1371）。
- 领域通过 `CreateRenderState_Concurrent` / `OnCreatePhysicsState` 虚钩子接入（ActorComponent.h:841-871）；但 UE 仍保留 `UPrimitiveComponent` 这一层基类特例，说明完全去除“图元”层并非必要。本仓 World 写死的是网格、灯光、天光与物理类型名，超出了这个程度。

## 保留的安全闸门

- 回调、遍历与 GC 回调期间禁止创建、销毁对象与注册类型。UE 的 Tick 管理器同样保存原始指针，Tick 期间不能安全销毁 tick 函数。
- 每个 Entity 只有一个物理位姿写入者。
- 入口处拒绝非有限值。UE 的 SpawnActor 也拒绝 NaN 变换（LevelActor.cpp:514-517）。
- 已发布的类型定义不可原地修改。

## 形成原因

1. **为验收设计的严格性进入了产品宿主。** 首错即停让验收结果确定，但 Play 宿主沿用了会抛异常的 `take()`。
2. **阶段措辞在文档改写为当前契约时消失。** 严格正缩放已有实际代价：Dungeons 为 52 个镜像实例额外烘焙了 18 个网格变体（[Dungeons 示例](../../examples/Content/Dungeons/README.md)第 17 行）。
3. **不变量与失败策略被绑定。** 不交给渲染器无效数据，被实现成整个调用失败，而不是排除这一项。
4. **契约写好了，宿主路径未接上。** `request/flush` 只在验收中被调用，游戏逻辑在运行期无法生成或销毁对象。

## 已定决策（用户，2026-10-17）

1. Play 中单个对象的数据或脚本错误默认隔离到该对象（排除该项、停用其行为、报告对象身份与源码位置）；验收与编辑器测试可配置为首错即停。
2. 支持负缩放；零缩放可以存储，呈现与物理按退化处理并诊断；NaN/Inf 检查保留。
3. 场景加载保留未知字段的原始数据并诊断，重新保存时写回；只有结构性损坏才拒绝整个场景。
4. 反射字段提供可编辑范围与每侧独立的锁，对应 Houdini 的 strict min/max。未锁定时允许输入超出范围的值；锁定约束由 Runtime 在所有写入路径执行，越界时夹到硬边界。常见写法是锁定在范围边界上（`LockMin` / `LockMax`）；当 UI 范围远小于硬边界时，可以单独指定硬边界（`HardMin` / `HardMax`），例如 emissive 的滑条范围是 0–10，硬上限是 65504。
5. Tick 顺序收窄为“阶段 + 显式依赖”，Begin/End 的配对与逆序保留；World 核心只保留成员、组合、空间与生命周期，领域能力按类型登记。

## 方案审阅结果（2026-10-17）

- 失败隔离（World W1）：隔离单位是出错的 Entity 或 Component 本身，不扩散到组件、子 Entity 或所属 Entity；Begin 成功的对象在正常退出时仍恰好调用一次 End；基础设施和不变量错误在隔离策略下仍然停机。
- 追加类型（运行时 R2）：新增 struct、enum、class、全局函数，以及给已有 class 新增方法，都改为兼容发布；删除、改名和已有类型的布局变化仍需重建。
- 场景保留数据（编辑器 E3）：挂在文档 ID、字段路径或材质槽名上，随保存、复制、子树导出、Rebuild Types 和 Play 快照流转；其中的引用与活引用遵守同一套规则。
- 锁定字段越界时夹到硬边界；只要存在硬边界，非有限值一律拒绝。

## 实施状态与后续决定（2026-10-18）

经用户批准，本地 main 已快进到集成点 `604cbba8`，包含 W1–W4a、W4 的 Source 阶段方法绑定、R1–R3b、E3 第一步与编辑器窗口验收修复。R3b 规则迁移、Dungeons 镜像迁移、R5、W4b、E3 第二步与渲染帧率批次仍在各负责人的本地分支上，等待下一轮集成。以下后续决定由总负责人在方案审阅中作出，用户可否决：

- 帧循环（W4）：`World::update(dt)` 是唯一帧入口，阶段为 `pre_physics → 模拟系统 step → post_physics → late`，各阶段以命令应用和自动 flush 结束；物理作为登记的模拟系统，不再驱动 World。阶段内只承诺显式前置（传递闭包）。Source 方法为 `tick`、`post_physics_tick`、`late_tick`。
- 镜像灯光：灯光朝向按“忽略缩放幅值、保留符号”组合，镜像预制件中的灯随几何镜像。
- 需要空间由类型决定：新增无字段的空间组件基类，Primitive 与 Light 派生自它（同 UE `USceneComponent`）。
- 环境组件（W4b）：场景光照设置改为场景中某个 Entity 上的唯一环境组件；均匀天光并入环境组件，`LightKind` 去掉 Sky（同 Godot `WorldEnvironment`、Unity 环境光）。DDGI 体积存为 `Array<ProbeVolumeSettings>`，依赖 R5。
- 值数组（R5）：Array 值字段按元素寻址，元素段复用 `FieldId`（owner 为数组类型、index 为下标，即“owner 的第 index 个成员”）；元素可继承数组字段的范围；场景中每个数组最多 65536 个元素。
- 类型校验（R3b）：校验器只在提交和消费时运行，不在每次写入时运行；失败带结构化字段路径（`ValidationFailure`），不扩展共享的 `Error`。
- 帧阶段 CPU 计时：foundation 只提供机制，阶段标记由各模块自己声明，宿主启动时组装目录；数值只用于诊断，不作为验收条件。

