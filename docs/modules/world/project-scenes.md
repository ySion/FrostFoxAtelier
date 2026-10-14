# 项目类型、场景与运行会话

## 准备与所有权

`editor::Session::Prepare` 在 Session 注册 World/Physics 类型并构造 World 后、创建首个对象前调用一次，用于准备项目 Source 类型、Program 和生命周期入口，并返回代码拥有者。Session 保持代码存活至 World 完成 End；Runtime 最后销毁。失败候选沿相同 RAII 清理。

编辑加载与运行加载都通过 `deserialize(text, Project&, Prepare={})` 恢复真实对象图。候选成功后由应用发布，失败不修改原文档或文件。字段默认值来自 Runtime 创建真实类型，加载不调用会额外生成默认网格组成的工厂。

## 真实类型与编辑

Entity 与 Component 保存真实限定类型名，以 Runtime find_type / is_a 校验。参数发现和编辑沿同一 FieldPath；显示名称不充当身份。标量与内联 record 递归编辑，枚举保存声明成员名，整数严格检查符号与范围，浮点必须有限。TRS 历史保存完整组；移除对象可由历史保活，但保活不授予文档成员资格。

## Scene v6

统一写出 v6，旧 v1–v5 严格导入后再按 v6 保存。顶层包含：

- `version: 6`、十进制字符串 `next_id`、作者配置 `world`。
- `roots`：有序顶层 Entity 文档 ID。
- `entities`：Entity 记录，每个包含 `id / type / name / enabled / properties / children / components / space`。
- `space`：null 表示无空间；否则保存 position、rotation、scale 和 origin。origin 为 null 时继承组合祖先，为字符串 `"0"` 时使用世界，为 Entity ID 字符串时使用显式空间参考。
- `components`：owner 的有序组件记录，包含自身 `id / type / name / enabled / properties`；网格与材质额外保存现有资源 payload。组件不保存 TRS、parent 或 root。
- `materials`：按文档 ID 去重的材质资源定义；网格槽使用 `resource` 引用该 ID。资产保留来源 GUID，匿名材质或实例保存内联参数，以及具名父材质与覆盖字段。

Entity、Component 与材质定义共用一个文件内的 uint64 ID 域，字符串编码避免 JSON 数值精度丢失。引用字段用 null 或 `{"$ref":"ID"}`，引用序列用这些值的数组；持有/观察属性由 Runtime 字段定义决定，不在 JSON 创造另一套引用类型。保存不记录 Runtime TypeId、ObjectId、FieldId 或后端句柄。

加载先创建全部真实对象，恢复标量、资源与引用，再按最终组合与空间关系统一校验和注册。空间参考作为整批计划提交，不依赖文件目录的先后顺序。文档加载与可移植子树导出拒绝场景外业务引用；同一 Session 内复制可以保留图外引用，发布时仍校验其目标存活和 World 归属。无效 ID、重复身份、多重归属、引用类型不匹配、组合/空间环、非法 TRS 或未知属性均拒绝整个候选。新增未保存字段保留类型默认值，未知已保存字段不静默丢弃。

StaticMeshComponent 的 mesh、base_color、roughness、metallic、emissive 和 materials 使用专有资源验证；命名材质槽保存资源身份与显式覆盖意图。相同匿名材质被多个槽或对象引用时，往返仍恢复为同一个实例。场景内联实例随 Entity 子树复制，共享资产、父材质与 Texture 保持共享。早期 v6 的逐槽内联 payload 仍可读取，重新保存使用统一资源目录。导入失败撤销本次新增的 Session 资源缓存与文档身份，保留既有资源及其编辑状态。

灯通过 LightComponent 的 `properties.settings` 保存，空间来自 owner Entity。Primitive 的 `properties.body` 保存 BodySettings 与 PhysicsMaterial，Cook 数据是派生缓存。加载按待发布图中的 owner 世界尺度读取有效工程 Cook 缓存，包含导入目标 parent 与所有 origin；内置网格消费 CPU 几何。缓存准备在 World 发布与 Begin 之前完成。

RagdollComponent.parts 与 VehicleComponent.wheels 是显式持有 Entity 引用序列，约束目标是 `properties.target` 观察引用。独立空间部件保留其真实 metadata 组件，网格由同 Entity 的 StaticMeshComponent 提供。

## 旧格式导入

v1–v5 的 Actor 主体映射到真实 Entity 派生类型，根组件 TRS 提升到主体 Entity；具有独立空间的组件转为子 Entity，原 parent 转为空间和组合关系。纯 SceneComponent 只留下对应空间实体；能力组件和业务派生类型保留真实类型，不降级为基础组件。旧 Body 配置沿严格兼容边界合并到 Primitive，不能无损映射的内容明确拒绝。

旧 constraint_target 场景序号仅在导入时解析；保存后使用统一文档引用。旧 RagdollPart / VehicleWheel 的 metadata 与网格分离，控制器恢复显式 parts / wheels 绑定。旧 v4 阴影预算字段严格校验后丢弃，使用当前直接光预算默认值。

## 运行与 World 配置

Session::start/update/stop 是运行宿主唯一驱动。start 创建 PhysicsScene 后开始 World，update 执行一次游戏 Tick 和物理子步，任一路径错误都停止清理。代码保留到 close，重复 stop 不重复 End。Project → Run 从编辑快照创建独立 Session，不复用编辑 Runtime 身份，也不自动把运行结果写回编辑场景。

`world` 保存共享 SceneLightingSettings，包括直接光预算；Session 历史负责持久化与撤销，Renderer 消费副本。诊断模式、GPU 缓存与实际 follow-focus 锚点不属于存档，详见[持久格式与责任](../editor/projects.md#持久格式与责任)。

端到端验收入口为 `project_scene_acceptance`、`world_acceptance`、`editor_document_acceptance`；Scene v6 与严格旧格式导入的结果以本轮实际执行记录为准。
