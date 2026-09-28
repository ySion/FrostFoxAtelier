# NVRHI 公开契约与作为自有引擎 RHI 基础的代价

历史来源记录：2026-09-25 的选型研究快照，保留固定版本的源码事实、许可证依据、比较理由和证据缺口。文中的“当前”“未确认”均指该次研究；gfx 已成为生产主体，现行选择见[模块决策](../../modules/rhi/decisions.md)，运行证据见[模块验收](../../modules/rhi/acceptance.md)。本文不再提供实施计划或待执行实验。

## 1. 结论摘要

本报告只研究 NVIDIA-RTX/NVRHI，不沿用旧项目接口、交接文件或后端结论，不选择正式后端。已阅读研究约定；外部证据均通过 Exa 搜索或获取。未写仓库、未安装依赖、未执行构建、格式化、lint、tests 或 GPU 实验。

证据标签：**[文档]** 官方文档或公开头文件注释的声明；**[源码]** 实际阅读的实现；**[判断]** 对项目的设计推断；**[未知]** 尚无足够证据或未运行验证。

1. **[文档+源码] NVRHI 是带资源保活、命令实例退休、上传子分配和可选状态追踪的底层执行抽象，不是仅把原生 API 改名的薄包装。** 采用它意味着接受这些机制的部分所有权，而不是只获得三个后端的函数表。[S1：Resources、Command List；S3：ICommandList；S6：CommandList::open/close/executed；S7：CommandListLifetimeTracker::runGarbageCollection]
2. **[文档+源码] “CPU 句柄释放安全”有明确边界。** 普通 binding set 可形成命令实例→binding set→资源的保活链；`trackLiveness=false`、bindless descriptor table、GPU instance buffer 引用 BLAS、原生句柄逃逸不享受同样的完整自动保护。[S1：Binding Layouts and Sets、Bindless Resources；S3：BindingSetDesc、IDescriptorTable、buildTopLevelAccelStructFromBuffer；S12：getNativeObject]
3. **[文档+源码] 自动状态追踪不是跨命令列表、跨队列依赖推导。** 调用方仍要确定资源进入/退出状态以及执行依赖；跨队列等待使用提交返回的实例编号，而不是复用命令列表对象本身作为完成凭据。[S1：State Tracking and Barriers、Synchronization；S5；S8：executeCommandLists/queueWaitForCommandList]
4. **[源码] 公开头文件存在面向多线程提交的 `ICommandListLifetimeTracker`，因此不能引用旧结论把 NVRHI 概括为只能单线程提交。** 但这也不等于任意同队列并发、共享一个 tracker、并发修改 Vulkan 队列待提交 semaphore 状态都安全。[S3：ICommandListLifetimeTracker；S7：Queue::submit]
5. **[判断] “窄适配层采用”值得进入路径验证，但目前不是胜出方案。** 适配层应表达引擎业务句柄、平台启动、shader 产物、呈现和故障协议，避免再包一套完整资源退休器及隐式状态机。若真实消费者必须自控 bindless、内存别名和跨队列调度，NVRHI 自动机制的净收益需要重新测量，不能预设。[依据 S1、S3、S5–S11]

## 2. 来源快照及版本边界

研究记录日期：2026-09-25。

通过 Exa 获取官方 GitHub commit API，得到提交 **`d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076`**，API 记录提交时间为 **2026-09-23T21:03:14Z**、提交说明为合并 optional resource/TLAS memory queries。随后重新获取下列关键文件的固定提交 URL；本报告以这些固定源码为主要依据。该 SHA 是实际返回值，不是推测，也不表示已经完成项目接入验证。

**发现滚动缓存差异：** 最初获取的 `main/include/nvrhi/nvrhi.h` 显示 `c_HeaderVersion=26`，固定 SHA 文件显示 **27**，固定 CMake 还包含默认关闭的 `NVRHI_BUILD_TESTS`。因此不能把不同 `main` 抓取结果拼成同一源码快照，也不能用 CMake 的 `project(... VERSION 1.0.0)` 唯一标识所读代码。[S0、S3、S4]

### 固定提交、已实际阅读的主要来源

| 编号 | 官方来源及实际定位 | 本轮用途 |
|---|---|---|
| S0 | https://api.github.com/repos/NVIDIA-RTX/NVRHI/commits/main — 返回 SHA、commit 时间 | 定位快照；该 API URL 自身是滚动入口 |
| S1 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/doc/ProgrammingGuide.md — Resources、Command List、State Tracking and Barriers、Synchronization、Buffers、Bindings、Readbacks | 公开编程模型 |
| S2 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/doc/Tutorial.md — Creating the Device、Creating the Swap Chain Textures、Filling the Resource Data、Drawing the Geometry | 集成边界及使用顺序 |
| S3 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/include/nvrhi/nvrhi.h — CommandListParameters、ICommandListLifetimeTracker、BindingSetDesc、IDescriptorTable、ICommandList、IDevice | 头文件契约及边界 |
| S4 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/CMakeLists.txt | 开关、targets、依赖版本及构建接入 |
| S5 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/common/state-tracking.cpp — beginTracking*、require*State、keep*InitialStates、commandListSubmitted | 状态追踪实现 |
| S6 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/d3d12/d3d12-commandlist.cpp — open、close、executed、commitDescriptorHeaps | 录制实例、状态收尾、提交与上传归属 |
| S7 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/vulkan/vulkan-queue.cpp — getOrCreateCommandBuffer、submit、runGarbageCollection、queueWaitForSemaphore | Vulkan 提交、退休及队列互操作限制 |
| S8 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/d3d12/d3d12-device.cpp — executeCommandLists、queueWaitForCommandList、CommandListLifetimeTracker、waitForIdle | D3D12 完成语义及错误路径 |
| S9 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/vulkan/vulkan-device.cpp — 构造函数、executeCommandLists、waitForIdle、runGarbageCollection | Vulkan 设备包装与错误路径 |
| S10a | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/d3d12/d3d12-upload.cpp — suballocateBuffer、submitChunks | 上传与 scratch 子分配 |
| S10b | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/vulkan/vulkan-upload.cpp — suballocateBuffer、submitChunks | Vulkan 上传重用 |
| S11 | https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/src/vulkan/vulkan-buffer.cpp — writeVolatileBuffer、submitVolatileBuffers、writeBuffer、copyBuffer、createHandleForNativeBuffer、Buffer::~Buffer | 录制保活、VCB、native import |

### 实际阅读、但仅取得滚动版本的补充来源

- **S12**：https://github.com/NVIDIA-RTX/NVRHI/blob/main/include/nvrhi/common/resource.h — `IResource`、`RefCountPtr`、`RefCounter`。读取到 `getNativeObject` 不 AddRef、引用计数实现；不冒充固定 SHA 核验。
- **S13a**：https://github.com/NVIDIA-RTX/NVRHI/blob/main/include/nvrhi/d3d12.h — `DeviceDesc`、后端专用接口。
- **S13b**：https://github.com/NVIDIA-RTX/NVRHI/blob/main/include/nvrhi/vulkan.h — `DeviceDesc`、queue semaphore 接口及 feature 声明字段。
- **S14**：https://github.com/NVIDIA-RTX/NVRHI/blob/main/LICENSE.txt — 实际读取 MIT 文本。
- **S15**：https://github.com/NVIDIA-RTX/NVRHI/blob/main/cmake/FetchAftermath.cmake — 实际读取下载路径、hash 和 imported library 配置。
- **S16**：https://github.com/NVIDIA-RTX/NVRHI/blob/main/README.md — 后端/平台声明、集成说明。README 是滚动来源，构建细节以固定 S4 优先。

精确定位采用“文件 URL + 类型/函数/章节”，没有把 Exa 转换后文本行号伪装成 GitHub 源码行号。Exa 清洗会吞掉部分尖括号内容，故本报告不把抓取文本作为可直接编译的源码副本。

## 3. 共同机制矩阵

| 维度 | 已核对契约 | 引擎仍需承担的责任与代价 |
|---|---|---|
| 公开抽象 | **[文档]** `IDevice`、`ICommandList`、资源、heap、pipeline、framebuffer、binding layout/set、descriptor table；公开 queue 枚举为 Graphics/Compute/Copy。[S3] | **[判断]** 不应将其当作任意队列拓扑、renderer 或 render graph。需先将真实消费者需求映射到这些能力。 |
| CPU 所有权 | **[文档+源码]** intrusive 引用计数句柄，强引用释放到零销毁；native object 返回不 AddRef。[S1、S12] | **[判断]** 自有语言句柄表可以拥有一个 NVRHI 强引用，但不能让业务句柄失效直接等价于原生资源销毁。 |
| GPU 保活 | **[文档+源码]** 录制实例记录引用；提交后 tracker 保留到 GPU 完成并 GC；binding set 可传递保活。[S1、S6、S7、S8、S11] | `trackLiveness=false`、descriptor table、native import/escape、GPU BLAS 引用须单独制定责任表。[S1、S3、S11] |
| 状态与同步 | **[文档+源码]** 入口状态必须可知；退出状态由显式转换、keepInitialState 或 permanent-state 约定解决；队列间有显式 wait。[S1、S3、S5、S8] | **[判断]** 状态正确不等于执行顺序正确；图调度与平台同步仍在引擎。 |
| 绑定与 Shader | **[文档]** layout/set 不可变；set 顺序匹配 pipeline layouts；shader creation 接收二进制；Vulkan slot offsets 要与编译产物一致。[S1–S3] | 产物编译、缓存、变体、热重载及 shader/layout ABI 校验不能靠普通 createShader 代办。 |
| 录制并发 | **[文档]** 多个 command list 可并发录制；D3D11 仅一个 immediate list 可 open。[S1、S3] | **[未知]** 本轮不为同一 list 同时被多个线程调用提供安全结论。 |
| 提交并发 | **[文档]** 每个提交线程、每条提交队列应有自己的 lifetime tracker；D3D11 不支持多线程提交。[S3] | 自定义 tracker 必须自行 GC；同一队列外部 wait/signal 状态的组合操作仍需验证/串行化边界。[S7] |
| 呈现 | **[文档]** 不创建 swapchain，不提供 present；可以包装 native swapchain texture。[S2] | 窗口、swapchain acquire/present、resize、失效重建、节流及同步策略属于平台层；教程不是完整的呈现协议。 |
| 故障 | **[源码]** 有 message callback、验证层、device-removed 检查及 Vulkan DeviceLostError 捕获；各路径返回行为不同。[S7–S9] | **[未知/判断]** 未确认统一恢复状态机；不能声称它会自动重建 device、资源和 swapchain，也不能说完全不处理设备丢失。 |
| 依赖/许可 | **[源码]** 后端及扩展开关可选，默认 header FetchContent；库自身 MIT 文本已读。[S4、S14、S15] | 固定 SHA、依赖快照、SDK 与分发许可核对、构建隔离仍需项目承担；不能把整个依赖闭包直接称为 MIT。 |

## 4. 生命周期、录制实例与 GC

### 4.1 不能合并成一个“资源活着”的三段生命周期

**[文档+源码]** 至少区分：

1. **CPU 逻辑拥有期**：调用方持有 `TextureHandle` 等引用。
2. **已录制但未完成的使用期**：调用方释放原始句柄后，录制实例仍可能拥有强引用；不要求等 execute 后才保活。
3. **GPU 已完成但尚未收集期**：GPU 完成不自动等价于内部引用即时释放；必须执行相关 GC。

具体源码证据：Vulkan `copyBuffer` 把源/目标资源加入当前 command buffer 的引用容器，`writeBuffer` 在录制时加入目标 buffer；D3D12 `open` 创建新的 instance，`executed` 把 instance 转交指定 tracker 或 queue 内部 tracker；Vulkan GC 在 submission ID 已完成时清空资源引用再把 command buffer 放回 pool。[S6：open/executed；S7：CommandListLifetimeTracker::runGarbageCollection；S11：copyBuffer/writeBuffer]

**[判断]** 引擎的 generational handle 可以在 CPU 侧先失效，NVRHI 资源继续在 GPU 使用期存活；这是两个不同事件，不是错误。风险在于适配层又建立按帧计数的第二套退休队列，并误以为“过了 N 帧”就能释放 native memory。

### 4.2 CommandList 对象不是一次提交

**[文档]** 同一个 NVRHI list 可以 record→close→execute 后立即再次 open，不必等上一轮 GPU 完成。D3D12 包装对象可持有多个原生命令列表/allocator 实例；Vulkan 从 queue 请求可用 command buffer。`close` 后直接 reopen、不 execute 被允许，但头文件明确标为未充分测试。[S3：ICommandList；S6：open/executed]

**[源码]** D3D12 `executeCommandLists` 一批命令列表执行后 signal queue fence，各 list 被赋予该次提交实例；Vulkan 一批 submit 使用同一个新 submission ID。[S8：executeCommandLists；S7：Queue::submit]

**[判断]** 引擎完成 token 至少应保留设备世代、队列和提交实例，不能只保留 `ICommandList*` 或假定不同队列的数字可相互比较。“设备世代”是建议的引擎设计字段，不是 NVRHI 已提供的恢复机制。

### 4.3 GC 的实际责任

- **[文档]** `IDevice::runGarbageCollection` 至少每帧调用；外部 tracker 的同名函数应频繁调用，例如每帧或每 simulation step。[S3]
- **[源码]** D3D12 device GC 遍历的是队列自己的 tracker；Vulkan device GC 同样进入各 queue 的内部 tracker。自定义 tracker 由 command list 明确选用，不能假定 device GC 顺便收集它。[S6–S9]
- **[源码]** `waitForIdle` 与 `runGarbageCollection` 是不同操作；已读实现中的 wait 不替调用者遍历自定义 tracker。[S8、S9]
- **[判断]** 关闭渲染会话时要有停止提交→等待/故障分支→处理每个 tracker→释放引用及平台对象的明确协议。正常 shutdown 和 device-lost shutdown 不能未经验证共用无限等待。

### 4.4 保护边界

**[文档]** 普通 binding set 持有所绑定资源的强引用；关闭它的 `trackLiveness` 仅把“set 不被提前释放”的责任转交应用，不是给 set 内资源建立新的弱引用模型。descriptor table 则明确不对所引用资源做强引用、状态追踪或自动 barrier；table 更新与 GPU 使用需要应用同步。[S1：Binding Layouts and Sets、Bindless Resources；S3：BindingSetDesc、IDescriptorTable]

**[源码]** Vulkan `createHandleForNativeBuffer` 设置 `managed=false`，析构只在 `managed` 分支销毁 VkBuffer。包装对象存在不能阻止外部 owner 先销毁原生 buffer。[S11]

**[文档]** `getNativeObject` 不 AddRef；`buildTopLevelAccelStructFromBuffer` 不对 GPU instance 数据中的 BLAS 做状态和 liveness tracking。[S12；S3]

**[未知]** 未完成所有后端 native texture/buffer import 的逐项所有权审计，不能把 Vulkan buffer 结论推广成全部 import API 的统一行为；尤其不能把教程所说的 swapchain texture 强引用解释为 Vulkan 原生 image 自带引用计数。

## 5. 状态追踪与跨队列同步

### 5.1 三种合法入口/出口策略

| 策略 | 已核对要求 | 使用条件/反例 |
|---|---|---|
| 显式追踪 | open 后首次使用前 `beginTracking*State` 告知真实 prior state；结束前 `set*State` 转换到约定出口，必要时 `commitBarriers`。[S3、S5] | **[判断]** 适合图编译器已知边界状态；不能用 beginTracking 冒充执行了转换，它只是告诉追踪器现状。 |
| keepInitialState | 描述符提供 `initialState` 与 `keepInitialState=true`；close 时恢复初始状态。[S3、S5、S6] | **[判断]** 适合边界固定、录制次序与执行次序解耦；反例是图已优化出连续使用状态，却被 close 的回归转换重新拆开。 |
| permanent state | 状态变为永久后不再改变；请求的使用状态必须兼容；永久标记在设置它的 list 被 execute 后对后续 list 生效。[S3、S5] | **[判断]** 适合上传完成后长期只读；不适合后续反复 UAV 写入或资源重新用途化。 |

**[源码细化]** keepInitialState 并非对所有新 texture 无条件从描述符状态开始：公共 tracker 在 texture 尚未 `stateInitialized` 时使用 Common，提交后更新初始化标记。故新建资源第一轮、导入资源及后续轮次不应只凭概念图实现；引擎要尊重后端真实创建状态。[S5：getTextureStateTracking、commandListSubmitted]

**[源码]** 未知 prior state 会进入 error callback；永久状态不兼容会报错。`beginTracking*` 实现主要修改 tracking 数据，不生成转换 barrier；`require*State` 生成 pending barrier。[S5]

### 5.2 自动 barrier 不包办依赖

- **[文档]** 自动 barrier 可关闭，设置在 close/reopen 间保留；关闭后由应用负责完整状态操纵。[S1：State Tracking and Barriers]
- **[文档]** 仅更新常量、不重新 set state 的连续 compute dispatch，不一定产生所需 UAV barrier；提供显式 UAV barrier 工具处理此情况。[S1]
- **[源码]** D3D12 close 即使在常规状态收尾路径也会调用 `keep*InitialStates` 并 commit。因此“关闭自动 barrier”与“关闭 keepInitialState 出口恢复”不是同一开关。[S6：close]
- **[文档+源码]** `queueWaitForCommandList(waitQueue, executionQueue, instance)` 使用 execute 返回的实例；D3D12 映射 queue fence wait，Vulkan 通过其 timeline semaphore 接入等待。[S1：Synchronization；S7、S8]

**[判断]** 图调度器可作为依赖和资源状态的唯一决策者，但必须明确 NVRHI 是按它的指令落 barrier，还是自行追踪某组资源。不能同时让 render graph、适配层和 NVRHI 分别猜测同一资源的“当前状态”。

## 6. 绑定、Shader 与上传管理

### 6.1 Binding 与 shader 产物

**[文档]** 普通 layout/set 不可变；pipeline 的 layouts 与 draw/dispatch state 的 sets 必须按相同顺序对应。binding item 可表达 subresource/format，原生 view 由 NVRHI 建立。HLSL t/u/b/s 槽位映射与 Vulkan descriptor namespace 不同，需要编译端和 `VulkanBindingOffsets` 约定一致。[S1：Binding Layouts and Sets、Vulkan Binding Offsets；S2：Creating a Graphics Pipeline]

**[头文件]** `createShader` 和 `createShaderLibrary` 接收 binary 与长度，而不是源代码及编译参数。[S3：IDevice]

**[判断]** NVRHI 可以承接已编译产物，但项目必须另定源语言、编译器、DX/SPIR-V 输出、反射/布局一致性、缓存键与热重载。这里不声称完整 shader 工具链在 NVIDIA 生态中不存在，只说这些职责不是上述公开 creation 调用提供的服务。

### 6.2 Volatile constant buffers 不是普通持久 buffer

**[文档]** VCB 每个 command-list 实例首次使用前必须 write；close 后其语义内容失效，新实例必须重写。一个实例内多次 write 按 API 顺序影响后续 shader 使用。应在 set state 前首次写入；push constants 则在 set state 后设置，公开大小上限为 128 bytes。[S1：Buffers、Push Constants、Pipelines and States；S3]

| 后端 | 已核对机制 | 采用代价 |
|---|---|---|
| D3D11 | **[文档]** DYNAMIC buffer；实现可能跨实例残留内容，但不能依赖此后端偶然行为。[S1] | 跨后端代码必须遵守每实例重写。 |
| D3D12 | **[文档+源码]** 每次 write 从 list upload 存储子分配，使用 root CBV；提交后 chunks 从 recording version 转成 submitted version。[S1、S6、S10a] | 长寿命 list 的上传高水位与重用归属必须纳入预算。 |
| Vulkan | **[文档+源码]** 固定 `maxVersions` 环状版本；源码用原子 compare-exchange 抢占可用版本，已录制未提交版本不可重用；耗尽时报告错误并返回，非自动扩容。[S1、S11：writeVolatileBuffer] | 文档按每帧次数×在途帧数估计，实际还需覆盖并发录制但未提交的版本；不能只按 swapchain 数量估计。后一项是基于源码的容量判断。 |

### 6.3 上传池与 scratch 池

**[文档]** `writeBuffer/writeTexture` 提供自动上传路径；也可显式创建 upload/staging 对象、映射后 copy。每个 list 的 upload/scratch working set 不主动收缩；大批量上传建议专用 list，结束后销毁。[S1：Command List；S2：Filling the Resource Data；S3：ICommandList]

**[源码]** upload manager 先使用当前 chunk，再寻找 submission 已完成且容量足够的 chunk，否则分配；`submitChunks` 把 recording version 转为 submitted version。D3D12 upload manager 的构造 memoryLimit 为 0，scratch 使用 `scratchMaxMemory`；后者不是通用上传池预算。[S6：构造函数；S10a、S10b]

**[源码]** Vulkan 普通 buffer 写入还有满足大小及对齐条件时走 `vkCmdUpdateBuffer` 的分支，不能把所有 writeBuffer 都描述成一次 staging buffer copy。[S11：writeBuffer]

**[判断]** 自有上传服务可以负责请求排队和资源流送策略，但如果最终再调用自动上传接口，不能宣称完全控制了 staging 分配、峰值或复用策略；应选择“请求调度交给引擎、存储子分配交给 NVRHI”，或者确实使用显式 staging 路径。GC 已释放飞行引用不代表 upload pool 已归还内存。

## 7. 线程、队列和提交约束

1. **[文档]** D3D12/Vulkan 可并发录制多个 list；D3D11 的 list 映射同一 immediate context，所有 list 都需要 immediate 参数，且仅一个 list 同时 open。[S1、S3]
2. **[文档]** 每个提交线程应为它提交的每个 queue 拥有独立 `ICommandListLifetimeTracker`；createCommandList 时指定。tracker 不是免费后台服务，应用负责频繁 GC。[S3]
3. **[源码]** Vulkan `Queue::submit` 有 mutex，但注释明确指出 `queueWaitForSemaphore/queueSignalSemaphore` 操纵队列持久 semaphore 列表，不适合与同队列多线程提交随意混用；GC 和 pool 回收也有各自同步边界。[S7]
4. **[源码+未知]** D3D12 已读 `executeCommandLists` 顺序为 Execute→Signal→逐 list executed；该函数本体未展示同队列提交事务锁。未完成 queue 字段和所有调用路径的并发审计，**不据此断言存在数据竞争，也不声称同队列多 producer 已获完整安全保证**。[S8]
5. **[文档+源码]** Guide 概括 graphics queue 必需，但实际 D3D12/Vulkan device 构造函数均按各个传入 queue 是否存在选择创建包装；滚动 Vulkan DeviceDesc 更写明不用的 queue 可为空。[S1：Device；S8、S9：构造函数；S13b] **[未知]** 这不是已经验证的纯 compute 产品支持承诺；需要初始化、查询和 shutdown 真实路径试验。
6. **[判断]** 若先验证窄适配层，可以按 queue 定义提交事务边界，把 native acquire wait、NVRHI submit 和 native signal 的相关操作放在同一 owner 路径。是否需要多线程同队列直接提交由真实工作负载决定，不将单队列/单线程写入正式需求。

## 8. 原生平台、呈现与故障边界

### 8.1 明确不承担的工作

- **[文档]** 原生 GAPI device 由应用创建，NVRHI createDevice 创建包装；D3D12/Vulkan 描述符接收原生 device、queue 等。[S1：Device；S2：Creating the Device；S13a、S13b]
- **[文档]** 不创建 swapchain、不提供 presentation；native swapchain texture 可包装用于 framebuffer。多 image 和显式同步被教程留给应用，并指向 Donut DeviceManager 示例。[S2]
- **[判断]** 因而窗口/事件、adapter 与 feature 选择、swapchain 策略、resize、acquire/present、帧节流不是采用 NVRHI 后消失的工作。Donut 是教程提供的外部参考，不是本报告批准的新运行时依赖。

### 8.2 设备丢失：有检测，不等于恢复闭环

**[头文件]** `waitForIdle` 的注释允许检测问题时返回 false，例如 device removal。[S3]

**[源码]** Vulkan `waitForIdle` 捕获 `vk::DeviceLostError` 返回 false；Vulkan queue submit 捕获 device-lost、发 message callback，随后仍返回提交 ID；D3D12 execute 检查 `GetDeviceRemovedReason`、发错误消息，但仍返回实例编号；本次读到的 D3D12 `waitForIdle` 正常函数末尾直接返回 true。[S7：submit；S8：executeCommandLists/waitForIdle；S9：waitForIdle]

**[判断]** 非零提交 token 不能当作成功执行或设备健康证明，单独依赖 `waitForIdle` bool 也不足以构成跨后端统一故障协议。引擎需要捕获错误、停止继续提交、隔离旧 device 世代和决定退出/重建。

**[未知]** 本轮没有找到并验证完整的跨后端 device recreation、资源重放、pipeline cache 恢复和 UI 故障呈现契约。只能报告“本轮未确认”，不能把文档缺失升级为无法自行恢复或完全不支持诊断。Aftermath 是可选诊断依赖，不是已验证的自动恢复机制。[S4、S15]

## 9. 依赖、可关闭选项和固定版本接入

### 9.1 固定 S4 的构建事实

| 项目 | 已读事实 |
|---|---|
| 基线 | CMake `3.11`，C++17。滚动 README 写 CMake 3.10，与实际固定 CMake 不一致，以 S4 为构建依据。 |
| 默认后端 | Vulkan ON；Windows 上 D3D11、D3D12 默认 ON；可分别关闭。 |
| 验证层 | `NVRHI_WITH_VALIDATION=ON`，可关闭；关闭不表示原生 API validation 自动开启。 |
| 扩展 | `NVRHI_WITH_NVAPI`、`NVRHI_WITH_RTXMU`、`NVRHI_WITH_AFTERMATH` 默认 OFF。 |
| D3D12 OMM | `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=OFF`，是独立可选项。 |
| 构建形式 | `NVRHI_BUILD_SHARED=OFF`；静态 targets 为 `nvrhi` 与选择的 `nvrhi_d3d11/nvrhi_d3d12/nvrhi_vk`；shared 形式合并到 nvrhi。 |
| 测试/安装 | `NVRHI_BUILD_TESTS=OFF`、`NVRHI_INSTALL=ON`、install exports 默认 OFF。列出这些选项不表示本轮运行了它们。 |
| Vulkan headers | 默认允许 FetchContent；tag 为 `v1.4.352`；可事先提供 `Vulkan::Headers` 或 `Vulkan-Headers`，并关闭 fetch。 |
| DirectX headers | D3D12 下默认允许 FetchContent；tag 为 `v1.717.0-preview`；需要 DirectX-Headers 与 DirectX-Guids targets，可外部提供。 |
| RTXMU | 开启后 FetchContent，默认 tag `v1.4`；不能据此认为未启用 RTXMU 就没有基本 ray tracing。 |
| NVAPI | 可提供已有 target，否则 find_package；不是核心必需依赖。 |
| Aftermath | 已读滚动 fetch 文件的默认 Windows/Linux archive 为 SDK `2025.1.0.25009`，设置 MD5 并创建 imported library；关闭此选项可避免该路径。[S15，滚动] |

以上均来源于 S4，Aftermath archive 细节单独标为 S15 滚动来源。

### 9.2 固定版本不等于只锁 NVRHI 一个 SHA

**[判断]** 路径验证的锁定清单应包含 NVRHI SHA、依赖 tag 对应的不可变提交/包 hash、所用 SDK、shader compiler 及编译参数、启用的 options、静态/动态和运行时库策略。`c_HeaderVersion=27` 可做 header/implementation 一致性检查，但不是完整 ABI、编译器及 C++ runtime 兼容性保证。[依据 S3：verifyHeaderVersion；S4]

**[源码]** 外部 header targets 优先于自动 fetch；fetch 关闭但目标缺失会产生 CMake FATAL_ERROR。因此可做离线、项目统一供给依赖的接入方案，但本轮没有证明所有选项组合已经配置成功。[S4]

**[源码+待验证风险]** RTXMU 分支直接读取 Vulkan header target 属性，而该 target 的初始化在 Vulkan 分支内；`RTXMU=ON、Vulkan=OFF、D3D12=ON` 是值得单独配置验证的组合。这里仅标记可见的 CMake 耦合，不宣称已经复现配置失败。[S4：NVRHI_WITH_RTXMU 分支]

**[文档]** 滚动 README 声明 Windows x64、Linux x64/ARM64，并给出 D3D12 Windows SDK 基线。[S16] **[判断]** 这些声明不能证明项目现有工具链、目标硬件、预览 DirectX-Headers 或所有扩展可用；也不能据此预选 D3D12。

### 9.3 许可证核对边界

**[原文已读]** NVRHI 根 LICENSE.txt 是 MIT 文本，包含许可、保留版权/许可通知条件及免责声明；但本次该文件只按 main 获取，尚未将许可证原文固定到同一 SHA。[S14]

**[未知]** 没有完成 Vulkan-Headers、DirectX-Headers、RTXMU、NVAPI、Aftermath 及最终分发闭包的许可证逐文件审计。不把 NVRHI 的 MIT 延伸成所有可选 SDK 的许可结论，也不作法律兼容性结论。关闭未使用扩展能减少依赖面；最终可分发内容仍需以实际锁定包的许可/NOTICE 为准。

## 10. 三种使用方式：条件与反例

| 方式 | 适用条件 [判断] | 主要代价 [判断] | 能推翻采用理由的反例 |
|---|---|---|---|
| 直接采用 NVRHI 作为 renderer 的 RHI API | renderer 主要为 C++；接受其资源、binding、command-list 语义；目标能力落在经验证后端集合；愿意把平台和 shader 工具链另做。 | 上层类型和调用次序依赖 NVRHI；升级跟踪公开头文件与后端差异；原生 escape 需要严格规约。 | 自有语言接口必须稳定而调用方大量暴露 NVRHI C++ 描述符；或首个真实消费者主要使用 bindless/GPU 构建而关键保活与状态已全部转回应用。 |
| 窄适配层采用 | 引擎需要业务句柄/设备世代、稳定语言桥及统一错误入口，但能够明确把普通 GPU 保活和基础资源实现交给 NVRHI。 | 适配层维护句柄映射、binding/shader ABI、队列 token、平台边界；不能逐方法机械复制完整 API。 | wrapper 同时维护自己的延迟删除队列、全局状态缓存和隐藏 staging pool，导致同一资源三套相互猜测的状态/生命周期机制；native import 的 owner 无法说清。 |
| 只借鉴机制，自建正式 RHI | 真实需求证明必须拥有完全不同的 queue、descriptor、memory-aliasing 或语言 ABI 契约；且团队承担后端实现与验证责任。 | 必须自己证明录制保活、提交实例、fence 退休、barrier 和错误处理，不再享有库维护成果。 | 真实消费者只需要普通图形/compute 和绑定集合，NVRHI 直接路径已经满足约束；自建只重复其机制，却没有可测量的接口或控制需求收益。 |

**[判断] 双重管理风险的切分原则：**

- 业务句柄失效归引擎；普通 NVRHI 对象引用退休归 NVRHI；外部 native memory 的最终释放归明确的外部 owner。
- 若采用图编译器，图负责执行依赖和边界状态；NVRHI 自动 tracking 或手动 barrier 路径应按资源/调用范围有一份明确所有权表。
- bindless 和 GPU instance 引用是显式例外，不能沿用普通 binding set 的 fire-and-forget 假设。
- GC 驱动、上传 pool 回收、逻辑资源卸载分别记录；不能只用“资源数量归零”证明 native 内存已归还。

这些是待对抗评估的设计候选，不是已批准的项目接口。

## 11. 尚未解决的问题

1. 项目首个真实消费者、必须支持的 shader/图形能力、硬件范围、并发规模和第二后端期限仍未确认；不评分、不排序赢家。
2. 同队列多 producer 的完整 D3D12 同步边界、自定义 tracker 的跨线程调用纪律和 native semaphore 事务边界尚需路径验证；头文件声明不能替代整个调用组合的证明。
3. 原生 import 对 device、texture、buffer、heap 的逐后端所有权清单未完成；本轮只确认 Vulkan native buffer 的 non-managed 行为及不 AddRef 的 native getter。
4. 固定版 Guide 仍称不支持 tiled/sparse texture，而头文件有 isTiled/tile mapping API，固定 Vulkan device/queue 实现也含 sparse 操作。**不能依据 Guide 那句话直接否决该能力；实际支持条件、限制及后端一致性未验证。**[S1：Textures；S3；S7、S9]
5. 文档中的“低开销”“便宜”等是作者描述，不是本项目测量；未测 CPU 录制/提交成本、descriptor churn、峰值显存/系统内存、常量版本压力。
6. 可选 SDK 全闭包许可证、固定 header tags 的包内容、预览 header 与项目工具链适配尚未验证。
7. 设备丢失后的等待安全性、清理顺序和重建协议没有统一运行证据；错误消息不是恢复验收。
