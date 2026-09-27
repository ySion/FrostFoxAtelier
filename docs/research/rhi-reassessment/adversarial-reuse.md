# 成熟 RHI 复用路线的独立对抗评估

## 结论与边界

**本轮否定的是“选一个成熟库，薄适配自然就足够”的无条件充分性，不是否定复用。** 已有证据确实支持复用能够省去大量后端实现；但候选对录制、绑定、状态、提交、回收和故障的责任切分不同。项目若接受其中一套语义，窄适配完全可能成立；若要求包装层隐藏这些差异，却同时保留任意录制取消、任意线程提交、可变绑定原地覆盖、稳定原生资源身份或另一套状态权威，包装就可能承担它原本想省掉的机制。

这不是候选库缺陷清单，也不是实现验收结果。下文严格区分：**已确认的契约边界、由边界推导的条件性冲突、尚未核验的疑虑**。尚无真实消费者、硬件范围和成本门槛，不能据此排出统一赢家。Windows x64 背景不决定必须使用某个后端。

已只读核对 `research-brief.md`、`evaluation-framework.md` 及六份证据报告；没有修改文件，没有构建、运行测试、GPU 或故障实验，没有派生代理。原始来源定位沿用报告中已核对的章节、符号和版本；本评估没有把滚动来源重新认证成固定快照，也没有把资料缺口写成能力不存在。

## 一、先公平陈述复用最强的成立理由

### 1. 库确实接管了必须有人实现的职责

- **NVRHI** 不只是后端函数改名：普通受追踪绑定路径提供录制实例保活、提交后退休、上传子分配、状态追踪和跨队列等待入口。由它持有 GPU 使用期引用，可以使引擎业务句柄先失效而不提前销毁 GPU 资源。见 `nvrhi.md` §3、§4.1、§4.3、§5、§6.3。
- **DiligentCore** 可独立于完整引擎家族使用，已实现原生对象的 fence 延迟释放、动态页和描述符管理、PSO/signature/SRB、context 与可选 swapchain。它不要求业务对象迁入另一个场景体系。见 `diligent.md` §3、§4、§6、§7。
- **bgfx、SDL GPU、wgpu** 分别通过帧/view 调度、应用级命令与资源版本机制、安全验证与状态跟踪，替调用者做了不同范围的工作。接受相应产品语义可以减少项目自行维护的部分；这些语义不应先被叫作“多余开销”。见 `portable-libraries.md` §2、§3.1–3.3。
- 原生基线明确说明 D3D12 录制不替应用持有所引用对象，allocator、动态存储和各种描述符也并非同一复用期限。**不用库不会让这些责任消失。** 见 `native-baseline.md`“已核对事实”“设计推断：自研实际要承担什么”。

### 2. “薄”应按责任切分，而不是按包装对象数量判断

窄适配可以合理包含业务 GPU 句柄、明确的 owned/borrowed 转换、提交前引用集合、完成 token、错误入口及维护调用。**Diligent 的提交前保活集合，或 NVRHI 的显式 GC 驱动，本身不等于又造一套 RHI。** GPU binding 投影也不等于复制一套业务对象世界。

危险信号是：库和引擎分别维护同一资源的权威状态；双方都隐式判断某块存储何时可覆盖；上层用固定帧数替代库的完成凭证；为模拟不相容语义而重放、重排或旁路大部分生产调用。此时“薄”应重新证明，而不是靠封装类名字维持。

独立 shader 工具、应用创建设备或自行呈现，均是合法职责切分，**不是库不合格的证据**。反过来，后端数量、成熟项目采用和功能列表也不是正确性或性能证明。工业实现展示的是不同的责任分层，不支持预先把完整 render graph、专用 RHI 线程或通用别名分配器列为所有候选必需项。见 `industrial-engines.md` §3、§5；`papers-and-talks.md` §3/S2–S5、§5。

## 二、六个核心异议

### 异议 1：录制保活与取消不能由统一的 Release/Cancel 名字推导

**被挑战主张：** 库有引用计数和延迟销毁，业务引用释放、未提交录制丢弃和正常提交都可用同一薄生命周期包装安全完成。

**最强成立条件：** 消费者接受候选公开的录制生命周期；适配层对未提交工作有明确 owner，并只承诺该候选实际允许的取消点。在此条件下，不必重做 fence 退休。

**依据与反例：**

- **已确认：** NVRHI 普通受追踪路径在录制阶段便持有资源，而不是提交后才开始保活。`close` 后不 execute 即重新 `open` 被允许，但头文件标为未充分测试。它既是复用的强证据，也是取消路径需要独立验收的直接理由；不能把“未充分测试”写成已知必然失败。见 `nvrhi.md` §4.1–4.2，固定 `ICommandList` 契约 [O1]。
- **已确认：** Diligent context 持有当前绑定，不等于 command list 保存了历史使用资源全集；其提交回收契约仍要求 deferred 使用资源在 immediate 执行前保持有效。因此不能从原生对象延迟释放推出“录制后可立即放掉所有引用”。见 `diligent.md` §4.1，`DeviceContext.h`、`RenderDeviceNextGenBase.hpp` 定位。
- **已确认：** SDL 获取 swapchain texture 后，该 command buffer 不可取消，且 texture 只能由该 command buffer 使用。若项目必须允许这个时点后的普通取消，改一个包装函数名无法满足；需要经产品认可的较早取消点或不同工作划分。见 `portable-libraries.md` §2“Presentation”、§3.2；[O2]。
- **证据缺口：** bgfx 已确认的是 encoder/frame 汇合与上传内存契约，不是任意独立录制对象的取消协议。wgpu-native 的 C 标准头、destroy/release 组合及取消后的引用释放未完整核验。Diligent 取消未执行列表时动态分配和状态元数据如何收束也未得到完整证明。不得由此宣布三者不支持丢弃。

**严重性与适用条件：** 对“业务 owner 可在 execute 前退出”或“任意时点可取消”的消费者是正确性门槛；若消费者接受固定批次 owner 和较窄取消点，通常可以通过明确适配解决。当前尚无已确认的本项目冲突，更没有已复现的库 bug。

**裁决：** 否定普遍自动保活/任意取消的推论；保留按库契约实现的窄适配；取消后清理与下一次录制的行为待实测。

**能改变结论的最小真实实验：** 从首个消费者截取一个可读回的真实工作，录制对资源 A 的使用后切换到 B，使“当前绑定保活”不能偶然掩盖 A 的历史引用。分别走合法提交与合法丢弃路径：NVRHI 使用普通 tracked 路径；Diligent 先按契约保留批次引用；其他候选严格使用固定版本公开入口。调用方逻辑引用退出后，确认旧工作输出、引用归还和后续再次使用资源的结果。SDL 只测试允许的取消点，不故意取消已获取 swapchain 的 buffer；不通过提前销毁仍在飞原生对象制造故障。若一个有界的录制 owner 足够且清理闭合，保留“窄适配足够”；若必须建立库外隐式命令重放/退休系统，或必需取消点不可表达，则否定该路线对该需求的充分性。

### 异议 2：对象活着，不代表旧 binding 和旧数据仍然有效

**被挑战主张：** 包装资源句柄并依赖引用计数，就能透明处理材质更新、绑定覆盖、动态数据和跨后端 shader 输入。

**最强成立条件：** 消费者接受候选的 binding schema 和版本化策略，更新频率可映射到其原生类别；普通绑定集合已覆盖真实使用，不要求包装层伪装共享可变描述符或任意布局。

**依据与反例：**

- **已确认：** NVRHI 普通 binding set 有传递保活；descriptor table 却不替被引用资源保持强引用、跟踪状态或生成自动 barrier，表更新还须应用同步。GPU instance buffer 指向的 BLAS 也不享有同样追踪。普通路径的成功不能覆盖这些例外。见 `nvrhi.md` §4.4，§6.1；[O1]。
- **已确认：** Diligent `ALLOW_OVERWRITE` 不会自动保护正在使用的 mutable binding；D3D12/Vulkan 上应用仍要保证 GPU 不再访问相应 SRB。static/mutable/dynamic 是绑定变化分类，与资源内容 usage 不是一回事。见 `diligent.md` §4.4；[O3]。
- **已确认：** SDL cycling 可以切换底层物理资源，轮换后的内容未定义，texture 按整体而非仅更新区域轮换。若上层要求部分更新保留其余内容或稳定原生身份，不能把 cycling 当透明同步开关。见 `portable-libraries.md` §2“状态/同步”、§3.2。
- **已确认：** NVRHI Vulkan binding offsets、SDL 格式专属 set/register 规则、bgfx shaderc 产物约定和 wgpu 的 feature/limit 与 shader 输入是不同资产契约。wgpu 的 mappable-primary-buffer 扩展也不取消映射与 GPU 使用互斥。见 `nvrhi.md` §6.1；`portable-libraries.md` §2“Binding/Shader”、§3。
- **对照证据：** Slang 论文支持参数块布局、特化与运行时绑定之间需要明确契约，但并不证明项目必须采用 Slang；共享 CPU 业务类型也不能自动证明 GPU ABI 相同。见 `papers-and-talks.md` §3/S1，论文 §2.6、§3.5、§5–7 [O4]。

**严重性与适用条件：** 当真实消费者需要在途材质替换、bindless 槽位复用、局部纹理更新或布局热重载时是正确性门槛；若只需固定布局、不可变集合和受控更新，上述边界可能很容易接受。没有证据证明这些功能必然属于首个消费者。

**裁决：** 否定“引用计数即覆盖安全”的主张；保留不可变集合、合法 dynamic binding、完成后覆盖等路线；具体 shader 表达能力、C ABI 扩展和成本待固定版本验证。

**能改变结论的最小真实实验：** 用首个消费者真实 shader 和数据布局，在一份旧工作仍未完成时生成一次新绑定/数据版本，分别读回旧、新期望值；只在确需时加入 bindless、部分 texture 更新或布局热重载。使用 NVRHI 新 set、Diligent dynamic/SRB 版本化或 fence 后覆盖、SDL 正确 cycling/重写范围、wgpu-native 实际可用 C API，而非故意违规覆盖。记录布局标识、物理/逻辑版本和最少维护对象。若这些原生策略完整且可接受，异议降为明确接入工作；若只能侵入后端、全局 idle 或复制另一套通用描述符体系才能保持必需语义，再否定相应路线。

### 异议 3：自动状态、并行录制和原生逃生口不能拼成任意调度器

**被挑战主张：** 自动 barrier 负责正确性，多线程录制负责性能；剩余特殊操作通过 native handle 补齐，渲染器可以不接受库的执行语义。

**最强成立条件：** 每个访问域只有一份状态权威，录制/提交顺序明确；库的自动追踪边界与消费者一致。原生互操作仅用于少数有完整交接协议的边界，而不是常态绕行。

**依据与反例：**

- **已确认：** NVRHI 自动追踪不推导完整的跨 list/跨 queue 依赖。`beginTracking` 只声明现状，不执行转换；关闭自动 barrier 也不等于取消 `keepInitialState` 的 close 恢复。连续相关 UAV dispatch 还存在需显式表达 barrier 的场景。见 `nvrhi.md` §5.1–5.2。它有面向多线程提交的 tracker，不能反向误判为只能单线程；同队列复合 native wait/signal 与提交的安全边界仍需核实，见 §7。
- **已确认：** Diligent `TRANSITION` 写共享资源状态元数据。并行手动管理共享资源需禁用相应自动追踪，并按真实执行顺序安排状态和 fence。手工 `SetState` 也不是 GPU barrier。见 `diligent.md` §3、§4.3。
- **已确认：** bgfx 的 Sequential 不能移除 view 分组、frame 汇合及 render-thread 边界；SDL command buffer 需在获取线程使用并提交，不直接支持“worker 录好交给任意专用 submit thread”的同一对象用法。见 `portable-libraries.md` §3.1–3.2；[O5][O6]。
- **已确认但不能扩大：** wgpu Rust 29.0.1 有 `transition_resources`、`as_hal` 等高级入口；这反驳“完全没有显式同步或 native 能力”，却不证明相同能力已由所选 C ABI 导出。unsafe 路线把责任交回调用者。NVRHI native getter 不 AddRef，已核验 Vulkan native buffer import 是 non-managed；Diligent native attach 同样不等于可以绕开其状态/fence 协议。见 `portable-libraries.md` §3.3；`nvrhi.md` §4.4；`diligent.md` §5.3。
- **对照证据：** Godot 自动设备图仍需对同 list 内相关 compute 使用表达边界；Filament 的 FrameGraph 与 Vulkan barrier 实现分层。没有工业证据支持“任何未观察到的 native 访问也会自动安全”。见 `industrial-engines.md` §4.2、§5。Whippletree 的 GPU 内任务调度也不能证明 CPU 多队列/多提交线程是需求，见 `papers-and-talks.md` §3/S2。

**严重性与适用条件：** 若必需线程交接、命令顺序或原生互操作与公开契约冲突，是架构阻断项；否则是可明确分层的实现责任。Diligent 取消录制是否留下可影响后续工作的状态元数据、NVRHI 特定并发提交组合是否安全，目前是疑虑，不是已证明的数据竞争或库缺陷。

**裁决：** 否定“自动状态＋native handle＝任意调度安全”；保留受支持的录制/提交模型、显式边界状态和经过证明的互操作。不能为了维持原先假设的线程模型，擅自把未确认的要求变成淘汰门槛。

**能改变结论的最小真实实验：** 先用真实消费者中一条生产者写→消费者读依赖建立串行同语义基线；仅在用户确认需要时增加独立并行录制和不同完成顺序，提交依赖保持明确。检查合法丢弃一次录制后，下一条链的状态仍正确。只有确需 native 互操作时，加入一次真实库→native→库交接，记录外部 owner、入口/出口状态、signal/wait 和完成凭证。若库的单一追踪体系即可承接，保留复用；若必须两层同时猜状态或无法表达必需线程/顺序，淘汰该接入方式。没有 native 需求时不为证明灵活性而人为增加逃生口。

### 异议 4：提交、完成、GC 和 frame 维护不是一个时刻

**被挑战主张：** 库会自动回收；统一 `EndFrame()` 或等若干帧就足以覆盖上传、描述符、未提交工作、离屏和退出。

**最强成立条件：** 适配层显式驱动候选规定的维护节奏，资源使用期由真实 completion 证明；应用逻辑帧、窗口 present 与存储回收不被混为一谈。

**依据与反例：**

- **已确认：** NVRHI 一个 list 对象可对应多个提交实例；device GC 不代替自定义 tracker GC，wait 与 GC 不等价。上传池可能保留高水位，Vulkan VCB 固定版本还要覆盖已录制但未提交的占用。见 `nvrhi.md` §4.2–4.3、§6.2–6.3。
- **已确认：** Diligent `FinishFrame` 不提交，deferred 上还要求相应 list 已执行；device `IdleGPU` 只等待已提交工作，`EnqueueSignal` 自身不 flush。主 swapchain 的隐含维护不能自动覆盖纯离屏消费者。见 `diligent.md` §4.2、§5.2。
- **已确认：** bgfx frame 的 CPU 握手不是 GPU fence；其 makeRef/transient 约束是特定上传数据契约，不能推广成所有原生资源“延迟两帧安全”。wgpu Rust `write_buffer` 在下一次 submit 执行传输，回调需由 submit/poll 推进；C ABI 的具体推进与重入仍未完整核验。SDL submit 的先后开始保证不能改写为前一提交全部完成。见 `portable-libraries.md` §2。
- **对照证据：** 原生 command-list Reset 与 allocator Reset 条件不同；工业图内 last-use、CPU 命令消费和 GPU fence 回收也不同。见 `native-baseline.md`“已核对事实”；`industrial-engines.md` §3“逻辑释放与 GPU 退休”、§5。

**严重性与适用条件：** 错误的完成解释可导致提前复用；缺少维护前进性可导致暂停呈现或关停时积压。这些是所有异步消费者要说明的边界，但当前没有证据证明候选在合法维护下泄漏。池保留容量不能被直接报告为泄漏。

**裁决：** 否定统一帧计数替代 completion 的设计；保留候选自己的退休器，不应再叠一个未经证明的按帧删除队列。池预算是否可接受必须等待用户预算与测量。

**能改变结论的最小真实实验：** 一份带动态上传和读回的真实工作提交后，暂停窗口呈现但继续按文档驱动候选维护；再完成正常停止提交、等待已提交工作、收集和销毁。区分逻辑句柄、未提交引用、在飞引用、已完成待 GC 引用、池容量。NVRHI 分别检验内部和自定义 tracker；Diligent 使用无主 swapchain 的显式序列；wgpu-native 使用实际 C 事件推进。若无需全局 idle 串行化日常工作即可排空且预算可解释，保留薄维护层；若要求的暂停/离屏模式无法前进，或只有隐藏第二套回收器才能工作，否定该模式。未来比较成本前先约定预算，不预设内存或时间阈值。

### 异议 5：有日志或提交 token，不代表生产错误协议已经闭合

**被挑战主张：** 成熟库已处理设备丢失；适配层统一返回 bool/token，release 关闭诊断即可。

**最强成立条件：** 产品明确允许“诊断后退出”或明确要求恢复，适配层能把该策略可靠映射到候选实际故障入口，并区分普通验证失败、surface 变化与 device loss。

**依据与反例：**

- **已确认：** NVRHI 已核验 D3D12/Vulkan 提交路径报告设备失效后仍可能返回实例编号；D3D12 与 Vulkan `waitForIdle` 的已读错误行为也不同。非零 token 不是成功或健康证明。见 `nvrhi.md` §8.2；固定实现定位 [O7]。
- **已确认：** bgfx `CallbackI::fatal` 对除 DebugCheck 外的 fatal 要求不能安全继续，其中包括 DeviceLost。若用户要求原进程透明恢复，这份已核对契约与该要求直接冲突；若允许诊断退出，它不是缺陷。见 `portable-libraries.md` §2“故障”、§3.1；[O8]。
- **已确认与未知分开：** Diligent 状态模式 VERIFY 在 Release 不做该验证；另有创建 null、shader status、Present/queue 检查和已观察到的无限等待路径，但报告未完成整个 device-loss/error/shutdown 链路，也未证明相关宏在所有配置下的完整行为。不能既凭 debug 成功宣称 release 安全，也不能据此宣称 Diligent 完全不处理故障。见 `diligent.md` §3“状态/同步、故障”、§9.4。
- **已确认与未知分开：** wgpu Rust 有 error scope、device-lost callback 和 surface 分类，但不能直接认证 C 宿主同样的线程/回调协议。SDL 最小化时获取成功且 texture 为 NULL 不是设备故障；统一设备丢失恢复仍未知。见 `portable-libraries.md` §2、§3.3、§6。
- **对照证据：** D3D12 `GetCompletedValue()==UINT64_MAX` 表示 device removed，不能先按普通“大完成值”解释。工业引擎也可能明确选择崩溃诊断退出；成熟不等于透明恢复。见 `native-baseline.md`“失效”；`industrial-engines.md` §1、§3“错误与设备丢失”；[O9]。

**严重性与适用条件：** 把故障误当完成是安全性门槛；退出还是重建属于产品选择。只有明确要求原进程恢复时，bgfx 已核对 fatal 策略才成为该路线的条件性淘汰依据。其他候选尚不能因资料不足直接淘汰。

**裁决：** 否定 token/bool 和 debug 验证替代生产错误协议；保留按故障策略选库。dev/release 的可观测性、有限关停与恢复分别验收，不能互相代替。

**能改变结论的最小真实实验：** 先在批准环境用真实无效 shader/资源创建等非 GPU 故障入口，分别检查开发诊断配置和生产配置能否观察失败、拒绝把失败对象当有效、停止相关工作并有限退出。再单独评估设备丢失：仅在获准隔离环境通过受控方式触发，检查等待者收束、token 解释、旧设备对象隔离与诊断；只有用户要求恢复才增加重建设备和资源的结果验证。软件边界注入只能证明传播，不能证明真实 GPU loss。若产品允许退出且候选能可靠退出，不能因不恢复而否定复用。

### 异议 6：API 名称和主仓版本不能证明真实接入闭包很薄

**被挑战主张：** 选成熟库并锁一个版本即可；有 C 接口或 C++ 包装就与自有语言天然兼容，预编译 shader 又能消除所有工具链成本。

**最强成立条件：** 固定真实来源、头文件与二进制组合，明确配置/构建/链接/分发四种依赖闭包；外层只转换 GPU 类型，语言 runtime 的借用、释放线程和回调规则能够自然匹配。

**依据与反例：**

- **已确认：** NVRHI 主证据已固定到 `d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076`，但部分补充头文件/许可仍为滚动来源；早先 main 头版本 26 与固定版 27 有差异。header version 检查不是完整 C++ ABI 保证；默认 header fetch、可选 SDK 仍需独立锁定。见 `nvrhi.md` §2、§9。
- **已确认：** Diligent 研究样本是 v2.5.6，不能代表之后版本；Core 可裁剪不等于 D3D12 本体零依赖。已读源码链涉及 Core ShaderTools、Python、fxc、SDK/ATL 检查，且该版本 x86_64 MSVC release 默认 AVX2 需与目标 CPU 范围核对。选择自有离线 shader 不自动消除库本体生成步骤，也不能未经工件验证宣称无需 dxcompiler DLL。见 `diligent.md` §2、§5.1、§6。
- **已确认：** bgfx 文档、SDL Wiki 与 wgpu-native trunk 未形成固定提交闭包；SDL 函数 since 3.2.0 不认证所有扩展属性均存在于该版。wgpu Rust 29.0.1 文档、native manifest 和滚动 workspace 不是同一发布认证；C 标准头本轮未完整取得。**这些都是锁定/证据门槛，不是“不支持”。** 见 `portable-libraries.md` §1、§6。
- **已确认：** Diligent 有 C/C++ interface，wgpu-native 提供 C ABI，这仍不自动解决 borrowed view、release/destroy、结构布局、回调线程/重入与语言 finalizer。NVRHI C++ 描述符不能仅靠换名就成为稳定语言边界。见 `diligent.md` §7.2；`nvrhi.md` §9.2、§10；`portable-libraries.md` §3.3。
- **边界：** shadercross 是可选独立工具；源码构建 Rust 工具链不等于使用预编译 wgpu-native 的 C++ 消费者必须运行 Rust 编译器。根许可证也不等于传递工件全部同许可。本轮不作法律兼容性判断。见 `portable-libraries.md` §4。

**严重性与适用条件：** 当前是可复现接入与交付门槛；只有实际闭包违反用户确认的工具链、CPU、ABI 或分发要求时，才构成阻断。版本证据较旧/不完整不能被当作库质量总分。

**裁决：** 否定“一份功能页＋一个 tag＋C ABI 名字即足够”；保留所有候选，先锁定打算验证的真实组合。允许从不同维护版本重新取证，不能把本轮样本升级成永久淘汰结论。

**能改变结论的最小真实实验：** 先固定候选、依赖、选项、shader 编译器与产物；在批准阶段构建并运行同一最小实际消费者，不只创建 device。保留配置记录、link/install manifest、实际 DLL/静态库和许可证清单。C++/自有语言路线必须经过真实桥接；wgpu-native 不得以 Rust 示例代替 C ABI 验收。检查布局数据由 GPU 读回一致，owner/finalizer 与回调在真实线程上可合法收束。若只需有界投影与少量生命周期入口，支持薄适配；若固定组合缺失必需入口或引入不可接受且无法裁剪的闭包，再淘汰该组合，而不是整个库家族。

## 三、条件化短名单：按用户选择分支，不作总排名

| 需求分支 | 保留候选及最强理由 | 淘汰或回退条件 |
|---|---|---|
| 希望保留自有 Renderer/平台/shader 链，接受低层资源与提交语义 | **NVRHI**：普通 binding 路径的录制保活、实例退休、上传和状态机制可直接省责；**DiligentCore**：可同时委托绑定、动态页、延迟回收和可选呈现。二者值得在同一消费者上对照，不预设性能优胜者。 | NVRHI 的真实必需路径若主要落在 bindless/native 等自动保护之外，应重算仍被省掉的职责；Diligent 若必需取消/长寿命录制/动态内容使用与保活、FinishFrame 无法相容，应降级该接入方式。只有经验证违反硬要求才淘汰。 |
| 愿意接受 view、排序及帧汇合，而非预先要求任意提交器 | **bgfx**：调度和资源异步处理由库集中承担，shaderc 可纳入离线资产链。 | 必需顺序/提交点只能通过额外 frame、重放或改库实现；或产品必须原进程设备恢复而固定版仍为已核对 fatal 契约。Sequential 不作为绕开全部帧模型的承诺。 |
| 希望应用级可移植 graphics/compute，接受命令与窗口线程规则 | **SDL GPU**：工作流和资源 cycling 可减少直接后端责任；无呈现消费者可不建窗口。 | 必需能力在固定版本/目标硬件不成立；必需 command-buffer 交接或取消点与契约冲突；稳定物理身份/内容保留只能侵入后端。先验证合法分工，而不是默认排除。 |
| 希望明确的验证、安全资源模型与 feature/limit 协商 | **wgpu-native**：作为 C++/语言桥候选，必须以实际 C ABI 验证；**wgpu Rust** 只在团队接受该宿主/桥接边界时作为另一接入路线，不能作为 C 能力的替代证据。 | 必需能力只在另一 Rust 版本/未导出的 HAL；生产路径不得不移除状态跟踪而团队不接受 unsafe 责任；固定 C ABI 的持有、回调或错误协议无法闭合。淘汰接入路线不等于淘汰家族。 |

所有分支都应优先尝试候选已有的合法策略，例如不可变绑定、明确录制 owner、正确的维护调用、可关闭依赖选项。**不能在用户未同意时，为迁就库悄悄删除已确认语义。** 若一个候选失败，先比较另一候选对同一需求的直接表达；只有已证明必需能力与候选职责切分不相容，再考虑受限 native 方案或自研。原生对照仍要承担 `native-baseline.md` 列出的生命周期、同步、绑定和故障验证，不能因“可控”免验。

## 四、必须先问用户的语义选择

以下会改变可见行为、支持范围或采用分支，不能由评估者编造答案：

1. **首个真实消费者与最低能力/硬件范围是什么？** 包括实际 shader、graphics/compute 需求，以及第二后端是否近期必需。Windows 不回答这些问题。
2. **上层要保留什么调度与取消自由？** 是否必须 worker 录制后集中提交、是否必须缓存跨帧未提交命令、在哪个阶段仍允许取消；还是可以接受候选的 frame/view 或线程亲和。这些是可分别回答的语义，不是要求用户指定 mutex 或线程数。
3. **必须支持哪些在途更新？** 固定绑定是否足够；是否需要材质热重载、bindless 槽位覆盖、部分纹理更新或稳定原生资源身份。不得为压力实验反向创造产品需求。
4. **是否确需第三方/native 互操作，谁拥有外部对象？** 如果有，应给出真实使用方和交接行为，而不是泛泛要求“留逃生口”。
5. **设备丢失的产品策略是什么？** 可诊断退出/重启，还是原进程恢复；暂停呈现、纯离屏和关闭时应保证什么前进性。
6. **允许的依赖与交付边界是什么？** C++/自有语言桥、可选 Rust 构建链或预编译工件、运行时 shader 编译器、目标 CPU 基线及可接受预算。预算用于随后实测，不由调研打伪精确权重。

不应提前甩给用户的实现细节包括：tracker 的具体容器、句柄槽位/代数编码、提交前引用集合的数据结构、在既定语义内选择 SRB 版本化还是合法 dynamic binding、精确 GC 调用位置，以及哪一个 helper 驱动 poll。工程方应根据固定候选提出并证明这些细节；只有它们改变产品语义、预算或发布闭包时才需要升级为用户决策。

## 五、条件化验证次序与停止规则

- **先完成异议 6 的固定组合与真实消费者接入。** 未锁定 C ABI/headers 或实际工件时，不开展跨候选“性能排名”。不是先造一层通用 RHI 再试着塞库。
- **生命、更新和回收优先：** 将异议 1、2、4 合并到同一真实消费者中，覆盖未提交、在飞、已完成待收集三个阶段。使用正确输出和真实完成凭证，不以“没崩溃”通过。
- **并发与 native 只按需求增加：** 异议 3 的多线程/多队列/native 分支仅在消费者需要时启用；同语义串行实现作为对照，不把单队列规定为生产限制。
- **故障分两级：** 常规错误在开发/生产配置均要可观察；真实设备失效另行批准并隔离。退出可用不等于恢复可用，模拟传播不等于真实 GPU loss 已验证。
- **只有正确性和语义成立后再比较成本：** 记录 CPU 临界路径、提交/复制、descriptor/上传版本、显存/系统内存和端到端延迟，是否超限由用户认可的预算判断。barrier 较少、源码较短或后端较多均不能独立判优。

**允许停止复用验证的证据**是固定路线无法表达已确认硬需求、合法路径仍违反生命周期/输出不变量、生产故障无法按要求收束，或实证闭包/成本不可接受。**不足以停止的理由**包括：某页没有写、未取得某头文件、样本不是最新版、存在 GPU wrapper、shader 工具独立、设备或呈现需应用负责。

## 原始来源补充定位

以下链接供主报告保留关键原文入口；具体证据范围、版本限制与已核验符号以相应本地报告为准，不表示本评估新增完成了源码审计。

- **[O1] NVRHI 固定公开契约：** `ICommandList`、`BindingSetDesc`、`IDescriptorTable`：https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/include/nvrhi/nvrhi.h ；编程指南 Resources/Command List/Bindings/State Tracking：https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/doc/ProgrammingGuide.md
- **[O2] SDL swapchain 获取与取消边界：** https://wiki.libsdl.org/SDL3/SDL_WaitAndAcquireGPUSwapchainTexture
- **[O3] Diligent v2.5.6 mutable/static 覆盖：** https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/ShaderResourceVariable.h ；context 生命周期/状态/维护：https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/DeviceContext.h
- **[O4] Slang 论文：** https://doi.org/10.1145/3197517.3201380 ，§2.6、§3.5、§5–7；解释参数布局与特化边界，不作为当前编译器行为或项目性能认证。
- **[O5] bgfx 帧/view/线程模型：** https://bkaradzic.github.io/bgfx/internals.html ，Sort-based draw call bucketing、API thread and render thread、Encoder API。
- **[O6] SDL command buffer 线程契约：** https://wiki.libsdl.org/SDL3/SDL_AcquireGPUCommandBuffer ；https://wiki.libsdl.org/SDL3/SDL_SubmitGPUCommandBuffer
- **[O7] NVRHI 固定故障/提交实现：** https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/d3d12/d3d12-device.cpp ，`executeCommandLists`、`waitForIdle`；https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/vulkan/vulkan-queue.cpp ，`Queue::submit`。
- **[O8] bgfx fatal 契约：** https://raw.githubusercontent.com/bkaradzic/bgfx/master/include/bgfx/bgfx.h ，`Fatal`、`CallbackI::fatal`；滚动来源。
- **[O9] 原生 device-removed completion sentinel：** https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue
- **[O10] wgpu 路线分开核验：** Rust 29.0.1 encoder：https://docs.rs/wgpu/29.0.1/wgpu/struct.CommandEncoder.html ；Device：https://docs.rs/wgpu/29.0.1/wgpu/struct.Device.html ；native 滚动扩展头：https://raw.githubusercontent.com/gfx-rs/wgpu-native/trunk/ffi/wgpu.h 。这三者不能替代一个固定 C ABI/二进制组合的验收。

**最终判断：** 现有证据足以保留成熟库复用为强候选，也足以否定“不先确认语义就保证薄适配足够”。最有区分力的下一步不是继续增加功能列表或选一个看起来最成熟的赢家，而是让同一个真实消费者暴露它所需要的录制 owner、绑定版本、状态权威、完成与故障边界，再检验哪个候选真正减少长期责任。