# World 与游戏对象

正式入口为 [world.hpp](../../../src/world/world.hpp)，数学值为 [math.hpp](../../../src/world/math.hpp)。`atelier_world` 链接 `atelier_runtime`；网格使用 [mesh.hpp](../../../src/geometry/mesh.hpp) 中不依赖窗口、GPU 或 Runtime 的不可变 CPU 资源协议。无窗口宿主不链接 renderer/RHI。语言执行由宿主适配 `atelier_source` 的已验证入口，[完整消费者](../../../acceptance/world/main.cpp)展示接线。

## 类型与所有权

```text
core.Object
├─ Actor
│  └─ StaticMeshActor
└─ Component
   └─ SceneComponent
      └─ StaticMeshComponent
```

- Actor/Component 是世界的业务模型。所有对象沿用 Runtime 的 TypeId、ObjectId、继承、引用与 GC；类型桶只提供查询索引，不决定执行顺序。
- `register_types` 一次发布 Vec3、Quaternion、Material 三个值类型与上述五个对象类型。元数据由 C++ 声明旁的自动反射生成；StaticMeshActor 追加在原有类型之后，拥有真实独立 TypeId，沿现有 Actor 继承链参与生命周期。额外业务类型在创建首个对象前注册。
- Actor 的隐藏强引用序列决定组成顺序，Component 强持 owner。一个组件的普通强引用会保住 Actor 和兄弟组件；没有外部根的组成环可被 GC 回收。
- Actor 的显式根引用和 SceneComponent 的空间父引用均为弱引用。组成负责拥有，空间关系负责坐标继承，两种顺序独立。
- StaticMeshComponent 继承完整局部 TRS，持有不可变 geometry::Mesh 资源和 Material 值。方块是 `geometry::cube_mesh()` 资源；典型方块对象只有 Actor 与一个作为根的 StaticMeshComponent。没有独立 Transform/Cube 类型、隐式 Transform 查找或 Counter 专用入口。
- `components` 返回普通根快照，保持身份与当前数据，不冻结字段。World 成员目录持有 Actor 根；查询、执行列表、请求和历史来源凭证不另建隐藏根。

Runtime 必须晚于 World、相关对象操作及任务结束。对象可以活过旧 World，但普通根不能延长 Runtime 或关闭的 World 服务寿命。

## StaticMeshActor 完整创建

`create_static_mesh_actor(World&, const StaticMeshActorDesc&)` 返回普通 ObjectRoot，指向已组装、尚未注册的 StaticMeshActor。描述包含 `mesh`（MeshRef）、`transform`（Trs）与 `material`（Material）；结果仅包含一个作为显式根的 StaticMeshComponent，没有独立 Transform。`root_component` 与 `components` 沿既有入口查询真实组成。

工厂先验证 TRS 与基础色，再沿现有 Runtime 创建和 World 组成机制组装对象。失败清理本次未发布对象；成功返回不会修改 World 成员、查询或呈现目录，也不产生生命周期通知或 Begin。开放的运行中 World 同样可以准备 Detached 图；随后 `World::attach` 才注册整组并按当前运行状态决定 Begin。关闭 World 或回调内重入会在创建前拒绝。

MeshRef 可为空，此时根组件依然存在但不产生绘制项。工厂没有内置方块分支；快捷创建或资源条目按需要传入 `geometry::cube_mesh()` 或其他共享网格。MeshRef 是不可变 CPU 内容的共享引用，不是持久资产 ID；World 的组件和提取快照按各自生命周期保有内容，renderer 消费同一协议。

编辑器创建应先取得完整 Detached 图，再一次 attach，并为 `Commit.changed` 记录一个 Actor 创建项；通知失败按已提交结果处理。现有 ActorRemoval 即可整组 detach/restore，保持 Actor、根组件与资源关系，勿把组装步骤拆成独立组件历史。原始 `create_actor/runtime.create` 只建立类型与默认存储，完整默认组成由这个工厂负责。

## 空间树与网格

`Attachment{component, parent, root}` 同时声明组成与空间挂接。`World::attach_components` 接受本 World 成员以及同 Runtime 的 Detached Actor；后者只建立对象组成，不登记成员、查询、呈现或运行回调。`attach` 校验已经完整的对象图后再注册。一个 Actor 可以没有空间组件；有空间组件时必须显式指定唯一根，其余节点必须沿同一 Actor 内的父链抵达根。批次内允许父组件晚于子组件出现；重复身份、多个根、跨 Actor 父引用、环与非法 TRS 在发布前整批拒绝。

根由 Attachment 明确指定，不按类型、注册顺序或组成位置推断。`root_component` 与 `scene_parent` 读取真实引用。根不能被挂到其他节点下面；移除有孩子的节点会失败。先移除或换父处理孩子，再移除该节点。移除最后一个根同时清除 Actor 根引用。

`reparent(component, parent, rule)` 在停止的 World 中调整同 Actor 内的非根节点：

- `keep_local` 保留局部 TRS，世界矩阵随新父变化。
- `keep_world` 先计算 `old_world * inverse(new_parent_world)`，完整验证后再同时发布父引用和局部 TRS。不能表示的剪切、反射、奇异父矩阵或溢出返回错误，原状态不变。浮点正交检查容差为 `1e-6`。

Trs 使用 position、xyzw 四元数、每轴严格为正的 scale；输入有限，四元数非零并规范化，不设置任意缩放下限。Matrix4 为行主序、行向量，`local = S * R * T`，平移位于 12–14；`world = local * parent_world`。世界矩阵可含层级复合形成的剪切，不重新分解成 TRS，也不保存第二份可编辑状态。溢出或无效身份不返回伪造的单位矩阵。

`set_geometry(Runtime&, component, resource)` 是网格资源的写入口：可在 Detached 对象加入世界前配置，也可在宿主边界替换成员资源；回调内重入失败。空资源撤下呈现注册，替换资源不改变 Component 身份或创建 Tick。geometry::Mesh 构造时验证非空三角形列表、有限坐标和有效法线；多个对象可共享同一不可变资源。当前 Material 表达线性不透明基础色，不包含资产导入、着色器图或资源预算系统。

## 生命周期与执行顺序

`bind_behavior(TypeId, Behavior{begin,tick,end})` 配置 Actor 或 Component 的可选执行入口，只能在 World 停止时修改。实际类型沿 Runtime 已有基类链查找最近的一份完整登记；派生登记整体替代祖先登记，显式空登记可关闭继承行为。Actor 和 Component 使用同一有序执行队列与失败清理机制；它只配置 World 调度，不声明第二套类型、方法身份或语言虚分派。

原生适配器可调用现有 Runtime MethodId；语言适配器调用经 Program.bind 验证的 Entry。所有回调都在 native borrow 外执行，携带 Runtime 和 ObjectId，接收者受根与 ExecutionScope 保护。source 当前支持的单入口热发布边界仍然适用。

| 时机 | 行为 |
| --- | --- |
| start | 按 Actor 成员顺序，每个 Actor 先自身 Begin，再按组件组成顺序 Begin；禁用对象也进入这次运行周期 |
| update | 同样顺序 Tick；只包括完成 Begin、启用且有 Tick 的对象，Actor 禁用同时阻止其组件 Tick；之后推进已有协程 |
| 运行中加入 | 新 Actor 先自身再组件 Begin；已有 Actor 新增组件只 Begin 新组件，后续 Tick 使用当前完整组成顺序 |
| enabled 改变 | 只改变 Tick 资格；禁用取消相关任务，不重复 Begin/End |
| 移除组件或 Actor 离场 | 先取消相关任务；移除组件只 End 该组件，Actor 离场先组件逆序 End 再 Actor End；随后解除参与/关系 |
| stop、close | 先停止 Tick 并取消任务，再按 Actor 成员逆序，每组先组件逆序 End，最后 Actor End |

只对成功完成 Begin 的对象执行一次 End。动态插入后的 End 以**当前组成顺序的逆序**为准，不以插入时间排序。没有回调的数据对象正常拥有生命周期，但不会产生空 Tick 调用。静态网格呈现不依赖 Tick。Actor Begin 可以读取完整组件组成，但组件 Begin 的业务副作用仍在随后执行。

Actor 或 Component Begin 失败停止后续 Begin，并逆序结束已经成功开始的对象。任一对象 Tick 首错停止本帧后续执行，整个 World 停止并完成 End 清理。End 失败仍继续其余清理，只报告首错；已产生的业务副作用不回滚。

`start_task` 接受活动 Actor 或 Component 的真实 Coroutine 启动器，World 持有 ready/suspended 任务。Actor 禁用或离场取消自身和组件任务；单组件禁用或移除只取消其任务，stop/close 取消全部任务。这里不提供线程调度器。

LifecycleObserver 的 attached/detached 表达 World 注册/注销：Actor 整组注册按 Actor、组件正序通知，注销按组件逆序、Actor 最后；World close 还反转 Actor 顺序。动态组件增删采用相同规则；Detached 组成变动不产生这些通知。注册通知可以看到已提交完整图；注销通知可以看到已撤参与关系的完整对象，通知期间根仍保活。通知出错继续交付整组事件，并以 `Commit.notification_error` 报告。activated/deactivated 继续表达启用资格变化，所有通知均不代替 Begin/End。回调中同步结构修改失败，可用 `request/flush` 在宿主边界延后执行。

正式创建路径为 `create_actor/create_component` 建立默认存储，`attach_components` 与字段写入准备 Detached 图，`attach` 校验并注册，之后由 start 开始运行。构造不访问尚未建立的 owner/World，GC 与析构不执行业务 End。本片没有额外 Initialize/PostLoad 钩子；文件解码及加载就绪协议仍待序列化增量。

## 状态、编辑与历史

Actor 的私有非拥有 `member_world` 是成员关系权威。`inspect_actor` 返回 Detached、AttachedInactive、Active 或 Invalid，以及 enabled/can_edit/can_present。`inspect_component` 还报告 owner 和独立激活资格。外部 Runtime 身份或错误类型是错误，本 Runtime 的已终结身份是 Invalid。

- 空 Actor 仍可选择和编辑；`can_present` 表示实际网格呈现注册。静止编辑 World 仍渲染已有网格。
- `read/write/set_transform` 使用实际 Component 和正式 FieldPath；编辑写要求 World 停止，也允许准备同 Runtime 的 Detached/未归属组件，拒绝其他 World 的成员。字段级 TRS 写与整值写使用同一验证。`remove_component/reparent/set_enabled` 同样支持 Detached 组成；编辑器 can_edit 仍以自身文档成员资格决定。
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

World 八组真实消费者覆盖 JIT 与协程、普通引用与 GC、World/历史/队列隔离、提交后失败、Actor/Component 有序生命周期、Detached 组成、空间树和通用网格资源，以及 StaticMeshActor 完整工厂、真实类型、材质/网格投影和整组身份恢复。编辑器文档链还覆盖一个根 StaticMeshComponent 的创建、继承字段编辑、整组位置恢复、旋转/缩放保留、根移除及同身份撤销。工厂增量的双配置记录为 `out/static-mesh-actor-dev-build.log` 和 `out/static-mesh-actor-release-build.log`；GPU 和窗口行为由各自实际窗口验收单独证明，之前的 `out/scene-candidate.json` 仍只记录其原冻结候选。
