# RHI 当前契约

本文件描述 [rhi.hpp](../../../src/rhi/rhi.hpp) 与固定 gfx 补丁实际提供的 Interface。它替代旧的自建 Device/Recording/Ticket 资源表方案；具体版本由 [依赖锁](../../../cmake/dependencies-lock.json) 指定。

## 执行域与所有权

- `Context::open` 按高性能顺序筛选同时支持 FL12_1 与 Mesh Shader 的硬件 adapter，跳过不合格候选；无合格硬件返回 unsupported，没有 WARP 回退。尺寸、窗口、验证模式和有限正 wait budget 在进入 gfx 前检查。
- 一个 Context 对应一个 owned gfx context、一个 owner 线程、一条 DIRECT queue。`frame` 异步推进 gfx 帧槽；`finish` 提交当前工作并等待完成。
- `Context`、`Pipeline` 不可复制，可移动。Pipeline 持有 Program/Kernel 和执行域；Context 外壳消失不提前销毁仍被 Pipeline 持有的执行域。最后一个所有者负责有限排空与释放。
- 所有改变状态的操作及最终销毁在创建线程进行。宿主方法拒绝异线程执行；Pipeline/执行域的违规最终销毁 fail-fast。借用的 gfx 函数仍由调用者遵守线程契约，不逐函数增加代理检查。
- 验证设置涉及进程级 D3D12 debug 配置，应在创建图形设备的初始化阶段确定；不支持并发切换验证配置。

## 管线准备与替换

`prepare(PipelineDesc)` 成功直接返回完整可用的 Pipeline，不存在需要调用方另行发布的半成品对象。

- 支持 Compute（CS）、Vertex（VS，可选 GS/PS）和 Mesh（MS，可选 AS/PS）。Mesh 使用设备能力检查，不支持时明确报错。
- source 输入仅在 prepare 期间借用，gfx 保存自己需要的源码。缺少主阶段、空的明确阶段或不属于所选种类的 source 阶段被拒绝。
- `shader_model` 是至多 11 字节的十进制 `major_minor` 后缀，格式与长度先于 gfx 编译检查；具体版本支持由 DXC 判断。文件模式下显式空 `file_path` 返回 invalid_argument，空指针仍采用 gfx 的默认当前目录。
- 设置 `file_name` 选择文件模式，`source` 不参与该模式。文件扩展名沿 gfx：`.comp`、`.vert`、`.geom`、`.frag`、`.task`、`.mesh`。可选文件不存在允许继续；存在的文件必须成功编译，主阶段缺失不能形成 Ready。
- 补丁把编译与反射失败贯通到 kernel 准备，拒绝缺失的主字节码；宿主最终同时检查有效 kernel、root signature 和 PSO。不能因其他阶段生成了 PSO 就忽略所请求阶段的错误。
- 先 prepare 新候选，成功后移动赋给旧 Pipeline；失败不改变旧 Pipeline。新 Program 不隐式继承旧参数，消费者重新绑定。旧资源沿 gfx 的延迟退休机制处理，不建立第二套版本/引用账。
- Program/Kernel 返回值都是借用，受 Pipeline 与执行域寿命约束，不是跨设备稳定身份。失败执行域不再返回可用的借用句柄，但 Pipeline 仍须释放后才能 close。

## 借用 gfx 的许可范围

`Context::borrow()` 只用于普通资源创建/销毁、参数设置和命令录制。消费者遵守 gfx 的句柄与绑定约束；释放后不得再借旧句柄发起操作。gfx 负责已经编码的 GPU 使用期，普通资源销毁不等待 GPU。

不能绕过 Context 调用 `gfxFrame`、`gfxFinish`、`gfxDestroyContext`、`gfxKernelReloadAll`、原生提交、替换 swapchain、外部资源导入或手工修改资源状态。专门的后端回归直接使用这些入口不代表它们成为普通消费者能力。借用不能越过 close 或执行域最终销毁。

普通 gfx 资源由消费者显式释放，Context 不复制资源管理器，也不追踪第二份资源 owner 表。`close` 的 busy 检查针对 Pipeline；原始资源退出由消费者履行 gfx 契约。

## 失败与有界关停

- gfx 保留 Recording/Closed/Submitted 状态与独立错误原因。Execute 已发生即记录 Submitted，后续 Signal 失败不撤销这个事实。
- 检查 Close、Signal、SetEventOnCompletion、等待结果和 Reset。事件醒来后重新比较 fence；先识别 device removal/最大 fence 值，不把它们当作完成。
- `wait_timeout_ms` 是每次原生 fence 等待的有限正预算，不接受 0 或 INFINITE；一次排空可能等待多个帧槽。超时返回 `ErrorCode::timeout`，不代表 GPU 完成。
- 执行失败后不可再普通录制、frame、finish 或 Reset。改变 wait budget 不清除失败状态。只允许所有者释放 CPU 持有，并通过 close 尝试清理。
- 已有 Pipeline 时 close 返回 busy。没有 Pipeline 时，正常 close 提交未提交的当前工作、排空并释放；重复 close 成功。
- `finish` 只有在排空与命令列表 Reset 都成功后才复位当前帧槽的常量池游标；连续同步执行可复用已完成的空间，不要求调用 `frame`。
- 失败后的 close 不再次 Execute，只在原 queue 追加新的完成标记并 drain；不能排空就返回错误并保留执行域。确认真实设备失效可以释放终态存储，但不代表提交成功或设备恢复。原生设备故障路径没有本轮动态验收。
- 最终析构无法证明安全释放时 fail-fast，不无限等待、不提前释放、不交给全局孤儿池。宿主应在正常退出前显式 close 并处理错误。

普通输入、准备与执行失败使用 `Result<T>`。本轮不宣称完整 native OOM/COM 故障注入覆盖，也没有把默认 C++ 分配异常变成无分配错误通道。

## 上层关系

gfx 就是 RHI 的资源与执行主体。未来 RDG 在这个执行契约上组织依赖，不复制 barrier、descriptor 或资源生命周期世界。业务语言包装复用项目权威类型体系；RHI 不持有业务对象、UI Node、GC 根或 JIT 回调。新增多 queue、并行录制、native import 或 presentation 支持，须先定义并验收对应契约。
