# UI / JIT 热更新审计

日期：2026-10-05。审查固定点为 `e42c383744e738f15e8a1083440d472552d6efd9`，覆盖工作树中第二阶段的已跟踪和新增文件。本次使用五个 `gpt-6.1-sol` 审查分工：项目规范、行为与生命周期、JIT 所有权、性能、声明前端；主负责人复核并实现修复。运行时负责人另行确认共享 Program 的现有契约。本轮修复与第二阶段实现一同交付至 `codex/world-editor-integration` 分支。

## 结论

发现了会让旧画面失去事件接收者的生命周期缺陷，以及首次挂载拒绝、容量和声明歧义等边界问题，现已修复。更新统一经过准备、Session 接受、旧 effects 结算、提交；可恢复失败保留旧实例树和交互。当前同步热更可以继续作为真实编辑器的基础。本轮未发现仍需阻断交付的 P1/P2；这一结论只覆盖本次改动和已运行场景。

当前契约与作者用法以[原生声明式 UI](../modules/editor/declarative-ui-proposal.md)为准。本文件记录发现、取舍与验收证据，不另立长期规则。

本记录对应 `404b2e6` 的审计交付。随后保存时整片克隆实例的路径已被[稳定实例上的源码 patch](ui-source-patch-20261005.md)替换；更新调度已定稿为[默认同步更新](../modules/editor/declarative-ui-proposal.md#默认更新策略)。下列性能、结构描述和后续候选项保留为本次审计的历史证据，不代表当前任务优先级。

## Spec：边界、行为和生命周期

| 严重度 | 问题与触发 | 处理与证据 |
| --- | --- | --- |
| P1 | 动态更新先移除子实例，后续编辑 key 或属性校验失败；旧控件还在，但其事件 owner 已经退休 | `PreparedUpdate` 保留旧成员关系，准备整棵子树；拒绝恢复输入、局部 state 和快照，接受后才移除 owner。真实点击验收覆盖失败卸载后的旧状态、key、incarnation 和事件响应 |
| P1 | 普通输入更新通过组件准备，但完整 Session 拒绝拓扑；原拒绝链只处理源码候选 | LiveDocument 同时拒绝普通准备结果；相同失败输入暂缓重试，纠正后恢复。Source 候选继续整体替换 |
| P2 | 首次挂载被 Session 拒绝，直接删掉区域后，下一帧又以相同输入创建同样的无效实例 | 保留区域配置与失败输入，释放候选实例，贡献空视图；修正输入或源码后重新挂载。验收连续重试三帧并检查没有保留实例 |
| P2 | 只计算最终输出控件，无法约束大量不输出控件的空参数组件 | 将每个宿主根区域的 10000 声明节点额度逐层传递，新建前检查。100×100 个空参数行的展开被拒绝，根和存活对象恢复到原数量 |
| P2 | `height`、`expand_height`、`single_line` 写入同一布局轴，结果依赖失效求值顺序 | 具体控件只允许一种高度策略；六种冲突顺序及准确行列均验收，普通组件输入不受误伤 |
| P2 | 组件类型可以与内建节点重名，组合与 Items 类型解析产生歧义 | 前端统一内建节点名称表，在类型名处拒绝；九个内建类型均覆盖 |
| P2 | 已存在区域忽略调用方变化后的 key aliases，可能继续使用旧事件路由 | parent 与 aliases 固定为区域配置，变化明确拒绝并保留原快照，恢复配置后清除诊断 |
| P2 | `Instance::close` 关闭 Context，但仍持有组件元数据和子类型，保留的 C++ 外壳继续持有 JIT | close 清空子树、元数据和代码引用；弱引用验收确认关闭后组件释放，close 可重复调用 |

新增验收位于 [ui_reload_transaction.inl](../../acceptance/editor/ui_reload_transaction.inl) 和 [ui_reload_profile.inl](../../acceptance/editor/ui_reload_profile.inl)。它们使用真实 Source/JIT、Runtime 和 Session，没有 mock 或另建解释执行路径。

## Standards：结构与冗余

- 作者语法与映射仍在 `declaration`，代码和实例树在 `component`，文件更新与宿主区域在 `LiveDocument`。Workspace 继续采样 World，Application 负责 Session/effects/render 顺序。
- 删除提前移动至 `retired` 的旧路径；子树准备结果本身持有待提交内容。没有新增一套宿主事务管理器。
- 删除 Region 中重复保存的完整输入快照；实例按变化槽位保存撤销值。只有发生拒绝时才保留失败输入，用于避免每帧重新创建失败候选。
- 稳定帧不再递归执行整树退休清理。key 序列没有变化时，跳过 key 集合构造和差集统计；变化控件片段通过 swap 提交或恢复。
- 每版 Module 的声明、源码位置和子组件关系独立；只共享生成代码完全相同的 Program。缓存仅来自仍活着的上一接受模块，没有跨文档或全局历史版本仓库。
- Dev 与 Release 开发工作树均支持保存更新。先前“开发构建”文案容易误读为仅 Debug，现已明确；没有把有效的 Release 作者行为删除。
- 已删除旧 `position.ui.source` 入口。第二阶段首次测量保留为历史证据，并标明后续修正与新数据的入口。

运行时负责人确认：同一活 Runtime、固定 UI Context 和固定 dispatch 入口下，已发布且不再 request/publish/close 的共享 Program 符合当前类型与代码契约。单个 Module 退休仅释放自己的引用；Runtime、服务和后端 DLL 仍须晚于最后租约销毁。

## 性能结果

### 真实编辑器窗口

六个组件的文件更新仍整体解析和校验；修改 Position 文案只重编受影响组件，源码注释变化复用所有组件代码。

| 配置 | 原整模块编译样本 | 本次单组件修改编译 | 本次保存至接受帧 | 注释恢复编译 / 接受帧 |
| --- | ---: | ---: | ---: | ---: |
| Dev | 390.7 ms | 144.5 ms | 391 ms | 4.445 ms / 250 ms |
| Release | 141.5 ms | 52.9 ms | 312 ms | 0.169 ms / 234 ms |

编译阶段约减少 63%。这是同机器先后两轮真实窗口流程的样本，不是统计吞吐上限；原数据见[第二阶段初始记录](native-ui-authoring-20261005.md)。保存到画面仍包括 100 ms 轮询、200 ms 去抖、实例迁移和布局绘制。

### 连续替换与资源

可重复命令（在对应构建目录执行）：

```powershell
./atelier_editor_document_acceptance.exe --profile-ui-reload ../ui-reload-audit-20261005/dev-reload.csv
```

Dev、Release 各运行 120 轮真实编译、实例替换和错误候选，每 30 轮清空 Session、释放 Module/Instance 并执行现有收集步骤。两类型场景中每次叶修改编译一个类型、复用一个类型；冷编译共 4 次，增量替换共 116 次。

| 配置 | 两类型冷编译中位数 | 单类型修改中位数 / P95 |
| --- | ---: | ---: |
| Dev | 54.32 ms | 27.57 / 29.86 ms |
| Release | 17.63 ms | 8.46 / 10.27 ms |

每轮替换及失败后均只有两个 UI Context；四个关闭检查点的 roots/live objects 都回到 0。挂载阶段可执行提交内存相对基线固定增加 8192 字节，关闭后每次回到基线。预热后 Release 私有内存保持 45,494,272 字节；Dev 前三个关闭点为 50,438,144 字节，最后为 36,724,736 字节。本工作负载没有观察到随替换次数增长的资源滞留；进程私有内存包含分配器保留空间，不能据此声称所有场景无泄漏。

## 最终验收

- Dev / Release：`editor_document_acceptance` 全部通过；`ui_acceptance` 20/20。
- 两种配置均完成完整工程窗口链：实际保存、错误恢复、原草稿 token、折叠状态及后续 World 编辑。
- Dev 证据：`out/ui-reload-audit-20261005/dev/run-96088-393203796/acceptance.txt`。
- Release 证据：`out/ui-reload-audit-20261005/release/run-64152-393393390/acceptance.txt`。
- 两处目录均有 `ui-authoring.csv` 和 `ui-live-{updated,error,recovered}.png`，已检查恢复后的画面。
- 资源原始数据及统计：`out/ui-reload-audit-20261005/{dev,release}-reload.csv`、`profile-summary.json`。
- `E:/ffa_testproj` 的 Config/Content/Source 文件集合与 SHA-256 前后相同，清单位于证据目录。工作树中的作者文案修改保持原样。
- 用户的 Release 窗口有未保存场景并锁住输出。以同一 CMake 生成的链接规则另产出 `out/release/atelier_editor_audit.exe` 完成验收，未关闭该窗口；临时生成规则位于 `out/release/build.audit.ninja`，没有新增产品构建入口。

## 后续优化的实际边界

1. **后台编译是下一项有价值的结构工作。** 当前 Program 全部操作仍在 Runtime 线程。后台化需要不可变编译输入、同步结果队列、主线程准入，以及关闭时在途任务的收束；当前 prepare 读取活 owner 状态，不能直接放进线程池。LLVM target 初始化和 Windows 展开登记也须明确并发责任。
2. **普通帧的全量宿主采样仍存在。** 可进一步测量列表快照和字符串构造，但必须保持撤销、对象删除、项目切换及名称变化的权威性。本轮未添加没有通知合同支撑的“脏缓存”。
3. **当前复用粒度是组件生成代码。** 辅助函数文本变化会使包含它的组件重新编译；多文件依赖、函数级增量编译、活动帧或类型布局迁移尚未实现。
4. 描述提交后的布局/绘制失败沿用 Session 的原恢复合同；不承诺回滚已经结算的输入效果。正常可恢复错误与分配失败分开处理。
