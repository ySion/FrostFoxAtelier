# 游戏世界与对象模型：研究与会审方案

状态：**I、II及III已本地验收，III的dev/release均为World acceptance: PASS (4/4)；IV由应用分支接续，尚未集成main，S3未启动。当前行为以[World模块文档](../modules/world/README.md)及正式公共头为准；本稿保留取舍与历史证据，不是第二份待批准排期。**

日期：2026-09-30。III实现基线为已验收的II提交`ce36a2ef71cfd31da39f98aa3c75004b69e77929`，含I提交`3985869a3c700ddddf190aae6291ea39d3616bf6`。唯一当前研究稿与实现位于`C:/Users/Arzuo/.codex/worktrees/actor-world/FrostFoxAtelier`，分支`codex/actor-world`；原共享根目录的本稿和三项词义修改已完整保全到此分支。第2节历史代码定位仍对应原main `9d33f4fc1c8a88481c93be4b166e6eb311456a1b`，不代表当前Core能力；I/II验收事实复用，不重复运行无关验收。

研究由对象模型负责人汇总。首轮三席分别调查生命周期、数据与执行、消费者迁移，独立结论后进行一次聚焦交叉质询。用户进一步要求正式基础派生后，复用同三席补UE类型合同、Godot/Stride对照和本仓映射，并做一次类型问题的交叉收口；原生命周期与存储证据复用。Runtime与应用负责人提供各自的合同候选及消费者delta。未新增专家或并列研究稿。

## 1. 本片采用的合同与研究边界

推荐建立独立的 **World 模块**：Actor 表达可组合的游戏参与者，Component 表达可独立增删的具体能力；二者均是同一 Runtime 的真实对象，以 `ObjectId` 为唯一运行期身份。用户进一步提出基础类型派生期望：正式的 Object→Actor、Object→Component→具体业务组件层次应由共享类型系统表达，不能以“都注册为object”或角色表代替。原main研究基线缺少该合同，现已由I补齐；历史说明见§4.2.1。World 拥有成员资格、组件归属及参与运行的规则。业务字段仍只存在 Runtime 对象中；查询索引、UI 绑定和绘制快照各有明确派生职责。

**本片采用普通GC组成：Actor离场且未被明确销毁时，普通强引用保留同一Actor及当前组成。** 这项选择经过§5公平比较后进入本轮派发，不由最初提议直接证明正确，也不承诺资源驻留或历史值快照。

Actor持正式Component定型强引用序列，附属Component采用强owner，必要关系元数据随对象存续。这里的“完整”只承诺身份、当前字段与组成关系，不自动保留World服务、资源注册、任务或撤销时的历史值。成员索引与管理登记不得以隐藏强根改变选定寿命。

本片组成的必要成本包括序列追踪、GC暂停时的形状变更、对象内关系寿命和窄受控访问，不能只补扫描回调。正式继承I只规定类型兼容和已声明引用边的追踪，不决定Actor/Component之间该声明哪些强边；目前没有发现I依赖C的具体耦合。

**本片close结束参与并撤根，明确destroy终结身份。** 析构旧World后仍可读取强持对象，并由对象协议终结整组。独立Transform和完整TRS已贯通；父子/多空间部件操作不在本片。

其余推荐可以独立成立：

- Actor 可以没有空间能力、没有可渲染组件；渲染只消费具备对应能力且仍参与呈现的组件。
- 组合语义与存储布局分开。保留Runtime稳定对象存储，World按组件类型组织查询索引，不引入另一套ECS实体身份或对象GC；索引的持有关系服从待选寿命合同。
- 结构变化在宿主静止点统一提交；回调或更新遍历中只提交请求。普通字段访问仍用现有 Runtime 路径，缓存字段标识。
- 编辑器的 Session 是 World 的编辑消费者；无窗口宿主直接使用同一 World，不依赖 Session、UI、窗口或 RHI。
- 整体替换固定 `SceneObject` 和位置专用路径，删除 `ObjectKey` 业务别名。UI 保留自己的控件身份；未来持久身份由文档/资产层定义。

close和destroy使用明确不同的合同，取舍见§5.3；World统一求S*R*T，renderer消费结果。基础类型派生与组成引用各有职责。

## 2. 原main研究基线的事实与缺口

以下路径、行号和行为固定于原main基线`9d33f4fc1c8a88481c93be4b166e6eb311456a1b`，用于解释迁移动因。IV已移除旧Scene，表中旧路径不再是当前实现入口；当前World行为见[模块文档](../modules/world/README.md)。表中“缺口”不等于本轮全部要实现。

| 事实与精确入口 | 可复用内容 | 新模型必须补齐或迁移的部分 |
| --- | --- | --- |
| 原基线`src/scene/scene.hpp`:8–45；`src/scene/scene.cpp`:17–32（已移除） | Runtime 唯一保存值，成员用根保活，字段路径冷端缓存 | 只有 `SceneObject{Transform{position}}`，单一 `object_type_`，最多 256 成员；无 Actor/组件协议 |
| `scene.cpp`:40–71、100–117 | 创建与目录插入分开；插入检查上下文、类型、重复身份；访问要求当前成员资格；宿主显式推进 GC | World 成员、组件 owner、启停及组合依赖；不能把现有 256 限额当引擎模型限制 |
| [runtime.hpp](../../src/runtime/runtime.hpp):34–80、159–175、264–296、359–409；[native.hpp](../../src/runtime/native.hpp):47、78、103 | 共享类型、`ObjectId(context,index,generation)`、根、强弱引用、字段访问和执行准入 | 没有Object基类TypeId、base_type/is-a、基类引用兼容、原生base地址调整或容器追踪；原生成员指针限本T，C++继承不会自动登记；ID尚无正式哈希 |
| [runtime.cpp](../../src/runtime/runtime.cpp):893、1249–1301 | 成组注册先完整验证再发布；成功 `create` 后封闭类型；失败创建不封闭 | 运行期新增/替换类型、对象布局迁移均未提供；添加已注册类型的组件实例不受“封闭注册”限制 |
| `runtime.cpp`:1238、1305、1446、1472；`runtime.hpp`:371–396 | 上下文/代际校验；引用写入屏障；只穿内联 record 的字段路径 | 组件字段必须换成组件接收者；不能假设 `Actor.components[i].x` 是现成字段路径；根不阻止显式销毁 |
| [execution.hpp](../../src/runtime/execution.hpp):76；[执行结构](../runtime-structure.md):53–103 | 同线程执行准入、执行根、显式 Coroutine、代码版本保留 | 无游戏系统调度；暂停协程仍持帧/代码，GC 不负责停用游戏行为 |
| [program.hpp](../../src/source/program.hpp):97–101；[program.cpp](../../src/source/program.cpp):376 | `request → prepare → try_publish`，兼容方法体更新 | 非通用方法表替换；不支持多类型业务目录、引用/记录调用参数、类型或布局热迁移 |
| [session.hpp](../../src/editor/session.hpp):20–55；[session.cpp](../../src/editor/session.cpp):9–109 | 预览直接写权威值，提交一条历史；删除前持根，undo 恢复原身份；成功才移动游标 | `Change` 只有 add/remove/position，单 Member 与 Vec3 载荷；没有组件关系或字段目标 |
| [workspace.hpp](../../src/editor/workspace.hpp):11–13；[workspace.cpp](../../src/editor/workspace.cpp):42–90 | 生产列表、数值控件、布局、搜索及输入生命周期 | `Cube N`、固定 XYZ，控件键使用 `ObjectKey` 算术编码；无真实组件目录 |
| [application.cpp](../../src/application/application.cpp):74–108、166–187、255–287、304–310、329–334 | 现有事件路由、取消编辑、accepted frame、窗口与完整帧链路 | 位置专用命令散布在事件、快捷键、失焦和关闭；每成员无条件输出一个 Cube |
| [renderer.hpp](../../src/renderer/renderer.hpp):35–44；[renderer.cpp](../../src/renderer/renderer.cpp):285–307、446–449 | `Cube` 是无业务身份的值快照，空实例可不绘场景；上传到 GPU 实例的路径可继续用 | 替换上游投影条件，不要求本轮扩展 RHI 或通用渲染器 |
| [editor.cmake](../../cmake/editor.cmake):2–8、22–30 | 已有无窗口场景验收骨架 | Scene 目前编入 editor_core；World 应成为仅依赖 Runtime 的正式模块 |

**已核对的复用边界：** `ObjectRoot::registered()` 只说明根所属 Runtime 尚存在，不证明目标仍活。恢复/操作需要 `type_of`、`hold(ObjectId)` 或实际访问的身份校验。`hold(ObjectReference)` 可以保留陈旧强引用，不能以“取得一个已登记根”替代目标存活检查。

### 2.1 不补造持久身份

应用负责人已检查 `src` 与 `acceptance`：`ObjectKey` 没有存档、加载、网络或外部格式消费者；当前保存行为是验收图片与报告。删除它不会丢失已实现的持久协议。

[旧编辑器方案](../first-editor-plan.md):65–80、95–102 曾把持久键职责预留给 Scene，这一职责不能无声消失。会审批准实施时，由应用负责人同步该计划及当前模块文档：运行期用 ObjectId；真正开发存档时，在文档/资产层建立持久键到当次 ObjectId 的映射。持久键不能取 ObjectId 的私有字段、内存地址或哈希结果。本轮不定义文件格式、不建立未被消费的持久键注册表。

## 3. 外部设计比较：采纳哪些机制

外部案例用于揭示约束，不直接决定项目范式。版本与来源见第 14 节。

| 案例 | 实际机制及适用条件 | 本项目采用的认识 | 不照搬的部分 |
| --- | --- | --- | --- |
| Unreal Engine 5.6 Actor/Component 生命周期文档 | Actor、组件注册、运行开始/结束和 GC 分属不同事件；组件注册才接入世界侧能力 | 构造/析构不能代替挂载、停用、离场；销毁和单纯离场要区分 [U1][U2] | 正式基础派生需做，但不复制全部UE类别、TickFunction依赖图或网络Actor协议 |
| Godot 4.5-stable Object/ClassDB/Node | ClassDB维护真实父类型；Object→Node→Node3D与Object→RefCounted分支不同；树parent、保存owner与存活策略不相同 [G1][G2][G5] | 继承成员查找和生命周期分类须有正式合同；空间只是部分对象能力 | 不把Node手动树释放与RefCounted分流搬成本项目另一套GC |
| Stride 4.3.0.2507 | EntityComponent使用CLR正式类型与引用；Entity组合组件、默认带Transform；ModelComponent通过处理器消费Transform [S1]–[S4] | 类型继承与实体组合分开；能力可以共享一个空间实例 | 不照搬所有Entity默认空间化；不把ComponentBase名称或IIdentifiable的Guid当作本项目对象寿命/身份合同 |
| Unity Entities 1.4.8 | 按 archetype/chunk 存储；增删组件会改变 archetype并迁移数据；启用状态与结构变化不同；命令缓冲在确定边界回放 [E1]–[E4] | 密集遍历有价值，但必须计算结构变化与搬移成本；明确提交边界 | 不将其可搬移数据假设用于当前 Runtime 原生对象；不引入另一份 Entity 身份 |
| Flecs v4.1.4 | 延迟操作在边界合并；表迁移、组件钩子和查询匹配有正式实现 [F1]–[F3] | 索引维护及结构发布应属于存储/世界模块；排队不等于已经执行成功 | 不把完整 Flecs world、实体 ID、流水线作为 Runtime 旁的第二个业务世界 |
| Godot 4.5 UndoRedo | 删除操作可用 undo reference 保留对象；预览后可只登记历史；其源码的历史对象处置可以真正删除对象，而操作失败路径并不提供本项目所需的整组原子恢复 [G3][G4] | 可达性、是否参与世界、是否仍可撤销是三件事；历史清理语义必须明示 | 不照抄任意 do/undo 回调列表，不以通用 UndoRedo 类名推导事务原子性 |

### 3.1 物理组织候选

| 候选 | 具体收益 | 真实成本与前提 | 结论 |
| --- | --- | --- | --- |
| 继续扩充一个固定 SceneObject | 当前代码最少 | 每加能力都修改总结构与固定 UI；不能表达独立组件身份、生灭和可选能力 | 拒绝 |
| Runtime对象组合+按类型的成员索引 | 身份/字段/GC不分裂；挂载已有类型不搬对象；能按能力批遍历 | 每对象当前仍分配，访问有身份解析/间接访问；组合边需要正式序列追踪 | 采用这一组织方向 |
| 把所有组件实值迁入 archetype ECS | 部分同质计算可获得连续数据、批量执行 | 必须解决现有原生地址稳定性、搬移/析构、引用追踪、语言类型及双身份；真实工作负载尚未证明收益 | 不作为首版；未来由 Runtime 存储契约和实际热路径驱动 |
| Runtime 权威对象 + 连续派生工作集 | 绘制、程序化计算等可批处理，只搬需要的数据 | 必须写明输入版本、结果回写与失效；不可另存一份可编辑组件状态 | 首版绘制投影即属此类，不预建通用同步框架 |

### 3.2 引擎类型对照的取舍

- **借UE的职责拆分，保留本项目的生产路径。** 真实Object/Actor/Component层次、基类访问与整链追踪值得采用；NativeRegistration和未来语言类型生产者仍发布同一TypeDefinition。当前没有理由再建UHT式平行生成流程、CDO/archetype默认对象复制、通用Outer/包系统、网络复制或GC cluster。空间派生与几何/碰撞Primitive是各自有代价的领域选择，不随“学UE”整套引入。[U3]–[U6]
- **借Godot的真实父类型查找，不复制脚本存储桥接。** ClassDB父链与原生ObjectDB身份清晰；其GDScriptInstance另有成员数组，Node/RefCounted又有不同释放机制。Frost的原生/描述型对象直接由共享Runtime管理，自有语言应成为定义生产者，而非再给每个对象旁挂一套脚本成员。[G5]–[G8]
- **借Stride的正式类型与受控组件集合，不复制默认空间和多重寿命协议。** CLR `IsAssignableFrom`、内部owner setter、统一集合验证与Transform处理器体现真实合同；Entity默认Transform、Guid标识、资源引用计数与CLR GC的分工属于其既有系统。本项目采用ObjectId及现有追踪GC，Actor仍可无空间。[S1]–[S4]

## 4. 统一责任与身份

```text
宿主：Runtime 生命周期、类型发布、World 与执行依赖的创建/关闭
  ├─ Runtime：TypeId / ObjectId / 真实字段 / 引用 / 存储 / GC / 执行准入
  ├─ World：Actor 成员、Component 归属、组合规则、启停、结构提交
  │    ├─ 组件类型描述：引用既有 TypeId，声明组合限制和真实能力
  │    └─ 查询索引：派生的 ObjectId/记录定位信息，没有第二份字段值
  ├─ Session：选择、属性预览、历史与可撤销脱离记录
  └─ 游戏更新 / 渲染提取：消费 World；源码执行仍走 source::Program
```

### 4.1 World 与 Runtime

World 显式借用 Runtime，后者必须活到所有 World、历史、相关任务和代码依赖完成关闭。类型注册由宿主统一完成，不让每个 World 构造函数偷偷建立另一份类型世界。一个 Runtime 可承载多个 World；不同 Runtime 对象不能互引。

当前建议不设Actor永久World affinity。成员归属仅由member_world表达：仍在A中的Actor不能直接attach到B；同Runtime、已经完全Detached且兼容目标的Actor，可以独立attach到开放的B。detach A和attach B是两个可分别失败的操作，不承诺原子或无缝转移，也不携带旧World任务/注册。目标预检须逐项验证组件依赖与接入能力；依赖特定World且无法重新接入的组件使attach被拒绝，不能凭同Runtime泛称兼容。历史restore的原World条件只属于该条历史，不定义Actor永久归属；首片不扩跨文档UI或多写机制。

World是宿主上下文，不要求成为Runtime对象，也不为世界归属再造一套Actor ID。Runtime原生可信绑定仍可能取得`Runtime&`；游戏对象的显式销毁须走对象协议唯一入口，由它在必要时调用所在World退出，再终结组成，不能直接逐个Runtime.destroy绕过关系清理。现有Core不提供权限隔离或销毁否决，这是一项可信代码合同。发现越权陈旧身份应报错并停止使用受损记录，不能补建新对象掩盖失效。

### 4.2 Actor、Component 与组合记录

- Actor是正式Object派生类型，承担参与者身份；可有零个组件，不内置必须存在的位置或渲染能力。无需为每种组件组合预制Actor子类；有实际类型差异时仍可沿同一Core合同派生。
- Component是正式Object派生类型；每个具体组件有自己的ObjectId、实际TypeId和字段。基类引用兼容由§4.2.3的Core增量提供，不能在World用C++指针强转模拟。组合只使用事先注册的类型。
- 每个已附属组件至多有一个Actor owner，不能同时属于两组。C下整体离场保留组内owner关系，单独移除组件后owner为空；owner反向边为强引用，保整组的代价见§5.3。
- C下Actor对象内的Component强序列是当前组合与顺序的权威，Component保存owner关系和启用意图；其他候选同样须有唯一组成记录。双向关系由同一组合操作维护，历史旧owner/顺序不能成为第二份live真值。
- `TypeId → 组合限制/执行或提取能力` 描述只引用现有类型元数据；它不是第二套字段、对象或语言类型定义。以 `Runtime::type_of` 校验实例，不凭 C++ RTTI 或注册字符串判断身份。
- 普通无身份小数据继续作为 record/value 内联。并非每个数值、材料参数或计算元素都升级成 Component 对象。

重复类型限制按真实能力声明：首片只安装一个根Transform；以后允许多个空间部件时，唯一的是根的指定而非所有Transform实例。其他类型若确有多实例语义可允许。不可用“查找第一个同类型组件”作为编辑目标或撤销身份。不另建角色继承、动态接口发现或任意查询语言。

### 4.2.1 正式类型派生与组合是两件事（原基线缺口已由I补齐）

用户提出的正式类型方向为：共享基础对象类型Object之下有Actor与Component两个分支，Transform/Cube/Counter等业务组件派生自Component。这里的Object是统一业务类型系统中的基类型，不是管理上下文的C++ `Runtime`类，也不是表示持有关系的`ObjectRoot`句柄。Actor拥有组件仍是组合关系，Component不因此派生自Actor。

现有统一注册、TypeId/ObjectId、对象存储与引用/GC已经具备；`TypeKind::object`只说明实例类别。它没有声明base type、`is-a`、基类引用接收派生对象或继承成员布局。即使C++写出`struct Cube : Component`，也不代表Runtime、描述型定义或语言编译器认识这一层次。

正式派生必须由Runtime线统一定义：基类型合法性与成组解析；精确动态类型和基类型兼容；基类引用赋值/接收者检查；继承字段的标识、布局和定位；基/派生构造析构及GC边遍历；原生C++子对象地址调整与描述型表示；语言/JIT采用同一关系。不得让World用字符串表或C++ RTTI补出另一套`is-a`。

Runtime负责人的定向核对进一步定位：`Runtime::Runtime`（`runtime.cpp`:795）只内建`core.f32/core.u32`；`validate_reference`（1324/1336行）与`write_reference_at`（1472行）使用精确类型；`resolve_path`（1446行）/`call`（1546行）要求字段/方法所属类型精确匹配；GC从实际slot类型（468行）经`trace`（735行）扫描登记字段，发布时计算`contains_references`（1061行），尚无base链。`NativeLayout`（`runtime.hpp`:234）也无基类地址调整合同。新增继承必须同时接通这些路径，不能只放宽一次引用赋值比较。

源码侧也须明确接入：`source/detail/bridge.cpp`:35起收集绑定类型标量字段，352行精确校验receiver；`source/program.cpp`:223的协程receiver也精确校验。现有类型快照没有base依赖。源码class语法可以后续，但已支持的原生/描述型类型绑定必须知道继承关系，或对尚未接通的基类receiver组合明确拒绝；不能宣称Core继承完成后JIT自然全部支持。

建议以单一对象类继承为首个Core合同范围，不默认追加多继承、虚继承、接口系统、自动下转型或完整虚方法分派。这些边界已在I中定稿；实现与端到端证据由Runtime负责人交付，III复用。

角色描述可以声明“该组件可提取Cube”“需要空间目标”“首片唯一根”等业务能力和组合限制；类型层次、成员发现与兼容性只来自Runtime。即使正式`Component`基类引用已成立，动态组件集合仍需要独立的GC追踪和写屏障合同；继承不会自动使`vector<Component引用>`成为可追踪字段。反过来，可追踪引用集合也不自动提供继承。

### 4.2.2 候选类型图与选择依据

推荐浅层正式派生与独立空间组合：

```text
core.Object                         共享对象类型根，无World/空间/tick负担
├─ game.Actor                      World成员与组件组合主体，可空组
└─ game.Component                  owner/启用/挂接运行的共同协议
   ├─ game.TransformComponent      local TRS、空间parent与根/部件语义
   ├─ game.CubeComponent           几何/颜色，显式绑定同Actor的Transform
   └─ game.CounterComponent        无空间行为示例
```

`Object`不在payload重复存一个ObjectId；实际身份与动态类型来自Runtime slot。`Actor`可以指定零个或一个空间根，不能继承Transform来强制每Actor空间化。`Component`不是装饰类名：任何具体组件都遵守唯一owner、author-enabled、结构变更准入和生命周期编排；这些是正式基类协议，不要求全变成公开可写字段。

owner/author-enabled只保存一次，随Actor/Component对象自身存续，World通过受控组合操作修改。有效启用状态由World阶段和这些权威值推导。字段`exposed`目前只有二值，公开可写owner会绕过结构不变量；以只读基类协议加窄受控关系操作表达，不假装现有FieldId已支持protected/readonly，也不为避开访问缺口复制owner表。离线组成的查询不依赖已经撤销的World活动索引。

UE式空间派生也是完整候选：

```text
core.Object
├─ game.Actor
└─ game.Component
   ├─ game.CounterComponent
   └─ game.SpatialComponent        自身拥有local TRS，可实例化为空pivot
      └─ game.CubeComponent        自身也是可独立挂接的空间节点
```

UE 5.6还在SceneComponent之下提供PrimitiveComponent的几何/渲染/碰撞共同职责；本项目目前没有这些共同实现，不预建同名空中间层。UE的UClass本身是UObject元对象，但本项目继续让TypeId/TypeInfo承担类型身份与描述，不为模仿UE把类型信息另分配成业务对象。[U3][U4]

| 具体用例 | 独立Transform与能力绑定（推荐） | Spatial→Cube派生 |
| --- | --- | --- |
| 空Actor→加一个Cube | Actor+Transform+Cube；Cube指向Transform | Actor+Cube，Cube本身含继承TRS，可指定为root |
| 移除Cube、保留空间锚点 | 删除能力后原Transform身份/值保留 | 若Cube就是root，其空间随它退出；要保锚点，须事先用空Spatial作为父pivot |
| 车辆与独立炮管，各有几何 | Actor+两个Transform+两个Cube，两个空间节点 | Actor+两个Cube，两个空间节点；几何与空间节点同生灭 |
| 车与炮管几何可拆、锚点保留 | 仍是上面的五个对象、两个空间节点 | 使用两个pivot+两个Cube+Actor，也是五个对象，但四个空间节点 |
| 渲染与其他能力共享同一姿态 | 共同绑定同一个Transform，没有额外局部自由度 | 可共同挂父pivot，但每个Spatial能力仍有自己的local TRS；非空间能力也可另行引用指定Spatial |

上述对象数只描述这些建模方式，不是性能排名。Spatial方案的local与parent world不是错误副本，而是不同自由度；它更直接表达独立pivot，最简独立部件通常更少对象。独立Transform方案多一个空间引用和查询连接，并要在移除/重绑时维护依赖，但能力寿命与空间锚点明确分开，多种能力共享坐标系更直接。基于“真实能力组合、可独立移除、允许空间共享”的引擎目标，推荐后者；不是因为现有代码只画方块。如果产品把每个可渲染能力本身定义为空间节点，可改选Spatial方案，并整体替换相应绑定/删除合同，不能并列维持两个默认模型。

**三种边各有唯一权威：** 类型继承在Runtime的base链；组件归属在选定的唯一对象组合记录；空间parent在Transform空间关系。查询、生命周期目录、UI与renderer都消费这些来源，不各自恢复或推断另一套继承/owner/parent。

### 4.2.3 Core最小正式继承合同（I已会审并派发）

以下记录I-a/I-b合同，交付进度见文首。它借鉴真实类型关系与完整追踪，不复制外部引擎ABI；II通用引用序列与受控操作同样不决定Actor/World业务寿命。

1. **根与发布。** Runtime内建`TypeKind::object`的`core.Object`，无业务字段、无物理基类负载，可按现有空对象规则create；公开`TypeId object_type() const noexcept`。它不要求所有C++类新增对象头部或继承一个C++Object类。`TypeDefinition`末尾增加`std::optional<TypeReference> base{}`，`TypeInfo`增加`std::optional<TypeId> base{}`；根无父，object省略base时解析为core.Object，record/scalar不允许base。显式base沿现有TypeReference解析已发布TypeId或同批name/native representation，支持前向关系。
   `NativeRegistration<T>::base<Base>()`设置唯一base声明和本段地址适配，要求public、无歧义的合法T→Base转换以及正式object基类型；只登记本T新增成员。不调用它时，T仍是Runtime中core.Object的派生类型，不从C++RTTI猜父类。成组注册完整验证后原子发布；无效/非object父类、继承环、与祖先同类成员重名（含hidden）报`invalid_definition`，原有`duplicate_name`等错误及成员命名空间规则保持。首次成功create后仍封闭，不增加abstract标志或元类对象体系。
2. **动态类型与引用声明分开。** 选定签名为`Result<bool> is_a(TypeId actual, TypeId expected) const`、`Result<ObjectReference> cast_reference(const ObjectReference&, TypeId target) const`和`Result<ObjectRoot> hold(ObjectId object, TypeId declared_type)`。`type_of(id)`始终给slot实际最派生类型；is_a对有效本Runtime对象类型含自身/祖先返回true，无关返回false，无效/跨Runtime/非object参数报`invalid_type`。
   cast只改变声明视图，保持原ObjectId与强弱形式，不产生root。上转/同型可保留空或陈旧身份；下转只在相关继承链内，非空时检查活实际类型，陈旧报`invalid_object`，实际不匹配报`type_mismatch`；相关typed null可转，无关null不能绕过兼容。弱引用不会因cast变强。
   `hold(id,declared_type)`先验证存活及actual is-a declared_type，成功才登记声明视图根；原`hold(id)`仍取得实际类型视图，`hold(ref)`仍可保存合法空/陈旧强引用。observe保持声明类型，try_acquire保持声明类型并取得强根，弱null/陈旧仍为nullopt。`write_reference`只接受源声明类型向字段声明类型的可赋值关系，不暗中下转；强字段不接弱，弱字段可接兼容强/弱。槽仍仅存ObjectId，读取按字段声明重建视图，slot.type不变。`ObjectReference::operator==`仍比较完整引用值；跨声明视图的对象身份比较用id()，不悄改既有相等合同。
3. **成员身份与地址。** `TypeInfo.fields/methods`提供祖先在前、本层在后的公开有效列表；继承条目保留原FieldId/MethodId和声明签名，不创建派生副本ID。Core私有存储/追踪区分本层字段与基段；hidden不进公开列表，但参加冲突校验和GC。find_field/find_method使用同一列表。
   read/write在object段允许祖先FieldId.owner，先adjust到声明基段再执行原locator/offset；进入inline record后仍严格该记录类型，不跨引用边。call允许派生receiver，self保留实际对象id，storage调整到声明者基段，signature.receiver不改成D。不适用成员owner用现有`invalid_member`，陈旧receiver用`invalid_object`，失败先于原生方法或字段写副作用。公开成员不重复，应用不能用显示名代替声明身份。
4. **表示与构析。** 见下表；NativeRegistration显式发布真实基类转换，不能猜offset为零。原生完整T只构造/析构一次，由C++完成其基类生灭；描述型派生由Runtime编排合法基段与新增字段，失败只逆序清理已成功构造部分。
5. **GC。** 从slot实际类型扫描完整base+derived声明（含hidden引用和内联record），每条祖先字段只扫一次；`contains_references`和访问/追踪计划包含base。以Base引用持有Derived不能漏掉Derived自己的唯一强边。地址适配只定位，不分配或执行游戏行为，保持现有屏障、遍历准入与代际失效合同。
6. **既有source/JIT同步闭合。** `bridge.cpp::bind_type`消费有效成员列表，把完整祖先链及inline record纳入依赖；继承native method的signature.receiver保持祖先，按binding.type is-a receiver校验。`program.cpp::Metadata::matches`包含base字段，Contract保存祖先依赖快照，首次声明固定binding.type静态视图。
   `bridge.cpp::execute_admitted`和`ProgramAccess::start_version`在用户副作用前共用receiver校验/持根路径。`ExecutionFrame`增加`Result<void> hold(uint32_t slot, ObjectId object, TypeId declared_type)`重载，调用Runtime对应hold，直接在self槽建立binding.type声明强视图；不先登记实际类型临时root再复制规范化。既有重载不变，槽内observe/try_acquire、change_reference/same_reference遵守绑定声明视图，同一ObjectId不变。
   基类Program只见基类有效目录，派生Program包含祖先成员，sync/async、Entry/Lease和挂起帧一致。纯标量无receiver环境保持原行为；源码new仍构造当前绑定类型，不构造接收者动态类型。字段访问仍经Core；发布只换兼容函数体，不改base/布局，不开放多类型目录、class语法或虚slot。描述型派native由宿主定义生产者提供。

**原生地址适配的存放位置：** `NativeLayout`增加`void* (*base_address)(void*) noexcept{}`，仅表达当前原生完整表示到直接原生基段的调整；父类型只在TypeDefinition.base声明，不复制到NativeLayout。NativeRegistration生成`static_cast<Base*>(static_cast<T*>(storage))`的thunk，不猜零偏移。手写native回调仍是可信注册合同，Core不宣称能证明任意错误回调安全。描述型布局器知道自己的基段，不让描述型定义提供native base_address。

| 派生/基类表示 | 推荐首增量范围与必要合同 |
| --- | --- |
| native / native | 支持真实、可访问、无歧义的非虚单基类；编译器生成Derived*→Base*调整thunk，完整T负责构析 |
| described / described | 支持Runtime基段与新增字段布局；对齐、构造、失败清理和析构顺序明确 |
| described / native | 纳入I：实际构造完整大小/对齐的native Base，再在其完整sizeof范围之外追加描述字段，不复用尾padding；可继续叠加描述型派生层。Base构析一次，动态类型为D，但不冒充C++D/RTTI/vtable；这是未来语言组件复用native基类的关键接线 |
| native / 有存储的described-only基类 | 报invalid_definition；无真实C++基子对象不能靠同名类型伪造。无负载core.Object根是统一引导规则的例外 |

I包含described/native这条接线，省略它不满足本稿I完成门槛。当前语言没有class声明语法；I由正式TypeDefinition生产预注册描述型派生类型，再让现有源码/JIT消费，同样属于共享类型世界，但不代表源码已能声明整个类。类型元数据及布局/追踪计划由Runtime拥有，Runtime结束后失效；现有Program、旧Lease和挂起帧的Runtime/宿主依赖寿命要求继续保持。

**方法选择边界。** 首增量MethodId固定选择已注册Invoker，Core没有依据实际TypeId寻找override的虚槽。原生Invoker内部可能使用C++虚调用，这是其原生实现行为，不代表描述型/语言也能发布override。需要固定基类行为时可用显式限定调用thunk；当前成员指针模板不能自动识别virtual，不声称新增一个标签就能检测禁止。World挂载/启停/卸载的结构步骤不可由派生覆盖，行为扩展使用明确绑定并遵守回调约束；统一虚覆盖、签名兼容与派发表另行定义。

类型身份是Runtime内的TypeId，实例身份是ObjectId及其generation；逻辑类型名称、定义版本、代码版本是不同概念。首版类型定义不可变，Program/Lease管理代码版本；不新增一套“组件版本ID”代替现有身份，也不把兼容函数体替换当作类型重载。多继承、接口、泛型、源码class、一般引用参数ABI、运行中改base与布局迁移不随本合同自动纳入。

**I的显性变化：** object具有真实Object祖先；基类引用、成员和Program接收者可以接受派生实例；TypeInfo公开列表包含祖先成员；原生基段与描述型尾字段共同追踪。**继续保持：** 同一类型/实例身份、代际失效、强弱差别、native借用限制、首次create封闭、record内联规则、FieldId不穿引用、MethodId固定Invoker、已有标量参数/结果及单绑定源码目录。继承不附带新的GC、组件权限或虚分派体系。

### 4.3 空间模型：三个独立维度

以下是拟定的核心语义，不是当前代码能力。Actor可以没有空间能力，非空间Counter无需Transform。空间关系不改变Actor/Component身份和组件owner；局部空间值是权威数据，世界矩阵是派生计算结果。

| 维度 | 代表用例与核心影响 | 首片取舍 |
| --- | --- | --- |
| 完整TRS：位置、旋转、缩放 | 一栋房子换朝向、一个箱子改变尺寸，不需要任何父子层次；只有position不能表达基本放置 | 建议首片定义并贯通完整TRS值、求值与绘制提取；不把“位置向量”定为完整Transform。Inspector专用旋转交互可后续，但非默认旋转/缩放必须经正式宿主路径可设置并正确出图 |
| Actor间父子空间关系 | 人物跟随车辆运动，两者仍有独立身份、组件和生灭；车不是人物的组件owner | 核心现在定义单父、无环、同World和显式重挂规则；首片可以不开放挂接操作，不把“不实现”写成“关系尚未定义” |
| 同Actor多空间部件 | 一台机器的机身、转台和炮管分别转动，却属于同一Actor | 核心允许多个空间组件、指定至多一个Actor根；首片只创建一个根，不实现部件编辑器或骨骼/socket系统 |

**TRS值与计算。** 局部位置用米，旋转用单位四元数，缩放为无量纲三轴值，默认分别为零、单位旋转、一。沿[现行坐标合同](../modules/renderer/contract.md)：左手系、+Y向上、row-major存储和行向量；局部矩阵`L = S * R * T`，父级存在时`W = L * W_parent`。首片接受有限位置、合法归一化旋转和正且非零的有限缩放，负缩放/镜像与奇异零缩放不默许进入未定义的剔除或逆变换路径；范围策略归正式字段/变换操作。

非均匀缩放与旋转叠加后，世界仿射矩阵不一定能无损分解为TRS。核心不把派生世界矩阵再分解写回第二份可编辑TRS。`keep_local`重挂保留局部TRS；`keep_world`需要计算新局部变换，不能表示为合法TRS时原子拒绝并说明原因，不静默丢掉shear。首片没有父级，但这些约束现在确定，避免以后把矩阵传播做成另一份权威状态。

首片变换修改经World的完整TRS校验/提交，不能公开几项字段后让renderer自行修正非法四元数或缩放。可信C++/直接Runtime写入并不自动履行这些领域约束；若后续允许源码直接改Transform，必须接到同一变换提交/校验政策。当前JIT纵向例选无空间Counter，不以该例证明任意空间字段写入合同已经完成。

**一套空间父关系。** TransformComponent记录局部TRS；空间parent逻辑上引用另一个Transform对象。内部部件的父链应归同Actor并最终抵达其指定根；Actor根可以挂到另一Actor的根或空间部件。完整空间图无环、每个节点至多一个parent，活跃关系两端必须在同World。根“唯一”指每Actor的根指定唯一，不是禁止该Actor以后出现多个Transform。空间parent是非拥有关系，不能使父Actor隐含拥有/级联销毁子Actor，也不以C++类继承表达它。

挂接、重挂和移除空间节点走World结构入口，同时验证成员、owner、无环与变换政策。首片未开放父关系时拒绝相关操作；不先造空调度器或图查询框架。退出World/删除父Actor时，若仍有操作组外的空间子节点，要求命令显式给出重挂/解除关系方案，否则拒绝；不能自动销毁独立子Actor，也不能让它突然跳到局部坐标。相关恢复记录在该能力实施时保存空间边与变换政策，恢复时重验端点身份。

**组件如何使用空间。** Cube等需要空间的组件明确以观察引用绑定同Actor的一个Transform；Actor组成负责保活空间对象，绑定本身不另建立寿命拥有者。首片绑定唯一根，未来可绑定机身或炮管。多个渲染/业务组件可以共享一个空间目标，不能各复制Actor位置。添加Cube前满足依赖；UI可把“添加缺失根Transform+Cube”作为一项明确组合动作，组件列表真实显示两者。移除被引用Transform时必须同时显式重绑/移除依赖者，否则原子拒绝。换根同样需要处理其余部件和绑定，不悄然挑第一个实例；离线引用目标失效时能力不可参与运行，不解引用旧地址。

**实施边界。** 本轮只修改研究。未来首片纳入TRS意味着需由renderer负责人配合扩展当前仅含position的Cube提取/实例变换合同，应用补相应端到端证据；这是明确的局部接口增量，非完整渲染器重写。Actor间挂接和同Actor多部件的操作/传播暂不纳入首片，但核心责任与不变量已经定义。若后续才增加parent字段或调整布局，需在新Runtime上下文预注册新定义并迁移消费者；封闭注册不支持运行期自动升级布局。

## 5. 生命周期：统一状态与引用合同

本节状态和转换已进入III实现与验收：有效但失活、close全体退出活动、普通引用保组、强owner和撤根后的离线存活各有明确操作。状态不替代引用合同，未运行不等于不可见。

### 5.1 权威事实、四种状态与允许转换

**三层事实由各自入口维护，由Actor对象协议统一查询。** Runtime负责ObjectId是否仍对应活对象；World负责唯一成员关系、运行模式及实际接入/退出；Actor保存运行enabled意图。对象模型据这些事实返回状态，调用者不自行拼接条件，也不能直接写state/active。有效运行资格可以派生，已建立的任务/注册则必须真实维护，不能因布尔条件变了便假称已清理。GC的标记颜色、扫描进度和待回收时机不是Actor业务状态；active不影响强边追踪。

成员关系的唯一事实为Actor内受控、非拥有的member_world关系，由World结构入口维护；World的成员根目录与ObjectId→位置索引负责保活/遍历/反查，不另保存可独立修改的state或第二份归属。提交保护内同步维护该关系与索引，脱离/关闭时先清关联再释放World寿命。Actor.enabled由同一入口修改，World负责运行模式与接入记录；ActorStatus是只读派生快照，不公开set_state，不通过复制状态修补索引不一致。

| 对外状态 | 身份与成员事实 | 读写、游戏执行与呈现许可 |
| --- | --- | --- |
| Detached（未入场） | 身份有效，无World成员资格；可能是新建或已脱离对象 | 可读数据/当前组成，可经对象协议明确终结或预检后加入World；首片Session不允许离线编辑。不能执行World行为、tick或进入其渲染提取；内部初始化不是通用离线写入口 |
| AttachedInactive（已入场、未运行激活） | 身份有效、仍是成员；World未运行、Actor被停用或运行接入未完成 | 可读；编辑模式下可经Session写入。没有游戏tick/任务资格；仍可按编辑呈现能力显示 |
| Active（运行激活） | 身份有效且是成员，World正在运行、Actor.enabled为真，所需运行接入成功 | 允许已注册的游戏执行；首片不同时开放Session编辑。是否产生绘制仍由呈现能力决定，空Actor也可以Active |
| Invalid（身份失效） | 查询ObjectId时发现空身份或其对象已终结 | 仅能报告失效，不能读取Actor字段/组成、运行或恢复；相同登记位置的新代际不是该Actor |

Invalid是对身份的查询结果，不是销毁前写进Actor的枚举值。查询先验证有效Runtime和ObjectId上下文/代际，再判断活对象是否为Actor，最后读取关系；跨上下文、活的非Actor对象分别报合同错误，不伪装成Invalid。通用查询不以调用方World筛选状态，活Actor属于另一World仍然有效；针对错误World的操作报归属错误。不建tombstone区分GC与显式destroy。Runtime结束后宿主不能调用已析构的Runtime查询。进入/退出可有内部过渡保护；过渡中的重入状态查询报“正在转换”，不暴露半更新状态或机械复制UE回调名为枚举。稳定点只返回上述四态。

**运行与呈现分开。** 此处Actor/Component.enabled表示游戏执行启用意图。World.stop或停用对象撤下游戏任务与模拟接入，保留仍合法的编辑呈现；detach/close才撤掉相应World的全部参与，包括呈现注册。渲染提取消费成员资格、呈现通道和具体能力的就绪结果，不以Active过滤编辑对象，也不凭有效ObjectId猜资源可用。首片不额外新增visible开关；已有绘制能力决定是否产生实例。

**组件有独立资格。** 组件运行激活要求其身份有效、仍附属当前Actor、Actor为Active、自身enabled及依赖满足、相应注册已成功；它不因Actor为Active便无条件tick。组件呈现资格单独按所属成员与呈现能力检查。移除组件先结束其参与，再解除Actor序列与owner边；Actor可保持原状态。强owner已进入本片实现，不随组件激活/停用增删强边；GC始终按正式引用图追踪。

下表给本片合同的稳定结果。每项变化经§6统一入口提交，准备失败不假称已转换。

| 操作与合法起点 | 完成后的状态 | 身份、组成与参与结果 |
| --- | --- | --- |
| 创建Actor | 新身份为Detached | 构造建立有效对象与初始组成，不自动取得运行/绘制资格；创建并加入的便利操作沿同一链完成 |
| 加入/restore：Detached，目的World开放且关系/依赖有效 | 编辑或停止World为AttachedInactive；运行World在接入成功后可为Active | 普通attach可接纳同Runtime兼容Detached对象，保同身份与当前组成；历史restore还须满足该条记录的原World/顺序条件，不抢占其他World成员 |
| World.start，或对成员set_enabled(true) | 条件满足的成员由AttachedInactive到Active；World未运行时仍为AttachedInactive | enabled是意图，start不强制改掉成员的禁用意图；组内禁用组件仍不运行 |
| 对成员set_enabled(false) | Active到AttachedInactive，原本未激活则不变 | 保身份、组成、成员根及可用编辑呈现，收束该对象游戏活动；不把停用改成detach |
| detach/历史Delete：AttachedInactive或Active | Detached | 先收束全部World参与，再撤成员/根；组成关系保留，历史或外部强引用按选定引用合同保活 |
| remove_component：所属Actor仍为成员 | 组件不再附属且不运行；Actor状态按剩余能力重算 | 保组件身份、清双方归属；摘边前取得调用者/历史需要的根，依赖不满足则整项拒绝 |
| World.stop | Active成员到AttachedInactive，其他成员不变 | 保身份/组成/成员及enabled意图，结束游戏活动；可继续显示和编辑 |
| World.close | 全体退出活动；仍有效的原成员为Detached | 不再接纳操作，清任务/注册和成员根；对象身份由后续GC或明确destroy终结 |
| 对象协议destroy：有效Actor，满足静止点及终结前置 | Invalid | Attached先经所在World退出，Detached不依赖旧World；随后共用当前组成清理与Runtime终结链，强引用不能否决，不可同身份undo |
| GC：对象已无可达强路径 | 回收前保持原状态，实际终结后为Invalid | World成员根应保证成员不会被GC；不可达离场图可回收，析构不补做游戏退出。仅持弱引用不保活 |
| Runtime结束 | 全部身份终结 | 普通根不延长Runtime自身寿命；宿主先完成World、任务及资源关闭 |

裁剪history/redo或销毁Session只撤对应根，不是Actor状态转换命令；若还有强路径，数据仍在，否则后续GC可令身份失效。stream unload须明确采用停止参与、detach、保存或destroy中的哪种操作，不能仅靠“卸载”一词推断状态。保当前组成也不代表保存删除时的历史值，见§9.2。

### 5.2 定型引用序列与受控关系操作

Actor序列声明元素类型Component、强形式固定，元素实际类型可为具体派生组件；GC据slot动态类型追踪完整对象。序列不新增对象身份，不需要variant容器、语言泛型或独立组件RTTI，可写序列不协变。

必要Core合同：

1. 序列字段storage/metadata、insert/replace/erase/clear和只读枚举；Core校验元素声明类型、同Runtime、强弱与引用屏障。World校验唯一owner、重复挂载、组合依赖和成员资格。
2. 首片限对象字段（含继承），由包含对象构造/销毁，原生与描述型遵守同一序列生命周期；通用record容器复制/赋值与参数ABI暂不开放。Actor的拥有组成不允许借公开整序列赋值绕过唯一owner，须走组合入口；未来若开放普通序列复制，语义应是共享引用而非克隆对象。
3. GC游标可能跨collect_step保存进度。扩容使地址失效，erase会左移未扫元素；形状变化必须修正/取消并重新安排追踪，逐元素计预算，不只给push加shade，不跨暂停保留元素裸地址。
4. 只读借用明确失效边界；引用遍历回调期间拒绝修改。Actor销毁、序列替换、结构提交不能留下悬空游标或span。
5. Actor组成/顺序、Component owner关系及author-enabled随对象自身存续，反向owner边为强引用。World只读协议与受控写入口读取同一记录；“增加GC scanner但记录仍由历史拥有”不符合C。
6. 关系访问采用窄受控宿主操作，禁止公开可写owner/整个组成序列绕过World，也不暴露可长期缓存的裸地址。现行hidden字段缺少普通World可用的访问入口，这是真实配套，不假装现有exposed二值已经提供protected权限。
7. 序列原语只负责通用引用/值/GC，不认识Actor、owner或游戏运行。双向边和生命周期由游戏对象模块的一次组合提交编排。其准备与提交所需的容量、屏障和失败保证必须在共享接口中明确，不能让Session手工串接若干可能半失败的写入。
8. 首片由原生World使用该协议。源码容器表达式、引用返回getter与完整多类型调用另行接线，底层可追踪不等于源码语法已全部开放。

### 5.3 选择依据：公平比较与反例

| 候选 | 适用需求与收益 | 必须接受的代价 |
| --- | --- | --- |
| C：普通Actor强引用保当前组成 | 需要用普通引用继续访问同一离线Actor、装配后重新入场；一个权威对象图即可表达 | 任意长期强引用都能延长组成及其强依赖的内存寿命；需有可靠的退出/重新接入协议；不保存历史状态 |
| World绑定寿命，终结时destroy | 关卡或运行域要求确定终结其对象；普通引用接受失效 | 世界退出是否终结必须明确；重建通常产生新ObjectId，不能承诺同身份undo；引用消费者必须处理失效 |
| 显式离线组 | 真有同身份离线需求，但要由明确持有者控制保留期、回收或转交 | 增加一项可见的持有合同；唯一组成记录应随对象或被转移，不得复制第二份live图；普通引用能否否决终结仍须选定 |
| 状态快照后卸载实例 | 远域休眠、保存、批量回收；不需要每个离线实例长期保持Runtime身份 | 恢复通常创建新身份，需要单独处理持久引用映射；快照有记录/重建成本，不能冒称原对象仍在 |

这不是四套并行实现计划。最终选择一套对象寿命合同，detach、永久destroy和保存等不同操作可以在该合同内各负其责。原B把World拥有和显式脱离记录合在一起；本轮将它们分清后公平比较，不再因用户曾提出C而直接排除。

**决定C适用范围的三个反例：**

1. Delete后外部强引用修改离线Actor字段、移除旧组件并增加新组件。重新加入World只能恢复当前图；回写删除时快照又可能覆盖合法修改。保活没有解决历史恢复，见§9.2。
2. 任务仅持卸载区某NPC，但其组件再强持其他Actor、任务帧或资产，整片可达图可能留下。真正外部根可使强环持续可达；追踪GC不是流式预算控制。停止世界注册可释放相应资源，却不自动释放对象强持的数据和资产。
3. 只持组件时，弱owner允许Actor先被收集，组件必须支持无活owner的离线状态；强owner保持Actor与所有兄弟组件，可能远超消费者所需。两者都合法，选择依据是组件消费者合同，不是“强环必泄漏”。

**场景不能单凭名称投票。** 临时离线装配和明确要恢复同身份的暂停有利于C；对象池只有复用同一逻辑对象时可利用C，若用于新逻辑对象，应终结旧身份并推进代际，防止遗留引用操作新用途对象。关卡转换可以保留少数角色，也可以让其处在较长寿命World，不能据此预先承诺跨World迁移。普通大世界静态内容可能只需快照或压缩表示，见§16 Snowdrop实例。

**行业证据的边界。** UE5.6分别描述EndPlay、组件注销、Destroy和GC，组件注销拆除render/physics state；在文档列出的关闭强制GC且快速重载子关卡条件下，EndPlay后的Actor可能以同一对象返回。它证明运行参与与物理回收不同步，不证明Frost应该由任意强引用保组。[Actor Lifecycle](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-actor-lifecycle?application_version=5.6)、[Components](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-in-unreal-engine?application_version=5.6)。Godot4.5的remove_child不删除子树，但Node不属于RefCounted；这是非删除移除的例子，不是普通强引用保活的证据。[Node](https://docs.godotengine.org/en/4.5/classes/class_node.html#class-node-method-remove-child)、[RefCounted](https://docs.godotengine.org/en/4.5/classes/class_refcounted.html)。专有引擎未知部分不能作为C支持票。

**原main基线的消费者事实。** 在`9d33f4fc1c8a88481c93be4b166e6eb311456a1b`中，Session删除记录保存同一Member根与位置，Undo恢复成员资格；删除记录没有字段快照，只有属性编辑另存before/after。该基线验收在离场期间只推进GC，没有离线写者。Scene写入先查活动成员，Session不开放离场写入；history之外尚无长期访问离线Actor的业务消费者。因此这组旧Undo证据不能证明C必需，也不能证明C错误。历史入口为`src/editor/session.cpp`、`src/scene/scene.cpp`和`acceptance/editor/scene.cpp`；后两者已由IV移除，当前消费者通过统一World入口工作。

**本片选择：采用普通GC对象组成，Actor强持当前组件。** 普通Actor强引用保同身份、当前字段与组成，世界参与和资源另行结束。这复用一张权威对象图；专用离线持有协议则需另定终结权、转交及普通引用失效规则，当前收益不足以承担这些维护成本。基础合同有独立设计价值，不能仅以当前是否已有离线消费者裁决。禁用、detach、历史Delete/Undo保同身份；新用途池复用或确定终结的内容卸载可明确destroy，强引用不能否决，stream unload不一概等同detach。

**附属组件owner采用强边。** 持有仍挂载的组件即可保持Actor及兄弟，避免Actor因GC消失而留下仍属组成的组件；代价是一枚组件强引用可能保住整组。追踪GC可回收无根环。组件经remove解除双向归属后可独立存活，旁观消费者用弱引用。若产品要求叶组件保数据却不保整组，弱owner仍是有效替代，须接受并定义owner失效状态。

首片保留Session单写者、只编辑当前成员的边界，不新增离线多写入口、全图快照或冲突版本设施。共享Runtime的可信调用者须遵守该边界，不能假称Core提供写权限沙箱。**本片合同已进入III实现，整体进度见文首。** 不再追加外部研究或假想负载设施。

### 5.4 World关闭：结束参与并撤根

**本片close结束世界参与、任务与资源接入，并释放World所持根。** 外部强引用可继续保留同身份的离线数据；这是本片已明确采用的可见语义。旧World的restore失败；后续加入其他World必须作为独立attach通过兼容性预检，不能靠close自动搬迁。它也避免为关闭追踪并终结所有曾归属该World的离场对象。另一方案是明确终结所管对象：若产品要求关卡结束后相关引用一律失效，该方案更直接，但必须定义终结范围并接受无法以旧身份恢复的结果。两者均先结束旧World服务。

若采用终结政策，才需要World能找到其仍有效的离线管理对象，例如非拥有ObjectId登记；它不能成为隐藏root或第二份组件图。登记验证上下文/代际、清理陈旧项、close去重，具体范围须先确定。不能先加登记再以它存在为理由扩大World终结权。

C下detach只解除成员资格、清历史只撤对应根；明确终结是另一个动作。若close仅脱离/撤根，仍被引用的数据可继续离线访问或经对象协议明确destroy，不再调用旧World资源；Runtime与对象协议须持续有效。后续独立attach按§4.1预检，若失败仍是Detached，不回滚为旧World成员；不由close政策承诺原子/无缝转移。

关闭与销毁已按本片合同落地；Core声明强弱边的能力不替业务决定边的方向。无论选哪项，Runtime最终析构都是独立的上下文终态，普通根不延长Runtime自身寿命。

## 6. 状态转换、操作与退出协议

### 6.1 成员操作、组合操作与对象查询

Runtime保有类型、身份与GC权威；对象模型提供统一Actor/Component协议；World执行成员/注册/任务副作用，Session只编排编辑与历史，renderer只消费呈现快照。状态转换没有第二条编辑器入口。下面为最小接口形状，错误名是待实现的领域含义，不在本轮扩展Runtime错误枚举。

| 接口草案 | 返回与责任 |
| --- | --- |
| inspect_actor(Runtime&, ObjectId) → Result<ActorStatus> | 先按§5.1验证身份/类型，返回四态及派生can_edit/can_present；Invalid结果不含可解引用的对象数据。查询不登记隐藏root；调用方用正式引用保证后续访问寿命，后续操作仍重验 |
| inspect_component(Runtime&, ObjectId) → Result<ComponentStatus> | 同样先验身份/类型，再给owner、enabled、运行激活与呈现资格；不靠Actor状态猜组件已完成注册 |
| create_actor → Result<ObjectRoot>；World.attach/restore | 构造成功先Detached；attach预检目标开放、同Runtime、完全离场、关系与组件接入兼容性。restore额外验证该条历史的原World/顺序及恢复前提，不给普通attach附加永久归属。便利创建入场沿同一链 |
| World.set_enabled(ObjectId,bool)、start、stop | set_enabled仅接受本World成员，修改运行意图并收束/建立实际接入；start/stop改变World运行模式，不清除成员的enabled意图 |
| World.detach_actor、attach_component、remove_component | detach结果包含普通Actor根和成员恢复资料；独立remove结果含组件根及owner/位置/依赖资料。关系修改与注册退出统一编排，原子更新Actor序列/组件owner与成员索引 |
| 对象协议destroy_actor(Runtime&,ObjectId) / destroy_component(Runtime&,ObjectId) | 唯一游戏对象销毁语义入口，位于现有World/对象模块。Attached先走当前World退出，Detached直接走共同关系清理与Runtime终结；不要求旧World仍存在，不扩源码/语言调用能力 |
| World.close | 结束全部旧World参与；若最终采用明确终结政策，调用同一对象销毁协议，不另实现一套World.destroy |
| World.request(同一操作) → 排队回执；宿主提交后取得完成结果 | 既有队列记录原World、目标完整身份及所需成员/owner前提，回放重验后调用同一协议。对象已离场/换World时拒绝该请求，不跟随目标去终结别的World成员。Queued只表示受理，历史仅在提交完成成功后更新 |

ActorStatus中的can_edit由World编辑模式、成员关系及操作准入共同决定，AttachedInactive本身不是编辑权限。C下只读对象协议可在Detached时使用，不依赖Session；首片公开编辑写仍要求当前成员。member_world是非拥有关系，close必须清掉，不通过Actor反向保持已关闭World。

恢复资料是memento，不复制当前组件图，也不是字段快照。历史接收容量、普通根复制等可能失败的准备先完成。restore重验该条记录的目标World与恢复前提；Actor已在B时，A的历史不能抢回它。旧World关闭后该次恢复失败，不解引用历史中的旧World地址。这些限制来自历史操作，不是藏在Actor上的永久affinity；普通attach仍按当前Detached状态独立预检。永久destroy是对象协议的独立命令，普通Delete/历史清理不调用它。

### 6.2 准备与提交：序列和owner不能半更新

1. 准入：World无遍历、提交或生命周期派发重入，Runtime满足check_code_publication的静止条件；暂停的GC周期可以存在。先完成冲突编辑的取消/提交。
2. 预检：全部ObjectId活着且属于正确上下文；类型/成员/owner/依赖/顺序有效。明确哪些变化属于同一组合操作，例如补Transform再挂Cube，不能拆成可见半步。
3. 准备：取得普通根，初始化未发布对象，为成员/类型索引、恢复资料和history预留容量；Core在一次同步`apply_reference_changes`中预检全部序列/引用槽变化，并准备计划、序列容量和GC游标所需资源。只reserve一个vector不足以证明提交无失败。
4. 提交：进入/退出标记仅作内部重入保护；退出操作先按§6.3收束活动，再解除相应关系或成员。完整借用保护目标后，Core同批提交Actor序列和Component owner引用写，强弱由正式槽元数据决定；内部apply无分配、noexcept、不调用用户代码。World同步更新member_world及已预备索引，完成稳定状态后派发完成通知。不能先insert成功再单独尝试可能失败的owner写。
5. 完成：同步成功返回时对象关系、成员与查询一致，结果可无抛出地移入预备history空间。准备失败不改世界/关系/游标；Session不能承担手工补写另一半关系的责任。

这不是通用数据库事务：只覆盖本操作声明的对象关系和索引。类型封闭、任意原生构造/方法副作用与进程级内存耗尽仍遵守既有Runtime保证。新增接口需给出明确的普通错误/分配异常边界，不能宣称任意副作用都可回滚。

Runtime负责人已核对现有`Impl::shade`（runtime.cpp:337）用slot侵入灰链，无队列节点分配；`write_reference_at`（1472行）验证后写ID并shade。但**已标记owner再次shade会直接返回**，不能用它重排取消了暂停游标的序列owner。II必须提供不增加marked计数的重新入灰路径，预备包含继承/序列的追踪深度，并在无分配提交中完成版本失效/游标取消/重排。这是真实接口前提，不是尚待发现的性能优化。

### 6.3 进入、退出、排队与错误

**进入先有对象和成员，再有运行资格。** 构造成功得到Detached；加入成员后至少为AttachedInactive，只有所需运行接入成功才发布Active。准备阶段的失败不改成员/关系。若成员已提交、后续运行接入失败，收回本次已建接入并沿stop协议收束World全部游戏活动，成员保留为AttachedInactive；完成结果区分“结构已提交”和“运行接入失败”，不假报attach从未发生，也不以enabled=true冒充Active。编辑呈现接入失败同样报告具体能力不可呈现，不将有效对象报Invalid。

**退出顺序：** 在既定宿主静止点先关闭目标的新操作/调度准入并进入内部退出保护；用准备时捕获的关系和普通根收束已有活动，取消或完成相关任务；按逆依赖撤所需注册；最后提交detach/remove/destroy对应的成员或组成变更，并撤相应根。停用/stop只撤游戏执行与模拟接入，detach/close撤全部旧World接入。GC析构不补做这些退出步骤。期间只允许受控清理使用捕获数据，不经普通World成员查询重新进入业务。

| 情况 | 明确结果 |
| --- | --- |
| 正常更新回调请求结构变化 | 返回Queued；到静止点重验原World及目标身份/关系前提后提交。排队destroy期间ObjectId仍有效；不新增Runtime PendingKill。目标失效报invalid_object，已离场报not_member，已入另一World报foreign_world，均不执行销毁 |
| 同步修改发生在遍历/进入/退出中 | 报busy；进入/退出回调产生的新请求在World仍开放时延后到下一边界，不递归提交；close开始后拒绝新请求 |
| 过渡中重入查询 | 报transition_in_progress，不读半提交字段/索引；已发布快照不能作为之后写入的永久许可 |
| 开放World中对合法成员重复set_enabled同值、对已停止World再stop；对已关闭World再close | 成功但changed=false，不重复通知、注册或新增历史；对已经Running且健康的World再start同理。幂等规则不绕过World准入、身份/类型和归属校验 |
| 重复attach同一成员/组件，或detach/remove已非成员目标 | 分别报already_member/already_attached或not_member/not_attached，不重复根和恢复记录；排队重复项逐项按提交时事实判断 |
| World操作的目标仍属于另一World、跨Runtime或类型不符 | 分别报foreign_world/context_mismatch/type_mismatch；活对象不因不属于调用方World而变Invalid。已Detached的同Runtime对象普通attach另做接入预检，组件无法适配目标时拒绝且保持Detached |
| 失效ObjectId再次destroy/restore/改字段 | 报invalid_object，无副作用；只读inspect返回Invalid。不记录墓碑来猜测是已destroy还是已GC |
| World开始Closing或已经Closed后的成员/启停/编辑操作及新请求 | 除重复close外均报world_closing/world_closed，即使成员尚未拆完或旧快照是AttachedInactive也不允许编辑；已有编辑取消与获准终结属于受控清理。对象已经Detached后可直接调用对象销毁协议，该路径不访问旧World |
| 依赖不满足或恢复冲突 | 提交前拒绝，关系、选择、历史cursor均不变；不能以“排队成功”移动cursor |

退出通知失败仍继续收束其他任务/注册，记录首因并停止游戏更新；已经提交的关系不伪回滚。若无法确认退出完成，保留内部过渡保护，不能报告完成或释放仍被使用的宿主服务。完成后的通知失败不改已提交事实。这里不提供任意用户副作用的自动补偿，也不建立通用FSM框架。

### 6.4 明确永久销毁

对象协议的destroy是唯一游戏对象终结入口，仍属现有World/对象模块，不新增manager、离线后端或对象身份。先验证Runtime、目标身份/类型、静止点及当前组成，准备Actor与组件根；Attached路径经member_world调用已有World退出链，Detached路径确认无旧注册/任务后直接继续，不读取创建或曾归属的World。两条路径随后复用同一组成引用清理及Runtime终结链：组件按逆依赖顺序终结，Actor最后终结。组来自当前Actor序列，不来自空间子树或历史快照。

World排队销毁只转发该协议，回放必须先满足原World及目标身份/成员条件；不能借统一入口跟随已入B的Actor去销毁B成员。close若采用终结政策，也在既有退出保护中复用同一终结链，不重复退出或递归提交。Detached的非静止调用报busy，由宿主在合法边界重试，不新建队列。组件单独destroy同样走对象协议，先处理依赖、撤参与并摘除owner边，不能留下活Actor指向已销毁的挂载组件。

Runtime当前只有逐对象destroy，不能原子标死全组。对外业务查询先让整组退场，静止点禁止重入并预检去重；清理期间普通根只保证存储不会因GC先消失，不阻止明确destroy。原生析构必须noexcept，不能递归create/destroy。

可信代码越权销毁导致中途陈旧身份时，报告终态合同错误并清理其余对象，绝不重发半组为live或让undo复活它。外部只持某组件也不能否决这次明确的组终结；它持有的身份随destroy失效。

### 6.5 关闭顺序与普通撤根

close在静止点开始后先标记World正在关闭，拒绝新操作/请求；随后在目标仍有效时受控结束冲突编辑，停止新调度，取消/完成既有任务，撤全部游戏及呈现注册；清未执行请求并给出关闭导致的失败结果；最后按§5.4获准政策清member_world、索引及成员根，或显式终结相应对象。正在关闭不再回到可运行World；清理错误须报告，但不能让强引用重新打开准入。

**关闭完成条件：旧World没有未收束的任务、回调或注册，不能再被提取呈现，Actor不再通过member_world指向它。** 排队回执不是close完成；未达到该条件不得报告关闭完成或析构所需服务。World.close不结束共享Runtime，后者由宿主在所有World/执行依赖结束后决定寿命。按当前撤根建议，外部强持的数据变为Detached，可查询、经对象协议终结，或独立预检后加入兼容World；旧World的restore/start仍失败。本片close不进一步终结数据，后续明确终结仍走对象协议。

Session.clear_history、redo裁剪和Session析构只释放其普通根/值；不排自动销毁队列，不执行生命周期回调，不阻挡World关闭等待token归还。外部引用存在时，C组成继续存活，除非已单独执行明确destroy或整个Runtime终结。

close令保留对象不再调用旧World/资源，宿主保证Runtime服务期；明确终结单独执行§6.4。World.stop、detach、Session关闭、World.close与Runtime销毁分别定义，不以隐藏根改变存活。

**三个实际轨迹与迁移落点：**

1. 编辑场景：构造Detached→加入编辑World为AttachedInactive；Transform/Cube可被提取、显示并经Session编辑，Counter没有游戏tick。start后符合条件者Active；stop后回AttachedInactive且仍显示。Workspace使用统一状态/可编辑结果，renderer使用呈现提取，不各自猜active。
2. Delete→GC→undo：Session在detach前准备历史根；完成后Detached、无World执行/呈现。推进GC仍能查同一身份与组成；undo在原World开放且预检成功时恢复成员，编辑World中为AttachedInactive。Session只在提交完成后推进历史；当前无离线写者，不增加全图快照或冲突版本设施。
3. 关闭World仍持Actor：普通强引用保目标，close完成并析构原World后，按当前撤根建议Actor仍为Detached且数据可读。通过对象协议明确destroy，同一Actor及当前组件身份均失效，外部强引用不否决；该链不得访问已析构World。另用仅释放最后强路径的样本验证普通GC回收，不能由显式destroy代证。数据跨close存续的预期以该政策获准为实施前提。

**有限E2E门槛，实施时在dev/release同一正式入口完成：** 上述三轨迹；组件disabled而Actor Active仍不tick；Queued destroy提交前有效、提交后Invalid，目标已换World时原请求拒绝且B成员保留；在场A直接attach B拒绝，完全detach后兼容attach B保同id，两步失败不伪回滚；重复启停/关闭不重复副作用；跨World/类型错误不误报Invalid；退出重入及关闭期间编辑被拒绝，最终无旧World任务/注册。用现有Counter和呈现能力覆盖，不为未接入系统搭mock。§11消费本节预期，III已在dev/release执行本节真实E2E，IV继续覆盖编辑器消费者。

## 7. 存储、查询、执行与成本

### 7.1 首版物理布局

Runtime继续保存对象实值和地址稳定性；在C布局候选中，Actor/Component内部保存权威组成及关系记录，World保存当前Actor成员根、紧凑成员目录和按TypeId分组的非拥有索引。索引只含ObjectId及必要定位，不复制业务字段或owner真值；其他寿命选择也不要求复制业务值。内部索引可以移动，ObjectId不变；查询借用只在当前无结构提交区间有效，跨提交/帧不能缓存裸地址。

需要 ObjectId→记录/owner 的正式查找，建议 Runtime 负责人提供 `std::hash<ObjectId>` 与 `std::hash<TypeId>` 作为小型配套：包含完整相等字段，以 `==` 消除冲突，不暴露私有槽、不定义持久哈希。小型冷端类型角色表可线性查询；热路径不能每个组件再次全扫全部 Actor。字典平均常数复杂度不等于最坏常数保证。

查询明确区分`exact(T)`与`assignable_to(Base)`：创建菜单/具体类型数量查询可以exact，组件面板的Component基类查询应包含具体派生对象。World复用Runtime正式`is-a`，不能再维护一套类型名枚举。类型注册封闭后可预绑定匹配的具体TypeId桶；exact遍历O(k)，基类查询O(t+k)，t为匹配具体桶数，均不意味着payload连续。若以后开放注册，需要正式的查询绑定失效合同。

首片不建通用布尔查询编译器。同型或同基类匹配多个组件时返回范围，不静默取第一个。每项仍通过Runtime读取时，身份解析、间接访问和字段路径成本依旧存在。整体Actor移出需处理其c个组件和相关索引；稳定UI顺序与运行索引顺序分开，不能把swap-remove下标当持久顺序或身份。

### 7.2 确定的阶段，有限的请求

首片用单线程宿主显式阶段：

```text
外部命令/编辑动作提交
  → 开始本次 World 更新（固定现有成员视图）
  → 按明确顺序执行已接入的游戏系统/行为
  → 结束所有执行/借用
  → 提交本轮排队的结构请求
  → 提取只读呈现数据（无窗口宿主可省略）
  → Runtime 安全点推进 GC
```

生命周期派发产生的请求不递归纳入当前批，留到下次显式提交；创建请求在完成前没有可用 ObjectId，返回的是 pending 状态或请求结果通道，不能提前写成功历史。创建后再创建依赖组件的首片场景用一项组合命令表达，不先引入临时 Entity ID 修补未提交对象。

每个请求提交时重验目标代际、World、owner 与依赖。失效请求明确失败、释放其持有资源；一批顺序请求按提交顺序处理，各请求原子性不扩展成整批万能事务。若确需整组原子操作，必须预先作为同一项组合请求验证。

执行顺序由实际依赖确定：例如业务更新先于渲染提取。当前没有工作线程任务图、读写集合自动推断、通用 DAG scheduler、固定/可变 tick 双管线或自动协程调度。不要让这些设施成为第一组组件的前置条件。

### 7.3 性能与资源：当前只有推断

- 类型/字段发现放冷端，循环中使用缓存 TypeId/FieldId 与类型索引；按真实系统批遍历，避免强制每 Actor 虚调用一个空 tick。当前 Runtime 每对象仍有存储分配与根登记（`runtime.cpp`:1265、1280），每次 `Entry.invoke` 仍有执行准入/调用帧成本；按类型遍历不等于已经提供 JIT 批量调用 ABI。
- World强根只对应当前Actor成员；组件经Actor序列追踪，历史/执行另持必要根。历史128项并不等于128个对象或固定内存上限，一个Actor根可以保许多组件。裁剪历史只撤根，按实际可达性回收，不能为限内存暗中destroy外部仍持有的组。
- 复用提取缓冲容量，输出 renderer 值快照；不为每帧每对象复制 ObjectRoot。GPU 生命周期仍沿既有 renderer/RHI 协议。
- 连续索引不代表 payload 连续；没有证据宣称“获得 ECS 性能”。本轮不运行基准，也不提出性能百分比。
- 真正出现大规模同质工作负载时，再在同正确性语义下测量 tick/提取耗时、分配和长期保留，决定是否给 Runtime 增加池/批存储或给系统增加派生工作集。不先加 benchmark 框架。

## 8. 与语言、JIT、类型封闭的接线

| 能力 | 首片承诺 | 不应误读为 |
| --- | --- | --- |
| 定义 | 宿主在第一次对象创建前发布 Actor/组件/record 全部定义，再建立引用这些 TypeId 的角色描述 | 每次挂载重新注册类型；角色表拥有第二份类型定义 |
| 正式派生 | native/描述型定义沿Core注册base，已有源码入口按共同继承合同消费预注册对象 | 当前语言已有class语法；C++继承自动被Runtime发现；角色表等于类型层次 |
| 动态组合 | 随时在合法结构边界创建、挂载、移除已注册类型实例 | Runtime 运行中可追加任意新类型 |
| 字段编辑 | 先由 World 定位真实组件 ObjectId，再使用该类型的内联字段路径 | Runtime 字段路径已能自动穿越 Actor→组件集合 |
| 源码行为 | 可用既有 Program 对一个真实组件类型建立绑定，self 指向同一组件；代码经 JIT 后才执行 | 已具完整游戏语言、任意多类型对象图访问或 World 结构调用 ABI |
| 兼容代码更新 | 在共同 Runtime 静止点发布已有入口的兼容函数体；Entry选择当前版本，旧 Lease/协程保留旧版本 | 原生方法表已替换、所有类型实例布局更新、旧执行帧迁移 |
| Coroutine | World拥有相关任务并显式推进/取消，沿已有根与版本寿命规则 | 挂起等于停止、GC会取消任务、已有 await/scheduler |

源码 `new` 只创建 Runtime 对象，不自动赋予 World 成员资格或 owner。首片不向脚本暴露可绕过 World 的结构能力；没有因为某对象类型被登记为组件就使它自动进入所有世界。后续语言 World 操作应成为现有共享对象机制的消费者，由真正的调用参数/多类型需求驱动，不建立脚本包装对象或备用 VM。

首片可使用非空间 Counter 组件做真实同步行为：无窗口宿主运行一次更新后，该组件的字段发生变化；禁用或离场后停止更新；通过现有 Program 绑定验证源码修改的就是 C++/编辑器可读的同一字段。不开编辑器源码面板或完整 Play 模式；现有编辑世界默认不跑游戏 tick，避免 preview/cancel 覆盖系统写入。

## 9. 编辑事务、选择与 UI 身份

### 9.1 显式字段目标

事务捕获`{World, ActorId, ComponentId, FieldId路径, 编辑token}`，以及合法的before/after值与所需根。继承字段沿原声明FieldId访问具体派生组件的同一ObjectId，Runtime处理基子对象调整；应用不计算offset，不生成基类包装对象。一次枚举不重复返回同一祖先字段，显示名不作字段身份。每次preview/commit/cancel/replay依据捕获目标，不依据当前选择、不重新找同类型组件。浮点范围、单位、步长及别名仍在应用参数描述中；范围约束由World/字段编辑策略统一消费，控件不单独校验一份。

首片覆盖真实使用的标量字段，不做通用值编辑器。XYZ 若作为整体向量动作，历史明确保存其复合目标与三个值；普通标量历史不能全塞进 `Vec3`。多字段写入先校验全部值，并对本片已知字段保留回滚；Runtime 普通 `assign_value` 或任意方法的副作用没有自动事务保证。

保留连续预览直接修改权威值、commit只记一次、cancel回 before、无变化不记历史。选择变化、组件/Actor结构动作、undo/redo前先结束活动编辑；取消失败则停止后续动作，不能先删组件再尝试经成员查询恢复它。取消也必须核对编辑token，迟到取消事件不能结束另一个新事务。

### 9.2 普通根、恢复资料与历史值

Actor增删、组件增删与字段修改共用一个Session游标。历史持普通ObjectRoot及恢复所需值，Actor的当前组成仍在实际对象内。根只保当前对象，不是某一时刻的值快照；下面的成员恢复不能单独承诺删除时全图恢复。

| 历史动作 | 持有什么 | 应用/撤销改变什么 |
| --- | --- | --- |
| Create / detach Actor | Actor普通根、原World与成员位置等恢复资料 | World成员资格/顺序；Actor组成保留，不在history内移动整张图 |
| Attach / remove component | 组件普通根、原owner身份、组成位置及确实改变的依赖关系 | 同一Actor强序列与组件owner边；摘除后由组件自己的历史根保活 |
| 字段修改 | 具体组件根、原声明FieldId路径、有类型before/after；若动作需要活owner，显式保存所需依赖 | 同一对象的同一字段；所保范围服从最终owner合同，不能靠未声明的隐藏根保证成功 |
| 复合结构动作 | 本次真正受影响对象的必要根和逆操作资料 | 同一原子操作声明的关系变化；不能快照整个场景冒充恢复 |

根在动作应用或撤销后都可以继续由该历史项持有，符合普通Runtime保活语义；不再按哪一侧持token来决定销毁。history容量准备在World提交之前，成功结果能可靠地移入历史。preview已修改字段时，commit也须先保证历史接收，失败沿既定cancel/错误路径处理，不能丢掉恢复值。

restore预检全部身份、World、owner、顺序与依赖，失败不更改关系、选择或cursor。正常线性重放先恢复Actor，再恢复此前独立移除的组件。外部操作已让原owner失效、转属或被明确destroy时，undo明确冲突；不夺取新owner、不替换为同类型新对象。需要哪些对象保持同身份应由动作合同明确。

**必须补足的反例：** Delete A后，另一个普通强引用将字段x从1改为2，并把组件C换成D。直接restore成员资格得到x=2与D；若产品要求删除时的x=1与C，需要明确的历史值/关系资料以及冲突处理，普通Actor根不足以保证。可选择离线修改仍经同一历史、阻止冲突写，或检测修改后拒绝恢复；也可明确“恢复当前离线对象”，但不能将其描述为回到删除时状态。最终策略须在IV前固定，不能默认外部引用只读或静默覆盖外部修改；本轮不预建通用快照/版本框架。

所有历史淘汰、redo裁剪、clear与Session析构只释放它们的根和值，不调用Runtime.destroy或World永久终结。必要反例：

- Delete A→undo→新编辑截断redo：A仍由World持有，丢弃旧记录不改变A。
- Create A→attach C→undo C→undo A→新分支：A和独立C各按实际根/强边决定存活；A当前序列不含C，丢弃后缀不能以旧快照把它重新并入或销毁。
- Delete A→清全部history/销毁Session：外部普通Actor强引用仍保完整当前组成；移除这最后强引用后才由GC按可达性回收。
- 独立remove C：只持Actor不能再保住C；历史根或外部组件根可以保住C及其字段，owner已经为空。

属性undo的选择政策暂保留现有行为（session.cpp:83–85会选中历史目标）。组件编辑目标始终是捕获的具体身份；永久destroy后旧目标失效时只清理该事务/控件状态，不能恢复payload或改写当前新选中对象。

### 9.3 UI 控件键

UI继续使用自己的 `ControlKey` 和现有 incarnation/accepted-frame/EditToken：

1. 工作区为 `{ActorId, ComponentId, 字段/动作}` 分配单调控件键，存储显式绑定；同一有效绑定跨普通帧保持键稳定，不每帧重新分配。
2. 组件删除、Actor离场或字段目标变化时撤销绑定并结束相关编辑；旧事件仍可能到达，必须通过现有 frame/incarnation 和目标成员/代际复核。
3. undo恢复同 ObjectId，但建立新的控件绑定/incarnation。业务身份恢复不意味着旧输入会话复活；新控件显示当前权威值。
4. 控件键退役后本次会话不复用；接近键空间上限时明确重建 UI 会话或拒绝分配，不让溢出别名复活。按控件生命周期移除表项，避免无限保留死绑定。

禁止 `first_object + ObjectKey`、`position_base + key*4+axis` 的编码/解码继续存在；也不能用 `hash(ObjectId)` 当无碰撞控件键。绑定表不保活业务对象，显示序号/名称不承担身份。一个对象可以有多个展示控件，并不意味着多套业务身份。

## 10. 消费者迁移与文件所有权

以下是本轮实施的责任边界。现有代码由相应负责人共同收口，不能各留一套兼容入口。

| 所有者 | 文件/模块 | 需要完成的变更与删除 |
| --- | --- | --- |
| 游戏世界与对象负责人 | 新 `src/world/`；现 `src/scene/scene.*` | World/Actor/Component协议、组合记录、索引、结构提交、首片类型/依赖；迁移完成删除旧Scene、SceneObject、ObjectKey/next_key、position_fields与位置专用包装，不保留第二个可变Scene |
| Runtime负责人 | `src/runtime/`、`src/source/`及必要语言/JIT接线 | 正式基础类型/单继承/兼容/成员与GC合同、源码接收者和类型依赖；不透明ID哈希及通用引用集合/受控访问原语。不得由World线私改GC、布局或代码发布 |
| 应用与编辑器负责人 | `src/editor/session.*` | 通用字段目标、普通根+memento历史；裁剪只撤根；删除Kind::position和四个位置专用编辑入口，迁移全部调用 |
| 应用与编辑器负责人 | `src/editor/workspace.*`、`src/application/application.*` | Actor/组件列表、真实字段绑定、全部动作与取消路径、选择/Focus缺失政策、组件过滤投影；保留已验收UI视觉及输入/完整帧协议 |
| 应用与对象负责人约定单人落笔 | `src/editor/camera.*`及共享数学值 | camera的scene::Vec3依赖移到一处共享数学值定义；不再复制一个Vec3，坐标/单位与现行相机保持一致 |
| 应用负责人维护总接线，对象负责人维护World目标 | `cmake/editor.cmake`、新增World构建片段、根CMake | 独立runtime-only World库；editor_core消费它；无窗口宿主链接同World，需要undo再链接Session。现行产品启动入口仍唯一 |
| 应用负责人，联合对象负责人 | `acceptance/editor/scene.cpp`、`acceptance/editor/window.cpp`与对应目标 | 迁移旧位置/key/member假设，改为下节实际纵向链；保留相关既有取消与输入回归条件，不复制新旧场景验收世界 |
| 应用负责人 | README、编辑器模块文档、现行计划；术语按需要由责任人更新 | 写当前行为、移动原持久键职责、标记旧路径已删除；本轮不改这些文件 |

依赖为I/II通用Core基础→III正式World→IV编辑器迁移，实际进度以文首为准。C、owner方向、离线历史语义和close须在对应业务实施前会审，不作为通用II的前置。本文不另立排期或自行派发业务实施。

renderer继续消费派生值快照；首片若按§4.3贯通完整TRS，则由renderer负责人把当前仅position的`Cube`/GPU实例/着色器世界变换一起扩展，应用传入统一求得的world矩阵，不在renderer另算一套父链。其他RHI基础未发现必需前置变化。应用不再对每Actor输出Cube；可渲染组件决定0/1/多条实例，owner==所选Actor可沿用高亮政策。

选择无空间/渲染组件的Actor合法。Actor Focus明确使用指定根空间目标的世界位置；组件面板检查非空间组件不改变这个策略。无根时禁用/无操作并反馈，不能暗造Transform或挑第一个组件。若后续提供独立组件Focus，再取其显式空间目标；相机只接世界点，无需认识整个类型层次。

### 10.1 正式共享接口与所有权

**III实际公共头与接线delta（本轮唯一接口）：** [world/world.hpp](../../src/world/world.hpp)与[world/math.hpp](../../src/world/math.hpp)。

- `register_types(Runtime&)`一次发布Actor/Component/Transform/Cube/Counter及数学record，返回Types；宿主在创建对象前完成额外类型注册。World借用同一Runtime，`runtime()/types()`提供持根、GC及源程序绑定所需访问；组件编辑走World.read/write/set_transform。
- `create_actor/create_component`返回普通根；World.attach接纳Actor，attach_components一次接纳一批显式Attachment。Cube必须明确绑定同组Transform；根Transform首片唯一，没有空间的Actor合法。
- `detach/remove_component`分别返回可移动ActorRemoval/ComponentRemoval，携带普通根、原位置及组件原owner。历史来源只保World寿命的弱凭证，既不保World也不形成Actor永久affinity；restore走同一校验/提交。Commit单列通知错误，已提交变化不能伪报为未发生。
- `inspect_actor/inspect_component`、`components`支持离场读取；成员状态与类型查询不生成第二身份。对象协议`destroy_actor/destroy_component`是唯一终结入口，World队列只转发并重验原目标关系。
- Types给出可缓存的position/rotation/scale字段路径；Trs使用position、xyzw四元数及正非零有限scale。Matrix4为16个float，行主序/行向量，S*R*T，平移12–14。renderer直接消费RenderItem.world。
- World.update使用实际Counter执行注册；可提供CounterUpdate适配既有Program.Entry，调用在借用之外。World接管通过TaskStart创建的真实Coroutine，在停用/脱离/stop/close时取消并释放；渲染注册和游戏执行分开。
- Core批次使用实际ReferenceField定位、SequenceSplice和ReferenceWrite；隐藏关系只在with_native保护中操作。预检/容量准备先于关系提交，成员索引同步发布；不改Runtime/source。
- 纯模块目标将由`cmake/world.cmake`中的`atelier_add_world()`建立`atelier_world`；无窗口`world_acceptance`只链接World与实际source消费者，不依赖Application/renderer。

I/II接口由Runtime负责人落实到正式Core声明；III以本节公共头实现业务合同。最终约束进入唯一公开入口和必要内部协议，不复制到兼容包装中。

| 接口面 | 输入/输出与责任 | 错误与有效期 |
| --- | --- | --- |
| Core类型/引用 | `object_type() const noexcept -> TypeId`；Definition.base为optional TypeReference、Info.base为optional TypeId；`is_a(TypeId,TypeId) const -> Result<bool>`；`cast_reference(const ObjectReference&,TypeId) const -> Result<ObjectReference>`；`hold(ObjectId,TypeId) -> Result<ObjectRoot>` | 完整错误及空/陈旧视图规则见§4.2.3；is_a无效类型为invalid_type，cast实际不兼容为type_mismatch，非空下转陈旧为invalid_object。cast不登记root，hold显式视图验证后才登记；ObjectReference默认==不变，身份用id比较 |
| Core成员 | 沿现有`read/write/call`；TypeInfo.fields/methods按祖先→本层给公开有效列表，保留声明FieldId/MethodId及签名；hidden仅进内部布局/追踪 | 不适用成员为invalid_member，陈旧对象为invalid_object，副作用前拒绝；Core通过NativeLayout.base_address或描述型布局调整地址，应用/JIT不计算offset；inline record路径保持原规则 |
| I-b绑定/self根槽 | bind_type收祖先/内联record依赖，Metadata::matches包含base；`ExecutionFrame::hold(uint32_t,ObjectId,TypeId) -> Result<void>`直接调用Runtime显式视图hold | 同步execute_admitted与异步start_version共用入场检查，self直接成为binding.type强视图；旧重载/纯标量环境保持。源码new构造绑定类型而非receiver动态类型；Entry/Lease/挂起帧不改变这项规则 |
| 窄宿主借用 | `with_native<T>(ObjectId, callback) -> Result<R>`，T须有本Runtime原生表示，支持描述型派生对象中的native基段 | callback前校验actual is-a T及合法表示。借用覆盖地址适配和整个回调；禁止create/destroy、GC、源码/JIT重入，允许合法字段/引用/受控序列写和必要嵌套同步借用。返回必须自有数据，不能逃逸指针、引用或span。这是可信C++合同，不是权限沙箱 |
| 序列只读 | size/read/visit，返回声明元素类型视图的ObjectReference或受限只读枚举；World候选以Component为元素类型 | 不暴露可变元素span；对象失效/元数据不匹配拒绝。枚举回调期间禁止结构变化，跨操作只保留ObjectId/正式引用 |
| 同步引用批次 | `apply_reference_changes(span<ReferenceChange>)`，只含定型序列splice及受保护ReferenceSlot写；两种定位（FieldId路径/受保护native槽）共用内部内核 | 完整preflight后无分配、noexcept提交；失败或准备期OOM不改逻辑内容，允许已预留容量变化。不返回跨帧prepared handle，不做通用字段/回调事务 |
| World活动查询 | 当前Actor成员、exact/assignable组件查询、启用/运行过滤 | 借用视图只在当前无结构提交区间有效；不为索引额外持离线对象根 |
| Actor/Component只读协议 | 普通Actor引用查询当前组成/顺序，Component查询owner及author-enabled | 不要求当前World成员/Session；强owner会反向保住Actor及当前组，明确destroy仍可终结身份 |
| 可撤销结构动作 | `detach_actor`返回Actor普通根+成员恢复资料；`remove_component`返回组件普通根+owner/位置/依赖memento；restore接收这些正式身份/资料 | World先校验并准备，成功结果可无抛出移动。Session须先准备history容量。失败关系/选择/cursor不变；恢复不得夺取对象或替换身份 |
| 属性编辑 | 显式Actor/Component ObjectId+原声明FieldPath；完整TRS动作有已校验规范值 | Session政策要求目标在可编辑World且owner匹配；离线对象只读不受此政策封死。复合TRS修改不是若干可能半失败的独立XYZ写 |
| 渲染提取 | World提供有效能力与统一求得的world-from-local值；应用加选择/颜色，renderer消费值快照 | renderer不持ObjectRoot、不遍历类型或owner、不重算层次。使用现行行向量`p * world * view * projection`，GPU资源寿命仍沿既有RHI |

**唯一序列变更形状：** `SequenceSplice{sequence, first, erase_count, inserted_refs}`。一次范围插入、删除、替换或清空都复用此内核；一批同序列至多一个splice，同一引用slot至多一次写，不定义多点修改脚本。需恢复排序时可以准备一次完整替换，不能让history逐步拼出半个关系。

“空Actor添加Transform+Cube”先创建两个普通有根对象，再以一个Actor序列splice插入二者、同批写两个owner引用槽，强弱按最终声明。插入值先规范化并复制到准备区，再reserve，避免输入span与原序列别名。Core校验范围/最终长度/类型/上下文；World校验同Actor不重复身份、实际增删集合与owner写集合一致。第二个create失败不先发布第一个组件，原生构造外部副作用仍不属于该引用批次回滚。

Core预检还包括所有目标借用、计划/序列容量、追踪深度和结构版本溢出；提交只移动无抛引用存储、写ObjectId、失效游标并入侵入灰链。新强边分别shade，已标记未扫完owner走明确重排路径。World在调用前另备好目录/索引/history/通知空间，引用批次成功后只发布其无失败元数据，再运行通知。Core不替World理解组件owner或任意容器。

### 10.2 依赖切片与完成门槛

当前进度见文首。I覆盖Runtime/source/JIT，II落实通用引用，III已验收实际World消费者；IV按应用分支的正式接线继续。

| 切片 | 唯一责任与实际产出 | 必须先有的接口 | dev/release真实E2E完成门槛 |
| --- | --- | --- | --- |
| I-a：Runtime正式单继承 | Runtime负责人：Object根、选定base/is-a/cast/hold视图签名、base_address适配、有效成员列表、完整GC/构析及opaque hash；三格支持、一格拒绝 | §4.2.3的确定草案经会审批准；不改类型封闭或布局热迁移政策 | native/native、described/described、described/native实际实例；非零base地址；Base声明成员访问Derived；基类强弱引用不漏派生hidden强边；相关null/陈旧转换与无关拒绝；跨视图id相同而引用值相等规则不变；构析/失败清理不重复。已有受影响Runtime消费保持正确 |
| I-b：现有source/JIT继承接线 | 同一Runtime负责人：有效成员/祖先快照、sync/async统一receiver、ExecutionFrame显式声明视图hold及发布兼容 | I-a选定接口与Metadata.base依赖；不靠World类型标签修补 | Base绑定在原生和描述型派native对象上执行；self复制/observe/try_acquire、yield后GC、旧Lease/兼容发布完整；无关receiver副作用前拒绝；new仍构造绑定类型、纯标量环境不变。class语法、多类型目录、统一虚override不纳入 |
| II：引用序列与窄受控操作 | Runtime负责人：声明元素类型的对象字段引用序列、with_native借用、有限同步引用批次、GC形变/预算及受保护槽 | I整体合同与§10.1通用接口边界；不依赖C、owner或close选择 | 用Core正式Holder/BaseElement/DerivedElement验证原语，不建临时World；强/弱槽按元数据，splice批次失败不半写；暂停GC与有限预算等真实边界，不据此偷定Component.owner |
| III：World与真实无窗口对象模型（双配置已通过） | 对象负责人：正式Actor/Component/Transform/Cube/Counter、唯一组合入口、成员/类型索引、运行阶段和生命周期 | I及实际所需II完成；寿命/owner/离线访问、组合依赖、空间范围与close已定稿 | 按选定合同验证普通外部Actor/组件引用、detach/destroy/close差异及资源退出；已验证普通强字段保当前组成；复合失败不半发布，同Runtime多World、真实Counter/JIT与TRS |
| IV：编辑器与renderer完整迁移 | 应用负责人统筹Session/Workspace/Application/Camera/验收，renderer负责人只改实例变换合同；对象负责人移除旧Scene | III稳定的操作结果/memento/只读查询/值提取；已有UI帧和输入生命周期继续使用 | 同一Session无窗口history链+正式窗口增删/字段预览一次undo/恢复同id/旧输入失效；离场但被引用对象不出图；完整TRS在GPU真正改变姿态/尺寸并可undo；旧入口全部删除；既有受影响S2行为保持，两配置通过 |

II使用Core正式对象图验证机制，不引入临时Actor/Composition服务。II通过只证明原语；选定产品寿命须由III正式Actor/Component与同一生产入口证明，不能用UI根或历史遮盖存活缺口。IV再验证Session裁剪/析构和实际开放的编辑边界。

每片只编译受影响目标并执行上述真实端到端门槛，不建unit/mock/benchmark框架。正式窗口验收由应用负责人按用户安排的前台时段运行，不在本轮研究擅自启动GUI。通过后只有新修改/失败/未解问题才扩大验证。

### 10.3 连续迁移与旧入口退出

I、II只扩展现有Core/执行合同，不建立游戏模型兼容层。III提供正式runtime-only World与无窗口消费者；它与IV构成同一个必须连续闭合的迁移工作包。III通过不能被报告为新编辑器已交付，也不能以旧Scene还能用为由把IV无限延期。

IV在同一改动中迁移所有Scene/Session/Application/Camera/renderer/验收消费者，再删除src/scene旧固定类型、ObjectKey、position专用入口和控件算术编码。没有Scene→World可变包装、双写同步、第二产品启动入口或平行组件RTTI。阶段中尚未迁移的旧产品保持原样，不把同一场景复制成两个可写世界；新产品交付门槛是完整迁移及旧入口删除。

文件按§10责任表单人归属，不为提前并行发布临时字段/回调/兼容层。本文作者负责World实现与本稿，整体交付边界统一见文首。

## 11. 最小完整纵向验收切片

III的dev/release真实World E2E已通过，IV继续验证实际Session和正式窗口。以下保留整轮完成门槛；所有检查在release有效，不新增单测、mock或普通改动基准框架。

### 11.1 同一模型的无窗口宿主

使用正式类型注册、World、Runtime 与必要的 Session，不创建 Application/Window/Renderer：

1. 预注册 Actor、Transform、Cube、非空间 Counter。创建空 Actor A：成员数1、组件数0、可渲染查询数0。
2. 一项明确组合动作给 A 添加 Transform+Cube：组件有各自 ObjectId、owner=A，渲染提取有1项；给 A 或另一个无空间 Actor 加 Counter，证明组件并非Cube别名。
3. 移除 Cube：A与Transform身份不变、仍在成员列表，渲染查询变0。undo恢复原Cube身份/owner/顺序并重新出现。
4. 经显式组件字段目标连续预览位置，多次preview→一次commit；undo/redo读真实Runtime值，cancel恢复起点且不新增历史。完整TRS路径设置非默认旋转/缩放，核对统一空间求值及提取结果；正式窗口再核对真实几何变化。重验非法值拒绝以及活动编辑先取消再结构变化。
5. 在没有外部离线修改的样本中删除A→GC→undo，恢复同一A及组件身份/值/关系；redo再退出。另设离线改字段/换组件样本，严格按最终选定的历史恢复与冲突规则验收，不能以保活代证历史值。离场对象不再运行或提取。
6. 移除 C 后添加同类型 D：旧字段目标、迟到控件动作和旧编辑token不得写 D。重复owner、将历史恢复应用到非原World、抢占另一World成员、跨Runtime恢复、缺少必需Transform、删除仍被依赖的Transform全部拒绝，目录/游标不变；不把这些错误扩成禁止兼容Detached对象普通attach。
7. 用真实 Counter 行为推进一次运行更新，验证同一组件字段变化；disabled、离场与stop后不再更新，编辑呈现可仍存在。至少一条现有Program/JIT绑定修改同一字段，类型/对象身份不复制。
8. 更新回调请求移除正在遍历的组件：当前视图直到边界保持有效，提交后下一查询不含它；请求状态和完成结果一致，生命周期回调内不能递归提交。
9. 两个 World 使用同一 Runtime：互不接受对方成员操作，关闭一个不破坏另一个；两个 Runtime 间引用/组件挂载拒绝。World关闭前取消相关任务，Runtime最后结束。

**下列保活案例是本片交付门槛，不能由显式destroy或常驻history代证：**

- 在正式类型发布阶段同时注册一个宿主持有者类型，以普通Actor强引用字段保存A。随后只保该宿主持有者的正常根，撤掉测试临时Actor/组件根；detach A并清空全部历史/redo或销毁Session，推进真实GC，仍从普通强边读到同A及完整组件id、顺序、owner、author-enabled与字段值。World活动查询/tick/投影仍为空。
- 另一个正常历史样本执行Actor detach→GC→undo，恢复相同身份与成员位置；独立remove组件→GC→undo恢复同组件/owner/顺序。Delete→undo→裁掉redo不改变live对象；错误World/owner/依赖恢复整条拒绝，cursor不动。
- 独立移除C后，Actor不再经组成边保C；没有其他强路径时，释放历史/组件根后C可回收。只持附属组件的样本必须保Actor整组，最后外部根释放后整环可收集。
- 在没有执行destroy的独立样本中，释放最后宿主强引用后完整组最终可收集，证明World索引、管理登记及UI没有隐藏保活。随后用另一样本，对仍被宿主持有的离线Actor明确destroy，验证Actor/组件身份失效，旧历史restore拒绝；两种结果分别报告。
- World.close按独立获准政策另列清楚预期的case，不能把close终结或Runtime析构代替上述普通detach/历史裁剪的C验证。

正式继承会审通过后，该链还须包括一个具体派生组件：基类声明字段在其同一个ObjectId上被编辑/撤销；Component基类查询包含它，exact查询与派生包含查询结果不同；字段不因基/派生视图出现两次而产生两个权威值。必要Core反例与source/JIT接线按§4.2.3合同统一完成，不用假角色表使验收通过。

Runtime实施前置使用现有真实端到端入口覆盖：Base强/弱引用保持Derived同id且不漏其强边；基类hidden引用只追踪一次；合法非零基地址调整及described派native基段；构析与失败清理各一次；无关/跨Runtime引用拒绝且不写字段；基类Program的同步及协程self复制/observe/try_acquire、跨yield与旧Lease仍指同Derived。定型序列还覆盖GC中途插入、删首元素、扩容、清空、销毁owner与有限work预算，以及引用批次最后一条无效时全部逻辑值不变。它们按I/II验证Core机制；C产品语义必须另过上述正式World链，不建单测/mock框架。

### 11.2 正式编辑器接入

从现有唯一产品入口，以正式控件完成空Actor→添加组件可见→移除组件消失但Actor保留→组件属性编辑/一次undo→删除/恢复。窗口证据核对实际GPU有无实例、Actor列表与组件面板及同身份恢复，不能只看CPU计数。

保留现有视觉、数值拖调、Unicode文本/输入法生命周期、取消、accepted-frame/失焦/Surface/关闭路由；重点验收组件删除后旧控件会话不影响替代组件。这里不启动S3、不增加pin、多窗、保存、节点图或完整Play模式。已有S2事实不在本轮研究重验；实施完成只在改动覆盖的正式路径做必要dev/release端到端验收。

**完成定义：** 新World同时服务两类宿主，全部旧Scene/位置专用入口已迁移，组件身份/关系/历史/运行参与的反例均满足选定语义。仅文档完成、启动成功或能摆方块均不构成对象模型实现完成。

## 12. 当前交付与后续消费者

- I、II及最小身份归属查询由Runtime负责人验收，III复用正式Core机制，不重建类型或GC。
- III的空Actor、组件/依赖、四态、TRS、实际JIT/任务、普通引用保组、close后统一destroy、独立GC及跨World请求边界已通过dev/release真实E2E；当前能力与限制见World模块文档。
- IV继续通过同一公共头迁移Session、应用及renderer，移除旧Scene/位置专用入口；III完成不等于编辑器整轮完成。
- 首片编辑仍为当前成员的单写者边界。父子/多空间部件操作、跨文档移交UI、资产/网络、通用调度与离线多写设施不纳入本轮。

## 13. 延后范围与已知约束

- 资产/保存格式：ObjectId和FieldId不能持久化；未来文档/资产层映射持久身份，类型布局升级需版本合同。现在不定格式。
- 网络复制：运行期ObjectId不能当跨进程身份；owner也不等于网络authority。现在不建设网络ID或复制系统。
- 物理/音频等外部系统：挂载、停用、卸载必须与其资源接入相应阶段；任意外部副作用不能承诺结构事务自动回滚。没有消费者不预做插件总线。
- 大规模计算：系统可以批处理，但当前未测量。未来对象搬移需要Runtime正式重定位合同，不凭句柄稳定推导可memcpy。
- 图表达：SOP/RDG/纹理等可各自表达其领域流程；对象组合不是蓝图式通用节点程序，本片不建设图设施。
- 运行期类型增加、一般源码对象图访问、布局迁移、原子/无缝跨World转移、跨文档移交UI、并发调度、通用Inspector、空间父子/多部件操作均未被本方案首片承诺。完全Detached后独立attach兼容World是§4.1的普通成员操作建议，不等于资源迁移框架。空间部分列出了候选语义，完整TRS建议进入首片；正式基础派生不在延期范围，I交付状态见文首。

## 14. 一手来源与版本

来源条目及交叉质询结论在本文件内集中维护，不另建旁路研究计划。外部行为只在所列版本/范围成立；没有据此提出已测量的性能收益。

### 本仓库合同

- [工程指南](../engineering.md)：所有权、地址稳定性、值与批处理、真实测量规则。
- [共享术语](../../CONTEXT.md)：对象、强/弱引用、上下文与类型/代码版本的区分。
- [Runtime公开入口](../../src/runtime/runtime.hpp)与[实现](../../src/runtime/runtime.cpp)：第2节所列符号；注册、引用、值与销毁事实由Runtime负责人定向核对。
- [执行结构](../runtime-structure.md)与[当前语言](../../LANGUAGE.md)：单类型绑定、执行/协程/代码版本范围；不将研究提案当作已实现语言能力。
- [编辑器当前说明](../modules/editor/README.md)、[首个编辑器历史方案](../first-editor-plan.md)：现有行为、取消与历史合同、尚未实现的持久身份职责；本轮不修改它们。

### Unreal Engine：5.6 官方文档/API描述

- **[U1]** [Actor Lifecycle](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-actor-lifecycle?application_version=5.6)：Spawn/初始化/BeginPlay、EndPlay和后续GC；仅用于区分事件职责。
- **[U2]** [Components](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-in-unreal-engine?application_version=5.6)；[UActorComponent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UActorComponent?application_version=5.6)：非空间/空间组件、registration与activation、DestroyComponent的公开合同。
- **[U3]** [UObject](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UObject?application_version=5.6)、[UObjectBase](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UObjectBase?application_version=5.6)、[UClass](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UClass?application_version=5.6)：对象的类由UClass描述；UClass经UField/UStruct派生，`GetSuperClass`公开父类型，`AssembleReferenceTokenStream`说明合并类层次GC信息。支持“真实base+完整追踪”的参考，不证明本项目需复制元对象体系。
- **[U4]** [USceneComponent](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/USceneComponent?application_version=5.6)、[UPrimitiveComponent](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UPrimitiveComponent?application_version=5.6)：前者有Transform/空间附着，后者再承担几何的渲染或碰撞职责；并非所有Component空间化，也不意味着本片需要物理/代理中间层。
- **[U5]** [UObjectBaseUtility](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UObjectBaseUtility?application_version=5.6)、[Object Handling](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-object-handling-in-unreal-engine?application_version=5.6)、[Object Pointers](https://dev.epicgames.com/documentation/en-us/unreal-engine/object-pointers-in-unreal-engine?application_version=5.6)：Outer用于包含/路径/包关系，Component owner可沿Outer找到Actor；可达性来自正式追踪的引用及根，不能据Outer字段名推导Outer→inner自动强持有。Actor持组件的强图须另有明确表达。
- **[U6]** [Objects：Destroying Objects](https://dev.epicgames.com/documentation/en-us/unreal-engine/objects-in-unreal-engine?application_version=5.6#destroyingobjects)：实际返回标题为5.6的页面含`MarkAsGarbage`及`gc.PendingKillEnabled`条件补充；禁用相应机制时，标记对象甚至已Destroy离level的Actor仍可能等强引用释放才最终回收。因此严格区分逻辑退场、游戏使用有效性与物理回收，不从页面旧叙述推导“所有配置立即失效/下次GC必回收”。本Runtime的显式destroy立即代际失效是本仓合同。

本次未读取Epic授权源码仓库，不提供UE源码SHA，也不根据文档推断未明示的内部调用顺序。

### Godot：4.5-stable

官方仓库tag核验SHA：`876b290332ec6f2e6d173d08162a02aa7e6ca46d`。

- **[G1]** [Node源码](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/scene/main/node.cpp#L3304)：`_set_tree → _propagate_enter_tree/_propagate_ready/_propagate_exit_tree`；相关实现269、304、322、391行。进入先父后子，ready/退出先子后父；`remove_child`在1715行附近处理脱树，PREDELETE承担递归释放。顺序属于Godot树合同，不作为本方案任意组件依赖排序的替代品。
- **[G2]** [Node::set_owner](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/scene/main/node.cpp#L2214)与[owner文档](https://docs.godotengine.org/en/4.5/classes/class_node.html#class-node-property-owner)；[Node3D::_notification](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/scene/3d/node_3d.cpp#L131)；[SceneTree删除队列](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/scene/main/scene_tree.cpp#L1600)：分别用于保存归属、空间关系接入与延后删除，不混成一个owner概念。
- **[G3]** [UndoRedo 4.5](https://docs.godotengine.org/en/4.5/classes/class_undoredo.html)、[EditorUndoRedoManager 4.5](https://docs.godotengine.org/en/4.5/classes/class_editorundoredomanager.html)：undo引用保留被删除对象、`commit_action(false)`登记已预览操作、每场景历史与显式context。
- **[G4]** [undo_redo.cpp](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/core/object/undo_redo.cpp#L346)：`Operation::delete_reference`（37行）、`discard_redo`（51行）、`add_undo_reference`（255行）、`_pop_history_tail`（290行）、`_process_operation_list`（346行）及`undo`（430行附近）。清引用可能真正删除对象；操作错误/已消失对象不形成本项目要求的整组回滚保证。这是源码观察，不是对所有编辑器操作原子性的概括。
- **[G5]** [Object/GDCLASS](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/core/object/object.h#L494)、[ClassInfo](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/core/object/class_db.h#L102)、[ClassDB查找](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/core/object/class_db.cpp#L1042)：`initialize_class`先初始化基类，ClassInfo保存inherits_ptr；登记检查base存在，is_parent_class/get_method/get_property_list沿父链。正式反射继承深入查找与访问，不能用角色集合替代。`Object::cast_to`（object.h:801）明确使用其有限继承假设，不授权本项目猜任意C++偏移。
- **[G6]** [ObjectDB登记](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/core/object/object.cpp#L2230)、[RefCounted/Ref](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/core/object/ref_counted.h#L196)、[RefCounted循环限制](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/doc/classes/RefCounted.xml#L7)：ObjectDB登记一个原生身份；Node不走RefCounted分支，Ref计数归零可memdelete且循环不能自动释放。本项目保持现有统一追踪GC，不复制这项寿命分流。
- **[G7]** [GDScript实例建立](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/modules/gdscript/gdscript.cpp#L151)、[Object类查询说明](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/doc/classes/Object.xml#L584)：GDScriptInstance另有members数组并关联原Object；它没有再分配一个原生ObjectID，但仍有脚本定义/实例成员桥接层。Frost要求语言定义直接进入共享TypeDefinition，不能以类似旁挂成员数组冒充完整统一类型。
- **[G8]** [Node3D求世界变换](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/scene/3d/node_3d.cpp#L630)、[VisualInstance3D](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/scene/3d/visual_instance_3d.h#L35)、[MeshInstance3D](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/scene/3d/mesh_instance_3d.h#L43)：渲染节点沿Node3D派生，各自有local TRS/空间状态；父类型链与实际Node父子关系仍是两张不同关系。本文不照搬其矩阵乘法约定，沿Frost现行行向量合同。

### Stride：正式4.3.0.2507

[官方站](https://www.stride3d.net/)链接`stride3d/stride`；[正式release](https://github.com/stride3d/stride/releases/tag/releases/4.3.0.2507)非draft/prerelease。annotated tag对象为`845e2e7b116a7314847f86af32af881d8cd12937`，剥离源码commit为`e023d874ba2985dbb132e9608f00d28d021b6f78`。下列全部固定commit，不使用master/nightly。

- **[S1]** [Entity](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/engine/Stride.Engine/Engine/Entity.cs#L31)、[EntityComponent](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/engine/Stride.Engine/Engine/EntityComponent.cs#L17)：Entity是sealed ComponentBase派生；EntityComponent只是abstract/IIdentifiable并提供内部写owner与Guid。前者常规构造添加Transform，后者不继承ComponentBase；本项目不按名字推导公共GC或强制Actor空间化。
- **[S2]** [EntityComponentCollection](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/engine/Stride.Engine/Engine/EntityComponentCollection.cs#L204)、[EntityProcessor](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/engine/Stride.Engine/Engine/EntityProcessor.cs#L176)：集合验证null/重复实例/类型限制/foreign owner并维护关系，移除清owner再通知manager；处理器用CLR `IsAssignableFrom`而非自建类型层次。其Get<T>返回首个可赋值项，不适合直接照搬到本项目多实例查询合同。
- **[S3]** [TransformComponent](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/engine/Stride.Engine/Engine/TransformComponent.cs#L220)、[TransformProcessor](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/engine/Stride.Engine/Engine/Processors/TransformProcessor.cs#L110)、[ModelComponent](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/engine/Stride.Engine/Engine/ModelComponent.cs#L24)、[ModelTransformProcessor](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/engine/Stride.Engine/Engine/Processors/ModelTransformProcessor.cs#L12)：Transform持TRS/父子关系并求矩阵；Model并不继承TRS，处理器声明Transform依赖，在其PostOperations注册/注销。这是能力共享空间的具体实现依据，其场景组织/Transform树耦合不自动成为Frost规则。
- **[S4]** [DisposeBase](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/core/Stride.Core/DisposeBase.cs#L12)、[ComponentBase](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/core/Stride.Core/ComponentBase.cs#L66)、[EntityManager](https://github.com/stride3d/stride/blob/e023d874ba2985dbb132e9608f00d28d021b6f78/sources/engine/Stride.Engine/Engine/EntityManager.cs#L231)：Entity除CLR内存可达性外还有显式资源引用计数/AddReference/Release/Destroy，组件并非全部继承这一资源协议。Guid是可设置标识，不等同ObjectId的context/generation存活校验；不向Frost复制另一套资源计数作为对象GC。

### Unity Entities：1.4 文档通道，读取时1.4.8

访问日期2026-09-29；页面元数据为1.4.8，生成日期2026-07-16。官方可用URL为`@1.4`通道，固定`@1.4.8`文档URL未提供可用页面；不将浮动通道链接伪称永久源码快照。

- **[E1]** [Archetype concepts](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/concepts-archetypes.html)、[Structural changes](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/concepts-structural-changes.html)：16KiB chunk、按组件类型分列、结构变化迁移与删除填洞、同步点约束。
- **[E2]** [Entity command buffers](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/systems-entity-command-buffers.html)、[Automatic playback](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/systems-entity-command-buffer-automatic-playback.html)：记录/播放时机、临时Entity及播放时重映射、buffer顺序。
- **[E3]** [Enableable components](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/components-enableable-use.html)：保留组件实值和archetype、改变查询参与，仍有写入依赖。
- **[E4]** [System update order](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/systems-update-order.html)：显式组与顺序作用域，不据此为本项目引入万能依赖推断。

### Flecs：v4.1.4

官方仓库tag核验SHA：`dcbaf278d15379aadea10421bbcbd3c4ead529a3`。

- **[F1]** [entity.c](https://github.com/SanderMertens/flecs/blob/dcbaf278d15379aadea10421bbcbd3c4ead529a3/src/entity.c#L188)：`ecs_add_id`（1744行）→`flecs_add_id`（465行）→defer或table transition→`flecs_commit`；`flecs_move_entity`先建立目的行、发送移除通知、搬实值、改索引、删旧行、再发新增通知。[table.c](https://github.com/SanderMertens/flecs/blob/dcbaf278d15379aadea10421bbcbd3c4ead529a3/src/storage/table.c#L1912)：move/ctor/dtor钩子或memcpy；1761行附近删除填洞也可能移动实值。
- **[F2]** [commands.c](https://github.com/SanderMertens/flecs/blob/dcbaf278d15379aadea10421bbcbd3c4ead529a3/src/commands.c#L1075)：`defer_begin`增加嵌套计数（87行），add/remove记录（248/264行），最外层`defer_end`切换队列并播放，按entity合并部分迁移并丢弃delete之后的陈旧操作（1131–1154行）。这是结构延后协议，不是外部副作用回滚事务。
- **[F3]** [Sparse](https://github.com/SanderMertens/flecs/blob/dcbaf278d15379aadea10421bbcbd3c4ead529a3/docs/ComponentTraits.md#L1804)与[DontFragment](https://github.com/SanderMertens/flecs/blob/dcbaf278d15379aadea10421bbcbd3c4ead529a3/docs/ComponentTraits.md#L532)：同一引擎也能分离组合与物理布局，特定路径仍有查询/功能限制；[缓存查询](https://github.com/SanderMertens/flecs/blob/dcbaf278d15379aadea10421bbcbd3c4ead529a3/src/query/cache/cache_iter.c#L124)仍需匹配表和失效维护。
- **[F4]** [pipeline.c](https://github.com/SanderMertens/flecs/blob/dcbaf278d15379aadea10421bbcbd3c4ead529a3/src/addons/pipeline/pipeline.c#L125)：125–208及587–655行根据实际读写声明推导staged写的merge边界。该证据说明自动调度需要正式读写合同，不能仅添加一个stage枚举就宣称支持并行。

## 15. 交叉质询记录与收口

| 质询 | 发现的真实问题 | 当前结论或条件草案 |
| --- | --- | --- |
| 生命周期→数据/集成：外部Actor根能否保组？ | 现有Runtime看不到外部记录中的动态组合边；清历史只撤根可以留下A、丢C | 若选C，需对象内强边及关系寿命；现有Core缺口不能替产品决定是否选择C |
| 主持→生命周期：是否为了现有Core限制而强迫整组销毁？ | World销毁权与普通引用能否保组成是正交问题 | C及其他寿命候选公平比较；close独立待审 |
| 数据→生命周期：同型组件与查询顺序 | `get<T>()`默认取第一个会使历史/系统含糊；桶顺序不等于逻辑顺序 | 以具体Component ObjectId操作；查询返回范围；唯一性按真实类型约束 |
| 集成→生命周期：组件先移除，再删除Actor如何undo？ | 已移除组件不再属于Actor当前组；复制旧全图会重复拥有 | 单独组件根保活，memento记旧owner；线性重放先恢复Actor再恢复组件，失败cursor不动 |
| 集成→数据：排队是否先记历史？ | pending若冒充成功，会使history与World分裂 | 无伪造ObjectId；静止点同入口提交完成后才记成功；同步返回前全部索引一致 |
| Runtime→对象：静止点与显式destroy | `check_execution_entry`不排除活动语言执行；无原子批destroy；根不阻止销毁 | 用`check_code_publication`加World自身准入；退出业务查询后逐项处置，明确不可逆失败边界 |
| 集成→历史清理：裁剪能否终结对象？ | 根据Delete等动作名destroy会误杀live或外部强持对象 | C下所有裁剪/析构只撤根，永久destroy是独立命令；不要求归还token |
| 主持/应用→身份：删ObjectKey后UI与存档怎么办？ | ObjectId不支持算术编码，hash有冲突且非持久键 | UI稳定绑定独立分配/退役并校验incarnation；未来文档/资产层定义持久映射，现行计划在实施时同步 |
| 数据/集成→编辑运行：preview与tick是否并写？ | cancel会覆盖系统期间写入 | 首片编辑World不跑游戏更新；运行期调试写与undo延期 |
| 用户→引擎类型：统一object分类是否等于基础类型派生？ | 当前连Object基类TypeId都没有，C++继承不自动发布 | 增加正式I1–I6合同与表示矩阵；角色描述不能仿造base/is-a；Native与现有source/JIT一并闭合 |
| UE席→Core：UClass/Outer/GC能直接复制吗？ | 元类对象化、Outer命名、组件强图与销毁配置是不同机制 | 借正式层次与完整追踪，不预建UClass元对象/Outer/CDO；UE销毁结论保留5.6配置限定 |
| 空间派生与空间组合 | 前者适合独立pivot，后者适合能力共享/替换后保锚点，删除行为不同 | 默认推荐独立Transform并显式绑定；保留空间派生反例/计数，不按品牌或多数票裁决 |
| 集成/Runtime→C：记录仅保存在World/history是否足够？ | 即使能扫边，外部记录先消失仍无法兑现普通Actor引用保组 | C元数据随对象存续，集合追踪/受控访问/记录寿命三者一起闭合；World只保非拥有查询/管理索引 |
| 正式Component基类引用能否缩小集合范围？ | 具体元素动态类型不同，不等于必须任意异构容器 | C收窄为声明类型Component的定型引用序列；仍处理扩容/erase与暂停GC游标，不把List做可写协变 |
| 方法继承是否顺带保证native/语言override？ | 现成员指针std::invoke可发生C++虚调用，Core没有统一虚槽 | MethodId选择固定Invoker；内部C++行为不宣传为语言override，结构步骤仍由World编排，统一虚分派延期 |
| 对象→Runtime：强序列与owner如何避免半提交？ | 分开insert/write即使各自安全也不保证组合原子性；仅reserve序列不够 | II条件草案以同步引用批次预检全部目标/资源，splice配owner引用写；强弱按最终元数据，不能提前选弱owner |
| Runtime→对象：暂停GC重扫能否只shade(owner)？ | 已标记owner的普通shade直接返回，会漏掉取消游标后尚未扫的字段 | II新增已标记owner重新入灰路径，复用侵入链、处理结构版本/预算，真实E2E覆盖 |
| 应用→切片：II是否先依赖Actor/World才能验？ | 先造临时Composition会形成循环和第二入口 | II用Core真实Holder/派生元素图验证机制；III/IV验证选定产品合同 |
| 用户→原C定案：提出的命题是否确实合理？ | 原结论把用户提出候选误当成正确性依据，并预设弱owner及历史值恢复 | 撤回定案；区分数据/组成与资源参与，比较真实正反场景；追踪GC强环不是弱owner的充分理由，保活不是快照 |

简要沿革：初稿比较寿命候选；随后一度按C已定收敛接口。用户要求中肯证实后撤回定案，复用原三席做有限反证，补行业一手证据且保留未知。后续按用户认可具体化有效/失活及退出合同；Core通用能力与业务选择分开，当前派发状态统一见文首。未新增专家或第二研究稿。

## 16. 行业补充：World、实例、存储与加载单元

Actor是某个类型的实例；“World中的Actor”可以描述它当前属于哪个世界、由谁安排运行。这个说法本身不决定对象payload嵌在World容器、只被World引用，还是位于独立Runtime存储；也不决定离World后的寿命。以下是可证事实及其边界，年份指公开材料年份，不推断这些引擎今天全部内部实现。

### 16.1 UE5.6：世界上下文与加载组织

官方UWorld文档把它描述为Actor/Component所在的顶层地图或沙盒上下文；世界包含persistent level及streaming levels，编辑器可有多个World。World Partition将空间内容组织为可流入/流出的网格cell，引用依赖会影响共同加载，加载与激活亦有区分。这支持把类型实例、活动世界、空间/加载组织分别说明，不能据此声称World逐个按值保存全部业务payload。[UWorld API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UWorld?application_version=5.6)、[World Partition](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine?application_version=5.6)、[Level Streaming](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-streaming-overview-in-unreal-engine?application_version=5.6)。Actor退出与GC边界见§5.3、§14 [U6]，不压缩为“离世界必死”或“强引用永不失效”。

### 16.2 EA／Frostbite

- **Mattias Widmark，EA DICE，GDC 2012，Terrain in Battlefield 3: A Modern, Complete and Scalable System。** 原PDF第14–15页区分四叉树节点与是否驻留的payload；第42–44页以raster tile及layout组织流加载，第59页受池容量约束释放/取得payload。这是Frostbite 2地形系统证据，节点不等同一般Actor，释放terrain payload不等同销毁游戏对象。[原始PDF](https://media.gdcvault.com/gdc2012/slides/Programming%20Track/Widmark_Mattias_Terrain_in_Battlefield3a.pdf)、[官方场次](https://www.gdcvault.com/play/1015676/Terrain-in-Battlefield-3-A)。
- **Ludovic Chabant，Electronic Arts，GDC 2018，A Tale of Three Data Schemas。** 原PPTX第29、32–35张区分编辑、存储与运行数据表示；第65及81–82张备注说明执行效率与live edit的表示差异。它支持索引/投影可以不同，不能证明通用Entity继承、GC、owner或对象地址合同。[EA原始PPTX](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc18-tools-tutorial-day-a-tale-of-three-data-schemas.pptx)、[EA发布页](https://www.ea.com/games/battlefield/news/a-tale-of-three-data-schemas)。
- **David Robillard，Motive/EA Project Technical Lead，2022-11-17，Inside Dead Space #3: Aboard the Ishimura。** “Joining It All Together”说明区域内容流入/流出须符合玩家进度；没有披露保留原对象还是保存后重建。[EA采访](https://www.ea.com/ea-originals/news/inside-dead-space-3-aboard-the-ishimura)。

**未知：** 一般Entity/Component基类、World物理持有方式、强弱边、卸载时原身份是否保留及World关闭对象政策。本轮不以地形或渲染资料填补这些空白。

### 16.3 Ubisoft：Snowdrop、Dunia、Anvil分别看待

- **Joshua Simmons，Massive Entertainment—A Ubisoft Studio，GDC 2024，Upgrading the Snowdrop Engine for the Massive World of Avatar: Frontiers of Pandora。** 原PDF第12页讨论对象加入world；第26、31页为128m scatter sector及SoA实例数据；第41页Static Mesh System将流入的静态对象打包成组，丢弃或不创建完整graph object，发生破坏时再重建full object；第54页区别Avatar服务器运行时streaming与The Division 2启动时加载。[GDC原始幻灯片](https://www.gdcvault.com/play/mediaProxy.php?sid=1034412)。第41页只证明该静态系统的表示变化，不证明Actor普通引用、ObjectId复用或GC合同；它反驳的是“所有可见实例必须常驻完整对象”的更强假设，不能作为候选C的直接反例。
- **Remi Quenin，Ubisoft Montréal，GDC 2015，Fast Iteration for Far Cry 4—Optimizing Key Parts of the Dunia Pipeline。** 原PDF第3–4页将世界描述/entities/properties与编辑器抽取、编译和打包分开；第53–54页介绍异步内容streaming及虚拟文件访问。它确认数据供应层次，不确认Actor成员协议、强边或卸载销毁政策。[GDC原始PDF](https://media.gdcvault.com/gdc2015/presentations/Quenin_Remi_Fast_Iteration_FarCry4.pdf)。
- **Nicolas Lopez，Ubisoft Montréal Technical Architect，GDC 2025（3月20日），Rendering 'Assassin’s Creed Shadows'。** 官方场次和原作者访谈确认Anvil及该作品的渲染背景；本轮已读材料未确认通用Actor/World成员、GC、owner或强引用合同，保持未知。[GDC官方场次](https://gdcvault.com/play/1035526/Rendering-Assassin-s-Creed-Shadows)、[GDC原作者访谈](https://gdconf.com/article/how-ubisoft-brought-feudal-japan-to-life-rendering-assassin-s-creed-shadows-with-nicolas-lopez/)。不以Snowdrop或Dunia行为代言Anvil。

因此没有可直接套用的单一“育碧对象模型”。本轮不以散布sector、坐标cell、资产包和Actor寿命相互替代。

### 16.4 Rockstar／RAGE

- **Brett Laming，当时Rockstar Leeds，GDC 2011，Creating Your Building Blocks: Modular Component AI Systems，Part 1。** 合讲PDF第13页将类型继承、世界结构、数据流/边界及批处理分列；第16页区分空间参考系、组合/依赖和任务顺序；第17页有cWorld/cWorldRegion示例，第18页区分结构与属性变化。没有看到这些图被声明为某版本RAGE生产对象模型，不能由讲者雇主推出RAGE采用特定GC或World销毁规则。[GDC原始PDF](https://media.gdcvault.com/gdc2011/slides/LamingMcGinnisChampandard_AISummit_CreatingBuildingBlocks.pdf)、[官方场次](https://gdcvault.com/play/1014572/Creating-Your-Building-Blocks-Modular)。合讲后半部分由其他机构讲者负责，不统称Rockstar实现。
- **Fabian Bauer，当时Rockstar North，SIGGRAPH 2019，Creating the Atmospheric World of Red Dead Redemption 2: A Complete and Integrated Solution。** 原始课程摘要确认云、雾、体积效果与间接光数据协作；本轮仅据摘要/讲者介绍，不声称取得内部对象层或实体流加载合同。[原始课程页](https://www.realtimerendering.com/advances/s2019/index.htm)。

**未知：** RAGE通用实体基类、World按值存储还是目录、成员加入/退出协议、组件保活方向、加载单元与对象寿命关系。公开证据不足以将其归到C或World绑定销毁任一侧。

### 16.5 对本项目能得出的结论

这些材料共同提醒我们分别定义实例身份、当前World参与、组成关系、空间关系与资源驻留。它们没有替Frost决定普通强引用应该保住多大对象图。统一身份/追踪GC是本仓事实；C与替代的取舍按§5.3评估。正式继承及通用引用机制服务各业务候选，不能用行业未知代替World所有权会审。
