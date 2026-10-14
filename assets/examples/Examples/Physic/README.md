# Physic 物理示例

在编辑器 **Project → Examples → Physic** 打开总览，或运行 `atelier_editor --physic`。资源浏览器的 **Project → Examples → Physic → Scenes** 包含以下场景，双击场景打开。编辑状态保持静止；**Project → Run** 从当前快照启动独立运行窗口，**Project → Stop** 结束运行，编辑文档不被模拟结果覆盖。运行窗口自动取景，使用当前组件配置持续模拟。

| 场景 | 观察内容 |
| --- | --- |
| `Overview.scene` | 刚体、角色、六部件布娃娃、四轮车、摆锤和移动平台的组合 |
| `RigidBodies.scene` | 箱、球、胶囊在重力下落到静态三角网格地面，最终休眠 |
| `Forces.scene` | 持续施力、激活时的一次冲量，以及周期运动学平台 |
| `Triggers.scene` | Sensor 触发、Ignore 穿透和 Block 支撑；接触事件由验收读取 |
| `Queries.scene` | 远近静态目标及球形重叠目标；验收执行 Ray、Sphere Sweep/Overlap |
| `Raycasts.scene` | 六个 SceneQueryComponent：通道 single/multi/test、对象类型 multi、球扫掠和 Sensor 球重叠；区分触碰、首个阻挡、遮挡后及被通道忽略的目标 |
| `Character.scene` | 胶囊角色移动、落地和台阶障碍 |
| `Constraints.scene` | 固定锚点、距离约束摆锤和初始冲量 |
| `Ragdoll.scene` | 六个胶囊刚体连接成布娃娃，落到障碍及地面，网格跟随各部件姿态 |
| `Vehicle.scene` | 动态底盘、四轮悬挂、持续油门和转向，轮子位置/旋转跟随 Jolt |

示例使用内置网格，不需要下载模型或动画。布娃娃以独立网格表示各物理部件，车辆以箱体和轮子网格表示底盘与车轮；当前没有骨骼蒙皮、动画混合或完整车辆资产编辑器。查询场景的射线/命中和触发事件由自动验收检查，视口尚无专用查询线或事件面板。

## 组件组合

沿现有 Entity Structure 的 **Add Component** 添加组件，公开数值字段在 Parameters 编辑并由 Scene v6 保存。网格和碰撞组件的 `body` 配置沿 Parameters 编辑，并通过通用反射属性保存。

| 组件 | 职责和依赖 |
| --- | --- |
| `PrimitiveComponent` 的 `body` | 网格与 CollisionComponent 共用的静态、运动学或动态物理配置；形状、材料和碰撞过滤 |
| `CharacterComponent` | 虚拟胶囊角色；每固定步读取 `velocity` 和一次性 `jump` |
| `RagdollComponent` | 拥有一个布娃娃控制器，`parts` 显式持有有序部件 Entity |
| `RagdollPartComponent` | 部件 Entity 上的胶囊元数据；`parts` 顺序决定索引，`parent_index` 指向先前部件，`rest_position` 相对控制器 Entity；网格为独立 StaticMeshComponent |
| `VehicleComponent` | 为动态 Body 添加四轮悬挂/驱动，`wheels` 显式持有车轮 Entity，每固定步读取 `throttle/steering/brake` |
| `VehicleWheelComponent` | 车轮 Entity 上的元数据，唯一 `wheel_index` 为 0–3；网格为独立 StaticMeshComponent，不额外创建轮子刚体 |
| `DistanceConstraintComponent` | 约束自身 Body 和目标 Entity 的 Body；`minimum/maximum` 为距离范围 |
| `ForceComponent` | 每固定步持续施力；每次激活后仅消费一次 `initial_impulse` |
| `KinematicMotionComponent` | 驱动运动学 Body，线性速度叠加正弦振幅及周期 |
| `SceneQueryComponent` | 可配置的射线/球扫掠/球重叠，固定步自动采样或一次请求；不需要自身 Body |

Body、Character、Ragdoll 在同一 Entity 上只能启用一种。角色、布娃娃和车辆的控制器 Entity 使用单位世界缩放；部件和车轮为其直接组合子 Entity，空间参考也是控制器 Entity，自身显示缩放只影响网格外观。布娃娃部件的物理尺寸来自 radius/capsule_half_height，车辆物理尺寸来自 Body/Vehicle 设置，不从显示网格猜测。

组件或依赖的 Entity/Body/可视部件停用、删除、离开 World 时释放对应后端对象；恢复后重新创建，旧句柄失效。重复 start/update 不重复创建。形状、关节及悬挂设置在激活时读取，修改后停用再启用；实时输入每个固定步读取。停止整个 World 清理全部物理对象。

距离约束使用 Runtime 的弱 Entity 引用，不延长目标生命周期。停止后通过 `set_constraint_target` 绑定；Scene v6 的 `properties.target` 为 null 或 `{"$ref":"文档ID"}`。保存拒绝场景外目标，加载先创建全部 Entity 再恢复引用；不保存 Runtime 身份或 Jolt 句柄。当前没有目标选择器 UI。

查询组件的局部 start/end 随 owner Entity 的空间参考链转换，radius 使用世界单位；settings、automatic 和 ignore_self 在每次查询时读取。配置随场景保存，请求和命中结果不保存。API、脚本方法、过滤和生命周期合同见[场景查询](../../../../docs/modules/physics/scene-queries.md)。已有用户 Physic 文件夹不会自动合并新模板；可仅复制缺少的 `Raycasts.scene` 到用户工程同名 Scenes 目录，刷新后打开。

## 验证

按根 README 准备锁定工具链，运行：

```powershell
cmake --build --preset dev --target physics_acceptance physics_examples_acceptance
cmake --build --preset release --target physics_acceptance physics_examples_acceptance
```

`physics_examples_acceptance` 先将全部 10 个场景写盘重开并实际模拟，检查约束、位姿、接触/过滤、查询、暂停/单步、停止重启及旧句柄失效，再通过正式编辑器和 D3D12 GPU validation 捕获编辑与模拟画面。窗口证据位于 `out/<配置>/physics-examples-evidence/`。`physics_acceptance` 另外覆盖 22 个底层真实场景，包括 Cook 缓存、边界输入、跨 World 拒绝、组件依赖和生命周期。详见[物理验收](../../../../docs/modules/physics/acceptance.md)。
