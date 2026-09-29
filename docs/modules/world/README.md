# World与游戏对象

正式公共入口为[world.hpp](../../../src/world/world.hpp)，数学值为[math.hpp](../../../src/world/math.hpp)。`atelier_world`只依赖`atelier_runtime`，不依赖Application、编辑器、renderer或RHI。JIT适配由宿主使用现有`atelier_source`提供；[无窗口消费者](../../../acceptance/world/main.cpp)展示完整接线。

## 对象与引用

- 宿主拥有Runtime，先调用`world::register_types`，再注册额外派生类型，最后创建对象。Types返回正式TypeId、Counter方法/字段以及Transform三组字段路径。
- Actor和Component都是core.Object的正式派生类型，Transform、Cube、Counter正式派生于Component。Actor可有零组件；对象与语言执行共用Runtime的ObjectId、存储及GC，没有第二实体身份。
- Actor的隐藏强引用序列是组成与顺序的权威。附属Component强持owner，**单个组件强引用会保住Actor及兄弟组件**；这个代价是明确合同。移除组件通过一次Core引用批次解除双向关系。
- Cube以弱引用明确绑定同Actor的Transform。每个Actor至多一个Transform，包括其派生类型；移除仍被Cube依赖的Transform会失败。允许多个Cube分别产生绘制项，UI可以限制其首片创建范围。
- `components`返回普通ObjectRoot快照；历史保活也使用普通根。它们不冻结字段或组成。无外部根的Actor↔Component环可由现有追踪GC回收；索引、请求与历史来源凭证不形成隐藏根。

World借用Runtime。Runtime必须晚于World、未结束任务及相关对象操作结束；根本身不能延长Runtime寿命。对象数据可以活过World，不能靠强引用继续使用关闭的世界服务。

## 状态、编辑与呈现

`inspect_actor(Runtime&,ObjectId)`统一返回状态和`enabled/can_edit/can_present`。它先验证上下文和身份，再访问类型与载荷；本Runtime已终结或空身份为Invalid，其他Runtime或非Actor类型是错误。`inspect_component`给出其独立启用、激活与呈现资格。

| 状态 | 含义 |
| --- | --- |
| Detached | 对象有效但不是World成员，可读取或明确终结；不能通过World编辑/运行/呈现 |
| AttachedInactive | 仍是成员，但World停止或Actor未启用；编辑模式下仍可显示、编辑 |
| Active | World运行且Actor启用；组件还须满足自己的enabled与执行资格 |
| Invalid | 对象身份已经终结；禁止再读载荷，不用墓碑区分GC与destroy |

Actor私有的非拥有`member_world`是唯一成员关系，World成员目录持根，位置/类型表是索引。状态只读、由权威事实派生；进入和退出有内部重入保护，没有独立可写state或GC业务状态。

`World.read/write/set_transform`要求当前组件成员，编辑写还要求World停止且未进入回调/关闭。`enabled`只影响游戏运行意图，不隐含隐藏对象。Cube的呈现注册与Counter执行注册分别维护：stop取消游戏任务和执行注册，保留编辑呈现；detach/close结束全部相关World参与。

## 结构操作与历史

1. `create_actor/create_component`建立Detached对象并返回普通根。
2. `World.attach`接纳Actor；`attach_components`接纳一批Attachment。先验证整个批次及依赖，再一次更新组成、owner和Cube绑定，最后发布索引。
3. 同步`detach`返回ActorRemoval，包含根与原成员位置；`remove_component`返回ComponentRemoval，另含原owner和组成位置。记录可无抛移动，`restore`恢复同一身份，失败不抢占其他归属。
4. 历史来源用World寿命的弱凭证，关闭后该次restore失效；它不保持World，也不限制Actor的永久归属。history裁剪只释放根，绝不触发永久销毁。

同Runtime的Actor完全脱离后，可独立加入另一个兼容且开放的World。在A中直接加入B会失败；detach A与attach B是两个可分别失败的步骤。当前类型的兼容性包括唯一Transform、Cube显式依赖、有效TRS及统一Runtime；没有资源迁移、原子转移或永久affinity规则。

`Result`错误表示操作未正常完成。`Commit.notification_error`明确表示**变化已经提交，但生命周期通知或终结清理失败**；消费者先记录已完成变化，再报告错误、停止后续动作，不能据此伪回滚。原生构造、用户回调副作用与进程级内存耗尽不提供通用回滚保证。关系批次的普通验证失败不半写。

## 执行、请求与退出

`start/stop/set_enabled`统一维护运行意图与实际执行注册。Counter默认调用其正式原生advance方法；可在停止状态安装CounterUpdate，适配同Runtime上的Program.Entry。更新期间身份受成员根及接收者根保护，source/JIT调用发生在所有with_native和引用遍历之外。

`start_task`接受实际Coroutine启动器。World拥有返回的ready/suspended任务，在update推进；组件/Actor停用、移除、stop或close会取消相关任务并释放帧。启动器是可信宿主接口，必须使用传入Runtime与接收者；本模块不提供通用线程调度器。

LifecycleObserver是提交/退出通知，不拥有必要的执行或呈现注册。Actor的激活通知表示其组的有效运行条件改变；组件直接加入、移除或启用变化也通知相应目标。回调在执行保护下运行：同步结构重入失败，允许请求队列延后；通知失败记录首因并停止游戏更新，已有关系不伪装成未提交。

`request`只返回排队号。宿主在静止点调用`flush`；回放重验原World、完整ObjectId和owner条件，不跟随已换World的目标。排队destroy不会提前使对象失效。**RequestCompletion只有Commit，没有同步移除的根或memento**；异步消费者若要保留对象，必须自己已有普通根。当前没有异步历史消费者，不另建结果variant或隐藏保活机制。回调产生的新请求在下一次flush处理。

close先拒绝新操作，收束真实任务并撤下执行/呈现注册，清除member_world及索引，最后释放成员根。完成后World不可重启；重复close无副作用，残留排队回执可由flush取回关闭错误。World析构必须处于合法宿主边界，会完成同一close流程。

`world::destroy_actor/destroy_component`是唯一游戏对象销毁入口。在场对象先经所在World退出，Detached对象直接进入共同关系清理与Runtime终结。原World已析构也能销毁离场整组；普通强引用不否决终结，旧身份不能恢复。Actor销毁先终结依赖者、再终结Transform，最后Actor。可信宿主不得直接Runtime.destroy游戏对象绕过这一入口。

## Transform数学

Trs包含position、xyzw四元数rotation及scale。输入必须有限，四元数非零后归一化，scale每轴严格大于零；没有任意epsilon缩放下限。字段级Transform写也经过同一TRS验证，失败保留原值。

Matrix4为16个float，行主序/行向量，`local = S * R * T`，平移位于12–14，3/7/11为零、15为一。World求唯一矩阵，`RenderItem.world`由renderer直接消费，不另存第二套可编辑矩阵。当前没有空间父子或多空间部件操作；未知/最终不可表示的矩阵不以identity冒充成功。

四元数单字段写可能规范化其他分量，所以应用层Transform编辑历史保存完整Trs before/after，再用set_transform恢复。World本身不建设快照或多写冲突版本体系。

## 验收

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target world_acceptance
cmake --preset release
cmake --build --preset release --target world_acceptance
```

两配置均输出`World acceptance: PASS (4/4)`。四组真实消费者覆盖：

- 空Actor、真实/派生组件、组成与依赖失败不半写、字段路径与完整TRS、编辑可见但不运行、同一Runtime的JIT Counter及真实协程取消。
- 普通Actor字段和组件字段保组，World关闭并析构后的对象读取与整组明确销毁，以及独立的不可达强环GC样本。
- 两个World隔离、Detached重新接纳、历史来源、跨Runtime/类型错误、旧World排队销毁拒绝及失效代际不复活。
- 提交后通知失败、重入拒绝/延后、关闭准入与实际任务/注册归零。

证据保存在实现树的`out/world-evidence/`：两配置构建/运行日志及最终源文件、二进制hash清单。本片未运行GUI，也未重跑已验收I/II的全部消费者。编辑器/renderer的IV完成由对应应用验收收口，III通过不等于整轮产品已经交付。
