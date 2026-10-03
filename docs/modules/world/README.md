# World 与游戏对象

正式入口为 [world.hpp](../../../src/world/world.hpp)，数学值为 [math.hpp](../../../src/world/math.hpp)。`atelier_world` 链接 `atelier_runtime`；网格使用 [mesh.hpp](../../../src/geometry/mesh.hpp) 中不依赖窗口、GPU 或 Runtime 的不可变 CPU 资源协议。无窗口宿主不链接 renderer/RHI。语言执行由宿主适配 `atelier_source` 的已验证入口，[完整消费者](../../../acceptance/world/main.cpp)展示接线。

## 类型与所有权

```text
core.Object
├─ Actor
└─ Component
   └─ SceneComponent
      └─ StaticMeshComponent
```

- Actor/Component 是世界的业务模型。所有对象沿用 Runtime 的 TypeId、ObjectId、继承、引用与 GC；类型桶只提供查询索引，不决定执行顺序。
- `register_types` 一次发布 Vec3、Quaternion、Material 三个值类型与上述四个对象类型。元数据由 C++ 声明旁的自动反射生成；额外业务类型在创建首个对象前注册。
- Actor 的隐藏强引用序列决定组成顺序，Component 强持 owner。一个组件的普通强引用会保住 Actor 和兄弟组件；没有外部根的组成环可被 GC 回收。
- Actor 的显式根引用和 SceneComponent 的空间父引用均为弱引用。组成负责拥有，空间关系负责坐标继承，两种顺序独立。
- StaticMeshComponent 继承完整局部 TRS，持有不可变 geometry::Mesh 资源和 Material 值。方块是 `geometry::cube_mesh()` 资源；典型方块对象只有 Actor 与一个作为根的 StaticMeshComponent。没有独立 Transform/Cube 类型、隐式 Transform 查找或 Counter 专用入口。
- `components` 返回普通根快照，保持身份与当前数据，不冻结字段。World 成员目录持有 Actor 根；查询、执行列表、请求和历史来源凭证不另建隐藏根。

Runtime 必须晚于 World、相关对象操作及任务结束。对象可以活过旧 World，但普通根不能延长 Runtime 或关闭的 World 服务寿命。

## 空间树与网格

`Attachment{component, parent, root}` 同时声明组成与空间挂接。一个 Actor 可以没有空间组件；有空间组件时必须显式指定唯一根，其余节点必须沿同一 Actor 内的父链抵达根。批次内允许父组件晚于子组件出现；重复身份、多个根、跨 Actor 父引用、环与非法 TRS 在发布前整批拒绝。

根由 Attachment 明确指定，不按类型、注册顺序或组成位置推断。`root_component` 与 `scene_parent` 读取真实引用。根不能被挂到其他节点下面；移除有孩子的节点会失败。先移除或换父处理孩子，再移除该节点。移除最后一个根同时清除 Actor 根引用。

`reparent(component, parent, rule)` 在停止的 World 中调整同 Actor 内的非根节点：

- `keep_local` 保留局部 TRS，世界矩阵随新父变化。
- `keep_world` 先计算 `old_world * inverse(new_parent_world)`，完整验证后再同时发布父引用和局部 TRS。不能表示的剪切、反射、奇异父矩阵或溢出返回错误，原状态不变。浮点正交检查容差为 `1e-6`。

Trs 使用 position、xyzw 四元数、每轴严格为正的 scale；输入有限，四元数非零并规范化，不设置任意缩放下限。Matrix4 为行主序、行向量，`local = S * R * T`，平移位于 12–14；`world = local * parent_world`。世界矩阵可含层级复合形成的剪切，不重新分解成 TRS，也不保存第二份可编辑状态。溢出或无效身份不返回伪造的单位矩阵。

`set_geometry(Runtime&, component, resource)` 是网格资源的写入口：可在 Detached 对象加入世界前配置，也可在宿主边界替换成员资源；回调内重入失败。空资源撤下呈现注册，替换资源不改变 Component 身份或创建 Tick。geometry::Mesh 构造时验证非空三角形列表、有限坐标和有效法线；多个对象可共享同一不可变资源。当前 Material 表达线性不透明基础色，不包含资产导入、着色器图或资源预算系统。

## 生命周期与执行顺序

`bind_behavior(TypeId, Behavior{begin,tick,end})` 配置组件的可选执行入口，只能在 World 停止时修改。实际类型沿 Runtime 已有基类链查找最近的一份完整登记；派生登记整体替代祖先登记，显式空登记可关闭继承行为。它只配置 World 调度，不声明第二套类型、方法身份或语言虚分派。

原生适配器可调用现有 Runtime MethodId；语言适配器调用经 Program.bind 验证的 Entry。所有回调都在 native borrow 外执行，携带 Runtime 和 ObjectId，接收者受根与 ExecutionScope 保护。source 当前支持的单入口热发布边界仍然适用。

| 时机 | 行为 |
| --- | --- |
| start | Actor 成员顺序，再按各 Actor 的组件组成顺序 Begin；禁用的组件也进入这次运行周期 |
| update | 同样顺序 Tick；只包括完成 Begin、Actor/组件均启用且有 Tick 的组件；之后推进已有协程 |
| 运行中加入 | 对新加入的组件按其组成顺序 Begin，后续 Tick 使用当前完整组成顺序 |
| enabled 改变 | 只改变 Tick 资格；禁用取消相关任务，不重复 Begin/End |
| 移除组件或 Actor 离场 | 先取消相关任务，再按该组组成逆序 End，然后解除参与/关系 |
| stop、close | 先停止 Tick 并取消任务，再按 Actor 成员逆序、组件组成逆序 End |

只对成功完成 Begin 的组件执行一次 End。动态插入后的 End 以**当前组成顺序的逆序**为准，不以插入时间排序。没有回调的数据组件正常拥有生命周期，但不会产生空 Tick 调用。静态网格呈现不依赖 Tick。

Begin 失败停止后续 Begin，并逆序结束已经成功开始的组件。Tick 首错停止本帧后续执行，整个 World 停止并完成 End 清理。End 失败仍继续其余清理，只报告首错；已产生的业务副作用不回滚。

`start_task` 接受真实 Coroutine 启动器，World 持有 ready/suspended 任务。组件/Actor 禁用、移除、stop 或 close 取消对应任务并释放帧。这里不提供线程调度器。

LifecycleObserver 继续作为附着、激活资格改变和离场通知，不能代替 Begin/End。回调中同步结构修改失败，可用 `request/flush` 在宿主边界延后执行。

## 状态、编辑与历史

Actor 的私有非拥有 `member_world` 是成员关系权威。`inspect_actor` 返回 Detached、AttachedInactive、Active 或 Invalid，以及 enabled/can_edit/can_present。`inspect_component` 还报告 owner 和独立激活资格。外部 Runtime 身份或错误类型是错误，本 Runtime 的已终结身份是 Invalid。

- 空 Actor 仍可选择和编辑；`can_present` 表示实际网格呈现注册。静止编辑 World 仍渲染已有网格。
- `read/write/set_transform` 使用实际 Component 和正式 FieldPath；编辑写要求 World 停止。字段级 TRS 写与整值写使用同一验证。
- `attach_components` 一次提交组成、owner、root、parent 引用；普通验证失败不半写。`detach` 只撤 World 成员资格，保留整个对象图。
- ActorRemoval 保存普通根与成员位置；ComponentRemoval 保存组件根、owner、组成位置、parent 和根身份。restore 恢复同一 ObjectId，重新验证全部关系及 World 来源，不抢占其他归属。
- 历史来源凭证不保持 World；关闭后失效。裁剪历史只释放根，不隐式 destroy。TRS 编辑历史保存完整前后值，因为四元数单字段写会规范化其他分量。
- `Commit.notification_error` 表示关系已提交、通知或清理出错。消费者先记录已完成变化，再报告错误，不能把它当作未提交。构造、副作用及进程级内存耗尽不提供通用回滚。

同一 Runtime 内完全离场的 Actor 可加入另一个 World。排队请求仅持 ObjectId 与原 World/owner 条件，flush 重新验证，不跟随对象迁入另一 World。回执只有 Commit，不返回移除历史或保活根；回调中新排的请求在下一次 flush 执行。

close 完成 End、撤呈现与成员关系、释放成员根，之后不可重启；重复 close 无副作用。`destroy_actor/destroy_component` 是游戏对象的终结入口，先经当前 World 退出再清理关系并调用 Runtime。整组按组成逆序终结组件，最后终结 Actor；结束参与与对象被 GC 收集是不同事件。可信宿主不得直接调用 Runtime.destroy 绕过此入口。

## 验收

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target world_acceptance editor_document_acceptance
cmake --preset release
cmake --build --preset release --target world_acceptance editor_document_acceptance
```

World 六组真实消费者覆盖 JIT 与协程、普通引用与 GC、World/历史/队列隔离、提交后失败、混合类型的有序生命周期、空间树和通用网格资源。编辑器文档链还覆盖一个根 StaticMeshComponent 的创建、继承字段编辑、整组位置恢复、旋转/缩放保留、根移除及同身份撤销。本机 `out/scene-candidate.json` 关联候选源码、配套产物与准确验收日志；复用未改源码的既有证据会单独标明。GPU 和窗口行为由各自实际窗口验收单独证明。
