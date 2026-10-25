# 项目类型、场景与运行会话

## 准备与所有权

`editor::Session::Prepare` 在 Session 注册 World/Physics 类型并构造 World 后、创建首个对象前调用一次，用于准备项目 Source 类型、Program 和生命周期入口，并返回代码拥有者。Session 保持代码存活至 World 完成 End；Runtime 最后销毁。失败候选沿相同 RAII 清理。

编辑加载与运行加载都通过 `deserialize(text, Project&, Prepare={})` 恢复真实对象图。候选成功后由应用发布，失败不修改原文档或文件。字段默认值来自 Runtime 创建真实类型，加载不调用会额外生成默认网格组成的工厂。

## 真实类型与编辑

Entity 与 Component 保存真实限定类型名，以 Runtime find_type / is_a 校验。参数发现和编辑沿同一 FieldPath；显示名称不充当身份。标量、枚举、String 与内联 Array 是叶，内联 record 递归保存；Array 保存为 JSON 数组，元素只能是标量、枚举或 String，元素是 record 或数组的类型不能保存或加载（`unsupported_operation`）。Parameters 编辑标量与枚举，不编辑 String 和 Array。枚举保存声明成员名，String 保存为 JSON 字符串，整数严格检查符号与范围，浮点必须有限。TRS 历史保存完整组；移除对象可由历史保活，但保活不授予文档成员资格。

属性记录由一个共用的叶解码器逐叶解码，场景加载、Play 增量和编辑器操作符参数都经过它。`editor::decode_properties`（[properties.hpp](../../../src/editor/properties.hpp)）是严格模式入口：目标是对象或调用方持有的记录值，未知字段、类型不符、越过硬边界和目标拒绝写入都报 `argument_mismatch`，不夹值，其他 Runtime 失败保留原错误码；整数字段中可无损转换的浮点值照常接受；记录中缺省的字段保持原值。带字段表和 `PropertyReport` 的重载是宽容入口，与场景加载相同：放不进字段的叶交给 report，越界的叶用夹值，只有 Runtime 失败才报错；`encode_properties` 是对应的编码入口，与场景保存共用叶编码。传入写入钩子时，内联叶交给钩子（例如收集成补丁），不写入目标；钩子不接收引用叶，所以不能与引用解析同时传入。Array 在调用方的存储里整体构造成一个值，作为一个叶写入或交给钩子，硬边界按元素检查。Play 增量对已存在对象的 String 与 Array 字段经 `World::write(ValueInput)` 整值写入，与标量叶一样经过类型规则与准入检查，被拒时整值恢复并以 error 级 `command_rejected` 报告。Play 增量按严格规则解码：越过硬边界的标量和 Array 元素都被拒，不夹值。

## Scene v9

只读写 v9，其他版本整体拒绝；仓库里的场景都是 v9。顶层包含：

- `version: 9`、十进制字符串 `next_id`。World 设置是 Environment Entity 上 `world.EnvironmentComponent` 的普通属性。
- `roots`：有序顶层 Entity 文档 ID。
- `entities`：Entity 记录，每个包含 `id / type / name / enabled / properties / children / components / space`。
- `space`：null 表示无空间；否则保存 position、rotation、scale 和 origin。origin 为 null 时继承组合祖先，为字符串 `"0"` 时使用世界，为 Entity ID 字符串时使用显式空间参考。
- `components`：owner 的有序组件记录，包含自身 `id / type / name / enabled / properties`；网格与材质额外保存现有资源 payload，EnvironmentComponent 额外保存 `sky_map`（天空贴图的资产键，空字符串表示没有贴图）。组件不保存 TRS、parent 或 root。网格槽的默认材质随网格资产解析，不写入场景。
- `materials`：按文档 ID 去重的材质资源定义；网格槽使用 `resource` 引用该 ID。资产保留来源 GUID，匿名材质或实例保存内联参数，以及具名父材质与覆盖字段。

Entity、Component 与材质定义共用一个文件内的 uint64 ID 域，字符串编码避免 JSON 数值精度丢失。引用字段用 null 或 `{"$ref":"ID"}`，引用序列用这些值的数组；持有/观察属性由 Runtime 字段定义决定，不在 JSON 创造另一套引用类型。保存不记录 Runtime TypeId、ObjectId、FieldId 或后端句柄。String 字段写成 JSON 字符串。

加载先创建全部真实对象，恢复标量、资源与引用，再按最终组合与空间关系统一校验和注册。空间参考作为整批计划提交，不依赖文件目录的先后顺序。文档加载与可移植子树导出拒绝场景外业务引用；同一 Session 内复制可以保留图外引用，发布时仍校验其目标存活和 World 归属。新增未保存字段保留类型默认值。

结构损坏总是拒绝整个候选：JSON 语法、重复字段、嵌套与容量上限、版本不是 9、无效或悬空 ID（含保留数据中的 `$ref`）、重复身份、多重归属、组合/空间环和非法空间参考。值层面的不符按加载策略处理：严格加载（`LoadPolicy::strict`）遇到首个诊断即拒绝；宽容加载（`LoadPolicy::tolerant`，默认）按对象保留无法应用的原始 JSON，报告诊断并继续：

- 未注册类型（`unknown_type`）：按原生基类创建占位对象——Entity 用 `world.Entity`，带 `mesh` 的组件用 `world.StaticMeshComponent`，其余用 `world.Component`。占位对象记录原类型名和全部数据，没有行为绑定，原生部分照常参与；保存按原类型写回。
- 未知已保存字段或键（`unknown_field` / `unknown_key`）：原值保留并写回。
- 类型不符（`mismatch`）：引用目标存在但类型不符、未通过共享 canonical TRS 检查的空间值、World 或组件校验拒绝的设置等，字段使用类型默认值，原值保留并写回。越过字段硬边界的值同样报告 `mismatch`，字段采用 `Runtime::constrain` 夹到边界后的值，原值保留并写回。整数字段中可无损转换的浮点值（如 `3.0`）直接应用并报告 `converted`。Array 按整值处理：任一元素类型不符时字段使用类型默认值；越界元素夹值后应用；两种情况都保留并写回整个原数组。
- 缺失或无法读取的资源（`missing_resource` / `unreadable_resource`）：工程目录中已不存在的 Mesh、Material、Texture、天空贴图报告 `missing_resource`；目录中仍列出但读不出来（文件损坏、类型不符）的报告 `unreadable_resource`，消息带读取错误。两者都由占位资源代替，引用键原样写回，资源文件本身不被改写。缺失 Texture 只让该输入没有贴图，材质的其他参数照常；缺失 Material（含实例的父材质）用默认参数代替，实例覆盖照常；缺失天空贴图时天空按没有贴图渲染（map = 1），键原样写回；缺失 Mesh 的组件不绘制、没有材质槽，`materials` 绑定表作为保留数据原样写回，表中引用的场景材质随之保活。网格的默认材质或它的贴图不可用时以替代品显示，警告记在组件的 `mesh` 上，不写入任何东西。缺失 Mesh 的三角形碰撞跳过 Cook 并另报一条诊断，运行时该刚体按 World 失败策略处理。占位资源不能作为工程资产编辑、保存或指派；在 Asset Browser 或 Control 中直接打开无法读取的资产仍然报错。恢复文件后重新打开场景生效。
- 保留数据中的引用与活引用规则相同：子树导出拒绝图外目标，同一 Session 内复制继续指向原目标；目标在文档中被删除时写为 null 并报告 `reference_dropped`，撤销删除后恢复。
- 编辑某字段会释放它的保留值；`Session::discard_retained` 丢弃一个对象的全部保留值，占位对象仍保留原类型。两者都进入 Undo/Redo。

CameraComponent（包括派生类型）在全部属性解码后、发布前执行整记录校验；near/far 各自在硬边界内仍必须满足 far > near。严格加载拒绝违反联合规则的记录；宽容加载在组件的 `properties` 上报告 `mismatch`，恢复真实类型的整记录默认值（包括作者引用和引用序列），并保留原始属性记录供保存与重开。此检查不依赖相机是否被选为主相机，也不代替运行中的消费端校验。

### 天空贴图

EnvironmentComponent 的 `sky_map` 是天空贴图的资产键。记录缺少 `sky_map` 时报告 `mismatch` warning，天空没有贴图；值不是字符串时同样报告 `mismatch`，原值保留并写回。

`sky_map` 是隐藏字段，没有公开 FieldId，`World::write` 和属性记录都碰不到它；只能经 `presentation::set_sky_map_binding` 与解析出的 `rendering::SkyMapRef` 成对设置。加载经 `Project::sky_map` 解析键：同一个键在 Project 刷新看到文件变化之前返回同一个指针，所以重新打开或撤销都不会让 SceneSnapshot 误判天空变了。

示例入口与 `--scene-lit-camera` 只在文档没有 Environment 时拟合 DDGI 体积：`initialize_environment` 创建 Environment，不进历史、不标脏，下次保存写出。已有 Environment 的设置属于作者，打开时不改动；官方示例都保存了自己的 Environment 或不需要拟合。

诊断记录使用 `world::Diagnostic`（[src/world/diagnostic.hpp](../../../src/world/diagnostic.hpp)），`phase` 为 `load`，`Session::load_issues()` 返回本次加载的诊断，`Session::issues()` 返回当前仍未解决的数据。没有改名重定向表；字段改名前后的值以 `unknown_field` 保留，改回原名后恢复。

`Session::issues()` 按对象的文档 ID 数值升序列出，同一对象内保留类型、字段和引用诊断的生成顺序，文档级数据最后列出；`serial` 从 1 起按返回顺序连续编号，不作为跨次查询的诊断身份。此顺序不依赖 Runtime ObjectId 的哈希或哈希表遍历顺序；`load_issues()` 仍保留加载时的报告顺序。

StaticMeshComponent 的 mesh、base_color、roughness、metallic、emissive 和 materials 使用专有资源验证；命名材质槽保存资源身份与显式覆盖意图。相同匿名材质被多个槽或对象引用时，往返仍恢复为同一个实例。场景内联实例随 Entity 子树复制，共享资产、父材质与 Texture 保持共享。槽绑定只经 `resource` 引用 `materials` 中的定义。导入失败撤销本次新增的 Session 资源缓存与文档身份，保留既有资源及其编辑状态。

灯通过 LightComponent 的 `properties.settings` 保存，空间来自 owner Entity。Primitive 的 `properties.body` 保存 BodySettings 与 PhysicsMaterial，Cook 数据是派生缓存。加载按待发布图中的 owner 世界尺度读取工程 Cook 缓存，包含导入目标 parent 与所有 origin；缓存缺失或校验不通过时从源网格重新 Cook 并写回，不产生诊断；内置网格消费 CPU 几何；Mesh 不可用时跳过。源网格可读但无法 Cook 时报告 `unreadable_resource` 并跳过该刚体。缓存准备在 World 发布与 Begin 之前完成。

RagdollComponent.parts 是显式持有 Entity 引用序列，约束目标是 `properties.target` 观察引用。独立空间部件保留其真实 metadata 组件，网格由同 Entity 的 StaticMeshComponent 提供。

## 运行与 World 配置

Session::start/update/stop 是运行宿主唯一驱动。start 创建 PhysicsScene 后开始 World，update 只调用 `World::update`，由它依次执行各 Tick 阶段和物理子步，它返回错误时停止并清理。代码保留到 close，重复 stop 不重复 End。Project → Run 从编辑快照创建独立 Session，不复用编辑 Runtime 身份，也不自动把运行结果写回编辑场景。

EnvironmentComponent 保存共享 SceneLightingSettings 的作者值，包括直接光预算；Session 历史负责持久化与撤销，Renderer 消费副本。诊断模式、GPU 缓存与实际 follow-focus 锚点不属于存档，详见[持久格式与责任](../editor/projects.md#持久格式与责任)。

端到端验收入口为 `project_scene_acceptance`、`world_acceptance`、`editor_document_acceptance`。
