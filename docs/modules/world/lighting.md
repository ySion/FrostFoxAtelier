# World Lit 数据合同

## 材质

`world.Material` 使用原有反射值类型与 StaticMeshComponent.material 字段，新增参数无需另一套材质对象身份。

| 字段 | 默认值 | 合同 |
| --- | --- | --- |
| base_color | (0.19, 0.42, 0.57) | 线性 RGB，有限且每通道在 [0,1] |
| roughness | 0.65 | 有限，[0,1]；0 保留为作者输入，着色器数值处理由 Renderer 负责 |
| metallic | 0 | 有限，[0,1] |
| emissive | (0,0,0) | 线性 HDR RGB，每通道有限且在 [0,65504]，与不可变 Surface 的 GBuffer 表达范围一致 |

`validate_material` 是公开的纯验证入口。`World::set_material` 按完整值验证后写入；`World::write` 对上述原生字段修改拥有式候选，再走同一验证和整值入口。两者使用停止状态、宿主边界及原有组成准入，支持同 Runtime Detached 准备，拒绝其他 World 成员。工厂及 render_items 也验证完整 Material。可信原生/Source 直接写入仍可能绕过 authoring setter，提取不会把无效数据交给 Renderer。

## 灯类型与参数

`LightComponent : SceneComponent` 是真实 Runtime 对象。它只有一个新增的公开反射字段 `settings : world.LightSettings`，沿现有 owner、根、空间父链、组成顺序、生命周期和派生类型查询运行。

`world.LightKind` 是 uint32 枚举：point=0、rectangle=1、directional=2、sky=3。Sky Light 是天光，沿同一 LightComponent 身份、属性和持久化入口表示。`LightSettings` 的反射字段如下：

| 字段 | 默认值 | 合同 |
| --- | --- | --- |
| kind | point | 只能取已声明种类 |
| color | (1,1,1) | 有限、非负线性 RGB，可大于 1，作为强度的 RGB 乘数 |
| intensity | 1 | 有限、非负；point 为 cd，rectangle / sky 为 cd/m²，directional 为 lux；编辑器创建 Sky Light 时初始化为 .03 |
| range | 10 | 世界米；局部灯必须 >0，directional / sky 忽略且允许 0；不得为负或非有限 |
| source_radius | 0 | point 为世界米；directional 为角半径，单位弧度且 <π/2；均非负有限；rectangle / sky 忽略 |
| width / height | 1 / 1 | 世界米；rectangle 必须 >0，其余种类未使用但仍要求非负有限 |
| two_sided | false | 矩形两面按同一 luminance 出光，不将每面强度减半 |
| casts_shadow | true | 直接灯的投影意图；实际支持和预算由 Renderer 决定；sky 忽略 |

单位按参考白色定义，color 再逐通道调制。这里保存作者数据，不在 World 引入曝光、色调映射、光通量换算或阴影资源管理。

点光、矩形光和平行光使用共享的解析光计算域：每通道 `color × intensity ≤ 2^32`；矩形光还要求 `color × intensity × width × height ≤ 2^32`，且面积必须为正、不超过 FP32 最大有限值。这是有效辐射量的乘积约束，不是对 intensity 单独限幅；例如很高强度乘很小颜色仍可有效。World authoring、提取和 Renderer 编码前使用同一范围检查，超界拒绝且保留原值，不静默降低亮度。此范围为 FP32 解析光采样的中间计算保留余量，不等同于最终 HDR 纹理上限，也不保证任意发光几何或 reservoir 权重均可表示；具体边界见 [Scene Lit](../renderer/scene-lit.md)。

同一 World 最多有一个 Sky Light，停用的组件也占该位置。Actor 入场、向已入场 Actor 添加组件以及 Light Type 修改，在发布前检查唯一性；场景加载使用相同组成入口，重复天光被拒绝。删除后可创建新天光；撤销恢复也执行同一检查。可信代码直接改写原生字段后，`light_items()` 仍会拒绝重复天光，不进行相加或静默选取。Sky Light 每通道 `color × intensity` 不得超过当前 HDR 表达上限 65504。

首版 Sky Light 提供均匀天空入射辐射；位置、旋转、缩放、range、发光面尺寸和直接灯阴影开关不影响它。空间字段只保留统一组件组成能力。没有启用的天光组件时环境辐射为零；没有可见天空、HDRI 或方向性天空分布的隐式资源。

`validate_light(LightSettings)`、`read_light(Runtime&, ObjectId)`、`World::set_light(ObjectId, LightSettings)` 为正式值接口。read 返回拥有式参数，包括 Detached/停用对象；set 与 World 标量属性编辑使用同一整组验证，未知枚举、错误 Scalar 类型、NaN/Inf 或范围错误不会部分改写原值。应用保存/加载须调用公开验证，不以反射原始存储写入代替领域校验。

## 提取与坐标

`World::light_items()` 返回 `LightRenderItem { actor, component, position, direction, right, up, settings }`。两个身份都是完整 ObjectId；它们只用于当前 Runtime 的关联，不落盘、不转换成裸地址，也不额外保活对象。Renderer 的 key 映射由应用适配，Renderer 无需依赖 World。

World 使用现有 presentation 目录共同登记网格与灯光。render_items 只返回网格，light_items 只返回 Actor 和 Component 均启用的灯，顺序仍为 Actor 成员顺序及组件组成顺序。停止的编辑 World 可以提取；零 intensity 仍保留条目和身份；Detached/已移除对象不会出现；关闭后的提取返回错误。`can_present` 和 presentation_registrations 表示登记，不等于该帧产生可见能量。

对 point / rectangle / directional，position 使用完整父链的 TRS 矩阵平移，因此父 scale 会缩放子节点的位置偏移。方向基由局部 quaternion 从子至根组合并归一化，忽略各级 scale/shear，得到正交单位 right(+X)、up(+Y)、direction(+Z)。direction 是光传播方向，矩形位于局部 XY 平面。range、source_radius 的物理长度、width/height 是世界米，不再乘任何 scale；方向光的 source_radius 为角度量。Sky Light 提取为中性位置/方向基，Application 单独发布它的均匀天光辐射。

参数值和坐标都在提取时验证。快照包含自有值，可在对象离场后继续读取；它不保存对象地址、ObjectRoot 或可重入回调。真实删除、撤销恢复与重新启用都沿原 Component 身份，不另建灯管理器或对象索引权威。

## 验收与接线边界

`world_acceptance` 包含真实消费者：反射枚举/值/对象和派生灯、Source/JIT 修改 LightSettings、Material HDR、标量与整组拒绝原子性、混合 mesh/light 提取、父级非均匀缩放、停用/启用、反序 End、删除恢复、Detached 及快照 GC 寿命。数值边界覆盖解析光乘积及矩形面积乘积的边界和相邻越界值、65504 自发光提取、原生绕过 setter 后的提取拒绝；开发和发布构建均使用同一消费者。

World E2E 验证数据与生命周期合同。场景文件、编辑器控件和 SceneSnapshot 接入由应用端到端验收覆盖，GPU 灯、天光、材质、历史与阴影消费由 Renderer 及正式编辑器窗口验收覆盖，见 [Renderer 验收记录](../renderer/acceptance.md)。单独的 World E2E 不替代这些集成证据。
