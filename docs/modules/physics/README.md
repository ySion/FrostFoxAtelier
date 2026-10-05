# 物理模块

`atelier_physics` 将锁定版本的 Jolt 接到 World，使用同一个 Runtime 的 Actor、Component 和对象身份。先调用 `world::register_types(runtime)`，再调用 `physics::register_types(runtime)`；二者都须在 Runtime 创建首个对象之前完成。`PhysicsScene::create(world, types)` 订阅该 World 的生命周期，宿主持有场景并在 World/Runtime 销毁前先销毁场景。同一进程的多个场景共享 Jolt 全局类型注册，场景本身各自拥有物理系统。

一个 Actor 最多启用一个 `BodyComponent`，并须有显式根 `SceneComponent`。普通 Actor 在根上设置位置与旋转，在非空间的 `BodyComponent` 上配置盒或球形状及静态、运动学或动态运动类型。`StaticMeshActor` 可将工厂创建的 `StaticMeshComponent` 作为根，再附加 `BodyComponent` 并选择 `triangle_mesh`：物理模块读取根的不可变 CPU 网格，按根缩放生成三角形碰撞形状。三角网格只支持静态刚体；运行期间经 `set_geometry`/`set_mesh_binding` 替换资源会在下一次 `PhysicsScene::update` 重建该刚体。盒/球的根缩放须为 `(1,1,1)`。目前不读取子网格组件组成复合碰撞形状。

宿主调用 `PhysicsScene::update(elapsed)` 先执行一次 `World::update`，再以 60 Hz 固定步长运行最多 8 个物理子步，超过该帧上限的积压时间被丢弃。动态刚体的结果写回 Actor 根位置和旋转，保留创作缩放；运动学刚体在每个子步从根读取目标。`World::set_transform` 仍只允许停止状态编辑；运行时运动学目标需要宿主或业务通过其受控运行路径提供。`set_gravity` 修改场景重力，`add_force` 与 `add_impulse` 仅接收已激活的动态 Actor。

接触监听器只把 Jolt 的开始/结束通知排入队列。完成全部子步后，`on_contact` 回调取得当前有效的两个 Actor 和 `BodyComponent` 身份；已移除刚体的过期通知不会交付。回调可以排队 World 请求，不能在物理更新过程中销毁场景。停用、分离或停止 World 会移除对应 Jolt 刚体；重新激活会按当前组件与网格创建新刚体。当前没有编辑器物理属性面板、网格烹饪缓存、连续碰撞/约束配置或专用服务器程序。
