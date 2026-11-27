# RHI 与 RDG 当前契约

本文件描述 [rhi.hpp](../../../modules/render/rhi/include/rhi/rhi.hpp)、[backend.hpp](../../../modules/render/rhi/internal/rhi/backend.hpp) 与 [graph.hpp](../../../modules/render/rdg/internal/rdg/graph.hpp) 基于固定 NRI 补丁实际提供的契约；具体依赖版本由 [dependencies-lock.json](../../../cmake/dependencies-lock.json) 指定。选型理由只维护于 [decisions.md](decisions.md)。

## 执行域与所有权

- `Device::open` 通过 `IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` 按高性能顺序筛选同时支持 Feature Level 12_1、Mesh Shader Tier 1、DXR 1.1 Inline RayQuery（Raytracing Tier 1.1）、`SV_Barycentrics`、D3D12 Enhanced Barriers 与 Shader Model 6.6 的硬件适配器（优先连接桌面输出的物理显卡，无桌面输出时回退首个合格硬件卡）；无合格硬件返回 `ErrorCode::unsupported`，不提供 WARP 或旧硬件回退。`wait_timeout_ms` 在创建前校验为有限正整数。
- 一个进程通常持有一个共享 `Device`，内部包含 Graphics、Async Compute（硬件支持时）与 Copy（硬件支持时）三条队列及各自单调递增的 64 位时间线围栏（`QueueTimeline`）。硬件不提供独立 Compute/Copy 队列时自动解析到 Graphics 队列。
- 全局统一绑定布局（Universal SM 6.6 Bindless Layout）：全引擎共享唯一的 `nri::PipelineLayout`——`register(b0, space0)` 64 字节（16 DWORD）根常量，配合 `RESOURCE_HEAP_DIRECTLY_INDEXED`（容量 262,144，槽位 0 保留为空哨兵，着色器统一经掩码边界守卫访问 `ResourceDescriptorHeap` 以杜绝 RDNA 2 标量预取有符号偏移越界）与 `SAMPLER_HEAP_DIRECTLY_INDEXED`（固定静态采样器表 `SamplerDescriptorHeap[0..8]`）。不再按着色器反射拼装根签名或逐帧拷贝描述符表。
- `Device`、`WindowSurface`、`Pipeline` 不可复制、可移动。`Pipeline` 共享持有底层 `DeviceState`；`Device` 外壳提前析构不会销毁仍被存活 `Pipeline` 引用的执行域。所有状态变更与最终析构必须在创建该 `Device` 的 owner 线程执行；违规跨线程析构或无法安全排空的析构直接进入项目唯一 `atelier::fatal` 出口。
- `Validation::gpu` 映射到 D3D12 Debug Layer + GPU-Based Validation。环境变量 `ATELIER_GPU_VALIDATION` 在 `Device::open` 读取：`debug` 将请求的 `gpu` 降级为 `debug_layer` 并向 `stderr` 输出一行说明；未设置或 `gbv` 保持原请求；其他值返回 `ErrorCode::invalid_argument`。开启验证层时启用详细设备移除诊断（DRED），并过滤 Agility SDK 1.620+ 已知的无害内部信息通知（Message ID 989）。

## 管线准备与替换

`BackendAccess::compile_pipeline(Device&, PipelineCompileDesc)` 在 owner 线程同步返回完整可用的 `Pipeline`；不存在半成品管线对象。

- 支持 `PipelineKind::compute`、`PipelineKind::vertex` 与 `PipelineKind::mesh`，默认 `shader_model = "6_6"`（十进制 `major_minor` 格式）。阶段源码仅在调用期间借用。
- 所有图形与计算 PSO 均绑定同一个全局 SM 6.6 Bindless `PipelineLayout`，编译阶段无需反射描述符槽位表。
- 替换管线采用“先编译新候选，成功后移动赋值替换旧 `Pipeline`”策略；失败不破坏旧管线。旧 PSO 与延迟释放的缓冲、纹理、加速结构及 Bindless 槽位统一交由 `DeferredReclaimer` 在三队列全部越过提交围栏水位后物理回收。

## 异步准备与 DXIL 磁盘缓存

`BackendAccess::submit_pipeline` 与 `compile_pipeline` 共用同一套 `PreparationJob` 编译、PSO 创建与 GBV 预热逻辑：

- `submit_pipeline` 在 owner 线程校验描述并深拷贝源码与状态，随后投递至最多 `min(8, 硬件线程数)` 个后台工作线程（各自持有独立 DXC 实例）。工作线程启动失败按宿主资源耗尽直接 `atelier::fatal`。
- `Validation::gpu` 启用时，工作线程在不提交的独立命令列表上录制一次 `SetPipelineState` 以在后台完成 GBV 着色器补丁预热，避免阻塞 owner 线程首次录制。
- `take_pipeline` 仅在 owner 线程非阻塞查询：未完成返回 `std::nullopt`，完成后恰好一次返回 `Result<Pipeline>`。丢弃 `PipelineJob` 即取消排队作业；`Device::close` 会取消排队项并等待正在执行的单步编译结束。
- `DeviceOptions::shader_cache` 为多进程可共享的持久 DXIL 缓存目录（空路径表示禁用）：键为覆盖缓存版本、DXC 版本与提交号、编译参数及完整源码的 SHA-256；条目带魔数与 SHA-256 校验，损坏或截断条目自动按未命中重编并以临时文件原子替换；单进程首次写入时若目录超过 512 MiB 则按修改时间淘汰至 256 MiB 以内。

## Render Dependency Graph（`atelier_rdg`）

`rdg::Graph` 每帧在线性 `LinearArena` 上构建，由 `rdg::Executor::execute` 编译并提交：

- **拓扑裁剪与反向活性分析**：仅保留写入外部导出资源（如交换链后台缓冲、持久历史纹理、读回缓冲）或显式声明 `mark_side_effect()` 的通道及其上游生产者，自动剔除未消费的死通道与无用瞬态资源。
- **D3D12 Enhanced Barriers 与跨队列转换律**：自动追踪每张纹理与缓冲的 `AccessBits`、`Layout` 与 `StageBits`。当资源在 Graphics 与 Async Compute 队列间交接时，严格在能力更强的队列（Graphics > Compute > Copy）侧发出匹配的布局转换屏障并插入跨队列时间线围栏同步。复用或导入的持久/瞬态纹理若前序阶段非空，即使采用 `Layout::UNDEFINED` 丢弃旧内容，也保留合法前序 `StageBits` 以满足 Enhanced Barriers 同步契约。
- **瞬态 Placed Resource 别名池**：图内声明的瞬态纹理与缓冲基于生命周期区间（`[first_pass, last_pass]`）在不重叠通道间复用 64 KiB 对齐的默认堆内存块（Buffer 满足 256 字节子分配对齐）及缓存的 Bindless SRV/UAV/RTV/DSV 视图；连续静态帧零驱动堆分配。
- **区间并集校准 GPU 计时器**：为每个活跃通道录制硬件时间戳并在 3 槽回读环完成后异步发布；多队列并发区间按墙钟校准后的并集计算总耗时，避免直接累加并行队列时长。

## 按需命令录制观测

`rdg::TraceSnapshot` 是独立于 GPU 资源生命周期的纯值 CPU 快照，用于在单次 `Executor::execute` 中按可选 `trace_scope`（如编辑器后处理范围 `"scene_lit_post"`）捕获实际执行的通道序列、队列类别与读写资源绑定摘要。未请求 `trace` 时零分配、零文件 I/O；录制中途因错误短路时标记 `interrupted = true` 并保留已录制前缀。它记录 CPU 侧命令组织事实，不等同于 GPU 已完成执行或显示器已完成扫描输出。

## 窗口与读回

- `WindowSurface::open(Device&, NativeWindow, SurfaceOptions)` 为指定 `HWND` 创建 3 缓冲 `DXGI_SWAP_EFFECT_FLIP_DISCARD` `IDXGISwapChain4`（`DXGI_FORMAT_R8G8B8A8_UNORM` 底层存储，`SurfaceOptions::srgb = true` 时创建 `RGBA8_SRGB` RTV），多个窗口可共享同一 `Device`。
- `target()` 在 owner 线程只读返回当前交换链尺寸与窗口可绘制状态（可见、非最小化且客户区宽高非零）。
- `sync_target()` 在绘制前同步窗口客户区尺寸：若宽高非零且与当前后台缓冲不同，等待 Graphics 队列当前围栏、回收旧缓冲并调用 `ResizeBuffers` 重建视图，不触发多余 `Present`；隐藏但客户区尺寸有效的弹出窗口同样可预先同步画布。
- `BackendAccess::present` 提交 `IDXGISwapChain4::Present`（支持无垂直同步下的 `DXGI_PRESENT_ALLOW_TEARING`），并在需要时处理呈现后尺寸调整与延迟资源回收。`FrameInfo::occluded` 精确反映 `hr == DXGI_STATUS_OCCLUDED`（例如物理显示器休眠，或进程运行于非当前输入桌面的沙盒桌面环境如 `exebox`）。
- 显式纹理读回通过 RDG Copy 通道调用 `CmdReadbackTextureToBuffer` 写入 `HOST_READBACK` 缓冲，行距满足 256 字节对齐，切片偏移满足 512 字节对齐。

### 呈现准入

- `SurfaceOptions::frame_latency_waitable` 启用时，交换链附加 `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` 并设置 `SetMaximumFrameLatency(1)`。
- `WindowSurface::wait_frame(timeout_ms)` 在编码前等待呈现容量：零超时直接非阻塞查询等待句柄；正超时使用 `MsgWaitForMultipleObjectsEx` 允许线程输入消息唤醒而不越权分发消息。取得容量许可后锁存至下一次实际 `present` 消费，中途 `not_ready` 返回不会丢失已获许可。未启用该选项的表面始终返回 `true`。

## 失败与有界关停

- 所有 GPU 等待均受 `Device::wait_timeout_ms()` 有限正预算约束，超时返回 `ErrorCode::timeout`，检测到 `DXGI_ERROR_DEVICE_REMOVED / RESET` 返回 `ErrorCode::device_lost`。
- 关闭顺序遵循“先释放上层 `MaterialBatch` / `Pipeline` 与 `rdg::Executor`、关闭 `WindowSurface`，最后 `Device::close` 取消后台作业、排空三队列并回收全部 Bindless 描述符与延迟对象”；重复 `close` 幂等成功。
