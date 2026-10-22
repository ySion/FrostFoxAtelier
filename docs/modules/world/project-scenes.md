# 项目类型、场景与运行会话

## 准备与所有权

`editor::Session::Prepare` 在 Session 注册 World/Physics 类型并构造 World 后、创建首个对象前调用一次，用于准备项目 Source 类型、Program 和生命周期入口，并返回代码拥有者。Session 保持代码存活至 World 完成 End；Runtime 最后销毁。失败候选沿相同 RAII 清理。

编辑加载与运行加载都通过 `deserialize(text, Project&, Prepare={})` 恢复真实对象图。候选成功后由应用发布，失败不修改原文档或文件。字段默认值来自 Runtime 创建真实类型，加载不调用会额外生成默认网格组成的工厂。

## 真实类型与编辑

Entity 与 Component 保存真实限定类型名，以 Runtime find_type / is_a 校验。参数发现和编辑沿同一 FieldPath；显示名称不充当身份。标量、String 与内联 record 递归编辑，枚举保存声明成员名，String 保存为 JSON 字符串，整数严格检查符号与范围，浮点必须有限。TRS 历史保存完整组；移除对象可由历史保活，但保活不授予文档成员资格。

属性记录由一个共用的叶解码器逐叶解码，场景加载、Play 增量和编辑器操作符参数都经过它。`editor::decode_properties`（[properties.hpp](../../../src/editor/properties.hpp)）是严格模式入口：目标是对象或调用方持有的记录值，未知字段、类型不符、越过硬边界都报错，不夹值；整数字段中可无损转换的浮点值照常接受；记录中缺省的字段保持原值。传入写入钩子时，内联叶交给钩子（例如收集成补丁），不写入目标；钩子不接收引用叶，所以不能与引用解析同时传入。Play 增量对已存在对象的 String 字段经 `World::write(ValueInput)` 整值写入，与标量叶一样经过类型规则与准入检查，被拒时整值恢复并以 error 级 `command_rejected` 报告。

## Scene v8

统一写出 v8，旧 v1–v7 读取时迁移，再按 v8 保存。顶层包含：

- `version: 8`、十进制字符串 `next_id`。World 设置是 Environment Entity 上 `world.EnvironmentComponent` 的普通属性，见下文 v6 迁移。
- `roots`：有序顶层 Entity 文档 ID。
- `entities`：Entity 记录，每个包含 `id / type / name / enabled / properties / children / components / space`。
- `space`：null 表示无空间；否则保存 position、rotation、scale 和 origin。origin 为 null 时继承组合祖先，为字符串 `"0"` 时使用世界，为 Entity ID 字符串时使用显式空间参考。
- `components`：owner 的有序组件记录，包含自身 `id / type / name / enabled / properties`；网格与材质额外保存现有资源 payload，EnvironmentComponent 额外保存 `sky_map`（天空贴图的资产键，空字符串表示没有贴图）。组件不保存 TRS、parent 或 root。
- `materials`：按文档 ID 去重的材质资源定义；网格槽使用 `resource` 引用该 ID。资产保留来源 GUID，匿名材质或实例保存内联参数，以及具名父材质与覆盖字段。

Entity、Component 与材质定义共用一个文件内的 uint64 ID 域，字符串编码避免 JSON 数值精度丢失。引用字段用 null 或 `{"$ref":"ID"}`，引用序列用这些值的数组；持有/观察属性由 Runtime 字段定义决定，不在 JSON 创造另一套引用类型。保存不记录 Runtime TypeId、ObjectId、FieldId 或后端句柄。String 字段写成 JSON 字符串。

加载先创建全部真实对象，恢复标量、资源与引用，再按最终组合与空间关系统一校验和注册。空间参考作为整批计划提交，不依赖文件目录的先后顺序。文档加载与可移植子树导出拒绝场景外业务引用；同一 Session 内复制可以保留图外引用，发布时仍校验其目标存活和 World 归属。新增未保存字段保留类型默认值。

结构损坏总是拒绝整个候选：JSON 语法、重复字段、嵌套与容量上限、未知版本、无效或悬空 ID（含保留数据中的 `$ref`）、重复身份、多重归属、组合/空间环和非法空间参考。值层面的不符按加载策略处理：严格加载（`LoadPolicy::strict`）遇到首个诊断即拒绝；宽容加载（`LoadPolicy::tolerant`，默认）按对象保留无法应用的原始 JSON，报告诊断并继续：

- 未注册类型（`unknown_type`）：按原生基类创建占位对象——Entity 用 `world.Entity`，带 `mesh` 的组件用 `world.StaticMeshComponent`，其余用 `world.Component`。占位对象记录原类型名和全部数据，没有行为绑定，原生部分照常参与；保存按原类型写回。
- 未知已保存字段或键（`unknown_field` / `unknown_key`）：原值保留并写回。
- 类型不符（`mismatch`）：引用目标存在但类型不符、未通过共享 canonical TRS 检查的空间值、World 或组件校验拒绝的设置等，字段使用类型默认值，原值保留并写回。越过字段硬边界的值同样报告 `mismatch`，字段采用 `Runtime::constrain` 夹到边界后的值，原值保留并写回。整数字段中可无损转换的浮点值（如 `3.0`）直接应用并报告 `converted`。
- 缺失或无法读取的资源（`missing_resource` / `unreadable_resource`）：工程目录中已不存在的 Mesh、Material、Texture、天空贴图报告 `missing_resource`；目录中仍列出但读不出来（文件损坏、类型不符）的报告 `unreadable_resource`，消息带读取错误。两者都由占位资源代替，引用键原样写回，资源文件本身不被改写。缺失 Texture 只让该输入没有贴图，材质的其他参数照常；缺失 Material（含实例的父材质）用默认参数代替，实例覆盖照常；缺失天空贴图时天空按没有贴图渲染（map = 1），键原样写回；缺失 Mesh 的组件不绘制、没有材质槽，`materials` 绑定表作为保留数据原样写回，表中引用的场景材质随之保活。缺失 Mesh 的三角形碰撞跳过 Cook 并另报一条诊断，运行时该刚体按 World 失败策略处理。占位资源不能作为工程资产编辑、保存或指派；在 Asset Browser 或 Control 中直接打开无法读取的资产仍然报错。恢复文件后重新打开场景生效。
- 保留数据中的引用与活引用规则相同：子树导出拒绝图外目标，同一 Session 内复制继续指向原目标；目标在文档中被删除时写为 null 并报告 `reference_dropped`，撤销删除后恢复。
- 编辑某字段会释放它的保留值；`Session::discard_retained` 丢弃一个对象的全部保留值，占位对象仍保留原类型。两者都进入 Undo/Redo。

### v7 → v8 迁移

v8 只给 EnvironmentComponent 增加 `sky_map`。读取 v7 时，没有该键的 Environment 记录补上空字符串，其余不变，保存写为 v8。v8 记录缺少 `sky_map` 时报告 `mismatch` warning，天空没有贴图；值不是字符串时同样报告 `mismatch`，原值保留并写回。

`sky_map` 是隐藏字段，没有公开 FieldId，`World::write` 和属性记录都碰不到它；只能经 `presentation::set_sky_map_binding` 与解析出的 `rendering::SkyMapRef` 成对设置。加载经 `Project::sky_map` 解析键：同一个键在 Project 刷新看到文件变化之前返回同一个指针，所以重新打开或撤销都不会让 SceneSnapshot 误判天空变了。

### v6 → v7 迁移

v6 把 World 设置存在顶层 `world` 段（键名来自与 World Settings、Control 共用的 ViewOption 目录），天光是 kind 为 sky 的 LightComponent。读取 v6 文档时：

- `world` 段的每个键写到 EnvironmentComponent 的对应字段：`probe_volume_N_min_x` 写到 `volumeN.minimum.x`，`probe_volume_N_count_x` 写到 `volumeN.count_x`，其余同名。未知键、类型不符或越界的值与普通属性一样按上文的加载策略处理，保留在该组件上；违反跨字段规则（例如 GTAO 与 Local GI 同时开启）时整个组件取默认值，原记录整体保留。`world` 不是对象时按 `mismatch` 保留在文档级；缺少 `world` 段时报告 warning。
- 按组合前序的第一个 Sky Light 提供 `sky_color`、`sky_intensity`（缺省分别为 (1,1,1) 和 1），`sky_enabled` 为该组件、所属 Entity 及全部祖先都启用。其余字段对天光本来无效，直接丢弃。之后的 Sky Light 各报一条 `conflict` warning 并移除，数据不保留。所有 Sky 组件都从原 Entity 上移除，Entity 保留名称、空间和其他组件；变空的 Entity 报一条 info 级 `converted`。
- 只有 `world` 段至少有一个键或存在 Sky Light 时才生成 Environment：一个名为 Environment、没有空间的根 Entity，追加在 `roots` 末尾，Entity 与组件的 ID 依次取 `next_id` 和 `next_id+1`，`next_id` 增加 2。仓库里的示例都是 `world: {}`：Scene Lit Room 因 Sky Light 生成 Environment，其余不生成。
- 子树导入（`instantiate_subtree`）不是文档：v6 的 `world` 段被忽略，Sky Light 不导入并报 `conflict` warning。

v1–v5 先按严格导入升到 v6（v4 的旧阴影字段在这一步校验后丢弃），再走上面的迁移，然后经 v7 → v8。仓库里的示例场景保持 v6，打开时迁移，保存后写为 v8。

`Session::lighting_supplied()` 表示文件是否提供了 World 光照：v6 的 `world` 段至少有一个键，或 v7 及以后的文件中有 EnvironmentComponent；只因 Sky Light 生成的 Environment 不算。示例入口与 `--scene-lit-camera` 只在未提供时拟合 DDGI 体积（`initialize_environment`：没有 Environment 时创建它，不进历史、不标脏，下次保存写出），之后该标志为真。

诊断记录使用 `world::Diagnostic`（[src/world/diagnostic.hpp](../../../src/world/diagnostic.hpp)），`phase` 为 `load`，`Session::load_issues()` 返回本次加载的诊断，`Session::issues()` 返回当前仍未解决的数据。没有改名重定向表；字段改名前后的值以 `unknown_field` 保留，改回原名后恢复。

StaticMeshComponent 的 mesh、base_color、roughness、metallic、emissive 和 materials 使用专有资源验证；命名材质槽保存资源身份与显式覆盖意图。相同匿名材质被多个槽或对象引用时，往返仍恢复为同一个实例。场景内联实例随 Entity 子树复制，共享资产、父材质与 Texture 保持共享。早期 v6 的逐槽内联 payload 仍可读取，重新保存使用统一资源目录。导入失败撤销本次新增的 Session 资源缓存与文档身份，保留既有资源及其编辑状态。

灯通过 LightComponent 的 `properties.settings` 保存，空间来自 owner Entity。Primitive 的 `properties.body` 保存 BodySettings 与 PhysicsMaterial，Cook 数据是派生缓存。加载按待发布图中的 owner 世界尺度读取工程 Cook 缓存，包含导入目标 parent 与所有 origin；缓存缺失或校验不通过时从源网格重新 Cook 并写回，不产生诊断；内置网格消费 CPU 几何；Mesh 不可用时跳过。源网格可读但无法 Cook 时报告 `unreadable_resource` 并跳过该刚体。缓存准备在 World 发布与 Begin 之前完成。

RagdollComponent.parts 是显式持有 Entity 引用序列，约束目标是 `properties.target` 观察引用。独立空间部件保留其真实 metadata 组件，网格由同 Entity 的 StaticMeshComponent 提供。

## 旧格式导入

v1–v5 的 Actor 主体映射到真实 Entity 派生类型，根组件 TRS 提升到主体 Entity；具有独立空间的组件转为子 Entity，原 parent 转为空间和组合关系。纯 SceneComponent 只留下对应空间实体；能力组件和业务派生类型保留真实类型，不降级为基础组件。旧 Body 配置沿严格兼容边界合并到 Primitive，不能无损映射的内容明确拒绝。

旧 constraint_target 场景序号仅在导入时解析；保存后使用统一文档引用。旧 RagdollPart 的 metadata 与网格分离，控制器恢复显式 parts 绑定；含车辆组件的旧场景不再能加载。旧 v4 阴影预算字段严格校验后丢弃，使用当前直接光预算默认值。

## 运行与 World 配置

Session::start/update/stop 是运行宿主唯一驱动。start 创建 PhysicsScene 后开始 World，update 只调用 `World::update`，由它依次执行各 Tick 阶段和物理子步，它返回错误时停止并清理。代码保留到 close，重复 stop 不重复 End。Project → Run 从编辑快照创建独立 Session，不复用编辑 Runtime 身份，也不自动把运行结果写回编辑场景。

EnvironmentComponent 保存共享 SceneLightingSettings 的作者值，包括直接光预算；Session 历史负责持久化与撤销，Renderer 消费副本。诊断模式、GPU 缓存与实际 follow-focus 锚点不属于存档，详见[持久格式与责任](../editor/projects.md#持久格式与责任)。

端到端验收入口为 `project_scene_acceptance`、`world_acceptance`、`editor_document_acceptance`。
