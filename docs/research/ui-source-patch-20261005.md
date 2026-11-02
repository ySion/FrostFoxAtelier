# UI 源码 patch：稳定实例与子树事务

日期：2026-10-05。实现起点 `404b2e66c8c2452161ad0817205763b8902c4146`。这是按[更新机制研究](ui-dsl-update-comparison-20261005.md)推进的后续实现与实际验收记录；长期用法以[DSL 文档](../modules/editor/declarative-ui.md)为准。

随后确认的正式默认模式为保存触发、同步准备、事务提交。后台调度按实际使用中的卡顿证据另行评估；决定统一记录在[默认更新策略](../modules/editor/declarative-ui.md#默认更新策略)。

## 结果

保存 `.ui.source` 后，兼容的 Instance / Runtime Context 保持原身份。仅新增、删除或不兼容子组件建立、结束相应生命周期。所有定义、代码租约、状态及子树变化沿同一准备、Session 接受、旧 effects 结算、提交链路处理。

- 稳定的 `InstanceData` 持有 Context，`DefinitionState` 持有可替换的代码、槽位、依赖、节点值与子实例。父代码未变时仍检查子组件的新定义。
- `LiveDocument` 保留一份区域表和 active/candidate Module，不再复制整份区域实例树。
- 删除 `Instance::create(previous)` 和公开 `publish`；输入与源码共同使用 `update(inputs, optional_definition)`，无参 update 幂等保留已有准备结果。
- 删除属性回到新定义默认值；state 按名称/类型保留；具名组合按名称/种类/组件类型匹配；Repeat 按 item key 保留。
- 新定义暂借旧子实例的 owner，Session 取消效果处理完成后才转移成员并回收移除的实例。关闭释放代码、元数据和 Runtime 根。

审查中处理了两个交界问题：源码拒绝不能撤销文件检查前刚收到的合法局部事件；源码候选首次挂载失败也不能把旧源码下合法的输入加入失败缓存。端到端验收均覆盖拒绝后继续操作与恢复。

## 端到端验收

| 入口 | Dev | Release | 核对内容 |
| --- | --- | --- | --- |
| `editor_document_acceptance` | PASS | PASS | 原文档/World、组合、事务、代码复用及新增源码 patch 场景 |
| `ui_acceptance` | 20/20 PASS | 20/20 PASS | 基础输入、布局、状态、稀疏更新与批次校验 |
| `atelier_editor --acceptance-project … E:/ffa_testproj` | PASS | PASS | 正常窗口、工程操作、保存热更、错误恢复、草稿 token、折叠状态、后续 World 编辑 |
| `--profile-ui-reload` | 120 轮 PASS | 120 轮 PASS | 实际 JIT/Instance/Session 替换、失败、关闭及资源采样 |

[源码 patch 消费者](../../acceptance/editor/ui_source_patch.inl)直接观察 Context 身份和真实生命周期计数，覆盖：

- 注释保存：零绑定求值、零 Context 创建/回收；无关区域的身份与绑定计数不变。
- 子组件代码修改、增删和换型：兼容子实例保留；新增创建一次；移除在提交后回收；类型变化只替换对应子实例。
- state 重排、新增及换型，属性删除，新 JIT 事件执行，文本草稿保留。
- 后续区域缺失、Session 拓扑拒绝、重复失败、同帧用户事件，以及源码失败后的首次挂载恢复。
- 关闭后 Runtime 根和存活对象返回基线。[组合消费者](../../acceptance/editor/composed_ui.inl)另覆盖 Repeat 根节点重命名时的实例、宿主 key 和编辑 token 保留。

窗口证据位于本地忽略目录：

```text
out/ui-source-patch-20261005/
  manifest.json
  dev-profile.csv
  release-profile.csv
  project-before.json / project-after.json
  dev/run-25724-400699093/acceptance.txt
  release/run-29724-400731046/acceptance.txt
```

各窗口目录含 `ui-live-updated.png`、`ui-live-error.png`、`ui-live-recovered.png`。源工程只复制 Config/Content/Source，前后逐文件 SHA-256 完全一致。原来运行的编辑器未关闭。manifest 记录相关实现与执行文件的 SHA-256，区分工作树改动和起点提交。

## 采样结果

真实两类型场景：Host 引用 Leaf，每轮修改 Leaf，Host 复用机器码。每配置 120 轮，其中 4 次冷启动、116 次局部源码替换；每 30 轮关闭并重新挂载。

| 指标 | Dev | Release |
| --- | ---: | ---: |
| 冷编译中位数 | 49.47 ms | 15.99 ms |
| 修改 Leaf 的编译中位数 / P95 | 25.59 / 26.10 ms | 8.07 / 8.37 ms |
| 局部替换的准备提交中位数 / P95 | 0.861 / 0.920 ms | 0.101 / 0.123 ms |
| 每次挂载至关闭前累计 Context 创建 / 回收 | 2 / 0 | 2 / 0 |
| 关闭后累计 Context 创建 / 回收 | 2 / 2 | 2 / 2 |
| 活跃时可执行提交页增量 | 8192 B | 8192 B |
| 关闭后可执行页增量、Runtime 根 / 存活对象 | 0 B、0 / 0 | 0 B、0 / 0 |

准备提交时间包括 Instance 更新、Session 接收描述、定义提交与旧租约释放，不含布局和渲染。Release 四次关闭后的进程私有内存均为 45,539,328 B。进程私有内存包括分配器缓存，不能直接当作实例内存。

真实编辑器的单次窗口样本：

| 保存 | Dev 编译 / 保存至接受帧 | Release 编译 / 保存至接受帧 |
| --- | ---: | ---: |
| 有效结构文字修改 | 155.886 / 407 ms | 62.440 / 360 ms |
| 错误源码保留旧界面 | 0.527 / 234 ms | 0.069 / 250 ms |
| 仅注释恢复 | 4.986 / 250 ms | 0.206 / 219 ms |

这些是不同负载下的本机样本，不能将两类型微场景的 0.101 ms 解释为整个编辑器热更延迟；窗口总耗时还包含文件检查、200 ms 去抖、帧推进及呈现。没有在相同负载下重测旧版本的准备耗时，不声称耗时提升倍数。直接验证的收益是兼容 Context 零重建、无关绑定零求值，以及旧对象/代码页正常回收。

## 保留的边界

- 仍全文件降低和检查、按组件编译；组件代码或结构变化重算该组件的绑定，不是逐表达式增量编译。
- 源码保存仍遍历已挂载实例并更新元数据，没有依赖索引跳过整个未变子树。编辑器仍向 Session 提交完整工作区快照。
- JIT 仍在 Runtime 线程同步执行。此改动解决实例更新粒度，不改变 Runtime 并发合同。
- 同名同类型 state 保留；新 state 的初始化先于旧值迁移。需要完全重置状态时应重新挂载，不靠修改初值强制覆盖。
- SS、C++ 保存热更、多文件 imports、任意 Runtime 业务类型布局热替换尚未提供。
