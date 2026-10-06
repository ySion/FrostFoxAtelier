# 编辑器工程工作流与对象检查审查

日期：2026-10-06。范围：`21abaa0a0b5ce1f5720748a90d2391a7a9bfca97` 到当前工作树，含本波次新增文件。需求依据为本轮用户指令，以及当前工程、源码编辑和 UI DSL 合同。未提交改动属于同一候选，不以旧版本通过记录代替本轮验收。

## Standards：结构与生命周期

| 发现 | 等级 | 处理 |
| --- | --- | --- |
| UI 描述已接受就发布 Source，真实布局仍可能失败；项目运行器可能退出，编辑器可能持续保留错误定义 | P1 | 项目 UI 由 `ProjectUI::prepare` 持有完整准备/恢复过程；主编辑器也在实际布局成功后提交。零 Surface 暂停发布，失败恢复旧定义 |
| 诊断跳转核对磁盘，却可能定位到早已打开的旧干净缓冲 | P2 | 先核对编译快照与磁盘，再刷新干净缓冲；脏缓冲和新磁盘版本拒绝过期定位；重复点击复用文档 |
| ProjectCode 重复检查 Source 已负责的生命周期函数签名 | 结构判断 | 删除重复签名规则；Source 负责语言合同，ProjectCode 保留 World 绑定时机检查 |

本轴原始发现 3 项，最高 P1；修复后的实际验收见下文。规则依据：项目要求所有权与生命周期清晰、替换后删除旧路径，并保持单一语义入口。重叠项仍保留在各自审查轴，不相加为独立缺陷数。

## Spec：用户可见行为与边界

| 发现 | 等级 | 处理 |
| --- | --- | --- |
| 错误 UI 保存不能可靠保留最后可用页面 | P1 | 将真实布局纳入候选事务，覆盖运行与预览两条消费者 |
| 跨文件错误跳转可能显示旧源码 | P2 | 版本核对及缓冲刷新，与上表同一修复 |
| 枚举通过任意整数编辑，产生 Scene 无法序列化的值 | P2 | 使用元数据命名选项和 radio 菜单；Session 拒绝未声明值，一次性属性操作失败回滚 |
| 仅剩 Redo 历史时 Rebuild 静默清空历史 | P2 | 重建确认同时检查 Undo 和 Redo |

本轴原始发现 4 项，最高 P1。其他本轮设计修订如下。

## 对象检查与子页分工

- 主窗体由菜单栏、工具架、中间停靠/Tab 内容区和底部状态栏组成。业务能力通过子页加入。
- Scene Tree 只显示 Actor。Actor Structure 显示 Actor 对组件的归属关系，提供组件选择与增删，标识空间根；不混入空间换父或资产模板语义。
- Parameters 展示实例的连续属性区。Actor 自身字段和所拥有组件的字段均可见；选择组件后只检查该组件。移除固定 Transform／Components 页签和 Parameters 内组成动作。
- 选择记录检查对象与所属 Actor。组件选择不丢失视口上下文，字段撤销恢复具体对象，组件删除回退拥有者；同一对象重复选择不改变草稿。
- 数值、bool、枚举复用同一参数行与现有输入链。没有声明默认值时不以零冒充默认值。

已删除旧页签键、页选择状态、旧 ComponentRow/remove 绑定和任意枚举数字路径。当前 DSL 文档从过时的 `declarative-ui-proposal.md` 名称改为 [declarative-ui.md](../modules/editor/declarative-ui.md)，所有仓库文档链接同步；历史研究保留为带日期的证据，不作为当前 UI 规格。误写到仓库根目录的验收产物移入 `out/`。

## Class 的含义与尚未提供的能力

目前有原生和 Source 类定义、运行时实例、字段声明默认值，以及按类型名与实例值保存的 Scene。还没有持久 Class 资产、组件模板、显式实例覆盖集或模板默认值编辑器；本轮没有把这些未实现能力包装进 Parameters。

UE 的 UClass 包含类默认对象及生成来源，Blueprint 的 Class Defaults 是对类型默认值的作者操作；实例值与类默认对象并非同一对象。参考 [UClass 官方 API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UClass) 和 [Blueprint Class Defaults](https://dev.epicgames.com/documentation/en-us/unreal-engine/blueprint-editor-defaults-tab)。后续类资产建设需要先明确持久身份、组成模板、覆盖与传播规则，再接入当前检查入口。

## Houdini 实机与官方资料比对

在用户授权的 Houdini 22.0.429 窗口中，选择 Geometry 容器时，Parameters 有 Transform／Render／Misc；选择内部 Box SOP 后，变为 Primitive Type、Connectivity、Size、Center、Rotate 等连续属性，没有固定的 Transform／Components 页签。因此借鉴目标驱动的参数组织和独立子页，不照搬 SOP 网络为 Actor 组件关系。

官方说明同样区分窗格/标签、节点实例参数和类型参数界面：[Panes](https://www.sidefx.com/docs/houdini/basics/panes.html)、[Parameters](https://www.sidefx.com/docs/houdini/network/parms.html)、[Edit Parameter Interface](https://www.sidefx.com/docs/houdini/ref/windows/edit_parameter_interface.html)。参数页可以由目标定义，但不能假定所有 Actor 有相同页面。

## 本轮端到端验收

| 验收 | Dev | Release |
| --- | --- | --- |
| 编辑器与受影响端到端目标编译 | PASS | PASS |
| 共享 UI | 23/23 PASS | 23/23 PASS |
| Source → 实例属性 → 场景 → 热更 → 独立运行，包括 UI 布局拒绝/恢复 | PASS | PASS |
| 场景类型、生命周期、字段历史、重复启停、Jolt 所有权 | PASS | PASS |
| 完整编辑器文档、对象检查、枚举及诊断快照 | PASS | PASS |

日志：`out/review-{dev,release}-{ui,integration,scenes,documents}.log`。对象检查使用真实 Project/Source 类型、共享 UI 命中、Session 和 Scene 往返；UI 更新使用真实文件和运行进程。26 份修改文档的本地链接无缺失目标。

发布版完整原生窗口验收通过，报告和截图在 `out/review-window-release-complete/`，覆盖菜单、停靠、字段编辑、组件增删/撤销、工程保存与重开。

用户要求继续验收后，已完成上次中止的两个专项；执行前重新确认 Dev／Release 编辑器构建为当前工作树：

| 本次窗口专项 | 结果 | 证据目录 |
| --- | --- | --- |
| Release 工程与主编辑器 DSL 保存热更 | PASS | `out/acceptance-resume-ui-release/run-83284-497998734/` |
| Dev Code 与工程 Source 工作流 | PASS，495 个接受帧 | `out/acceptance-resume-source-dev/` |

保存热更专项验证语法与真实布局错误不发布候选、旧查询输入身份及折叠状态保留、修复后恢复更新、活动字段草稿保留、最小化期间不发布及恢复后单次发布。`ui-authoring.csv` 中布局失败保持 revision 3，修复发布 revision 4；最小化恢复发布 revision 6。资源浏览器同步覆盖布局和绑定错误、Tiles 布局变更及恢复。

源码窗口专项覆盖真实菜单、光标与选区、Ctrl+S/JIT 发布、错误保存与诊断点击、修正、大段剪贴板和撤销；随后验证 Actor Structure 添加 Source 组件、数值/bool 属性、场景保存重开、跨文件诊断、独立游戏运行和 UI 预览以及关闭。本次没有重跑系统 IME；此前的用户中止记录不计入通过结果。

窗口复验还捕获并修复了枚举菜单路由拦截数值字段右键菜单的问题：枚举分支限定于 Combo，数值框继续走既有复制、粘贴和默认值菜单。窗口菜单边界用例改为选取最右侧字段，并留出明确的显示器工作区余量；不再依赖旧参数页布局。
