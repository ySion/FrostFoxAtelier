# Presentation：网格、灯光与环境

正式入口为 [presentation.hpp](../../../src/presentation/presentation.hpp)。`atelier_presentation` 在 World 核心之上定义网格、灯光、环境与材质资源的类型和规则，依赖 world、material、geometry 与 rendering（只用到 `scene_lighting.hpp` 头文件）；World 核心不引用其中任何类型。物理链接它，三角网格碰撞经 `read_static_mesh` 取网格。持久类型名沿用场景格式的 `world.` 前缀（`world.StaticMeshComponent` 等），C++ 命名空间为 `atelier::presentation`。

`presentation::register_types(Runtime&)` 在 `world::register_types` 之后调用，登记 material 资源类型与本库类型，`core.String` 不存在时一并登记。

## Scene

`presentation::Scene::create(World&, Types)` 在宿主边界为一个 World 建立呈现：登记网格（要求组件已有几何）、灯光、环境三项能力，以及“每个 World 至多一个 EnvironmentComponent”的准入。每个 World 只创建一个 Scene，它先于 World 析构；网格登记带谓词，重复创建会累积登记。编辑器 Session（编辑与 Play 共用）在 World 旁持有它，由 `Session::presentation()` 取得。`meshes() / lights() / environments()` 返回这三个 `CapabilityId`，可经 `World::members(id)` 查询桶。

`render_items()`、`light_items()` 与 `environment()` 只遍历这些桶，经 `World::spatial_pass` 读取空间，对象失败经 `World::contain` 写入 World 的诊断日志，按 World 的失败策略隔离或使调用失败。

### 提取缓存

Scene 缓存提取的结果，供消费者只重建变化的部分。缓存只经 `update()` 和 `render_items()` 推进。

- 缓存内容：网格桶每个位置一个 `MeshSlot`，`item` 是该位置的 RenderItem（隔离或退化时为空），`changed` 是最后改变它的更新序号；上一次更新的灯光结果；上一次的 Environment 设置。
- 布局：`World::revision()` 前进时，网格槽全部清空重建，`mesh_layout()` 记下这次更新的序号。在此之前，每个槽位都对应同一个组件，消费者可以按槽位保留自己的派生数据。
- 每次更新：`update()` 在一次 presentation 遍历中重新读取每个网格成员的空间和组件字节，与缓存逐字节比较，只对变化的网格校验 Material、重算 surfaces；带材质槽的网格每次都重新捕获 surfaces 并按指针比较，所以共享材质资源的编辑也能发现。灯光与 Environment 每次完整提取，与缓存比较。Runtime 没有字段写入通知，经 `Runtime::write`、`with_native` 或 JIT 绕过 World 写入的值也由这些比较发现。
- 版本号：`update()` 返回最近一次改变了某个网格槽、网格布局、灯光或 Environment 设置的更新序号。它单调、只在内容变化时前进；消费者保存上次同步的版本号，版本号相同时整帧跳过，否则只处理 `changed`、`mesh_layout()`、`lights_changed()` 大于该值的部分。`render_items()` 也推进缓存，序号与 `update()` 共用，所以其他调用方插入的提取不会让消费者漏掉变化。
- 所有权：缓存的 RenderItem 共享持有网格几何与 surfaces，组件被销毁或换了网格、材质后，旧内容保留到下一次更新。Scene 先于 World 析构时一并释放。
- 失败隔离：隔离或退化的网格不留在缓存中，每次更新都重新读取并报告，持续的失败和零缩放警告按更新次数累计 `repeats`；修复后在下一次更新回到缓存。调用失败（halt 策略或 World 级错误）时返回错误，失败前已更新的槽保留新值和序号；下一次成功的更新返回的版本号不小于这些序号，消费者不会漏掉它们。

静止帧的耗时下限来自“每次更新都要读一遍”：为了发现绕过 World 的写入，所有呈现成员的世界矩阵和组件字节每帧都要读取比较，开销与呈现成员数成正比，与变化多少无关。根本的解法是 Runtime 层的字段写通知或脏标记，它牵涉语言直接访问字段的设计，属于运行时的架构决定，目前没有。

## 网格

`create_static_mesh_entity(World&, StaticMeshEntityDesc)` 创建尚未注册的完整 StaticMeshEntity：Entity 自身具有 TRS，仅包含一个 StaticMeshComponent。描述提供共享 mesh、material、transform 与 mesh_source；空网格仍保留组成身份但不产生绘制。工厂失败清理本次对象，成功后由同一 attach 入口发布。

`set_mesh_binding` 同时设置持久来源键与不可变 MeshRef，清理不匹配材质槽；`set_geometry` 清除来源键。二者经 `World::admit_component_edit` 准入，改动后重新求值网格登记的谓词。`read_static_mesh` 支持 Detached 与空网格，供存档读取。`material.Material / material.Texture` 是同一 Runtime 的资源对象，槽绑定强持资源；Renderer 只消费拥有型不可变 Surface 快照，见[项目场景](../world/project-scenes.md)。

`render_items()` 按成员前序与组件顺序提取网格，结果容量等于实际网格数；零网格或纯灯光场景不分配结果存储。空间不可求值时诊断记在失败的 Entity 上，材质记在组件上，该项跳过；零缩放空间中的网格不绘制，报告 `degenerate_value` 警告。

材质与灯参数的校验结果按桶位置缓存：某个位置上的当前值与上次在该位置通过校验的值逐字节相同时，跳过 `validate_value`，其余照常执行。这样做成立，是因为类型校验器是对值字节的纯检查，已发布的类型规则也不再改变，所以不需要按对象身份核对，也不依赖写入通知。Runtime 直接写入和生成代码写入的值，下一次提取时同样会被校验；未通过的值不进入缓存，每次提取都会重新校验并报告。每个网格每次提取只借用一次组件。

## 材质

`world.Material` 使用原有反射值类型与 StaticMeshComponent.material 字段，新增参数无需另一套材质对象身份。

| 字段 | 默认值 | 合同 |
| --- | --- | --- |
| base_color | (0.19, 0.42, 0.57) | 线性 RGB，每通道硬边界 [0,1] |
| roughness | 0.65 | 硬边界 [0,1]；0 保留为作者输入，着色器数值处理由 Renderer 负责 |
| metallic | 0 | 硬边界 [0,1] |
| emissive | (0,0,0) | 线性 HDR RGB，每通道硬边界 [0,65504]（UI 范围 0–10），与不可变 Surface 的 GBuffer 表达范围一致 |

Material 的全部规则都是单字段的反射硬边界（`AFIELD` 的 `LockMin / LockMax / HardMax`），没有跨字段校验器。Runtime 写入越界时夹到边界，NaN/Inf 拒绝；`validate_material(Runtime&, Material)` 用 Runtime 检查一个完整值。`set_material(World&, ObjectId, Material)` 先按完整值检查，失败即拒绝、不夹值，再经整值 `World::write` 写入；`World::write` 的单字段写入与其他字段相同，见 [World 编辑](../world/README.md#编辑呈现与资源)。两者使用宿主边界及原有组成准入，支持同 Runtime Detached 准备，拒绝其他 World 成员。工厂也检查完整 Material。可信原生直接写入不经硬边界，`render_items` 提取时用 Runtime 校验 material，越界项按对象诊断（field 为 `material.<字段>`，value 为越界值）并跳过，不会把无效数据交给 Renderer。

## 灯类型与参数

`LightComponent : SpatialComponent` 是真实 Runtime 能力对象，它的 owner Entity 必须有 Transform。公开字段 `settings : world.LightSettings` 保存灯参数；空间来自 owner Entity，沿实体空间参考链、组合顺序、生命周期和派生类型查询运行。

`world.LightKind` 是 uint32 枚举：point=0、rectangle=1、directional=2。天光不是灯，而是 [Environment](#环境) 的设置；v6 场景中的 Sky Light 在读取时并入 Environment。`LightSettings` 的反射字段如下：

| 字段 | 默认值 | 合同 |
| --- | --- | --- |
| kind | point | 只能取已声明种类（校验器） |
| color | (1,1,1) | 非负线性 RGB（硬下限 0），可大于 1，作为强度的 RGB 乘数 |
| intensity | 1 | 硬下限 0；point 为 cd，rectangle 为 cd/m²，directional 为 lux |
| range | 10 | 世界米，硬下限 0；局部灯必须 >0（校验器），directional 忽略且允许 0 |
| source_radius | 0 | 硬下限 0；point 为世界米；directional 为角半径，单位弧度且 <π/2（校验器）；rectangle 忽略 |
| width / height | 1 / 1 | 世界米，硬下限 0；rectangle 必须 >0（校验器），其余种类未使用 |
| two_sided | false | 矩形两面按同一 luminance 出光，不将每面强度减半 |
| casts_shadow | true | 直接灯的投影意图；实际支持和预算由 Renderer 决定 |

单位按参考白色定义，color 再逐通道调制。这里保存作者数据，不在 World 引入曝光、色调映射、光通量换算或阴影资源管理。

点光、矩形光和平行光使用共享的解析光计算域：每通道 `color × intensity ≤ 2^32`；矩形光还要求 `color × intensity × width × height ≤ 2^32`，且面积必须为正、不超过 FP32 最大有限值。这是有效辐射量的乘积约束，不是对 intensity 单独限幅；例如很高强度乘很小颜色仍可有效。这条乘积规则在 LightSettings 的类型校验器中，World authoring、提取和 Renderer 编码前使用同一范围检查，超界拒绝且保留原值，不静默降低亮度。此范围为 FP32 解析光采样的中间计算保留余量，不等同于最终 HDR 纹理上限，也不保证任意发光几何或 reservoir 权重均可表示；具体边界见 [Scene Lit](../renderer/scene-lit.md)。

单字段规则是反射硬边界，跨字段规则（种类、局部灯 range、矩形尺寸、方向光角半径、解析光乘积）是 LightSettings 登记的类型校验器，二者都由 Runtime 执行。`validate_light(Runtime&, LightSettings)`、`read_light(Runtime&, ObjectId)`、`set_light(World&, ObjectId, LightSettings)` 为正式值接口。read 返回拥有式参数，包括 Detached/停用对象；set 先按完整值检查（包括跨字段规则，即使组件原值已经违反规则），失败即拒绝、不夹值，再经整值 `World::write` 写入，准入与精确恢复同 write。World 标量属性编辑先经 Runtime 写入（硬边界夹值，NaN/Inf 与错误 Scalar 类型拒绝），再检查类型规则与准入，失败时恢复原值。应用保存/加载须调用公开验证，不以反射原始存储写入代替领域校验。

## 环境

`world.EnvironmentComponent : Component` 保存整个 World 的 Scene Lit 作者设置：阴影、效果、曝光、DDGI 传播、天光（`sky_enabled`、`sky_color`、`sky_intensity`）以及 4 个具名探针体积 `volume0..volume3`（`world.ProbeVolume`，探针数拆为 `count_x/y/z`）。字段、默认值和单字段硬边界与 `rendering::SceneLightingSettings` 一一对应，`environment_settings(component)` 给出对应的设置；跨字段规则（GTAO 与 Local GI 互斥、体积 min < max、间距精度、Follow Focus 内部格子、4096 探针共享预算、天光每通道 `color × intensity ≤ 65504`）是组件登记的类型校验器，直接复用 `scene_lighting_error`。它不是 SpatialComponent，不需要 owner Entity 的 Transform。

天光首版是均匀天空入射辐射：`sky_enabled` 为假时环境辐射为零，颜色和强度保留；没有可见天空、HDRI 或方向性天空分布的隐式资源。宿主用 `rendering::sky_radiance(settings)` 求 `Frame.environment`，Renderer 接口不变。

每个 World 至多一个 EnvironmentComponent，停用的也计数；这是 Scene 登记的准入。`Scene::environment()` 返回启用中的那一个的设置；没有、或它被停用时返回 `SceneLightingSettings{}`，不保留任何缓存，所以移除或停用后立即回到默认值。经脚本、`with_native` 或 JIT 绕过写入路径而变成无效的 Environment，在 lighting 阶段当前的诊断遍历里记一条 `invalid_value`，提取返回默认值，不沿用上一次的有效值；halt 策略下提取失败。作者编辑跨字段的组合时用组写入，见 [World 编辑](../world/README.md#编辑呈现与资源)。

## 提取与坐标

`Scene::light_items()` 返回 `LightRenderItem { entity, component, position, direction, right, up, settings }`。两个身份都是完整 ObjectId；它们只用于当前 Runtime 的关联，不落盘、不转换成裸地址，也不额外保活对象。Renderer 的 key 映射由应用适配，Renderer 无需依赖 World。

Scene 创建时登记网格与灯光两个桶。render_items 只返回网格，light_items 只返回 Entity 和 Component 均启用的灯，顺序仍为 Entity 成员顺序及组件组成顺序。停止的编辑 World 可以提取；零 intensity 仍保留条目和身份；Detached/已移除对象不会出现；关闭后的提取返回错误。桶中的登记不等于该帧产生可见能量。

position 使用完整父链的 TRS 矩阵平移，因此父 scale 会缩放子节点的位置偏移。方向基沿 Entity 空间参考链组合每一级的正交部分 R·D，其中 D = diag(sign(scale))，忽略 scale 幅值与 shear，零分量按正号处理；得到正交矩阵 O 后取 right = O(+X)、up = O(+Y)、direction = O(+Z)，det(O) < 0 时 right 取反，使三者仍为右手正交单位基。全为正缩放时即各级 quaternion 的组合；幅值均匀时与网格同一局部轴的镜像方向一致，因此镜像祖先下的矩形光与方向光随几何一起镜像。direction 是光传播方向，矩形位于局部 XY 平面，以 position 为中心、宽沿 right、高沿 up，关于 right 轴对称，所以 right 取反不改变发光面。range、source_radius 的物理长度、width/height 是世界米，不再乘任何 scale；方向光的 source_radius 为角度量。零缩放祖先只使位置偏移塌缩，朝向保持作者值，灯光不按退化处理。

参数值和坐标都在提取时验证。快照包含自有值，可在对象离场后继续读取；它不保存对象地址、ObjectRoot 或可重入回调。真实删除、撤销恢复与重新启用都沿原 Component 身份，不另建灯管理器或对象索引权威。

## 验收与接线边界

`world_acceptance` 同时链接 presentation，包含真实消费者：反射枚举/值/对象和派生灯、Source/JIT 修改 LightSettings、Material HDR、标量与整组拒绝原子性、混合 mesh/light 提取、父级非均匀缩放、停用/启用、反序 End、删除恢复、Detached 及快照 GC 寿命。数值边界覆盖解析光乘积及矩形面积乘积的边界和相邻越界值、65504 自发光提取、原生绕过 setter 后的提取拒绝；开发和发布构建均使用同一消费者。

World E2E 验证数据与生命周期合同。场景文件、编辑器控件和 SceneSnapshot 接入由应用端到端验收覆盖，GPU 灯、天光、材质、历史与阴影消费由 Renderer 及正式编辑器窗口验收覆盖，见 [Renderer 验收记录](../renderer/acceptance.md)。单独的 World E2E 不替代这些集成证据。
