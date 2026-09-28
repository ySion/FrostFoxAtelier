# bgfx、SDL GPU、wgpu / wgpu-native：正式 RHI 基础候选比较

历史来源记录：2026-09-25 的选型研究快照，保留固定版本的源码事实、许可证依据、比较理由和证据缺口。文中的“当前”“未确认”均指该次研究；gfx 已成为生产主体，现行选择见[模块决策](../../modules/rhi/decisions.md)，运行证据见[模块验收](../../modules/rhi/acceptance.md)。本文不再提供实施计划或待执行实验。

## 结论摘要

**不选定后端，也不判定任何候选已经能够集成本项目。** 首个渲染消费者、硬件范围、必需功能和并发规模尚未确定；旧占位实现不作为需求依据。

| 候选 | 条件性定位（设计推断，不是采用决定） | 最重要的非薄封装差异 |
|---|---|---|
| bgfx | 可作为接受其帧调度模型的渲染基础；若正式 RHI 要自行拥有提交调度，则更适合作为局部参考 | view 分桶与排序、encoder 收束到 frame、API/render 线程流水线是执行语义，不只是不同函数名 |
| SDL GPU | 可作为以可移植图形/计算功能为边界的应用级 GPU 基础；若目标超出其能力集合或窗口线程契约，应降为局部参考 | command buffer 绑定获取线程并在该线程提交；swapchain 获取绑定窗口创建线程；shader binding 有严格布局约定；cycling 改变资源背后的物理存储 |
| wgpu / wgpu-native | 可作为接受验证、资源状态跟踪和 feature/limit 契约的基础；必须分别验证 Rust 路线与 C ABI 路线 | 安全模型不是可任意删掉的 debug 层；native 扩展和 unsafe 互操作不是标准 WebGPU 可移植保证，也不能从 Rust API 自动推导为 C API 能力 |

依据见 [B1]、[B2]、[S1]–[S6]、[W1]–[W9]。三者都不能仅凭“跨平台”或“更易用”认定为适合自有引擎。

## 1. 来源快照与证据边界

检索记录日期：2026-09-25。外部资料通过 Exa fetch 获取，未使用博客作为结论依据。该次只读研究未安装依赖或运行 GPU 实验；以下保留来源事实与证据限制，不作为生产采用或运行通过的证明。

| 来源组 | 版本状态 | 本次实际核对层级 |
|---|---|---|
| bgfx 官方文档 | 页面标注 `1.146.9292`；URL 是滚动文档，未取得对应提交 SHA，不据此宣称最新 release | internals、tools、build、API 文档；`master/include/bgfx/bgfx.h` 中相关声明与注释；根 LICENSE |
| SDL GPU Wiki | 滚动文档；所核对函数标注自 SDL `3.2.0` 提供，这不是该页面所有扩展属性都存在于 3.2.0 的证明 | API 契约和官方概览；SDL 根许可证；SDL_shadercross README。未追踪各 GPU 后端实现函数 |
| wgpu Rust API | 关键 API 使用版本路径 `29.0.1`；另核对部分滚动架构资料 | Device、Queue、Buffer、CommandEncoder、CommandBuffer、BindGroup、ShaderSource、Surface、CurrentSurfaceTexture 官方生成文档 |
| wgpu-native | `trunk` 滚动源码，无提交 SHA；Cargo.toml 自身版本为 `0.0.0`，不能当 release 版本 | README、Cargo.toml、`ffi/wgpu.h` 部分原生扩展声明与注释、MIT/Apache 许可证全文 |
| wgpu / native 版本关联 | native/trunk 的 core、types、hal、naga 依赖声明为 `29.0.1`；wgpu/trunk workspace 声明为 `30.0.0` | 这是 manifest 声明对比，不是同一发布快照或锁文件解析结果；未证明最新版本、二进制版本或 ABI 对齐 |

**源码核对边界：**本次直接核对公开头文件、manifest 和许可证；没有声称完成后端实现、锁粒度、驱动调用或设备丢失恢复的源码审计。官方文档中的内部机制仍是“官方文档事实”，不是已独立审计实现。

### 一手来源索引

- **[B1]** [bgfx Internals](https://bkaradzic.github.io/bgfx/internals.html)：`Sort-based draw call bucketing`、`API thread and render thread`、`Resource API`、`View API`、`Encoder API`、`Transient buffers`。
- **[B2]** [bgfx 公开头文件](https://raw.githubusercontent.com/bkaradzic/bgfx/master/include/bgfx/bgfx.h)：`BGFX_HANDLE`、`Fatal`、`CallbackI::fatal`、`alloc/copy/makeRef`、`createShader`、`createFrameBuffer(void* _nwh, …)`、`begin/end`、`renderFrame`、`getInternalData/overrideInternal`。滚动源码；按符号定位，不伪造固定行号。
- **[B3]** [bgfx Tools](https://bkaradzic.github.io/bgfx/tools.html)：`Shader Compiler (shaderc)`、`Building shaders`。
- **[B4]** [bgfx Building](https://bkaradzic.github.io/bgfx/build.html)：依赖、amalgamated build；[bgfx LICENSE](https://raw.githubusercontent.com/bkaradzic/bgfx/master/LICENSE)。
- **[B5]** [bgfx API Reference](https://bkaradzic.github.io/bgfx/bgfx.html)：`frame`、`reset`、`RendererType`、capabilities。
- **[S1]** [SDL GPU 概览](https://wiki.libsdl.org/SDL3/CategoryGPU)：工作流、后端要求、uniform、cycling、功能范围。
- **[S2]** [SDL_AcquireGPUCommandBuffer](https://wiki.libsdl.org/SDL3/SDL_AcquireGPUCommandBuffer)。
- **[S3]** [SDL_SubmitGPUCommandBuffer](https://wiki.libsdl.org/SDL3/SDL_SubmitGPUCommandBuffer)。
- **[S4]** [SDL_CreateGPUShader](https://wiki.libsdl.org/SDL3/SDL_CreateGPUShader)：各 shader 格式的资源布局。
- **[S5]** [SDL_ReleaseGPUBuffer](https://wiki.libsdl.org/SDL3/SDL_ReleaseGPUBuffer)。
- **[S6]** [SDL_WaitAndAcquireGPUSwapchainTexture](https://wiki.libsdl.org/SDL3/SDL_WaitAndAcquireGPUSwapchainTexture)。
- **[S7]** [SDL_CreateGPUDeviceWithProperties](https://wiki.libsdl.org/SDL3/SDL_CreateGPUDeviceWithProperties)：shader 格式、后端特定选项、兼容性条件。
- **[S8]** [SDL LICENSE](https://raw.githubusercontent.com/libsdl-org/SDL/main/LICENSE.txt)；[SDL_shadercross 官方 README](https://github.com/libsdl-org/SDL_shadercross)。
- **[W1]** [wgpu-native README](https://github.com/gfx-rs/wgpu-native)；[native Cargo.toml](https://raw.githubusercontent.com/gfx-rs/wgpu-native/trunk/Cargo.toml)；[wgpu Cargo.toml](https://raw.githubusercontent.com/gfx-rs/wgpu/trunk/Cargo.toml)。
- **[W2]** [wgpu 29.0.1 CommandEncoder](https://docs.rs/wgpu/29.0.1/wgpu/struct.CommandEncoder.html)；[CommandBuffer](https://docs.rs/wgpu/29.0.1/wgpu/struct.CommandBuffer.html)；[Queue](https://docs.rs/wgpu/29.0.1/wgpu/struct.Queue.html)。
- **[W3]** [wgpu 29.0.1 Buffer](https://docs.rs/wgpu/29.0.1/wgpu/struct.Buffer.html)；[Device](https://docs.rs/wgpu/29.0.1/wgpu/struct.Device.html)。
- **[W4]** [wgpu 29.0.1 BindGroup](https://docs.rs/wgpu/29.0.1/wgpu/struct.BindGroup.html)；[ShaderSource](https://docs.rs/wgpu/29.0.1/wgpu/enum.ShaderSource.html)。
- **[W5]** [wgpu 29.0.1 Surface](https://docs.rs/wgpu/29.0.1/wgpu/struct.Surface.html)；[CurrentSurfaceTexture](https://docs.rs/wgpu/29.0.1/wgpu/enum.CurrentSurfaceTexture.html)。
- **[W6]** [wgpu-native ffi/wgpu.h](https://raw.githubusercontent.com/gfx-rs/wgpu-native/trunk/ffi/wgpu.h)：`WGPUNativeFeature`、`WGPUNativeSType`、原生 surface 状态。滚动源码。
- **[W7]** [wgpu-hal 架构文档](https://docs.rs/wgpu-hal/latest/wgpu_hal/)：验证职责、primary/secondary backends、安全边界。滚动生成文档，仅解释 wgpu 内部分层，不增加第四个候选。
- **[W8]** [wgpu README](https://raw.githubusercontent.com/gfx-rs/wgpu/trunk/README.md)：后端平台矩阵、Naga 路线；滚动源码。
- **[W9]** [wgpu-native MIT](https://raw.githubusercontent.com/gfx-rs/wgpu-native/trunk/LICENSE.MIT)；[Apache-2.0](https://raw.githubusercontent.com/gfx-rs/wgpu-native/trunk/LICENSE.APACHE)；许可证选择声明见 [W1] manifest。

## 2. 横向机制矩阵

下表为已核对事实；“未知”不表示能力不存在。

| 维度 | bgfx | SDL GPU | wgpu / wgpu-native |
|---|---|---|---|
| 公开抽象 | 类型化资源 handle、view、程序/状态、Encoder；view 描述目标、清除和变换，draw 经排序执行。[B1][B2] | Device、Buffer/Texture/TransferBuffer、graphics/compute pipeline、CommandBuffer、render/compute/copy pass、Fence；应用级图形与计算层。[S1] | Rust 提供 Instance/Adapter/Device/Queue、资源、pipeline、bind group、encoder/pass/command buffer；native 是基于 core 的 C 接口，标准头与 `ffi/wgpu.h` 扩展分开。[W1]–[W4] |
| 录制与提交 | `begin/end` 的 encoder 录制汇入下一帧；`frame` 汇合 encoder 并交换提交/渲染帧；排序、底层调用、flip 在 render thread 执行。[B1][B2] | command buffer 由实现管理；submit 后不能再用。文档保证前次 submission 所有命令开始执行，先于后续 submission 任一命令开始；不能改写成前次工作全部完成。[S2][S3] | `finish` 产生 CommandBuffer，Queue 提交成组命令；Rust CommandBuffer 实现 Send，适合移交。这不是整个 C ABI 所有对象的线程安全证明。[W2] |
| CPU/GPU 所有权与生命周期 | `alloc/copy` 数据由 bgfx 释放；`makeRef` 不复制内容，无释放回调时须跨至少两次 frame 保持数据有效；释放回调可能在任意线程。Transient 数据只在当前帧窗口内有效。[B1][B2] | ReleaseGPUBuffer 在安全时释放底层资源，但调用后应用不可再引用该 buffer；transfer buffer 用于上传/下载，读回可用 fence 等待。[S1][S5] | Buffer 有 clone handle、destroy、异步 map/unmap；内容初始化为零；映射与 GPU 使用互斥。Queue::write_buffer 立即复制到 staging，但传输在下一次 submit 执行。[W2][W3] |
| 状态/同步 | 资源操作记录后交 render thread；创建 handle 可立即供提交引用，不等于调用时 GPU 资源已创建。frame 的 CPU 线程握手不能当 GPU 完成 fence。[B1] | cycling 在资源仍被 pending/active 命令引用时轮换内部资源，必要时新建；轮换后后续命令看到的数据未定义，须重新写入；texture 整体轮换，不只所操作子区域。[S1] | core 承担验证和资源状态/生命周期跟踪；Rust 29.0.1 `transition_resources` 是 native-only 高级 barrier/互操作入口，不能断言完全不暴露状态转换。[W2][W7] |
| 线程模型 | Resource API 内部 mutex 保护；不同 view 可由不同线程设置，同一 view 并发修改不安全；encoder 数有配置上限；end 须在 begin 所在线程。[B1][B2] | 多线程可各自录制；同一 command buffer 只能在获取线程使用，必须在该线程提交。[S1]–[S3] | Rust Queue 可跨线程共享；encoder 可变录制受借用契约约束；map/完成回调由 submit/poll 推进，并在触发处理的线程执行。C 同对象并发/回调重入规则未完整核对。[W2][W3] |
| Binding | Uniform handle 与 texture/buffer/image 等绑定随 draw/dispatch 设置；不是原样暴露后端 descriptor heap。[B2][B3] | 按 shader stage/资源类别分槽，布局符合格式专属规则；每 stage 四个 uniform slot，资源计数须与 shader 一致。[S1][S4] | BindGroup 对应 BindGroupLayout 的资源集合，绑定到 render/compute pass；device 只能使用创建时请求的 feature/limit，不能因硬件支持便越界使用。[W3][W4] |
| Shader 产物 | createShader 接收 shaderc 离线 binary；shaderc 使用 GLSL-like 输入、宏、varying 约定，按目标 profile 产出。[B2][B3] | 创建 device 时声明可提供格式，创建 shader 时提供所选后端格式；有 SPIR-V、DXBC/DXIL、MSL/Metallib。shadercross 是可选独立工具/库。[S1][S7][S8] | Rust ShaderSource 按 feature 支持 WGSL、SPIR-V、GLSL、Naga IR，解析/验证/转换；此 enum 不等于 C ABI。native manifest 声明 WGSL/SPIR-V/GLSL 输入支持。[W1][W4] |
| Presentation | 外部窗口 handle 可用于 framebuffer；多窗口受 SWAP_CHAIN capability 约束且该 framebuffer 不可采样；frame/renderFrame 管呈现；reset 只改 backbuffer 不改窗口尺寸。[B2][B5] | claim SDL_Window；窗口创建线程获取 swapchain texture；仅该 command buffer 可引用，提交自动呈现；获取后不可取消；最小化可成功返回 NULL；swapchain texture 只写。[S6] | Surface 与窗口创建库分开；配置/获取/提交/present 有明确流程。29.0.1 区分 Timeout/Occluded/Outdated/Lost/Validation；重配时不可保有旧 surface texture。[W5] |
| 故障 | Fatal 包括 DeviceLost；CallbackI::fatal 注释要求除 DebugCheck 外不可安全继续，应通知并终止。不能包装成 bool 就声称可恢复。[B2] | 核对的创建/提交/获取 API 以 NULL/false 与 SDL_GetError 报错；最小化 NULL texture 不是错误。统一 device-lost 通知和恢复契约未知。[S2][S3][S6][S7] | Rust Device 有 error scope、未捕获错误和 device-lost callback；Surface 文档说明 device 丢失需重建 device 与资源。C 行为须另核验。[W3][W5] |
| 后端/附加约束 | 枚举包括 D3D11/12、Metal、OpenGL/ES、Vulkan、WebGPU 及平台专属项；枚举不等于机器可用，应查 supported renderer/caps；不能由枚举推断 NDA 平台权限。[B1][B2][B5] | 官方列 Vulkan、D3D12、Metal；D3D12 基线为 Windows10+、FL11_0、Binding Tier2，特定资源数量约束可放宽 Tier1；Vulkan/Metal 也有必需能力和放宽选项。[S1][S7] | 官方矩阵把 Vulkan/Metal/DX12 列为主要路线，GL 是 downlevel/best effort；wasm 可用 WebGPU/WebGL2，但不等于 native C 库提供浏览器部署。[W1][W8] |

## 3. 非薄适配差异、采用与淘汰条件

### 3.1 bgfx：排序与帧模型

**事实：**bgfx 按 view 优先的 key 排序，提交顺序不必是执行顺序；有 Default、Sequential、DepthAscending、DepthDescending。setViewMode 须在向该 view 提交 draw 前设置；compute dispatch 的排序编码采用 sequential 路线。资源命令、draw 和帧交换处于 bgfx 调度体系。[B1]

**设计推断：**全部改为 Sequential 只能改变 view 内 draw 排序，不能自动移除 view 分组、frame 汇合、资源命令时序和 render thread 边界。因此包一层自己的 CommandList 不会自然得到任意命令 DAG 或自有提交器语义。

**事实：**应用可在 init 前调用 renderFrame 接管 render thread 驱动，也可进入单线程模式；这说明执行载体可集成，不说明帧语义被取消。getInternalData/overrideInternal 有 render-thread 限制。[B1][B2]

- **采用条件：**renderer 接受 view/排序；帧级提交和异步资源创建符合消费者；shaderc 可纳入资产链；fatal 策略符合产品要求。
- **淘汰为通用基础的条件：**必需工作流要求无损保留自有命令顺序/资源命令插入点或独立于 frame 的提交，而适配只能新增帧、重放命令或改库；或者必须原进程 device-lost 恢复但固定版本仍只有已核对的 fatal 契约。
- **局部参考：**encoder、handle、资源异步创建与帧级工作汇合。
- **未知：**所需后端实际 capability、销毁到物理回收的细节、原生互操作同步协议、真实 shader 的表达能力、规模与 limits 是否匹配。没有性能上限结论。

### 3.2 SDL GPU：应用工作流和可移植功能边界

**事实：**官方面向广泛硬件支持与可移植功能集合，并提示需要有限硬件支持的前沿功能时可能不适合；与此同时，创建属性确实允许放宽部分硬件要求，不能夸大为“完全没有能力分支或后端选项”。[S1][S7]

**事实：**SPIR-V vertex 资源用 set0、uniform 用 set1；fragment 对应 set2/set3；binding 连续且按 sampler、storage texture、storage buffer 排序。HLSL register/space 和 MSL argument index 有各自映射。[S4]

**设计推断：**离线 binding remap 改变 shader 资产 ABI，不只是重命名 bind 函数；既有任意 register/set 或特殊资源模型需证明转换保真，不可假设 shadercross 自动处理一切。

**设计推断：**窗口线程获取 swapchain，加上 command buffer 同线程使用/提交，使窗口提交必须与窗口线程协调。worker 录制后交给独立 submit thread 的设计不能直接照搬。改成 worker 离屏、窗口线程合成虽可能可行，但属于工作划分变化。

**设计推断：**cycling 不是透明同步开关：同一逻辑资源可能对应另一内部资源；需要稳定原生分配身份或部分更新保留其余内容的上层，必须显式管理版本和重写范围。[S1]

- **采用条件：**必需 workload 在功能范围内；接受 SDL_Window 呈现契约；正确使用 cycling/fence；可统一 binding schema 并管理目标后端产物。纯离屏可以不创建窗口。[S1]
- **淘汰为通用基础的条件：**消费者必需功能未提供；线程安排无法符合窗口契约；资源身份或 shader 要求须侵入后端才能保持。
- **局部参考：**pass API、cycling、统一坐标约定。
- **未知：**device-lost 恢复、完整资源 API 跨线程规则、原生导入/导出与外部同步、shadercross 对项目 shader 的覆盖、各扩展属性最低 SDL 版本。函数 since3.2.0 不代替属性级版本核验。

### 3.3 wgpu：标准、安全层、原生扩展与 C ABI

**事实：**官方架构文档说 core 验证服务于可接收不可信内容的安全场景，须全面跟踪资源状态和生命周期，不是方便时才启用的调试服务。Buffer 零初始化、映射/GPU互斥也是公开契约。[W3][W7]

**设计推断：**不能用“release 关验证”让调用者自行同步或持久映射的模型变成安全 API 的等价用法。必须选择受支持的具体原生路线并承担其责任。

**事实：**原生并非完全受标准 WebGPU 限制。ffi/wgpu.h 有 texture binding arrays、nonuniform indexing、multi-draw indirect count、mappable primary buffers 等扩展和后端条件；ray query 标为 experimental。MappablePrimaryBuffers 放宽的是 usage 组合，不是允许普通 Buffer 映射期间并发供 GPU 使用。[W6][W3]

**事实：**Rust29.0.1 提供 transition_resources，用于 barrier 批处理和 native interop；文档说明多个 command buffer 提交时可能插入修正状态的额外 command buffer。unsafe as_hal、trusted/passthrough shader 入口将安全责任交回调用方；passthrough 不验证输入，可能导致驱动崩溃。[W2][W3]

**设计推断：**escape hatch 不等于自由插入原生操作；资源状态、时序、销毁和验证必须形成完整协议。直接采用内部 HAL 也不是添加几个薄 C 包装：HAL 文档明确需要显式 barrier、调用方验证，且承认安全要求尚未完整文档化。[W7]

- **采用条件：**接受验证与标准化资源/绑定模型；可以 feature 协商；shader 在 Naga 输入范围内，或充分接受 unsafe 路线；C++ 工程能固定 native 库、头文件和工具链组合。
- **淘汰某条接入路线的条件：**必需功能只存在于另一个 Rust 版本或未导出的 HAL；C ABI 必需扩展在固定组合不可实现；只有移除状态跟踪才能匹配消费者而团队不接受接管 unsafe 协议。不能由此淘汰整个家族。
- **局部参考：**验证层与底层后端责任划分、显式 feature/limit 契约、状态跟踪与错误域。
- **未知：**C ABI 同对象并发、引用计数与 destroy/release 组合、回调线程/重入、Rust与C扩展覆盖、二进制 ABI/依赖、目标后端高级功能。Rust Send 不证明任意 C 调用安全。

## 4. 依赖与许可证简核

| 候选 | 已核对 | 不应据此推断 |
|---|---|---|
| bgfx | 根 LICENSE 是 BSD-2-Clause 文本；构建文档列 bx、bimg、GENie，也有 amalgamated 接入方式。[B4] | 根许可证不是所有第三方工具/平台SDK的合规结论；未实际接入项目构建 |
| SDL GPU | SDL 根 LICENSE 为 zlib；shadercross README 声明 zlib；SPIR-V 转换依赖 SPIRV-Cross，DXIL 编译用 dxcompiler/dxil，DXBC 跨平台另有工具依赖。[S8] | SDL GPU 不等于必须运行时携带全部 shader 编译工具；离线/运行时编译应分别盘点；未审计依赖闭包 |
| wgpu-native | manifest 为 MIT OR Apache-2.0，两份文本均取得；声明 cdylib/staticlib；依赖 core/hal/types 和可选 Naga，build-dependency 有 bindgen。[W1][W9] | GitHub 摘要只显示 Apache 不替代双许可证声明；workspace表不等于发布产物依赖；Rust构建依赖不等于C++使用者必然需运行Rust工具链 |

这只是来源核对，不是法律合规意见。固定实际构建配置后，才能形成可信传递依赖清单。

## 5. 历史证据缺口

1. **未固定SHA。**滚动文档和trunk/master不能直接变成依赖锁；版本化Rust文档也不固定native发布组合。
2. **C标准头未完整取得。**`https://raw.githubusercontent.com/gfx-rs/wgpu-native/trunk/ffi/webgpu-headers/webgpu.h` 返回 `CRAWL_NOT_FOUND`；已取得README路径说明、ffi/wgpu.h扩展声明及manifest，未把Rust引用/线程规则冒充为C头核验结果。
3. **旧surface名称不能照搬。**29.0.1的`enum.SurfaceError.html`返回`CRAWL_NOT_FOUND`；随后按Surface文档取得`CurrentSurfaceTexture`，最终报告采用后者。
4. **故障证据不对称。**bgfx有fatal契约；wgpu Rust有设备/表面丢失说明；SDL统一恢复协议及各候选C++宿主实际行为待固定版本验证。未做故障实验。
5. **没有集成或性能结论。**没有编译/链接/运行、依赖闭包审计或工作负载测量。本轮产物是可追踪的语义差异、条件判断与验证路径，不是排名。

**最终判断：**三者均可进入有前提的对抗评估。bgfx问题是是否接受view/frame调度所有权；SDL GPU是是否接受应用级能力边界和窗口/命令线程契约；wgpu-native是是否接受安全状态模型、能否用固定C ABI完成原生能力。应由首个真实消费者和路径实验决定，而非旧接口形状或跨平台宣传。
