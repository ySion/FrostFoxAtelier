# 编辑器跨模块审查与帧率策略

日期：2026-10-17。范围：主目录 `E:/FrostFoxAtelier`，HEAD `5e5d73c8` 加当前未提交改动。应用负责人统筹；布局负责人负责 DSL/UI/Layout，世界模型负责人负责 World/Runtime 提取链。本文记录审查和建议，不代表这些建议已经实现。

审查阶段没有运行 GPU、重新采集性能基线、修改产品代码或帧率默认值，也没有提交推送。已有 Dev/Release 集成编译通过；已有 CPU-only 对照与 World/viewport/materials 端到端证据继续有效，不能替代新 GPU 候选的图像和帧率验收。用户随后批准的修复与验证状态另行列在本文末尾。

## 结论与优先级

三路审查已完成，**未确认 P1 或本轮引入的对象保活/回滚寿命错误**。有四个应优先处理的确定 P2 问题，另有一处 P3 状态归属残留。这里的“未确认”不等于已验证所有极端组合。

| 顺序 | 发现 | 归属与性质 | 最小方向 |
| --- | --- | --- | --- |
| 1 | F1：可恢复渲染错误立即重复尝试 | Application，既有错误路径 | 同一宿主入口内有界重试，保留消息/Control处理 |
| 2 | W1：稀疏 World 按全部 Entity 分配空间表 | World，本轮优化引入的资源开销回归 | 第一个实际网格求值时才预留，参考呈现规模 |
| 3 | U2：全选后每行线性扫描选择集 | Editor，既有 O(N×S) 成本 | 同次 describe 一份选择 Set |
| 4 | U1：小结构变更全树二次方搬移 | UI/Layout，既有最坏复杂度 | 同一入口的最终拓扑/子序列批量提交 |
| 5 | U3：Workspace 保留浏览器外域展开 key | Editor，P3 生命周期归属残留 | 仅记录自有绑定域 |

U4 的 DSL 变化片段复用、U5 的真正范围化，以及 F2–F5 的帧率/唤醒策略需分别形成受边界约束的增量；当前证据不足以承诺任何新的 FPS。先消除确定浪费与错误路径，再按真实窗口评估大改动。

## 证据口径

- 地牢：3572 个 Entity，3381 个 Mesh instance，481 份几何。
- 已有独立 CPU 调用中位数：SceneSnapshot 3.984 → 3.062 ms；最大化 Workspace 1.749 → 0.232 ms；正常停靠 Workspace 11.578 → 7.671 ms。最后一项没有接受 UI 帧、UI prepare 或 Present，只能比较同条件调用，不能作为完整产品稳态帧耗时。
- 审查时同旧 1440p 接受相机的保守视锥统计：668 个可见实例，连续可见几何段 557；后续正式窗口与性能证据见本文末尾。
- 原始证据见 `out/pipeline-baseline-20261016/scene-cpu-{before,after}.json`；测量范围与本轮之前的优化见 [CPU 链路记录](editor-cpu-pipeline-20261016.md)。
- P1 表示应优先阻止的正确性/寿命问题；P2 表示明确性能浪费、条件性错误或需要完善的策略；P3 表示非阻塞的边界/维护问题。下文分别标注确定问题、设计缺口与待测影响，避免把推断写成复现结果。

## 当前帧率实际策略

| 情况 | 代码行为 | 依据 |
| --- | --- | --- |
| 主窗口可绘制 | 主循环持续调用 step，每次都推进服务、描述控件、prepare、提取可见视口场景并 render | `main.cpp:146`；`Application::step`，`application.cpp:1174–1369` |
| 主窗口 VSync | 产品默认 true；专用 render-performance 入口传 false | `application.hpp:26`；`application.cpp:72–80`；`main.cpp:109` |
| 静止/交互、前台/后台 | 没有独立调度分支；失活会取消交互，不改变呈现节奏 | `application.cpp:1060–1061`、`1174` |
| 最小化、隐藏或零客户区 | 先 poll_project、Control、GC，再等待消息或 20 ms 超时；不描述/绘制主 UI | `rhi.cpp:245`；`application.cpp:1196–1212` |
| 被其他窗口遮住 | drawable 只查可见标志、IsIconic 与客户区；没有一般遮挡策略 | `rhi.cpp:245`；gfx swap chain 为 FLIP_DISCARD |
| Renderer skipped | 已做过本次前段工作，再等待消息或 10 ms | `application.cpp:1365–1369` |
| 可恢复 Renderer 错误 | 更新诊断后直接返回 true，下一次立即重试 | `application.cpp:1317–1321` |
| 独立菜单弹窗 | VSync false；按 dirty 重绘；主 Scene 提交前先处理一次，接受主帧后再同步锚点 | `popup.cpp:27–31,155–179`；`application.cpp:1281,855–913` |
| Windows 原生模态框 | Show/MessageBox 在应用线程；操作系统处理模态交互，产品 step 暂停 | `project.cpp:12–42,44–66` |
| Edit/Play | Play 是子进程，编辑器继续处理自己的场景；子进程使用独立 host 循环 | `project_execution.cpp:67–85`；`project_runner.cpp:124–188` |

没有发现独立固定帧帽与 VSync 叠加的实现。gfx 的 `Present(vsync ? 1 : 0, ...)` 与 `waitFence(next_index)`分别承担呈现节奏和帧槽安全复用；二者耗时不能直接解释为重复限帧。相关实现见 `third_party/gfx/atelier.patch:870,932,972,998`。

### 计数含义

- HUD 每约 0.5 秒显示接受帧之间的墙钟平均频率；截图、skipped、不可绘制时清空采样。它不是 GPU 计时倒数，也不证明每个接受帧都被屏幕扫描显示。依据：`Application::update_viewport_fps`，`application.cpp:1164–1172,1337`。
- `FrameTiming` 仅为当前成功接受且启用采样的 step 记录，包含其局部对象析构；失败/跳过调用不形成同类 CPU row。性能验收的相邻接受步墙钟间隔仍包含中间等待、跳过调用和验收开销。依据：`application.hpp:17–22`、`application.cpp:1177–1188,1360`、`acceptance/editor/performance.inl:448–466,479,544–553`。
- Renderer/RHI 的子阶段嵌套在 Application render 时间内。Popup 的准备/提交可能落在事件处理或其他应用阶段，不能只看主 Surface 的 RHI 数值评价整个菜单响应。
- 独立 `Project Run` 的日志 frame 是循环步数，最小化也递增；这不等同于编辑器的 accepted_frames。依据：`project_runner.cpp:184–188`。

## Application 审查发现

### F1 · P2 · 持续的可恢复渲染错误会立即重复执行

**确定问题。** `Application::step` 在 `renderer_.render` 返回非 terminal 错误时只设置 status 并返回 true；`main` 随即再次执行完整前段。与 skipped 和 prepare 失败分支不同，这条路径没有等待或退避。编码开始之前的校验、资源或管线准备失败可能进入该分支；具体失败在现场的持续时长尚未实测。

**影响：** 未修正的输入或持续准备错误可能反复执行相同工作，消耗 CPU，并使其他桌面任务与控制请求响应变差。终止性设备错误已经退出，不应混为一谈。

**最小修复：** 在同一 step 调度边界为可恢复错误记录原因和重试截止时间；输入/文档/目标变化可以触发提前重试，消息与 Control 服务仍推进。不要在 Renderer 内新增 sleep，也不要把可恢复错误直接改成终止错误。用现有真实错误/恢复链验证重试有界、更新后恢复和关闭可达。

**依据：** `src/application/application.cpp:1317–1321`；`src/renderer/renderer.cpp:1066–1079,1130–1141`。

### F2 · P2 · 静止与后台窗口仍执行完整帧链

**确定设计缺口。** `step` 没有活跃、空闲和前后台节奏分支，主窗口也没有把 `Reply.redraw` 用作帧准入条件。因此静止 Smooth/MatCap、无视口的静态页面，以及后台但仍标记可见的编辑器都会继续构建和提交帧。

**影响：** 不必要的 CPU/GPU 工作会与 Play 子进程或其他桌面应用竞争。实际资源占用与改善幅度需要实机对照；降低后台频率本身不降低前台单帧成本。

**最小修复方向：** 先在 Application 统一管理可绘制性、应用窗口组活跃状态和下一次服务/渲染时间；保留持续执行需求。完整事件驱动停帧要等 UI 定时需求、Control/热更唤醒和 Renderer 持续采样需求都明确后再接入。

**依据：** `src/application/application.cpp:1196–1214,1233–1239,1295–1315`；`src/ui/session.hpp:296–302`。

### F3 · P2 · 不能依靠 DXGI_STATUS_OCCLUDED 完成遮挡节流

**确定平台边界。** 当前交换链使用 `DXGI_SWAP_EFFECT_FLIP_DISCARD`，RHI 把 Present 的 `DXGI_STATUS_OCCLUDED` 转为 `FrameInfo.occluded`，Renderer 据此决定 accepted。微软文档明确说明 flip model 不返回该普通遮挡状态。因此这个标志不能证明应用已经识别“窗口被盖住”。

**最小修复方向：** 把最小化/隐藏、前后台、系统可见性提示和真正可绘制性分开；优先处理可靠的可观测状态，不引入逐窗口计算遮挡区域的复杂系统。窗口被覆盖时的实际 DWM/Present 行为仍需实机核对。

**依据：** `src/rhi/rhi.cpp:245,267`、`src/renderer/renderer.cpp:1121`；锁定依赖构建源码 `gfx.cpp:1107`；[Microsoft DXGI_STATUS](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-status)。

### F4 · P2 · 等待和模态期间缺少独立的宿主服务机会

**确定现状，响应延迟待测。** Present 与 GPU fence 等待在 UI 线程同步发生；fence 使用普通 `WaitForSingleObject`，默认故障等待上限 10 秒。原生模态 Show/MessageBox 也暂停产品 step。菜单虽按 dirty 提前提交，后续输入仍受主线程等待约束。

**最小修复方向：** 后续若引入帧就绪等待，把可消息唤醒的准入点放到帧编码之前的安全 host 边界。不得在持有编码状态、UI 发布或 World 事务中随意 Dispatch 任意业务消息。模态期间需要明确允许的取消/只读状态与 busy 响应，不能为了 Control 响应而重入文档修改。

**依据：** `third_party/gfx/atelier.patch:19,870–889,972–998`；`src/application/project.cpp:37,52,61`；[Microsoft Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)。

### F5 · P2 · 当前轮询依赖必须先补齐，才能采用长时间空闲等待

**确定契约约束，不是当前队列已经饿死。** Control worker 入队后不向 UI 线程发送唤醒，主线程每次 step 只取一个请求；当前依靠持续帧或 20 ms 轮询处理。UI DSL 100 ms、源码/工程 500 ms 的检查也由 host 调用触发。只把主循环改成无限等待，将漏掉这些工作，以及悬浮说明的定时出现。

**最小信号：** 待处理 Control/后台结果的就绪通知；源码/DSL/UI 延迟效果的最近截止时间；宿主当前是否需要继续推进。通知只能安排安全阶段的处理，不直接在线程回调执行引擎命令。通知句柄的拥有者要晚于停止等待者销毁。

**依据：** `src/control/control.cpp:259–267,290–307`；`src/application/editor_control.cpp:817`；`src/ui_language/live_document.cpp:20–27`；`src/editor/source_editor.cpp:89`；`src/editor/project_code.cpp:157–158`；`src/ui/session_tooltip.inl:64–73`。当前 `MsgWaitForMultipleObjectsEx` 传零个句柄，只等消息或超时；接口语义见 [Microsoft MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex)。

## UI / DSL / Layout 审查

负责人：**负责人 | 布局引擎开发**。采用 Standards（结构、所有权、成本）与 Spec（既有行为与更新合同）两路核验。未确认 P1 或新的候选/回滚生命周期错误；以下是当前生产链问题或能力边界，不能笼统称为本轮 WIP 新回归。

### U1 · P2 · 小结构变更触发全树最坏二次方搬移

**确定复杂度问题，毫秒影响待测。** `session_update.inl:126/135/291` 标记结构变化后，`Session::Impl::synchronize` 把全部保留节点先移到 root，再逐一移回。`LayoutDocument::move` 对 children vector 执行线性查找、删除和插入。N 行树中增删少量节点，最坏可产生 Θ(N²) 搬移。

**最小完整修复：** 保留 synchronize 入口，由 LayoutDocument 承担最终父关系/各父子序列的批量校验、预留和拓扑提交。只先脱离会被退役祖先带走的保留子树根；同父重排一次提交子序列。不能仅对 changed parent 逐一 move：最终合法的祖先反转可能在中间步骤临时成环。计划失败不部分提交，dirty/retired 可重试；已经成功的 Control 提交与旧 accepted frame 的保留合同不变。

**E2E 缺口：** 大树单行增删的操作量、保留子树跨父、退役祖先下迁出、合法祖先反转、同父重排，以及后续测量/布局失败保留旧帧再重试；延续滚动、折叠和 DPI 覆盖。

**依据：** [Session synchronize](E:/FrostFoxAtelier/src/ui/session.cpp:393)、[LayoutDocument move](E:/FrostFoxAtelier/src/ui_layout/layout.cpp:1065)。

### U2 · P2 · 多选投影按行重复扫描选择集合

**确定复杂度问题。** Scene Tree 每行调用 `Session::is_selected`，后者线性扫描 selected_entities。N 行、S 个选择的成本为 O(N×S)；3572 项全选约需 638 万次身份比较，每次 describe 都重复，包括业务值未变时。

**最小修复：** 每次 describe 从当前 selected_entities 建一次已有 foundation Set，所有行共享查询。保留当前身份及选择/撤销链，无跨帧 selection 缓存。现有选择验收覆盖结果和历史，需补真实大集合连续等值投影的工作量观察。

**依据：** [Tree 行投影](E:/FrostFoxAtelier/src/editor/workspace.cpp:670)、[is_selected](E:/FrostFoxAtelier/src/editor/session.hpp:44)。

### U3 · P3 · Workspace 收集了其他视图的展开状态

**确定的状态归属残留。** Workspace 无筛选接收全部 accepted tree_rows，却只随自身业务 Binding 清退；Asset Browser 的目录展开另有自己的 map 和清退链。目录反复增删后，Workspace 的外域 key/bool 可能累积到 reset。这里不保活文字、GPU 或业务对象，不能据此解释当前主要耗时。

**最小修复：** 写入时限定自身业务绑定域和 tree_world，浏览器继续管理自己的状态。用现有目录增删流程验证自有展开状态保持、外域记录不累积。

**依据：** [展开状态写入与清退](E:/FrostFoxAtelier/src/editor/workspace.cpp:445)、[目录状态拥有者](E:/FrostFoxAtelier/src/editor/asset_browser.cpp:80)。

### U4 · P2 优化项 · Repeat 单项变化仍复制和验证全部保留片段

**确定路径。** Repeat 任一 item 变化后，全部保留 child 都复制别名 keys、prepare；无变化 child 也复制完整 Control 片段，父级再逐项 validate/equal，嵌套组合放大成本。稳定输入已有快退，不能把单项变化路径说成每帧递归 build。

**最小方向：** 扩展现有 PreparedUpdate 的片段表示，让未变 child 引用已发布片段，变化区间才物化/验证，沿原提交与拒绝链。既有 composed_ui、ui_reload_transaction 覆盖身份/重排/失败恢复，缺少大 Repeat 小改动的复制和验证工作量覆盖。

**依据：** [稳定快退和 Repeat 更新](E:/FrostFoxAtelier/src/ui_language/component.cpp:711)，关键循环在 728–734、756–801。

### U5 · P2 方案项 · 当前树/列表裁剪不是前段虚拟化

**确定能力边界。** visible 检查之前已经同步完整 order、计算内容范围/滚动，并生成全部已排列行的 ControlBox/tree_rows；draws 也按全量预留。宽度/字体失效后，离屏但参与布局的文字仍可能测量。独立 VirtualList 尚未接入该生产链，现有 UI 验收仍要求保留 257 个框，只限制栅格数。

真正范围化需要同时迁移依赖全量接受帧行的键盘、范围选择、过滤/展开、scroll extent、anchor 和 identity 合同。建议复用已有 VirtualList 范围原语及同一 Session，不另建输入后端；这需要先审查方案，不能顺手删除离屏框。

**依据：** [Session 布局与帧生成](E:/FrostFoxAtelier/src/ui/session.cpp:399)，相关循环 516–561、643–685；[当前验收合同](E:/FrostFoxAtelier/acceptance/ui/main.cpp:277)。

### 已保留的机制与调度边界

- `LayoutDocument::compute`（`layout.cpp:1119–1129`）对静态输入复用几何；文字度量、Coverage 与旧接受帧文字快照已有缓存/不可变持有。
- Asset Browser `table_data`（`asset_browser.cpp:155–171`）按 dirty 复用模型，List/Details/Tiles 共用原生 Table；并非每帧复制完整资源表。
- Scene Tree 仍每次新建 ItemList/行输入，等值 ItemList 深比较，LiveDocument::append 再复制 Control、Session::set_controls 全值比较。后续可先比较本次真实业务值并保留等值 ItemList，或沿现有 update_controls 接变化片段；不能用 Session revision 代替 Runtime 可变内容的检查。
- Session 当前没有 next_deadline/needs_frame/tick。Tooltip 记录 hover 时间后，在 prepare 检查 500 ms；不继续 prepare 就不会主动出现。Caret 当前固定绘制，没有已实现的闪烁定时需求。
- LiveDocument 的 100 ms 检查/200 ms 稳定窗口仅在 describe 调用，成功源码 commit 才增加 revision；诊断错误与普通输入变化不增加该 revision。未来调度必须拿到现有拥有者的期限/变化状态，poll 到期也不等于必然重绘。

## World / Runtime 提取链审查

负责人：**负责人 | 游戏世界与对象模型**。覆盖成员与类型目录、空间求值、几何/灯光/材质快照、引用与 GC，独立复核当前容器复用和 UI 身份查询；未修改或重新构建代码。

### W1 · P2 · 稀疏 World 的空间表预分配回归

**本轮优化引入、源码可确认的资源开销。** 修复前，每次 `World::render_items()` 在确定是否有实际网格之前，就按全部 Entity 数量 reserve 两张 SpaceGraph 表；调用结束释放。大量逻辑 Entity、零或很少网格的场景也承担这两份分配。

**源码已修复，Dev/Release 构建及验收通过：** 到第一个实际网格通过 owner 校验、需要空间求值时才建立 SpaceGraph；两张表按 `min(成员数, 呈现目录大小)` 预留，祖先超出容量时正常扩容。结果数组同样在首个网格处预留，纯灯光场景也不为网格结果分配容量。调用结束销毁缓存，后续提取继续读取 Runtime 当前数据；没有新增目录或跨帧缓存。

**真实端到端增量：** [sparse_presentation](../../acceptance/world/main.cpp) 使用正式 Runtime、World、网格和灯光，复用项目的 mimalloc 统计。空 World、纯逻辑、纯灯光及移除全部网格后的结果容量必须为零，后三者的总分配必须严格等于同配置的真实空 World 基线。两个网格共享八级空间祖先，再加入 256 个无关逻辑 Entity 后总分配必须不变；同时覆盖目录顺序、容量增长、直接 Runtime TRS 写入、无穷值拒绝及恢复。稀疏提取的分配必须高于空 World 基线，以确认计数能观察到真实工作。验收不依赖耗时阈值或 mock，World 验收目标增加 mimalloc 私有依赖。

**Debug 统计边界修正：** MSVC 的 Debug 迭代器检查为空 `std::vector` 的构造和移动分配代理，返回值容量为零也可能有总分配。采样仅包围正式 `World::render_items()` 调用，验收的 `take` 和结果搬运在采样窗口之外；正式 API 返回容器的固定管理开销以同配置空 World 为基线，日志输出各场景的实际总字节量。保持迭代器检查和严格的相等比较，不宣称所有配置下总分配都为零。此修正只涉及验收统计与断言，生产语义保持不变。

**实际结果：** 2026-10-17 两配置均为 `World acceptance: PASS (16/16)`、exit 0。Dev 空／纯逻辑／纯灯光／清空网格均为 48 bytes、结果容量 0；Release 对应总分配均为 0 bytes。增加 256 个纯逻辑 Entity 前后，稀疏提取在 Dev 均为 8592 bytes、Release 均为 8064 bytes。日志位于 `out/world-w1-20261017/{dev,release}-world.log`；原始 Debug 断言失败保留在 `dev-before.log`。这些分配结果不代表已经测得耗时或 FPS 改善，既有 CPU 对照仍按其历史版本解释。

**实现：** [World::render_items](../../src/world/world.cpp:484)。

### 已确认保持的边界

- SpaceGraph 的节点/矩阵和复用遍历容器限定在单次求值上下文；失败后后续求值会清理临时遍历状态，不依赖 Session revision 隐藏 Runtime TRS 写入。[遍历入口](E:/FrostFoxAtelier/src/world/hierarchy.cpp:124)
- World 类型目录包含禁用组件，UI 用 ObjectId 集合替代临时 Root 不改变原 World/Runtime 的拥有关系；查询不成为对象存活的权威来源。[当前 UI 查询](E:/FrostFoxAtelier/src/editor/workspace.cpp:427)
- 材质引用访问仍位于受保护借用内；同帧 overlay 在 SceneSnapshot 成功刷新后消费几何成员。未发现本轮新增的悬空借用或跨 Runtime 缓存。
- 保留重挂接、移除/撤销、空间循环/跨 World 拒绝、非法变换恢复与独立 Runtime 边界。现有验收不代表所有组合已覆盖，仍建议补组件禁用后删除/撤销、材质改绑、切换 Runtime 后旧 UI Binding 退休的组合端到端场景。

### Play 与后台节奏的关键约束

`PhysicsScene::update` 先调用 `World::update(elapsed)`，随后才以 60 Hz 累积执行物理步，每次最多 8 步；超过上限的时间记录为 dropped。World 的 Tick 和协程 resume 仍每宿主调用推进一次，固定物理步不代表整个游戏逻辑已固定步进。

因此，降低整个 Project Run 循环频率会改变 Tick/协程推进频率；宿主已有 dt≤0.1 秒截断还可能先丢掉时间。后台渲染预算必须与仿真服务分开决定，不能把“物理有累积器”当成任意降低宿主频率都正确的依据。

**依据：** [PhysicsScene::update](E:/FrostFoxAtelier/src/physics/physics.cpp:726)，固定步/上限/丢弃在 750–756；[World::update](E:/FrostFoxAtelier/src/world/execution.cpp:316)，Tick 与 coroutine 在 328–342；[Project Run dt](E:/FrostFoxAtelier/src/application/project_runner.cpp:146)。

## 建议的帧率策略与职责

保留唯一 `Application::step` 宿主入口，协调三件事：推进必要服务、判断渲染需求、在允许的节奏提交。RHI 提供帧槽/呈现事实，Renderer 提供持续采样需求；两者都不另建限帧循环。

| 条件 | 建议行为与切换条件 |
| --- | --- |
| 前台输入、拖动、相机移动 | 消息立即处理，下一次可用帧提交。目标频率遵循用户刷新率/帧帽设置；不为每个子窗口另加 VSync 等待 |
| 前台静止但存在渐进光照、反射、AA 或动画 | 按明确持续需求继续采样；没有编辑事件不代表已收敛，不猜测固定 N 帧后停止 |
| 前台静态内容且没有持续需求 | 由可见内容变化或最近截止时间触发整帧；先省去完整无效帧，不先增加第二套区域渲染路径 |
| 后台但可见 | 保留命令/文件/任务响应，对有需求的呈现采用可配置的较低预算；重新活跃或交互后立即恢复。具体默认值另行实测定稿 |
| 最小化、隐藏等不可绘制状态 | 停止 GPU 提交，按消息/任务/检查截止时间推进服务；恢复时重建接受帧和输入几何 |
| 原生模态对话框 | 保持模态所有权；明确可响应的状态/取消范围。避免重入业务事务 |
| Play 子进程 | 仿真推进策略与渲染预算分开；父编辑器后台节流不控制子进程的游戏时间 |
| 性能验收 | 继续明确关闭人为帧帽/VSync，固定场景/相机/viewport，分别记录接受步墙钟、各 CPU 阶段和完成 GPU 样本 |

当前 Play 使用真实 dt 上限 0.1 秒，headless 有限步验收使用固定 1/60；这不是一个已独立于渲染的通用仿真调度器。改变渲染频率前必须核对 World/Physics 的累积与截断合同，不能让后台降帧无意改变游戏时间。

渲染时间继续沿 `RenderClock`：已知暂停后建立新基线，活跃渲染间隔上限 0.1 秒；它不与 Play 仿真时间绑定。正常等待不制造 camera_cut，不重置所有 GI 历史。依据：`src/application/render_clock.hpp:8–24`、`application.cpp:1212,1311–1315,1330`。

## 修复实施顺序建议

1. 先关闭可恢复错误的紧循环，以及负责人审查确认的正确性缺口。
2. 优先修复已有路径中的确定重复工作和结构复杂度；用必要的真实端到端场景验证身份、生命周期和失败回滚。
3. 明确服务就绪、UI 最近截止时间、持续渲染需求这几个最小信号，再讨论前后台/空闲帧率设置。默认值目前保持不变。
4. 空闲实机时验证前后台切换、最小化恢复、原生弹窗、菜单、控制请求与热更，以及 Scene Lit/AA 收敛和 Play 时间。端到端画面/输入正确后再比较 FPS、尾延迟与桌面公平性。

## 批准后的实施状态

- **F1 Dev/Release 编译及 Release 真实窗口验收通过：** 可恢复渲染错误按 20/40/80/160/250 ms 退避，最大 250 ms；主线程先服务消息、Project/Control 和 GC，再判断是否执行控件描述/场景提取/渲染。正常接受帧或新文档清除退避，终止错误仍退出。Release surfaces 已实际通过真实 Renderer 参数拒绝、Control 查询、自然到期恢复和原生关闭链。恢复检查从一次真实拒绝产生的 deadline 开始，恢复输入后不再改动 deadline。Dev 窗口用例本次未运行，Release 证据见本文末尾。
- **U2 Dev/Release CPU 文档验收通过：** 每次可见 Scene Tree describe 构建一份当前选择 Set，避免逐行扫描集合；选择/清空/再选择的真实 UI 投影检查通过。
- **U3 Dev/Release CPU 文档验收通过：** Workspace 仅保存自己的 object/structure Binding 和 tree_world 的展开状态；真实 Asset Browser/Scene Tree 接受帧的域隔离检查通过。验收访问器仅用于观察内部拥有状态，没有增加产品调试入口或替代渲染/输入路径。
- **W1 Dev/Release World 16/16 通过：** 空／纯逻辑／纯灯光／清空网格的结果容量为零；同配置总分配等于固定返回容器基线；增加无关逻辑成员没有增加稀疏提取分配。统计边界、实际数字和日志详见 W1 段落。
- **U1 Dev/Release CPU 验收通过：** LayoutDocument 沿同一节点池提供最终拓扑批量提交，Session 原 synchronize 调用，替换逐项拆卸和重新挂接。准备验证与容量后才提交最终关系，失败保留旧输入/发布；保留节点身份与草稿、换代、祖先反转、失败恢复及大量兄弟重排均已覆盖。布局 46/46、生产 UI 27/27、最小消费者通过，详见[完整树批次验收](../modules/ui-layout/acceptance.md#完整树批次增量)。没有扩展 U4/U5 或另建 Session 发布入口。

同批补充的 `direct_shadow_cache` 仅为视口诊断，默认 false；Control/view/performance configuration 复用统一 ViewOption 元数据，Dev/Release CPU 文档验收已通过独立开关、幂等和 Scene/Undo 隔离检查。Renderer 后端、shader 和 GPU 验收由渲染负责人负责；这里不更改 `direct_clustered` 默认值。

U2/U3 与诊断开关的实际日志为 `out/foundation-review-20261017/{dev,release}-viewport-edit.log`，同次 Entity composition 场景通过；材质及颜色事务的回归结果为 `{dev,release}-materials.log`，均 exit 0。这些是 CPU 端到端结果；随后完成的真实窗口结果单独记录在下文，Direct/DDGI 图像与性能专项见[Renderer 验收](../modules/renderer/acceptance.md)。

## 本轮交付证据

主目录 `E:/FrostFoxAtelier` 的 Dev/Release 均已构建 Editor、Renderer 验收、编辑器文档验收、World、生产 UI、布局及最小消费者、Control 验收程序。构建日志为 `out/foundation-review-20261017/dev-build.log` 与 `release-build-final.log`；Renderer 最后的验收用例修订单独通过两配置增量编译，日志为 `out/render-cost-20261017/{dev,release}-final-build.log`。World 统计修订的最终编译日志在 `out/world-w1-20261017/{dev,release}-build.log`。

CPU 集成阶段的产物和源码 SHA-256 清单保存在 `out/foundation-review-20261017/evidence.json`。本轮未提交或推送；证据对应主目录的工作区候选，不仅是已有 HEAD。随后窗口验收继续使用同一 Release Editor；Renderer 验收程序的后续修订及 GPU 图像/性能专项以其[验收记录](../modules/renderer/acceptance.md)为准。U4/U5 和空闲帧调度仍未纳入本批实现。

修复后的只读复审未留下已确认的阻断项。F1 自然到期恢复检查已在 Release 真实窗口通过；World 统计修订经独立复核确认仍能识别按全部成员预留的原回归。结果只覆盖实际执行的场景，不扩展为全部极端组合或全部 GPU 行为已经验证。

## Release 窗口验收与完整布局实测

2026-10-17，直接运行已冻结的 Release Editor `--acceptance-surfaces`，没有重建产品。证据目录为 `out/render-cost-20261017/gpu-acceptance/editor-surfaces-release/`：`run.json` 保存完整二进制 SHA-256、起止时间及 exit 0；`stdout.log` 记录完整 surfaces 与内嵌 performance acceptance PASS，`stderr.log` 为空。`surfaces.csv` 的 `render-retry,validation-control-recovery-close,passed` 确认 F1 已执行；拖动期间 RT 改尺寸为 0/8 帧，暖菜单冗余呈现为 0/5 次。验收进程已退出，没有绕过前台保护。

完整布局的只读分析使用渲染负责人已有的地牢报告 `out/pipeline-baseline-20261016/cost-final-20261017-01/{render-performance,performance-summary}.json`。Release、关闭 VSync，每个非 Lit case 采样 240 个接受步；以下均为 CPU 墙钟均值。

| 模式／布局 | 接受步频率 | Application step | controls | UI prepare |
| --- | ---: | ---: | ---: | ---: |
| Smooth 最大化 | 160.65/s | 6.221 ms | 0.554 ms | 0.080 ms |
| Smooth 完整 Dock | 56.81/s | 17.599 ms | 7.862 ms | 3.072 ms |
| MatCap 最大化 | 148.77/s | 6.718 ms | 0.587 ms | 0.087 ms |
| MatCap 完整 Dock | 54.09/s | 18.483 ms | 8.249 ms | 3.270 ms |

完整 Dock 比最大化增加 11.378/11.765 ms；`controls + ui_prepare` 分别解释其中 90.53%/92.18%，占 Dock 总 step 约 62%。`controls` 包含 Workspace 描述、`set_controls` 和 effects，`ui_prepare` 是整个 `Session::prepare`，不能分别当作纯 DSL 或纯布局求解耗时。Scene Tree 仍每次产生全成员 ItemList、深比较等值输入，append 复制 Control，Session 比较完整描述；prepare 仍生成完整排列行和接受帧几何。U1 的结构批量提交没有消除稳定帧的这些工作。U4 仅在 Repeat 输入变化后触发，稳定输入已有快退；此报告不足以把毫秒归因到某个单一循环，也不证明每帧都重新编译 DSL 或完整求解布局。

两种布局的 receiver 都是 2560×1440，均为 3381 instances、2713 culled、557 draws。Snapshot 仅增加 0.220/0.116 ms，render/present 整段仅增加 0.430/0.393 ms，其中 encode 增加 0.367/0.355 ms，父子时间不能重复相加。GC 不超过 0.004 ms、帧槽等待约 0.0014 ms、resize 为 0、Present 约 0.10 ms；本次数据不支持它们作为 Dock 差值的主因。Dock 完整 target 为 4315×2319，最大化为 2668×1730，前者像素数约 2.17 倍；这不是同完整窗口分辨率比较。Smooth/MatCap 没有 GPU 分段计时，不能据此声称 GPU 成本为零。

剩余热点优先沿现有入口拆分测量面板投影、DSL prepare/append、`set_controls` 与 UI 帧构造，再减少已证实的全量复制和比较。等值 ItemList／片段复用与稀疏更新必须保留直接 Runtime 写入、选择、展开、过滤、热更及 DPI 失效合同；不能仅依赖 Document revision。生产行虚拟化仍须完整维护键盘、范围选择、滚动与身份合同。本次只分析和更新证据，没有实施这些后续优化。
