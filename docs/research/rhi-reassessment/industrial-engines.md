# 工业引擎图形层实践：Unreal Engine、Godot RenderingDevice、Filament

历史来源记录：2026-09-25 的选型研究快照，保留固定版本的源码事实、许可证依据、比较理由和证据缺口。文中的“当前”“未确认”均指该次研究；生产 RHI 与 RDG 现已采用 NVIDIA NRI + `atelier_rhi` + `atelier_rdg`，现行选择见[模块决策](../../modules/rhi/decisions.md)，运行证据见[模块验收](../../modules/rhi/acceptance.md)。本文不再提供实施计划或待执行实验。

## 1. 结论摘要

本报告只比较可公开核验的机制，不推荐把三套引擎的内部图形层直接接入 FrostFoxAtelier，不选定后端，也不把项目的 Windows x64 背景当作架构裁决。

- **事实：三者的“图”不在同一个抽象层。** UE RDG 是显式声明 pass 资源依赖的高层调度框架；Godot RenderingDeviceGraph 在设备抽象内部从命令使用推导依赖；Filament FrameGraph 管理渲染过程的资源与 pass，而实际 Vulkan layout/barrier 仍有后端实现。[U1][G2][F1][F5]
- **事实：存在工作线程不等于底层 API 可多写者并发调用。** UE 文档区分前端录制与后端翻译；Godot 4.4 的多项 list 操作有 render-thread guard，且该版本 `SECONDARY_COMMAND_BUFFERS_PER_FRAME` 为 `0`；Filament 的 `CommandStream` 明确限定单写线程。[U2][G1][F3]
- **事实：逻辑释放、CPU 命令消费、GPU 完成是不同的生命周期边界。** Godot 将资源放入 frame 的待销毁队列，等待对应 fence 后处理；Filament Vulkan command buffer 持有资源引用，完成后才重置资源管理器。[G1][F4]
- **事实：故障诊断不是透明恢复。** UE 5.6 GPU crash 文档描述检测后退出；Godot 4.4 的 Vulkan 提交路径在 `VK_ERROR_DEVICE_LOST` 后打印诊断并触发崩溃；Filament 核对的路径只足以证明 fence 状态记录和提交断言，不足以证明设备恢复。[U4][G4][F4]
- **设计推断：应先借鉴可解释的不变量，而不是线程数量、宏系统或通用图框架。** 优先把资源身份、上传数据所有权、提交完成、状态推导边界和跨帧导入导出讲清楚；是否需要命令重排、多队列、独立 RHI 线程或 aliasing，应交由真实消费者实验决定。

## 2. 来源快照与证据等级

### 2.1 版本范围

| 对象 | 本次固定范围 | 核验程度与限制 |
|---|---|---|
| Unreal Engine | `application_version=5.6` 的 RDG、Parallel Rendering、Threaded Rendering、GPU Crash 官方文档 | 文档正文已通过 Exa 取得，返回正文标题标示 5.6；抓取标题同时出现 403 包装，不能据此声称实时匿名站点访问完全正常。未取得引擎实现源码、提交 SHA 或内部后端代码。 |
| Godot | 上游 `4.4-stable` 标签源码；4.4 官方文档 | 实际核对 `rendering_device.cpp/.h`、`rendering_device_graph.cpp/.h`、`rendering_device_driver.h` 与 Vulkan driver 关键路径。固定的是标签，不是已验证不可变 SHA。 |
| Filament | 上游 `v1.56.0` 标签源码 | 实际核对 FrameGraph、CommandStream、DriverAPI、Engine/Renderer、Program、VulkanTexture、VulkanCommands 等接口与实现。固定的是标签，不是已验证不可变 SHA。 |
| 补充材料 | Filament 官方 FrameGraph notes；Epic 官方源码访问说明 | 滚动来源，仅作解释和访问边界说明，不能覆盖固定标签源码事实。[F9][U5] |

这些是便于追踪比较的**样本版本**，不表示它们是当前最新版本。本次不以搜索摘要证明实现。源码经文本抽取后部分 C++ 模板尖括号被省略，因此以文件 URL 和实际函数名定位，不复制抽取后的源码用于编译，不编造 GitHub 行号。

证据标记：**事实-源码**表示核对到实现/接口；**事实-文档**表示官方公开契约或说明；**设计推断**表示对本项目的条件性建议；**未知**表示当前证据不足。

### 2.2 关键定位索引

| 编号 | 一手来源与定位 |
|---|---|
| U1 | UE 5.6《Render Dependency Graph》：Shader Parameter Structs、Render Graph Builder、Setup and Execute Timelines、RDG Resources and Views。[官方文档][U1] |
| U2 | UE 5.6《Parallel Rendering Overview》：Threading Overview、Graphical Commands and the Command List、Synchronization。[官方文档][U2] |
| U3 | UE 5.6《Threaded Rendering》：Thread specific data structures、Rendering resources、UObjects and Garbage Collection。[官方文档][U3] |
| U4 | UE 5.6《Dealing with a GPU Crash》：Recognizing a GPU Crash、RHI breadcrumbs、DRED。[官方文档][U4] |
| G1 | Godot `rendering_device.cpp`：线程 guards、`uniform_set_create`、`compute_list_add_barrier`、`_free_internal`、`_begin_frame`、`_stall_for_frame`、`submit/sync`、`screen_prepare_for_drawing`、`execute_chained_cmds`。[源码][G1] |
| G2 | Godot `rendering_device_graph.cpp/.h`：`ResourceTracker`、`reset_if_outdated`、`_add_command_to_graph`、`_group_barriers_for_render_commands`、`end`。[实现][G2]／[头文件][G2H] |
| G3 | Godot `rendering_device_driver.h`：文件开头 Design principles，barrier、command buffer、queue、swapchain 接口。[源码][G3] |
| G4 | Godot Vulkan driver：`command_queue_execute_and_present`、`fence_wait`、`print_lost_device_info`、`on_device_lost`。[源码][G4] |
| G5 | Godot 4.4《Internal rendering architecture》及 RenderingDevice 类参考。[架构文档][G5]／[类参考][G6] |
| F1 | Filament `FrameGraph.cpp/.h`：`compile`、`execute`、`readInternal`、`writeInternal`、`Builder::sideEffect`、`import`。[实现][F1]／[接口][F1H] |
| F2 | Filament `Engine.h`：Thread safety、Multi-threading、资源追踪、`Builder::build`、`createSwapChain`；内部 Engine 的 `FEngine::execute`。[接口][F2]／[实现][F2C] |
| F3 | Filament `CommandStream.h`：三类 `DECL_DRIVER_API*` 宏、`debugThreading`、`allocateCommand`；DriverAPI 的创建/销毁/绑定/呈现调用。[命令流][F3]／[DriverAPI][F3A] |
| F4 | Filament Vulkan：`VulkanCommandBuffer::acquire/reset`、`VulkanCommands::flush/gc/updateFences`。[头文件][F4H]／[实现][F4] |
| F5 | Filament VulkanTexture：`transitionLayout`、`samplerToAttachmentBarrier`、`attachmentToSamplerBarrier`。[源码][F5] |
| F6 | Filament Renderer：`Export SSR history` pass、`resources.detach`、`view.commitFrameHistory`、`beginFrame/endFrame/skipFrame`。[源码][F6] |
| F7 | Filament `Program::shader/shaderLanguage/descriptorBindings` 与 `BufferDescriptor` 移动和回调契约。[Program][F7]／[BufferDescriptor][F7B] |
| F8 | Filament backend CMake：PRIVATE_HDRS、依赖、生成的 vkshaders；不是独立稳定接入契约证明。[构建定义][F8] |

## 3. 机制比较矩阵

表内事实仅适用于上述版本；“未核验”不是“没有实现”。

| 维度 | Unreal Engine | Godot RenderingDevice | Filament |
|---|---|---|---|
| **RHI/Driver 与 Renderer 的边界** | **事实-文档：**RHI 提供跨平台图形接口；RDG setup 声明资源/pass，execute lambda 录制 RHI 命令。场景语义和渲染算法不是 RDG/RHI 自身职责。[U1][U2] | **事实-源码/文档：**Forward+、Mobile 使用 RD；Compatibility/OpenGL 不走 RD。RD 负责较高层验证和设备资源管理，RDD 提供底层对象与命令；RDD 设计说明要求不反向查询 RD 的高层状态。[G1][G3][G5] | **事实-源码：**FrameGraph 属于 renderer 内部，execute 接收 DriverApi；CommandStream 再分派至具体 Driver。`FEngine::execute` 消费命令缓冲，而不是重新执行整个 FrameGraph。[F1][F2C][F3] |
| **资源身份与 CPU 所有权** | **事实-文档：**RDG 指针归 builder 所有，builder 销毁后无效；底层 RHI 对象不能因有 RDG 描述符就提前访问。游戏对象与 scene proxy 的线程所有权需分离。[U1][U3] | **事实-源码：**公开设备资源以 RID 表示，RD 持有各类 owner 和资源依赖图；RDD 使用类型化 ID。`_free_dependencies` 会处理依赖方，不能把 RID 理解为独立、无依赖的裸 native handle。[G1][G3] | **事实-源码：**Engine 跟踪用户对象，用户仍应主动 destroy。`BufferDescriptor` 不可复制、可移动，以 callback 表达 CPU 上传内存归还；不能在入队后立即复用底层内存。[F2][F7B] |
| **逻辑释放与 GPU 退休** | **事实-文档：**RDG 控制 transient 生命周期与 aliasing；外部资源可越过图生命周期。`FRenderCommandFence` 示例跟踪渲染线程进度，不能据此推出 GPU 已完成。**未知：**5.6 各 RHI 后端实际 retirement 实现未核验。[U1][U3] | **事实-源码：**`_free_internal` 使 RID 退出 owner，同时把底层资源放入当前 frame 的 disposal 列表；`_begin_frame` 先 `_stall_for_frame`，再 `_free_pending_resources`，按管线、绑定、shader、纹理/缓冲等依赖顺序释放。[G1] | **事实-源码：**FrameGraph 计算 first/last use 并在对应 pass 创建/释放逻辑资源；Vulkan command buffer 的资源管理器保留引用，`gc` 检查 fence 成功后 `reset`。图内 last-use、后端引用归还、真实 GPU 完成不可混为一谈。[F1][F4][F4H] |
| **状态和 barrier 所有权** | **事实-文档：**RDG 从 pass 参数得知依赖，负责 subresource transitions、split barriers、async-compute fences 等。不能由此推断绕过 RDG 的原生操作也自动受追踪。[U1] | **事实-源码：**RDGraph 由资源使用构建依赖，在 `end` 分层排序并合并 barriers；RDD 的 `command_pipeline_barrier` 执行后端映射。纹理改变 usage，即使两个访问均只读，也可能按写依赖处理；还追踪 slice 重叠和前帧 stages。[G2][G2H][G3] | **事实-源码：**FrameGraph 表达 read/write、usage 与资源生存期；实际 Vulkan layout、同 layout 下读写可见性由 `VulkanTexture` 的 transition 和 attachment/sampler barrier 等后端路径处理。不能把所有同步功劳归到 FrameGraph。[F1][F5] |
| **Shader 输入与绑定描述** | **事实-文档：**参数 struct 的反射元数据同时参与 shader 绑定和 RDG 资源依赖声明；static uniform buffer slots 服务于跨 pass/view 复用缓存 draw commands 的场景。[U1] | **事实-源码：**`uniform_set_create` 对 shader set、binding、类型和数量等做匹配，并收集资源 tracker；源码到 SPIR-V 通过注册的编译函数完成，再交 driver 生成/读取其 bytecode。不是把 Godot 高层材质语言直接定义为 RD 契约。[G1][G5] | **事实-源码：**`Program` 接收 shader blob/language、set/binding/type/name 描述、specialization constants、push constants；该标签 DriverAPI 已有 descriptor-set layout、update 和 bind 调用。材质资产生产链不等于 Driver 绑定接口。[F7][F3A] |
| **并发录制与提交** | **事实-文档：**render thread 前端录制，RHI thread 翻译，前后端可分别并行；RDG pass lambda 应无副作用，使用 immediate command list 会排除该 pass 的并行执行。具体默认线程配置不能从旧说明推定。[U1][U2] | **事实-源码：**list 录制存在 render-thread guard；本标签 secondary command buffer 每帧数量宏为 0，不能把“存在 worker 路径”写成默认启用并行录制。transfer worker、main/transfer/present queue 另有明确结构，但不证明通用多队列 async-compute 调度。[G1][G2][G7] | **事实-源码：**Engine API 不自动保证线程安全；CommandStream 当前单写者，后台消费命令。FrameGraph `compile` 使用 stable partition 保留活跃 pass，`execute` 顺序遍历它们；此处不是 CPU 并行 pass 调度器。[F1][F2][F3] |
| **跨帧资源** | **事实-文档：**图资源可为 transient 或 external；注册既有 RHI 资源或从图提取资源使其生命周期越过本图。上层仍需管理历史资源语义与失效条件。[U1] | **事实-源码：**资源 tracker 保留 usage/access 及前帧 stages；frame 槽管理可重用缓冲和释放队列。这是跨帧 hazard/退休机制，不是 TAA history 的算法管理器。[G2H][G2][G7] | **事实-源码：**`FrameGraph::import` 不接管被导入资源生命周期。Renderer 的 SSR history 导出 pass 标 side effect，用 `resources.detach` 保存到 View history，图执行后 `commitFrameHistory`。这是高层 Renderer/View 的策略。[F1H][F6] |
| **窗口、swapchain 与呈现** | **未知：**本次没有核验 UE 5.6 viewport、swapchain 和平台窗口具体实现；不据通用 RHI 文档臆造最小接入接口。[U2] | **事实-源码：**RD 将窗口映射到 context surface 和 driver swapchain；`screen_prepare_for_drawing` 获取 framebuffer，需要 resize 时先等待在途工作。呈现可与 main queue 提交合并，也可通过 semaphore 转至独立 present queue。[G1][G3] | **事实-源码：**Engine 从 native window 创建 SwapChain，Platform 负责平台上下文；Renderer 执行 begin/end frame，DriverAPI 有 makeCurrent/commit。`FrameGraph::present` 只添加防裁剪依赖，不能等同于原生 swapchain present。[F2][F3A][F1H][F6] |
| **错误与设备丢失** | **事实-文档：**5.6 GPU crash 文档描述退出和诊断；RHI breadcrumbs、DRED 用于识别正在执行的工作与疑似失效资源。诊断路径不构成自动重建设备保证。[U4] | **事实-源码：**RDD 常以空 ID、bool、Error 表达失败；Vulkan `vkQueueSubmit` 返回 DEVICE_LOST 后打印故障信息并触发 `CRASH_NOW_MSG`。支持 breadcrumbs、可用时读取 `VK_EXT_device_fault`。该结论限所核验分支。[G3][G4] | **事实-源码：**Engine 构造可失败；Vulkan `updateFences` 可记录 DEVICE_LOST，`flush` 对提交成功作断言。**未知：**未找到并核验完整自动恢复契约，不把这些检查夸大为可恢复设备状态机。[F2][F4] |
| **依赖、许可与接入成本** | **事实-文档：**源码访问有 Epic/GitHub 账户及许可流程；本次未接触授权受限源码。**设计推断：**可学习公开机制，不作为可自由抽取的库或已审查许可方案。[U5] | **事实-源码：**根 LICENSE 为 MIT 文本；RD/RDD 使用 Godot 的对象、容器、RID、线程、项目设置等体系。**设计推断：**抽取并非复制一个接口文件；第三方依赖需逐项审查，不由根许可证推导全部依赖许可。[G1][G3][G7][G8] | **事实-源码：**根 LICENSE 为 Apache-2.0；backend CMake 有静态库目标，但核心 DriverApi/CommandStream 被列为 private headers，并依赖 math/utils；Vulkan 路径还涉及 bluevk、vkmemalloc、vkshaders、smol-v 等。**设计推断：**有 library target 不等于独立稳定公共 RHI 产品。[F8][F10] |

## 4. 最重要的版本与层次纠偏

### 4.1 Unreal：文档版本选择不等于正文完全没有历史遗留

**事实-文档：**选择 5.6 的 Threaded Rendering 页面仍有旧式 `ENQUEUE_UNIQUE_RENDER_COMMAND_*` 和“RHI 函数仅能从 rendering thread 调用”等叙述；Parallel Rendering 页面又明确介绍 RHI thread 与前后端并行，Synchronization 段还直接使用 Unreal Engine 4 作为叙述背景。[U2][U3]

**结论边界：**可从中学习“线程专属数据、命令传递、确定性生命周期”的原则，不能将旧宏、旧线程调用限制、固定落后一帧或两帧当作 UE 5.6 当前实现的精确契约。RDG 文档中 single-thread builder 等说明也仅作为该文档事实，不替代实现审查。未核验 UE 的 native barrier emission、实际提交拓扑、GPU 退休队列和所有并行 API 调用限制。

### 4.2 Godot：自动同步有粒度边界

**事实-源码：**`compute_list_add_barrier` 不是空操作：它保存当前 compute 状态，结束当前 list，开始新 list，并重新绑定 pipeline、uniform sets、push constants。因此，自动设备图不能被解释为任意一个 compute list 内的 dispatch 依赖都无须调用者表达。[G1：`compute_list_add_barrier`][G1]

**事实-源码：**4.4 的图代码具备 secondary command buffers/worker 支持，但 RD 的配置宏为 0。旧开发文章或 PR 中“可以并行录制”的描述不能升级成这个正式标签的默认行为。[G1][G2]

**设计推断：**如果 FrostFoxAtelier 采用自动 hazard tracking，必须先规定追踪粒度——整 pass、dispatch、纹理子资源或 buffer range——并明确内部依赖、外部 native callback、别名和资源逃逸怎样声明。不能把未观察到的访问当作不存在。

### 4.3 Filament：FrameGraph 不是另一份 UE RDG

**事实-源码：**v1.56.0 的 `FrameGraph::compile/execute` 足以证明裁剪、first/last-use、资源 usage 汇总及顺序 pass 执行；源码没有在这条路径实现一个类似 UE RDG 的 CPU 并行 pass 执行器。官方滚动 notes 的图论解释和 future work 不能覆盖这个固定标签实现。[F1][F9]

**事实-源码：**`FrameGraph::present` 的实现注释和代码仅添加依赖；真正呈现调用位于 Renderer/SwapChain/Driver 路径。SSR history 的 side effect/export 也来自 renderer。[F1H][F6]

**未知：**本次没有完成 ResourceAllocator 的物理内存复用/aliasing 全链路审计，不能由 first/last-use 直接宣称 Filament 实现了 UE 式 transient heap aliasing。

## 5. 值得借鉴的不变量与适用条件

以下均为**设计推断**，不是已经批准的 RHI 方案。

| 候选不变量/实践 | 对项目有价值的条件 | 不适用或暂不应复制的部分 |
|---|---|---|
| 区分应用对象、录制数据、GPU 资源的所有权和完成点 | 只要存在异步上传、GPU 在途工作或后台线程，就应说明谁保持数据有效、何时可复用、何时可销毁。依据三者的独立生命周期机制。[U3][G1][F4][F7B] | 不必照搬 UObject/GC/scene proxy、Godot 全部 RID owner、Filament 全套对象管理器。CPU 命令处理完成不能替代 GPU completion。 |
| 每个受追踪访问域有唯一、可解释的状态权威 | Renderer、图和 driver 共存时，应能说明资源的最后状态由谁掌握，native escape 如何归还状态。Godot 与 Filament 展示了不同但明确的分层。[G2][G3][F5] | 不预先裁定所有 barrier 都归 graph 或都归 backend；不允许两层独立推导后互相假设对方已同步。 |
| transient、imported、persistent/history 明确分开 | 出现后处理、多视图、前帧纹理或外部图像时，显式外部所有权和 import/export 可以减少隐含生存期错误。[U1][F1H][F6] | 没有真实临时内存压力时，不必先建通用 aliasing allocator；没有 temporal 消费者时，不必先复制复杂跨帧 history 框架。 |
| binding schema 可验证，并能辅助访问声明 | 自有语言与原生共享类型体系若要产生 shader 数据，必须在边界核对字段布局、资源种类、set/binding、数组长度和访问方式。[U1][G1][F7] | 共享 CPU 类型并不自动证明 GPU ABI 相同；不直接复制 UE shader 宏和全套材质 permutation，亦不把某引擎资产编译链绑定到 RHI。 |
| 把调用并发、录制并发、GPU 队列并发分别设计 | 真正的 CPU command generation 瓶颈出现时，可逐级启用并行，并保留同语义串行基线。[U2][G1][F1][F3] | 不因桌面有多核便复制专用 RHI thread、每 pass job、secondary command buffers 或多队列调度。不能把工作线程存在当成公共 API 任意线程安全。 |
| 资源退休不能只依赖“窗口下一帧一定会呈现” | 窗口最小化、离屏处理、暂停呈现、关闭时仍可能有待消费命令与待释放资源。Godot 的 pending-resource 状态与 Filament `skipFrame` 的 tick/flush 路径值得分析。[G7][F6] | 不复制以无条件全设备 idle 解决所有回收的做法；也不假设专用服务器必须初始化 GPU。 |
| 故障可定位，恢复策略单独决策 | 把资源名、提交标识、pass 标识、失败阶段与设备代次联系起来；区分验证失败、资源创建失败、surface 变化、device loss。[U4][G4][F4] | 不把大型引擎的 terminate 策略自动变成产品要求；也不承诺没有资产重建策略支撑的透明恢复。 |

### 不应从工业实现照搬的复杂度

- UE 的缓存 Mesh Draw Commands/static uniform slots 解决的是其跨 pass/view 缓存与资产生态的问题；小型引擎没有相同复用模式时，不能直接推定收益。[U1]
- Godot 的隐式 DAG 依赖 tracker、slice 交叠、attachment 覆盖范围、跨帧阶段、驱动差异和命令序列化；“调用接口简单”不意味着实现复杂度小。[G2]
- Filament 的 S/R 两阶段异步对象创建与命令流，需要与句柄分配、后台消费、同步查询和销毁语义协同；只复制队列会留下数据所有权漏洞。[F3]
- 三者的特定 tile/subpass、材质、scene/view 或平台 workaround 属于上下文相关方案；当前不作为 FrostFoxAtelier 的需求来源。

## 6. 尚未确定的内容

1. **项目需求未知：**首个实际渲染消费者、目标 GPU/驱动范围、需要的 graphics/compute 能力、第二后端时间表、真实并发规模尚未确认。因此没有总分、赢家或预估性能收益。
2. **UE 源码未知：**没有获取或索取授权受限源码；公开 API/文档里的类名和示例不能作为私有实现已核验的替代品。窗口呈现、后端状态跟踪和 GPU retirement 的具体实现仍待合法公开证据。
3. **后端覆盖有限：**Godot 和 Filament 的详细原生路径以 Vulkan 为主；没有把其 Vulkan 行为推广成所有 D3D12/Metal/OpenGL 后端共同保证。
4. **恢复覆盖有限：**Godot 核实的是 Vulkan 提交失败分支，Filament 核实的是 fence/submit 相关路径；没有完成所有失败入口、shutdown、callback 取消与设备重建的全链审计。
5. **资产与编译边界：**已核验设备 shader/binding 输入，没有审计完整 shader 编译器依赖、二进制缓存兼容、热重载或自有语言生成 shader ABI 的可行性。
6. **许可范围：**只确认 Godot/Filament 根许可文本和已读文件头，以及 Epic 官方源码访问流程；不宣称依赖闭包已完成许可证合规审查，不作衍生作品法律判断。
7. **版本与性能：**未核验上述标签对应的完整 SHA，未进行 GPU/CPU 测量，也没有以项目文章中的作者测量替代本项目实测。
8. **材料缺口：**没有把未完整观看/取得的作者工程演讲计为已审查证据。Filament ResourceAllocator 的一次候选路径获取失败，未将其作为 aliasing 证据。

## 参考链接

[U1]: https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine?application_version=5.6
[U2]: https://dev.epicgames.com/documentation/en-us/unreal-engine/parallel-rendering-overview-for-unreal-engine?application_version=5.6
[U3]: https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine?application_version=5.6
[U4]: https://dev.epicgames.com/documentation/en-us/unreal-engine/dealing-with-a-gpu-crash-when-using-unreal-engine?application_version=5.6
[U5]: https://www.unrealengine.com/en-US/ue-on-github
[G1]: https://github.com/godotengine/godot/blob/4.4-stable/servers/rendering/rendering_device.cpp
[G2]: https://github.com/godotengine/godot/blob/4.4-stable/servers/rendering/rendering_device_graph.cpp
[G2H]: https://github.com/godotengine/godot/blob/4.4-stable/servers/rendering/rendering_device_graph.h
[G3]: https://github.com/godotengine/godot/blob/4.4-stable/servers/rendering/rendering_device_driver.h
[G4]: https://github.com/godotengine/godot/blob/4.4-stable/drivers/vulkan/rendering_device_driver_vulkan.cpp
[G5]: https://docs.godotengine.org/en/4.4/contributing/development/core_and_modules/internal_rendering_architecture.html
[G6]: https://docs.godotengine.org/en/4.4/classes/class_renderingdevice.html
[G7]: https://github.com/godotengine/godot/blob/4.4-stable/servers/rendering/rendering_device.h
[G8]: https://github.com/godotengine/godot/blob/4.4-stable/LICENSE.txt
[F1]: https://github.com/google/filament/blob/v1.56.0/filament/src/fg/FrameGraph.cpp
[F1H]: https://github.com/google/filament/blob/v1.56.0/filament/src/fg/FrameGraph.h
[F2]: https://github.com/google/filament/blob/v1.56.0/filament/include/filament/Engine.h
[F2C]: https://github.com/google/filament/blob/v1.56.0/filament/src/details/Engine.cpp
[F3]: https://github.com/google/filament/blob/v1.56.0/filament/backend/include/private/backend/CommandStream.h
[F3A]: https://github.com/google/filament/blob/v1.56.0/filament/backend/include/private/backend/DriverAPI.inc
[F4]: https://github.com/google/filament/blob/v1.56.0/filament/backend/src/vulkan/VulkanCommands.cpp
[F4H]: https://github.com/google/filament/blob/v1.56.0/filament/backend/src/vulkan/VulkanCommands.h
[F5]: https://github.com/google/filament/blob/v1.56.0/filament/backend/src/vulkan/VulkanTexture.cpp
[F6]: https://github.com/google/filament/blob/v1.56.0/filament/src/details/Renderer.cpp
[F7]: https://github.com/google/filament/blob/v1.56.0/filament/backend/include/backend/Program.h
[F7B]: https://github.com/google/filament/blob/v1.56.0/filament/backend/include/backend/BufferDescriptor.h
[F8]: https://github.com/google/filament/blob/v1.56.0/filament/backend/CMakeLists.txt
[F9]: https://google.github.io/filament/notes/framegraph.html
[F10]: https://github.com/google/filament/blob/v1.56.0/LICENSE
