# RHI 当前契约

本文件描述 [rhi.hpp](../../../modules/render/rhi/include/rhi/rhi.hpp) 与固定 gfx 补丁实际提供的 Interface；具体版本由 [依赖锁](../../../cmake/dependencies-lock.json) 指定。选型与被否决方案的理由只维护于 [decisions](decisions.md)。

## 执行域与所有权

- `Context::open` 按高性能顺序筛选同时支持 FL12_1、Mesh Shader、DXR 1.1、SV_Barycentrics 与 Shader Model 6.5 的硬件 adapter，跳过不合格候选；无合格硬件，或创建后的 gfx 设备拿不到 DXR 接口，都返回 unsupported，没有 WARP 或无光追回退。尺寸、窗口、验证模式和有限正 wait budget 在进入 gfx 前检查。
- 一个 Context 对应一个 owned gfx context、一个 owner 线程、一条 DIRECT queue。`frame` 异步推进 gfx 帧槽；`finish` 提交当前工作并等待完成。
- `Context`、`Pipeline` 不可复制，可移动。Pipeline 持有 Program/Kernel 和执行域；Context 外壳消失不提前销毁仍被 Pipeline 持有的执行域。最后一个所有者负责有限排空与释放。
- 所有改变状态的操作及最终销毁在创建线程进行。宿主方法拒绝异线程执行；Pipeline/执行域的违规最终销毁 fail-fast。借用的 gfx 函数仍由调用者遵守线程契约，不逐函数增加代理检查。Context 内部的准备工作线程不触碰 gfx context，见“异步准备与 DXIL 磁盘缓存”。
- 验证设置涉及进程级 D3D12 debug 配置，应在创建图形设备的初始化阶段确定；不支持并发切换验证配置。
- `Validation::gpu` 映射到上游明确的 `EnableGPUBasedValidation` 标志，保留宿主在设备创建前的能力检查。进程环境变量 `ATELIER_GPU_VALIDATION` 在 `Context::open` 读取：`debug` 把请求的 `gpu` 降为 `debug_layer`，并向 stderr 写一行说明；未设置或 `gbv` 保持请求；其他值使 `open` 返回 `invalid_argument`。它只降级，从不提高宿主请求的级别；验收门禁用它按目标选择 GBV 范围，见 [README](../../../README.md#构建与端到端验收)。上游创建标志位值会随升级变化，调用方使用具名常量并随依赖一起重编译，不持久化裸位值。项目扩展的 `ForceRGBA8SwapChain` 使用不与上游标志冲突的位；DRED 保留上游实现，当前 Context 不默认开启额外设备移除诊断。

## 管线准备与替换

`prepare(PipelineDesc)` 成功直接返回完整可用的 Pipeline，不存在需要调用方另行发布的半成品对象。

- 支持 Compute（CS）、Vertex（VS，可选 GS/PS）和 Mesh（MS，可选 AS/PS）。Mesh 使用设备能力检查，不支持时明确报错。
- source 输入仅在 prepare 期间借用，gfx 保存自己需要的源码。缺少主阶段、空的明确阶段或不属于所选种类的 source 阶段被拒绝。
- `shader_model` 是至多 11 字节的十进制 `major_minor` 后缀，格式与长度先于 gfx 编译检查；具体版本支持由 DXC 判断。文件模式下显式空 `file_path` 返回 invalid_argument，空指针仍采用 gfx 的默认当前目录。
- 设置 `file_name` 选择文件模式，`source` 不参与该模式。文件扩展名沿 gfx：`.comp`、`.vert`、`.geom`、`.frag`、`.task`、`.mesh`。可选文件不存在允许继续；存在的文件必须成功编译，主阶段缺失不能形成 Ready。
- 补丁把编译与反射失败贯通到 kernel 准备，拒绝缺失的主字节码；宿主最终同时检查有效 kernel、root signature 和 PSO。不能因其他阶段生成了 PSO 就忽略所请求阶段的错误。
- 全局根签名在收集所有 shader stage 后统一计算 [64 DWORD 预算](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits)：根常量按实际 DWORD 数计费，当前反射路径的每个资源描述符表计 1。超限时优先把最大的可回退 `$Globals` 块转为现有 CBV 描述符表，保留阶段可见性、寄存器和各变量的反射偏移；参数设置仍走同一 `gfxProgramSetParameter`，常量数据使用既有常量池及帧槽寿命。预算回退不迁移含 `gfx_DispatchID` 的根常量块，以保留多次间接 dispatch 的直接更新语义；没有可回退块仍超限时准备失败。局部 ray tracing 根签名不使用此全局预算策略。
- 先 prepare 新候选，成功后移动赋给旧 Pipeline；失败不改变旧 Pipeline。新 Program 不隐式继承旧参数，消费者重新绑定。旧资源沿 gfx 的延迟退休机制处理，不建立第二套版本/引用账。
- Program/Kernel 返回值都是借用，受 Pipeline 与执行域寿命约束，不是跨设备稳定身份。失败执行域不再返回可用的借用句柄，但 Pipeline 仍须释放后才能 close。

## 异步准备与 DXIL 磁盘缓存

`submit(PipelineDesc) -> Result<PipelineJob>` 与 `prepare` 是同一个准备作业：同一套编译、反射、root signature/PSO 和 GBV 预热代码，`prepare` 只是在 owner 线程内联执行它。

- owner 线程在 `submit` 中按 `prepare` 的规则检查描述，复制源码、名称和 DrawState；文件模式在此读入阶段文件。返回后描述不再被借用。
- 工作线程在第一次 `submit` 时启动，至多 min(8, 硬件线程数) 个，各自持有 DXC 实例。它们只做编译（或缓存命中）、反射、root signature 与 PSO 创建，只接触自己的作业和 device，不触碰 gfx context。
- `Validation::gpu` 时，工作线程在一个从不执行的命令列表上为每条管线录制一次 `Set*RootSignature` + `SetPipelineState`。GBV 在任何命令列表第一次设置某个 PSO 时生成补丁版本，这一步把该成本移出 owner 线程的第一次录制；执行时仍由 GBV 校验，覆盖面不变。在创建 PSO 时就打补丁的做法会让当前 SDK Layers 崩溃，不采用。
- `take(job)` 只在 owner 线程调用，从不等待：作业未完成时返回空；完成后恰好一次返回 Pipeline 或失败。Program/Kernel 只在 owner 线程登记进 gfx。工作线程上的失败以值返回，不跨线程抛异常。已取走的作业或属于其他 Context 的作业返回 invalid_argument。
- 作业不是 Pipeline，不会让 `close` 返回 busy。丢弃 PipelineJob 即取消：排队的作业直接丢弃，正在运行的步骤做完后释放产物。
- 没有 Pipeline 时，`close` 先取消所有排队作业，等正在运行的步骤结束，释放未取走的产物，然后销毁执行域；之后 `take` 返回 closed。运行中的单个步骤不能中断：普通情况下是一次编译或 PSO 创建；GBV 下的一次预热在 8 线程争用时最长实测约 128 s。

`ContextOptions::shader_cache` 是可由多个进程共享的 DXIL 缓存目录，空路径表示禁用。它是持久格式：

- 键为 SHA-256，覆盖缓存格式版本、dxcompiler 版本与提交号、全部 DXC 参数（主文件名、profile、入口、16 位类型等）和完整源码。取不到编译器版本时不使用缓存。
- 条目 `<键>.bin` 由 56 字节头（魔数 `ASHC`、格式版本、两段长度、SHA-256 校验和）、DXIL 和反射数据组成。命中时跳过预处理与编译。
- 读到不可读、截断、格式不符或校验失败的条目，一律按未命中处理：重新编译并覆盖，不报错。写入先写临时文件，再原子替换。
- 编译期间读取过 `#include` 文件的阶段不写入缓存，因为被包含文件的内容不在键里。
- 每个进程第一次写入时检查目录：超过 512 MiB 就按修改时间删除最旧的文件，直到不超过 256 MiB。
- 缓存格式变化时提升格式版本，旧条目不再命中，随后被淘汰，不做迁移。缓存的任何失败都只让该阶段重新编译。

## 借用 gfx 的许可范围

`Context::borrow()` 只用于普通资源创建/销毁、参数设置和命令录制。消费者遵守 gfx 的句柄与绑定约束；释放后不得再借旧句柄发起操作。gfx 负责已经编码的 GPU 使用期，普通资源销毁不等待 GPU。

不能绕过 Context 调用 `gfxFrame`、`gfxFinish`、`gfxSyncWindowTarget`、`gfxDestroyContext`、`gfxKernelReloadAll`、原生提交、替换 swapchain、外部资源导入或手工修改资源状态。专门的后端回归直接使用这些入口不代表它们成为普通消费者能力。借用不能越过 close 或执行域最终销毁。

普通 gfx 资源由消费者显式释放，Context 不复制资源管理器，也不追踪第二份资源 owner 表。`close` 的 busy 检查针对 Pipeline；原始资源退出由消费者履行 gfx 契约。

默认堆 buffer 的自动只读用途（顶点、索引、shader SRV、indirect 与 copy source）累积为兼容的状态集合，range 与原始 buffer 共享这一状态。已经包含的读用途不再发屏障；显式转换后的状态增加读用途时仍发必要的转换屏障。写入、UAV 读写与 AS 同步不因这项合并被省略；离开 IA 读状态写入时按实际安装的 IA 资源判断解绑，下次 draw 重新安装。仅 bind 而未 draw 的逻辑句柄不替代实际安装状态；经 range 别名写入也遵守同一规则。消费者不需要也不得手工维护这套状态。

`ByteAddressBuffer` 反射创建 RAW SRV，不使用 buffer 的逻辑 stride 定义地址或尺寸；`StructuredBuffer` 保留元素 stride。RAW 视图以四字节 word 计数，范围至少包含一个完整 word，D3D12 要求视图起点 16 字节对齐（`D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT`）；shader 的 Load 地址仍相对该视图。公开 `gfxCreateBufferRange` 不代替这些着色器视图约束，消费者须给出合法范围。

## 按需命令录制观测

`CommandTrace` 是一次性的自有 CPU 快照，不拥有 GPU 资源、Context 或 Runtime 根。请求在录制前分配固定存储：32 条命令、1024 条绑定、每个名称 128 字节，总计小于 1 MiB。`CommandTraceScope` 仅在 Context owner 线程、单次 render 尝试内借用 Context 和快照，二者在 scope 结束前不得移动或销毁；不允许嵌套观察器。RAII 在正常结束和异常退出时解除借用，完整性单独记录；同一快照不能重入或拼接多个尝试。完成后可移动到写入线程，期间只访问自有记录，不再访问 gfx。

gfx 在实际安装/复用根参数与 descriptor 的路径报告绑定，在真正发出 Dispatch 或单条 DispatchIndirect 后提交命令记录；安装失败但没有发命令的暂存不发布成节点。当前覆盖后处理链的 2D SRV/UAV（包括 descriptor 缓存命中）、sampler、根常量与 CBV；不支持或无法从现有安装路径确认的绑定标为 unknown，使快照 incomplete，不按 setter 猜测。明确安装的 null/dummy 与 unknown 区分。常量只保留绑定与范围，不读回内容；间接参数只记录实际缓冲区/range，不读取 GPU group 数。绑定种类不声明完整资源访问集合。

回调不分配、不抛异常、不加锁、不做 I/O 或 GPU 等待；容量或名称超限标 truncated 并停止收集。快照内的物理键只供导出时去重，不是作者资源身份，原生地址不写入产物。关闭观测仍有分支检查，不承诺严格零成本；关闭时不枚举诊断参数、不分配/复制快照、不写文件或增加 GPU 工作，尚无成本测量结论。状态转换、屏障、命令提交和资源退休完全沿 gfx 原路径，不建立影子状态或 RDG 调度器。

当前消费者是 [Editor Control 的后处理录制视图](../editor/control.md#后处理录制视图)。它分别展示录制事实、观测完整性与实际 render 结果，不把录制当成 GPU 完成或显示。

## Scene Lit：inline RayQuery 与离屏资源

- 每个打开的 Context 都支持 inline RayQuery（见上方的硬件门槛），消费者不再检查能力。不新增 DispatchRays 管线种类，使用 `PipelineKind::compute`、`shader_model="6_5"` 和普通 `prepare`；HLSL 通过 `RaytracingAccelerationStructure` / `RayQuery` 访问。
- AS 沿同一个 `borrow()`：`gfxCreateGeometryTriangles` → `gfxCreateBottomLevelAccelerationStructure` / AddGeometry → `gfxBuildBottomLevelAccelerationStructures` → instance SetBottomLevelAccelerationStructure / SetTransform / SetInstanceID / SetInstanceMask → TLAS AddInstance / Build → `gfxProgramSetParameter(gfx, program, name, tlas)` → compute dispatch。变换是 row-major 3×4；InstanceID 由 Renderer 映射到自身场景数据，RHI 不保存 World 对象。
- **Geometry 接收 buffer 句柄的所有权**，update 会销毁旧输入句柄，destroy 会销毁当前输入句柄。共享 raster 顶点/索引存储时，把 `gfxCreateBufferRange` 创建的独立 range 句柄交给 Geometry；原始 buffer 由原 owner 保持，不得将同一句柄交给多个 Geometry 或再重复销毁。BLAS 的 geometry 列表、TLAS 的 instance 列表及 instance 的 BLAS 引用均不转移对应对象所有权。
- 可更新 BLAS 首次 build 使用 `kGfxBuildBottomLevelASFlag_Updateable`。update 只改变允许的顶点位置/输入地址，不改变 geometry 数量、顶点数量、索引内容与格式等拓扑条件；条件变化调用 build。TLAS update 保持 instance 数量，增删 instance 后调用 build。任何被 TLAS 引用的 BLAS 更新后，先更新或重建对应 TLAS，再执行查询。
- gfx 在 AS build/update 前后处理 AS 自身的 UAV 屏障及 scratch 复用；AS 始终处于专用 AS 状态，消费者不手工改状态。AS 对象、输入 range、结果 buffer 和 Pipeline 可在最后一次 GPU 使用已编码后释放，物理退休沿 gfx 帧槽；后续录制仍需访问的 Geometry/BLAS/instance/TLAS 必须保持有效。退出按 TLAS → instance → BLAS → Geometry → 原始 buffer 清理，再释放 Pipeline 并 `close`。
- 不透明二值遮挡可以使用 `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`；需要最近遮挡距离、命中 primitive、重心坐标或 ray-hit 材质查找时遍历至完成并读取 committed hit，不能把 first-hit 距离当作最近距离。当前接入只验收三角形、不透明 compute inline 查询。
- 离屏 FP16 使用 `DXGI_FORMAT_R16G16B16A16_FLOAT`；多颜色附件通过同一个 draw state 和 `gfxCommandBindColorTarget` 绑定。`DXGI_FORMAT_D32_FLOAT` 深度可在后续 compute 阶段作为 `Texture2D<float>` 采样。离屏颜色值可大于 1，不改变窗口 SDR/HDR 策略。
- `gfxProgramSetParameter(..., texture, mip)` 选择 mip，UAV 只访问该 mip，SRV 从该 mip 开始暴露剩余 mip。状态跟踪粒度是整张 texture：允许顺序 UAV 写 → SRV 读，禁止同一次 draw/dispatch 将同一 texture 同时绑定为 SRV 和 UAV，即使选择不同 mip。逐级缩减可使用不同 texture 交替读写。
- `gfxCommandDispatchIndirect` 消费 GPU 生成的三个 uint（X/Y/Z group count），仍由 gfx 处理 UAV → indirect 状态。
- `gfxCommandBeginTimestampQuery` / End 沿现有帧槽录制；`gfxTimestampQueryGetSample(gfx, query)` 返回最新已完成的 `GfxTimestampQuerySample { double duration_ms; uint64_t frame_sequence; }`。`frame_sequence == 0` 表示尚未发布样本，duration 0 本身可以是有效测量。原始 duration 先对 uint64 GPU ticks 作差再换算为毫秒，不做平滑。
- 样本仅在 owned Context 的 `frame` 等待并回收对应帧槽之后发布；getter 不录制、不提交、不等待、不消费样本。开始/结束新计时仍保留上次完成值；重复读取同一 sequence 不得重复计入统计。`finish` 不推进或发布这些查询，关闭 Context 前需要最后几帧统计时可显式推进剩余帧槽。
- sequence 从 1 开始，按该 Context 的 `frame` 提交单调递增，未计时的提交也计数；同一次提交中全部查询共享同一 sequence。它不是应用的 accepted frame、Present 完成或跨 Context 全局序号。同一 query 每次提交只能记录一次。Renderer 在每次 `frame` 返回后读取各 pass，仅将相同非零 sequence 的原始值归为一帧；跳过某个 pass 时它保留旧 sequence，应标为缺失。getter 只保留最新完成样本，不保留未读取的历史；要收齐样本应每次 `frame` 后读取。
- `gfxGetRecordingFrameSequence(gfx)` 返回当前正在录制、下一次 `Context::frame` 将使用的同一个 sequence；无效、失败、非录制或 interop context 返回 0。重复查询、普通录制及 `finish` 不推进序号，`frame` 才推进；Renderer 不自行镜像调用次数。录制 timestamp 与 raycounter 拷贝时取得该标识并存入读回 ring，收到相同 sequence 的完成样本后读取对应槽。拷贝须在该次 `frame` 前编码，且槽在消费前不能被新 GPU 写入覆盖；同一 DIRECT queue 的已完成帧 fence 覆盖二者。如果多个 render 只调用 `finish`、未调用 `frame`，它们属于同一 RHI 帧区间，消费者另存自身 render 标识来区分，不能把 sequence 当每次 render 的编号。
- 旧 `gfxTimestampQueryGetDuration` 继续返回原有平滑值，首次结果也与初值 0 混合；p95/p99 使用新 sample 接口的去重原始值。新接口复用原有 query heap、readback buffer 和帧槽回收，不增加每帧等待或原生提交。验证模式耗时仅用于确认计时路径，不作为产品性能指标。

`Context::frame(vsync, &details)` 的 `FrameInfo::cpu` 同时给出本次提交的 CPU 侧墙钟分段；不请求 details 时不读取计时时钟。结果仅在整次调用成功后发布，不改变队列提交、Present 或等待策略。`frame_sequence` 对应本次提交，GPU timestamp getter 则可能仍对应更早的已完成提交，消费者须分别记录身份。

| CPU 字段（微秒） | 区间 |
| --- | --- |
| `prepare_us` | timestamp resolve 录制、窗口检查、自动尺寸纹理准备及提交前屏障 |
| `submit_us` | 命令列表 Close、ExecuteCommandLists、当前帧槽 Signal |
| `present_us` | Present、结果处理及下一个 backbuffer 索引查询；离屏为零 |
| `wait_us` | 下一个帧槽的 fence 完成检查及必要等待 |
| `resize_us` | frame 内发生的 swapchain 重建，包含其排空；无尺寸变化为零 |
| `recycle_us` | 帧槽 reset、完成 timestamp 发布、状态衰减、常量池与录制状态复位 |
| `reclaim_us` | 延迟资源与 descriptor 回收 |
| `total_us` | 上述区间之和，限定于 gfx frame 内部 |

这些区间包含线程被抢占或阻塞的时间，不等于 CPU 活跃执行时间或 GPU 时长；不可与 GPU pass 时长简单相加。Renderer 的命令录制、截图 `finish`、绘制前 `sync_window_target` 以及 RHI 外层参数检查不在该 total 内，应由调用层另行测量。

DXR 的查询与更新限制依据 [SM 6.5](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_5.html) 和 [AS 更新及同步规范](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#synchronizing-acceleration-structure-memory-writesreads)。实际硬件证据见 [验收](acceptance.md)。

## 窗口与读回

- `Context::target()` 在 owner 线程返回实际 backbuffer 尺寸和窗口可绘制状态；隐藏、最小化或零 client 尺寸不可绘制。它不执行 resize。
- `Context::sync_window_target() -> Result<TargetInfo>` 由 owner 在线程所属的窗口尺寸更新后、开始本次绘制录制前调用；Renderer 须在进入 execute 回调前调用，不能在回调中调整正在使用的目标。成功返回同步后的实际目标，调用方再按这个尺寸准备画布、viewport 与读回存储。它不改变 `target()` 的只读语义。
- 隐藏窗口只要 client 尺寸有效也会同步，以支持弹出菜单显示前准备；返回的 `drawable` 仍为 false。同尺寸、最小化、零 client 尺寸、窗口已销毁或无窗口 Context 不提交、不等待、不重建；尺寸沿 gfx 既有最小 8×8 约定。关闭、异线程与失败 Context 沿既有错误入口拒绝，重复调用不清除错误。
- 实际尺寸变化复用 gfx 的自动尺寸纹理更新、当前命令提交、有限排空、资源退休与 `resizeBackBuffers`，恢复同一帧槽的录制及 backbuffer 状态；不调用 Present，不推进 recording frame sequence，也不发布 timestamp 样本。已排队的上传不会被丢弃，auto-resize 纹理句柄保持有效但内容不保留；尺寸不变不增加上述 GPU 工作。尺寸变化仍可能等待在途 GPU，调用方不能把它视为无等待操作。
- 同步失败进入既有执行终态，只允许清理/close，不返回半完成的目标。自动尺寸纹理分配失败显式返回错误，不再记录错误后继续报告 resize 成功。`frame` 末尾原有 resize 继续处理录制期间的窗口变化。
- `frame(vsync, &details)` 沿唯一原入口推进；可选 details 仅在整个操作成功后更新，包含提交前目标、完成后目标和 DXGI 遮挡状态。Present 成功之后的 resize/等待/Reset 失败仍是终态，不发布部分成功。旧的 `frame(vsync)` 调用不变。
- `ContextOptions::rgba8_window=true` 显式请求 SDR RGBA8，backbuffer 使用 sRGB RTV。默认 false 保留 gfx 按显示器选择格式的行为；10 位显示器的默认目标不保证 RGBA8。renderer 对需要的实际格式另做校验。
- backbuffer 可与独立深度目标一起绑定：Pipeline 显式声明颜色格式等于 backbuffer 格式，并声明 depth 格式；不提供颜色声明的 depth-only 路径保留原义。
- `gfxCommandCopyBackBufferToBuffer` 与 `gfxCommandCopyTextureToBufferPitched` 用于显式诊断读回；单采样、单平面 2D subresource 0，目标为 CPU-read buffer，行距须为 256 的倍数且不小于原生行字节数，buffer offset 为 512 的倍数，存储覆盖所有行。普通紧密 copy 入口保持不变。

### 呈现准入

- `ContextOptions::frame_latency_waitable` 默认关闭，只允许窗口 Context。启用时 gfx 用 `FRAME_LATENCY_WAITABLE_OBJECT` 创建交换链，RHI 检查 `SetMaximumFrameLatency(1)` 成功并取得非空等待句柄，否则创建失败；没有静默退回。它只限制排队呈现，不改变 VSync、三个资源帧槽或 fence 回收寿命。
- owner 在每帧（包括首帧）编码前调用 `wait_frame(timeout_ms)`。有限等待可由 DXGI 呈现容量或线程消息唤醒，不分发消息、不执行回调。输入或超时返回 false，不授予呈现许可；容量就绪返回 true，并由 Context 保留到真正调用 `frame`。二者同时就绪时仍保留容量许可，重复检查、`finish` 和上层 `not_ready` 不消费它。零超时用于单次检查，不用于忙轮询；未启用的 Context 总是返回 true。
- RHI 独占句柄和许可。resize 读取交换链实际 flags 传给 `ResizeBuffers`，保留同一交换链的句柄和已获许可；不支持通过借用 gfx 替换交换链。close 失败保留句柄与执行域，成功关闭或安全最终析构才 `CloseHandle`；关闭后重开取得新句柄，首帧重新准入。原生等待失败进入既有执行终态，只允许清理。

## 阴影 atlas 原语

资源与命令继续经 `Context::borrow()` 使用 gfx，管线经 `Context::prepare()` 创建。

- `gfxCommandClearImageRects(gfx, texture, rects, rect_count, mip_level=0, slice=0)` 清除指定 image 内的矩形列表。`D3D12_RECT` 仅作为坐标值传入；使用纹理创建时的 clear value，不依赖当前 viewport/scissor。颜色和深度 image 共用原有 clear 路径，深度带 stencil 时保留既有的同时清除语义。
- 每个矩形是该 mip/slice 像素空间中的非空半开范围 `[left,right) × [top,bottom)`。坐标必须非负且完全位于 mip 尺寸内，允许矩形重叠。Context、texture、mip/slice 与整个列表在编码前检查；任一矩形非法，整批不创建 view、不转换状态、不编码 clear。
- 在 Context/image 有效的前提下，`rect_count == 0` 是无操作，`rects` 可为空；非空列表不能传空指针。列表仅在调用期间借用。矩形外内容以及其他 mip/slice 保留；`gfxCommandClearImage` 和 `gfxCommandClearTexture` 仍分别清整个 image 和整个 texture。
- `gfxDrawStateSetDepthBias(draw_state, constant, slope_scaled=0, clamp=0)` 设置固定光栅 depth bias。constant 使用 D3D12 的整数 bias 单位；slope/clamp 必须有限，可为负，非法输入不改变旧状态。默认三项均为零。它不等于接收端世界空间偏移。
- bias 随 DrawState 快照进入 Vertex 与 Mesh 的 PSO，后续修改 DrawState 不改变已经准备好的管线。引擎不设 PSO 缓存；光栅状态进入实际 PSO 描述。DXIL 缓存只保存编译产物，见上文“异步准备与 DXIL 磁盘缓存”。
- atlas 仍按整纹理跟踪资源状态：先完成所有脏 tile 的 DSV 写入，再解除深度附件绑定并作为 SRV 采样。不同 tile/mip/slice 不能成为同一次绘制中同时写入与采样同一纹理的依据。页表版本、tile 淘汰及过滤保护边属于 Renderer；物理资源释放沿既有 gfx 退休机制。

最小调用示例（bias 数值需由具体阴影投影确定）：

```cpp
const D3D12_RECT dirty_tiles[] = {{0, 0, 256, 256}, {256, 0, 512, 256}};
gfxCommandClearImageRects(gfx, atlas, dirty_tiles, 2);
gfxDrawStateSetDepthBias(shadow_desc.draw_state, 256, 1.0f, 0.01f);
// shadow_desc is prepared through Context::prepare before encoding draws.
```

## 失败与有界关停

- gfx 保留 Recording/Closed/Submitted 状态与独立错误原因。Execute 已发生即记录 Submitted，后续 Signal 失败不撤销这个事实。
- 检查 Close、Signal、SetEventOnCompletion、等待结果和 Reset。事件醒来后重新比较 fence；先识别 device removal/最大 fence 值，不把它们当作完成。
- `wait_timeout_ms` 是每次原生 fence 等待的有限正预算，不接受 0 或 INFINITE；一次排空可能等待多个帧槽。超时返回 `ErrorCode::timeout`，不代表 GPU 完成。
- 执行失败后不可再普通录制、frame、finish 或 Reset。改变 wait budget 不清除失败状态。只允许所有者释放 CPU 持有，并通过 close 尝试清理。
- 已有 Pipeline 时 close 返回 busy。没有 Pipeline 时，正常 close 先取消并等待准备作业，再提交未提交的当前工作、排空并释放；重复 close 成功。
- `finish` 只有在排空与命令列表 Reset 都成功后才复位当前帧槽的常量池游标；连续同步执行可复用已完成的空间，不要求调用 `frame`。
- 失败后的 close 不再次 Execute，只在原 queue 追加新的完成标记并 drain；不能排空就返回错误并保留执行域。确认真实设备失效可以释放终态存储，但不代表提交成功或设备恢复。原生设备故障路径没有本轮动态验收。
- 最终析构无法证明安全释放时 fail-fast，不无限等待、不提前释放、不交给全局孤儿池。宿主应在正常退出前显式 close 并处理错误。

普通输入、准备与执行失败使用 `Result<T>`。本轮不宣称完整 native OOM/COM 故障注入覆盖，也没有把默认 C++ 分配异常变成无分配错误通道。

## 上层关系

gfx 就是 RHI 的资源与执行主体。未来 RDG 在这个执行契约上组织依赖，不复制 barrier、descriptor 或资源生命周期世界。业务语言包装复用项目权威类型体系；RHI 不持有业务对象、UI Node、GC 根或 JIT 回调。[编辑器 renderer](../renderer/contract.md) 按窗口持有执行域，原生弹出窗口使用独立 Renderer/Context。多 queue、并行录制、native import、HDR 或同一执行域承载多个窗口仍须定义并验收对应契约。
