# Presentation：网格、灯光、相机与环境

正式入口为 [presentation.hpp](../../../modules/engine/presentation/include/presentation/presentation.hpp)。`atelier_presentation` 在 World 核心之上定义网格、灯光、相机、环境与材质资源的类型和规则，依赖 world、material、geometry 与 rendering（只用到 `scene_lighting.hpp` 头文件）；World 核心不引用其中任何类型。物理链接它，三角网格碰撞经 `read_static_mesh` 取网格。持久类型名沿用场景格式的 `world.` 前缀（`world.StaticMeshComponent` 等），C++ 命名空间为 `atelier::presentation`。

`presentation::register_types(Runtime&)` 在 `world::register_types` 之后调用，登记 material 资源类型与本库类型，`core.String` 不存在时一并登记。

## Scene

`presentation::Scene::create(World&, Types)` 在宿主边界为一个 World 建立呈现：登记网格（要求组件已有几何）、程序化生成器（`ProceduralGeneratorComponent`）、灯光、环境、相机能力，以及“每个 World 至多一个 EnvironmentComponent”和“每个 Entity 至多一个 CameraComponent”的准入（含派生与停用组件）。创建时也检查已有图。每个 World 只创建一个 Scene，它先于 World 析构；网格登记带谓词，重复创建会累积登记。编辑器 Session（编辑与 Play 共用）在 World 旁持有它，由 `Session::presentation()` 取得。`meshes() / procedural_generators() / lights() / cameras()` 返回对应的 `CapabilityId`，可经 `World::members(id)` 查询桶；环境通过 `environment()` 提取设置。

`render_items()`、`light_items()` 与 `environment()` 只遍历这些桶，经 `World::spatial_pass` 读取空间，对象失败经 `World::contain` 写入 World 的诊断日志，按 World 的失败策略隔离或使调用失败。

`camera(entity)` 返回该 Entity 唯一相机的位姿和投影参数值快照，允许停用对象供作者查看；`main_camera()` 则解析 Environment 的显式弱引用，并检查存活、归属和有效启用。解析期间持有临时根，返回后不保活业务对象；无效选择返回错误，不修改引用、不另选相机。空间旋转只沿真实父链复合局部四元数，缩放大小和符号均不参与。相机不进入网格/灯光提取缓存；每次更新及 late flush 后重新解析，Runtime 引用写入不要求 World revision 改变。`CameraSnapshot::matrices(aspect)` 生成左手 +Z 前方、行向量、无限远 reversed-Z 透视矩阵（设备深度为 near/view Z），并拒绝不可表示的矩阵；同一快照派生 `far_depth=near_clip/far_clip`，消费端据此保留相机 +Z 有限 far 裁剪，不能只复制 view/projection。深度 0 是无穷远方向，不是可除以 w 的有限点；无限远深度映射不表示无限场景范围。字段、编辑器和 Play 合同见[场景相机](../editor/projects.md#场景相机)。

### 提取缓存

Scene 缓存提取的结果，供消费者只重建变化的部分。缓存只经 `update()` 和 `render_items()` 推进。

- 缓存内容：网格桶每个位置一个 `MeshSlot`，`item` 是该位置的 RenderItem（隔离或退化时为空），`changed` 是最后改变它的更新序号；上一次更新的灯光结果；上一次的 Environment 设置。
- 布局：`World::revision()` 前进时，若可渲染网格与程序化生成器能力桶的成员序列未发生变化，则保留已有槽位与材质缓存，仅重建父链空间依赖并标脏发生父级重挂的子树；只有可渲染成员集合或顺序变化时才清空重建网格槽并推进 `mesh_layout()`。在此之前，每个槽位都对应同一个组件，消费者可以按槽位保留自己的派生数据。
- 每次更新：在一次 presentation 遍历中消费 Runtime 自上次[变化游标](../../runtime-model.md#写代号与变化游标)以来的 `changed_objects`。布局建立时为每槽记下空间链（自身 Entity 在前），链上 Entity 与网格组件分别指向该槽；仅空间变化只更新矩阵，不重捕获材质。组件/绑定变化或材质依赖变化才重新捕获 surfaces。每次成功捕获同时取得实际遍历到的 live 材质、父链、纹理身份，Scene 按 ObjectId→槽建立反向索引；去重的依赖再用 `changed_since` 检查死亡，因为死对象不会出现在 `changed_objects` 中。经 `Runtime::write`、`with_native` 或 JIT 绕过 World 的写入同样盖章。脏槽仍按桶顺序处理；失败槽逐次重试。灯光组件或灯光链上的 Entity 被写过、有灯未被绘制、或呈现位姿移动了灯时，灯光整体重新提取并与缓存比较；Environment 每次读取。未变捕获使用只读借用，变化的派生 surfaces 缓存写回后才取本 Scene 的结束游标；各 Scene 独立消费，不互相清除变化。`update()` 必须在宿主安全点调用：在 Runtime 回调、原生借用或执行期间取游标返回 busy；即使无脏槽也不跳过此准入。
- 版本号：`update()` 返回最近一次改变了某个网格槽、网格布局、灯光或 Environment 设置的更新序号。它单调、只在内容变化时前进；消费者保存上次同步的版本号，版本号相同时整帧跳过，否则只处理 `changed`、`mesh_layout()`、`lights_changed()` 大于该值的部分。版本号只在同一个 Scene 内可比；`identity()` 在进程内唯一、不被之后的 Scene 复用，消费者用它识别换了的 Scene。`render_items()` 也推进缓存，序号与 `update()` 共用，所以其他调用方插入的提取不会让消费者漏掉变化。
- 所有权：缓存的 RenderItem 共享持有网格几何与 surfaces，组件被销毁或换了网格、材质后，旧内容保留到下一次更新。Scene 析构时释放全部缓存，它总是先于 World 析构。
- 失败隔离：隔离或退化的网格不留在缓存中，每次更新都重新读取并报告，持续的失败和零缩放警告按更新次数累计 `repeats`；修复后在下一次更新回到缓存。调用失败（halt 策略或 World 级错误）时返回错误，失败前已更新的槽保留新值和序号；下一次成功的更新返回的版本号不小于这些序号，消费者不会漏掉它们。

静止成功帧不逐槽深捕获材质，也不读取网格或灯光成员；仍需取游标、列举被写对象、按去重依赖检查死亡，以及读取和校验 Environment。依赖数可随独立材质数增长，不能因此声称整帧开销与场景规模无关。布局变化清空重建索引；索引只保存完整 ObjectId，不持 ObjectRoot，不延长业务对象寿命。

### 呈现位姿

`update(presented)` 接收本帧的呈现位姿 `PresentedPose { entity, position, rotation, world_position, world_rotation }`：这些 Entity 本帧画在 position/rotation，而它们在 World 中的位姿是 world_position/world_rotation。物理用它做固定步长的渲染插值（见[物理](../physics/README.md#渲染插值)），presentation 不依赖物理，由宿主把两者接起来（`Session::presented_poses()` 交给 `SceneSnapshot::refresh`）。

- 每个网格和灯沿自己 Entity 的空间链向上（含自身）找第一个列在表里的 Entity E。两种位姿不同时，它整体做刚体变换 D = T(p_presented)·R(q_presented·q_world⁻¹)·T(−p_world)：网格画在 M_world·D（行向量），灯的位置和三根轴同样变换；两种位姿相同或找不到时画在 World 矩阵。D 只含旋转和平移，所以镜像、非均匀缩放和子节点的局部偏移都保留。相同位姿的条目是边界：宿主列出被移动 Entity 之下的每个模拟 Entity，例如运动父刚体下的睡眠子刚体，它和它的视觉子孙就停在 World 位姿，不跟着父体晃。
- 缓存的 `item` 仍是 World 位姿，`render_items()`、`light_items()`、拾取和 gizmo 只看权威值；呈现矩阵另存在槽的 `presented` 中，灯光快照 `light_snapshot()` 是应用呈现位姿之后的结果。`presented` 变化也推进该槽的 `changed` 和版本号；物体在动时每帧都变，表空了以后 `presented` 全部清空，版本号回到静止。`render_items()` 不改变 `presented`。
- 表为空、且上一次没有呈现矩阵时，整段跳过，静止场景没有额外开销；否则只检查表中 Entity 之下的槽和上一次有呈现矩阵的槽，沿布局时记下的空间链查表，开销与这些槽数乘以链深成正比。
- 隔离或退化的槽不在缓存里，没有呈现矩阵；呈现矩阵不重新校验材质。

Renderer 用上一帧 Frame 里的矩阵算运动向量（`RenderScene` 按 `stable_key` 记录上一帧该对象的 `prev_world`），所以插值帧的运动向量与画面一致。

## 网格

`create_static_mesh_entity(World&, StaticMeshEntityDesc)` 创建尚未注册的完整 StaticMeshEntity：Entity 自身具有 TRS，仅包含一个 StaticMeshComponent。描述提供共享 mesh、transform、mesh_source 与网格资产的槽默认材质；空网格仍保留组成身份但不产生绘制。工厂失败清理本次对象，成功后由同一 attach 入口发布。

`set_mesh_binding` 同时设置持久来源键、不可变 MeshRef 与该网格的槽默认材质（`MeshBinding::defaults`，不持久化，宿主随网格解析），清理不匹配的活槽绑定；Session 另负责保留 Reimport 后的休眠作者关系。`set_geometry` 清除来源键和默认材质。每个默认材质必须对应新网格中唯一的槽。材质表面按槽依次取显式绑定、默认材质、Default World Grid。二者经 `World::admit_component_edit` 准入，改动后重新求值网格登记的谓词。`read_static_mesh` 支持 Detached 与空网格，供存档读取。MS/MI 共用 Runtime `material.Resource` 角色，槽绑定强持资源，见[项目场景](../world/project-scenes.md)。

`render_items()` 按成员前序与组件顺序提取网格，结果容量等于实际网格数；零网格或纯灯光场景不分配结果存储。空间不可求值时诊断记在失败的 Entity 上，材质记在组件上，该项跳过；零缩放空间中的网格不绘制，报告 `degenerate_value` 警告。

灯参数的校验结果按桶位置缓存：当前值与上次通过校验的值逐字节相同时跳过 `validate_value`，其余照常执行。Runtime 直接写入和生成代码写入的值，下一次提取时同样会被校验；未通过的值不进入缓存。材质按各正式资源的不可变捕获身份复用，未变化的槽表 owner 也复用。

## 材质

StaticMeshComponent 没有内联固定材质值。显式绑定和默认绑定的正式引用序列均指向 `material.Resource`；MS/MI 作者语义归材质模块，presentation 不持有 GPU 或另一套参数 schema。

`material_surfaces(Runtime&, ObjectId)` 返回共享不可变的 `vector<ProgramSurfaceRef>`；`RenderItem::surfaces` 复用同一结果。每槽用 `capture_program` 捕获完整程序、参数、图片及诊断，正常无绑定槽用 `default_world_surface()`。定义/父链失效的品红替代属于材质解析结果，不在 presentation 重新合成固定参数。

Scene 的依赖来自同一次真实捕获，不从最终 `Surface.inputs()` 反推：包含 live 空图像与仍存在的 disabled/dormant 纹理边，以观察恢复和诊断变化。成功回退后移出已死依赖；成功捕获即使 Surface owner 相同也替换依赖（例如等值换父），不为纯拓扑变化推进可见版本。直接绑定的材质死亡或捕获错误仍遵循 isolate/halt、首错顺序和重复诊断，不伪装成成功回退。单对象 `material_surfaces` 仍每次捕获，没有跨调用观察缓存。

宿主将这份 CPU 候选冻结为完整 SceneSnapshot，完成 MaterialBatch 准备后采用；只有 Renderer 接受的帧才能替换屏幕拾取快照。presentation 提取成功不表示 GPU 已准备或已显示。材质定义、作者格式和采样合同见[材质系统](../renderer/materials.md)。

## 灯类型与参数

`LightComponent : SpatialComponent` 是真实 Runtime 能力对象，它的 owner Entity 必须有 Transform。公开字段 `settings : world.LightSettings` 保存灯参数；空间来自 owner Entity，沿实体空间参考链、组合顺序、生命周期和派生类型查询运行。

`world.LightKind` 是 uint32 枚举：point=0、rectangle=1、directional=2、spot=3。天光不是灯，而是 [Environment](#环境) 的设置。`LightSettings` 的反射字段如下：

| 字段 | 默认值 | 合同 |
| --- | --- | --- |
| kind | point | 只能取已声明种类（校验器） |
| color | (1,1,1) | 非负线性 RGB（硬下限 0），可大于 1，作为强度的 RGB 乘数 |
| intensity | 1 | 硬下限 0；point / spot 为 cd，rectangle 为 cd/m²，directional 为 lux |
| range | 10 | 世界米，硬下限 0；point / spot / rectangle 必须 >0（校验器），directional 忽略且允许 0 |
| source_radius | 0 | 硬下限 0；point / spot 为世界米；directional 为角半径，单位弧度且 <π/2（校验器）；rectangle 忽略 |
| spot_inner_degrees / spot_outer_degrees | 25 / 40 | 聚光灯内外半角（度），spot 要求 `0 <= inner <= outer` 且 `outer in [0.5, 89]`（校验器），其余种类未使用 |
| width / height | 1 / 1 | 世界米，硬下限 0；rectangle 必须 >0（校验器），其余种类未使用 |
| two_sided | false | 矩形两面按同一 luminance 出光，不将每面强度减半 |
| casts_shadow | true | 直接灯的投影意图；实际支持和预算由 Renderer 决定 |

单位按参考白色定义，color 再逐通道调制。这里保存作者数据，不在 World 引入曝光、色调映射、光通量换算或阴影资源管理。

点光、聚光灯（`Spot`，含 `spot_inner_degrees` ≤ `spot_outer_degrees`）、矩形光和平行光使用共享的解析光计算域：每通道 `color × intensity ≤ 2^32`；矩形光还要求 `color × intensity × width × height ≤ 2^32`，且面积必须为正、不超过 FP32 最大有限值。这是有效辐射量的乘积约束，不是对 intensity 单独限幅；例如很高强度乘很小颜色仍可有效。这条乘积规则在 LightSettings 的类型校验器中，World authoring、提取和 Renderer 编码前使用同一范围检查，超界拒绝且保留原值，不静默降低亮度。此范围为 FP32 解析光采样的中间计算保留余量，不等同于最终 HDR 纹理上限；具体边界见 [Scene Lit](../renderer/scene-lit.md)。

单字段规则是反射硬边界，跨字段规则（种类、局部灯 range、聚光灯内外锥角、矩形尺寸、方向光角半径、解析光乘积）是 LightSettings 登记的类型校验器，二者都由 Runtime 执行。`validate_light(Runtime&, LightSettings)`、`read_light(Runtime&, ObjectId)`、`set_light(World&, ObjectId, LightSettings)` 为正式值接口。read 返回拥有式参数，包括 Detached/停用对象；set 先按完整值检查（包括跨字段规则，即使组件原值已经违反规则），失败即拒绝、不夹值，再经整值 `World::write` 写入，准入与精确恢复同 write。World 标量属性编辑先经 Runtime 写入（硬边界夹值，NaN/Inf 与错误 Scalar 类型拒绝），再检查类型规则与准入，失败时恢复原值。应用保存/加载须调用公开验证，不以反射原始存储写入代替领域校验。

## 环境

`world.EnvironmentComponent : Component` 保存整个 World 的 Scene Lit 单轨作者设置：直接光候选预算（`direct_candidates`）、阴影（`shadows`、`contact_shadow_length`）、四层全局光照（`indirect_lighting`、`gi_quality`、`indirect_gain`、`ssilvb`、`ao_radius`）、三级镜面反射（`reflections`、`reflection_distance`、`reflection_roughness`）、三态天空（`sky_mode`、`sky_color`、`sky_intensity`、`sky_rotation` 和天空贴图）、近场体积雾（`fog_enabled`、`fog_density`、`fog_height_falloff`）以及后处理（`bloom`、`exposure_ev`、`tonemapper`、`antialiasing`）。字段、默认值和单字段硬边界与 `rendering::SceneLightingSettings` 一一对应，Scene 提取时在模块内部转换为这份设置；跨字段规则（枚举合法性、有限非负区间、天光每通道 `color × intensity ≤ 65504`）是组件登记的类型校验器，直接复用 `scene_lighting_error`。它不是 SpatialComponent，不需要 owner Entity 的 Transform。

`sky_mode` 为 `none` 时天空辐射为零且背景为纯黑；`physical_atmosphere` 使用物理大气散射与太阳方向；`hdri_sky_map` 使用 `sky_color × sky_intensity × map(旋转后的方向)`（未绑定贴图时退化为 `map = 1` 均匀天光）。`sky_rotation` 是绕世界 +Y 的偏航角（度，硬边界 [−180, 180]，与 Transform 绕 +Y 的正旋转同向），原值进入 `SceneLightingSettings::sky_rotation`。天空贴图是一对隐藏数据：持久的资产键和解析出的 `rendering::SkyMapRef`，没有公开 FieldId，只能经 `set_sky_map_binding(runtime, component, {key, map})` 成对设置、`read_sky_map` 读取；有贴图必须有键，键不超过 4096 字节且不含 NUL，键解析不到时贴图为空。它与其他编辑一样在 World 派发期间返回 busy。Scene 提取把贴图指针原样放进 `SceneLightingSettings::sky_map`，设置比较按指针，所以只有换了贴图对象才算变化。宿主仍用 `rendering::sky_radiance(settings)` 求 `Frame.environment`，Renderer 从 `frame.view.lit` 读模式、贴图与旋转。

每个 World 至多一个 EnvironmentComponent，停用的、派生类型的也计数；这是 Scene 登记的准入，创建 Scene 时也检查 World 里已有的内容，已有两个以上就拒绝创建（此时只留下可复用的环境登记）。`Scene::environment()` 返回启用中的那一个的设置；没有、或它被停用时返回 `SceneLightingSettings{}`，不保留任何缓存，所以移除或停用后立即回到默认值。经脚本、`with_native` 或 JIT 绕过写入路径而变成无效的 Environment，在 lighting 阶段当前的诊断遍历里记一条 `invalid_value`，提取返回默认值，不沿用上一次的有效值；halt 策略下提取失败。作者编辑跨字段的组合时用组写入，见 [World 编辑](../world/README.md#编辑呈现与资源)。

## 提取与坐标

`Scene::light_items()` 返回 `LightRenderItem { entity, component, position, direction, right, up, settings }`。两个身份都是完整 ObjectId；它们只用于当前 Runtime 的关联，不落盘、不转换成裸地址，也不额外保活对象。Renderer 的 key 映射由应用适配，Renderer 无需依赖 World。

Scene 创建时登记网格与灯光两个桶。render_items 只返回网格，light_items 只返回 Entity 和 Component 均启用的灯，顺序仍为 Entity 成员顺序及组件组成顺序。停止的编辑 World 可以提取；零 intensity 仍保留条目和身份；Detached/已移除对象不会出现；关闭后的提取返回错误。桶中的登记不等于该帧产生可见能量。

position 使用完整父链的 TRS 矩阵平移，因此父 scale 会缩放子节点的位置偏移。方向基沿 Entity 空间参考链组合每一级的正交部分 R·D，其中 D = diag(sign(scale))，忽略 scale 幅值与 shear，零分量按正号处理；得到正交矩阵 O 后取 right = O(+X)、up = O(+Y)、direction = O(+Z)，det(O) < 0 时 right 取反，使三者仍为右手正交单位基。全为正缩放时即各级 quaternion 的组合；幅值均匀时与网格同一局部轴的镜像方向一致，因此镜像祖先下的矩形光与方向光随几何一起镜像。direction 是光传播方向，矩形位于局部 XY 平面，以 position 为中心、宽沿 right、高沿 up，关于 right 轴对称，所以 right 取反不改变发光面。range、source_radius 的物理长度、width/height 是世界米，不乘任何 scale；方向光的 source_radius 为角度量。零缩放祖先只使位置偏移塌缩，朝向保持作者值，灯光不按退化处理。

参数值和坐标都在提取时验证。快照包含自有值，可在对象离场后继续读取；它不保存对象地址、ObjectRoot 或可重入回调。真实删除、撤销恢复与重新启用都沿原 Component 身份，不另建灯管理器或对象索引权威。

## 验收与接线边界

`world_acceptance` 同时链接 presentation，在开发和发布构建中保留四个呈现侧代表：

- Environment 组写入后准入失败，所有叶恢复到作者原字节，包括 NaN payload、负零和天空强度；不比较错误正文。
- 单 Scene 的材质等值换父后修改新父 `roughness=.7`，渲染项必须得到输入值 `.7`，防止依赖关系静默失效。
- StaticMeshEntity 工厂在已分配 Entity/Component 后绑定失败，空 Runtime 的对象与根数恢复为零。
- Scene 快照持有 Mesh，销毁 Entity/Component 不使内容失效；释放快照并更新 Scene 后，最后一个 Mesh 所有者释放。

World E2E 验证数据与生命周期合同。场景文件、编辑器控件和 SceneSnapshot 接入由应用端到端验收覆盖，GPU 灯、天光、材质、历史与阴影消费由 Renderer 及正式编辑器窗口验收覆盖，见 [Renderer 验收记录](../renderer/acceptance.md)。单独的 World E2E 不替代这些集成证据。
