# RHI 验收与证据

最近一次完整 RHI 验收于 2026-10-16 在 Dev、Release 两种配置通过，覆盖基线 `5e5d73c8ce1fbf4c07a100dc4cd61e42ed939f54` 加本批矩形清除与固定 depth bias。本文记录实际运行证据，不替代 Renderer/Application 的产品验收。

## 命令与环境

在仓库根目录的 PowerShell 中执行：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target rhi_acceptance
cmake --preset release
cmake --build --preset release --target rhi_acceptance
```

`rhi_acceptance` 运行正式消费者和三个独立后端回归。主进程上限 180 秒，各回归上限 60 秒；这与 Context 的单次 GPU 等待预算不同。检查在发布构建中同样有效。

| 项目 | 已验证条件 |
| --- | --- |
| 平台与硬件 | Windows x64，AMD Radeon RX 6800 XT，vendor 0x1002 |
| 驱动与帧槽 | 32.0.21045.5002，3 个帧槽 |
| 正式消费者与 Mesh PS 回归 | D3D12 debug layer + GPU-based validation |
| null-list / timeout 回归 | D3D12 debug layer |
| 依赖与工具链 | 版本、归档与 hash 以 [依赖锁](../../../cmake/dependencies-lock.json)、[工具链锁](../../../cmake/toolchain-lock.json)为准 |

## 正式验收矩阵

以下场景在最近一次 Dev、Release 验收均通过，各进程退出 0，D3D12 InfoQueue 的 warning/error 均为 0。

| 场景 | 实际检查 |
| --- | --- |
| Compute → Mesh → Vertex | 6 帧；24,576 个颜色像素、12,288 个深度像素、90 个 GPU 参数字全部匹配 |
| 参数与生命周期 | 同帧换值/换 buffer、跨帧常量、编码后提前释放、在途管线替换 |
| 管线准备 | 26 个错误候选被拒绝，旧管线保持；可选 GS/AS 缺失可用，存在但错误时拒绝 |
| 执行域 | 异线程拒绝；Pipeline 保持执行域；有 Pipeline 时 close busy；重复 close 成功；4 类非法创建输入在设备创建前拒绝 |
| 根参数预算 | VS/PS 各 32 DWORD 加 SRV；CS 64 DWORD 加 UAV；混合根常量/CBV、跨阶段不同偏移及更新结果正确 |
| 间接命令 | 单次/多次 compute、连续 64 次 finish；Vertex、Indexed Vertex、Mesh 各 2,048 像素正确 |
| Buffer range 复制 | 上传偏移 256、读回偏移 512；8,192 图像字节匹配，前后哨兵不变 |
| 绘制前窗口同步 | 6 次隐藏窗口 resize；每次首次 Present 即使用新尺寸；285,531 个 RGBA 像素全部匹配，详见下一节 |
| inline RayQuery | SM 6.5 compute；同次提交中 BLAS/TLAS build → trace → update → trace；命中、mask、instance、primitive、重心、最近距离和 first-hit 遮挡正确；提交前释放后的退休正确 |
| Scene Lit 资源 | 双 FP16 MRT，包含 HDR 值；D32 深度 SRV；mip 2 顺序 UAV/SRV；GPU 生成间接派发 |
| 原始 GPU 时间戳 | 8 个 query × 6 帧，共 48 个样本；完成序号、重复读取、旧平滑接口和 GPU 结果读回关联正确；finish 不推进录制序号 |
| 阴影 atlas 局部清除 | D32 与 RGBA32F、3 个 mip × 2 个 slice；32,256 个读回字匹配。半开矩形及边缘像素、tile 外保留、viewport/scissor 独立、空列表、非法批次整批拒绝、原整图清除入口、深度比较采样和编码后退休均通过 |
| 固定光栅 depth bias | Vertex/Mesh 各 7 种状态 × 2,048 像素；默认/显式零、正负 constant、slope-scaled、正负 clamp、非有限输入拒绝并保留旧值、prepare 后 DrawState 修改不影响已有 PSO 均通过 |
| null-list / Mesh PS | 无 command list 的 interop 清理；Mesh PS 回归检查 2,048 个像素 |
| 有限超时与严格一次执行 | 真实 queue 等待 CPU gate；失败后拒绝录制/推进/reset；放行后只排空，GPU 计数严格为 1 |

生产调用经 `Context` 与许可的 gfx 借用范围执行。验收中的原生 fence gate、interop 和外部 readback 只用于验证对应原生边界，不是业务接口。

本批独立工作树的构建与验收日志位于 `out/rhi-shadow-support-20261016/`；标准矩阵逐进程日志仍写入 `out/<配置>/rhi-acceptance.log` 和 `rhi-regression-*.log`。Dev 与 Release 的矩阵退出码均为 0，新增场景输出 `Shadow atlas RHI acceptance: PASS`。非法输入、错误 shader 与 timeout 拒绝场景会产生预期的 gfx 错误输出；通过条件是实际结果和 D3D12 InfoQueue 检查，不是 stderr 为空。

## 绘制前窗口同步

[资源与窗口消费者](../../../acceptance/rhi/resource_cases.cpp)创建并管理自己的不激活窗口，依次切换 641×73、257×181、703×97、320×96、641×73、257×181。

- 隐藏时调用 `sync_window_target()`，返回新尺寸和 `drawable=false`；显示后只调用一次 `frame`，`rendered` 与 `next` 均为新尺寸。
- 逐像素核对全屏三角形，确认首次呈现没有旧 viewport 留边；auto-resize 纹理同步变化，前次 GPU 使用沿既有退休机制结束。
- 同步不改变 recording frame sequence 或 last Present 结果。真实 fence 阻塞队列时，同尺寸、隐藏同尺寸、零客户区和最小化调用仍可返回，证明没有额外排空。
- 无窗口、异线程、关闭后的重复调用符合合同。尺寸变化遇到阻塞队列时触发有限 timeout；重复调用保留终态，放行 GPU 后可安全清理并重复 close。

调用时机与生命周期见[窗口合同](contract.md#窗口与读回)。此处验证 RHI 在首个 Present 前同步尺寸的能力；菜单打开耗时及 execute 回调保护以应用接线验收为准。

## 日志与诊断解释

最近一次配置与完整输出保存在 `out/rhi-window-target-sync/`：`configure-dev.log`、`configure-release.log`、`dev.log`、`release.log`。最终补丁从 pristine 源码重放后的四个 gfx 文件与两种已验收配置的源码 hash 一致。

正式运行器每次更新 `out/<preset>/rhi-acceptance.log` 和 `rhi-regression-{null-list,mesh-pixel-state,timeout}.log`。许可证与补丁部署于 `out/<preset>/licenses/rhi/`。`out/` 受 Git 忽略，本机证据不随克隆交付；这些路径不能作为其他工作树已通过的证明。

坏 shader、故意阻塞的 GPU 等待和失败后录制会打印预期错误，结果应结合场景检查、退出码与 InfoQueue 判断。null-list 回归刻意将一个 Program/Kernel 留给 Context 清理，会产生上游句柄未显式释放诊断；正式消费者没有该诊断。

## 重要变更的验证来源

| 日期 / 增量 | 当时验证与证据 |
| --- | --- |
| 2026-10-10 gfx 升级 | `17996ce` 上由 `e039b255` 升至 `a8d9bd64`；迁移创建标志与既有补丁，并修复嵌套 SDK 下载目录失效。Dev/Release 的 RHI、基础 Renderer、Scene Lit 均通过。配置、构建、运行日志与图像在 `out/gfx-upgrade-a8d9bd64/`；运行库版本未变 |
| 2026-10-08 Scene Lit 接入 | `402fab0` 基线上新增 RayQuery、AS 屏障、FP16/D32/mip、间接派发和原始 timestamp；Dev/Release 真实 GPU 验收通过。完整输出为 `out/scene-lit-{dev,release}.log` |
| 2026-10-05 根参数预算 | `495bf51` 上真实 Renderer 复现超过 64 DWORD；修正后 Dev/Release RHI 和 Dev Renderer 通过。前后日志为 `out/dev/root-budget-{renderer,rhi}-{before,after}.log` 及 `out/<preset>/root-budget-rhi-after.log`。后续双配置 Renderer 证据见上方 gfx 升级记录 |

### DDGIFilter 展开循环的驱动故障

2026-10-10 的独立单管线探针中，DXC 成功编译失败 shader；关闭 validation 后，`Context::prepare` 仍在 `D3D12Core::CreateComputePipelineState → amdxc64.dll` 内触发 `0xC0000005`，尚未录制 draw/dispatch 或创建 TLAS。仅将 5×5 双层循环的 `[unroll]` 改为 `[loop]`，同一探针即可 prepare/close 并退出 0。

该故障在升级后的 gfx 上仍能复现；驱动访问冲突没有返回可转换成 `Result` 的 HRESULT。证据在 `out/ddgi-unroll-probe-20261010/`，包括 `dxc.log`、`cdb-disabled.log`、`prepare-loop-disabled.log` 和 `manifest.json`。这是一份 shader 快照的单管线诊断，未裁剪到最小指令集，也不代表当前 Renderer 仍使用该 shader。

## 审计修正的生命周期观察

2026-09-27 的九项缺陷已修正：初始化数组、内部 Buffer 引用、DXGI 拥有引用、常量池复用、shader 输入边界、Mesh 选卡、range 偏移、参数数组释放和公开 C++23 要求。正式矩阵持续覆盖其中的可观察行为；以下是当时额外执行、在 Dev/Release 各通过的观察，不标作本次重测：

| 观察 | 当时结果 |
| --- | --- |
| 常量池 | 各三轮，每轮 64 次 dispatch/finish；完成后游标为 0，容量保持 65,536 字节 |
| 参数数组 | 反复切换 2 个/1 个 buffer 参数，Program/Context 退出后跟踪数组为 0 |
| DXGI 引用 | 三轮正常退出及初始化失败退出，释放观察者最后一份引用后均为 0 |
| 部分初始化 | 第二个 CreateCommandAllocator 使用真实无效 list type；创建失败并完成清理 |
| 公开编译要求 | 独立 CMake 消费者仅链接 `atelier_rhi`，继承 C++23 后成功编译公开头文件 |

当时的观察副本和日志位于 `out/review/rhi-fixes/`，汇总为 `out/rhi-verification.json`。它们没有增加生产测试钩子，也不等于全部原生对象或 OOM 故障普查。修正的维护理由见[模块决策](decisions.md#补丁与依赖维护)。更早的原生 SDK clear/copy 探针仅验证链接与硬件可行性，已由正式 gfx 验收接替。

## 未覆盖范围

- 真实 device removal、Signal/事件/Reset 驱动故障、native OOM、完整原生对象泄漏普查；DRED 保留上游实现，但未进行真实设备移除验收。
- 多卡选择、不支持 Mesh 的硬件、其他 GPU/驱动、WARP、HDR 显示，以及同一执行域承载多个窗口。
- 并行录制、多 queue、生产原生资源导入、设备恢复、通用 RDG。
- DispatchRays、透明/程序化光追几何、AS compaction；现有验证仅覆盖不透明三角形的 compute inline RayQuery。
- Runtime Texture 与业务语言/UI 的资源贯通链，以及稳定的产品性能基准。启用 validation 的 GPU 耗时只用于功能验证。

窗口和 Renderer 的其他产品能力见[Renderer 验收](../renderer/acceptance.md)，业务资源剩余缺口见[资源连接提案](../resources/design-proposal.md)。
