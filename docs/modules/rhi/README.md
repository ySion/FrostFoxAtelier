# RHI：gfx 主体与窄宿主 Interface

当前实现位于 [src/rhi](../../../src/rhi/rhi.hpp)，采用固定 gfx 及可重放补丁。资源、反射绑定、descriptor、barrier、命令与退休继续由 gfx 负责；`atelier::rhi` 只承担硬件/验证门禁、execution owner、管线准备与生命周期。

这是 Compute、Mesh、Vertex 三类管线的真实离屏实现，不是旧 SDK clear/copy 探针，也不是完整 renderer。现行行为以 [contract.md](contract.md) 为准，逐项运行证据以 [acceptance.md](acceptance.md) 为准。

2026-09-27 的[模块审计](audit-2026-09-27.md)所列九项缺陷已修正，新增间接大常量、range 复制及输入边界回归通过 dev/release；生命周期观察与覆盖边界见[验收证据](acceptance.md#审计修正的生命周期观察)。

## 入口

- [公开头文件](../../../src/rhi/rhi.hpp)：`Context::open / prepare / frame / finish / close`，move-only `Pipeline`。
- [生产实现](../../../src/rhi/rhi.cpp)：选择硬件 adapter，不自动回退 WARP；统一 owner 线程和错误终态。
- [正式消费者](../../../acceptance/rhi/main.cpp)：同一 Interface 上的六帧 Compute → Mesh → Vertex、间接参数、颜色/深度回读、提前释放与管线替换。
- [后端回归](../../../acceptance/rhi/upstream_regression.cpp)：无 list 的 interop 清理、Mesh PS 资源状态、真实 fence gate 超时与严格一次执行。
- [决策与理由](decisions.md)、[固定来源记录](../../research/rhi-reassessment/README.md)。

## 构建与运行

在仓库根目录的 PowerShell 中执行；各 preset 首次使用先配置：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target rhi_acceptance
cmake --preset release
cmake --build --preset release --target rhi_acceptance
```

`atelier_rhi` 是静态库；`atelier_rhi_acceptance` 和 `atelier_gfx_regression` 是正式可执行目标。`rhi_acceptance` 编译并运行主场景与三个有超时保护的回归进程。只编译主消费者使用 `--target atelier_rhi_acceptance`。

依赖版本/下载与 hash 的唯一权威是 [dependencies-lock.json](../../../cmake/dependencies-lock.json)，工具链是 [toolchain-lock.json](../../../cmake/toolchain-lock.json)。[cmake/rhi.cmake](../../../cmake/rhi.cmake) 部署 DXC、Agility、PIX 运行库及 `licenses/rhi`；[atelier.patch](../../../third_party/gfx/atelier.patch) 可从固定上游源码重新应用。普通 CPU 分配沿项目 mimalloc 接入，GPU 内存由 gfx/D3D12MA 管理。

部署时保留同目录 DXC、Agility、PIX、mimalloc 运行库及许可证，不能只复制 exe。共享根构建与依赖锁的跨模块整合由主持协调；运行场景、预期诊断和限制集中在 [acceptance](acceptance.md)，不另建会话交接规则。

## 当前边界

验证过的业务执行模式是 Windows x64、硬件 D3D12、一个 owner 线程串行驱动 owned context。gfx 内部仍有异步 GPU 工作和多帧槽，不代表并行录制或多 queue。RHI 不链接业务 Runtime/JIT/语言/UI；顶层工程仍配置 JIT，不能据此声称配置工程无需 LLVM。

没有实现或未验收：通用 RDG/renderer、业务语言绑定、并发 RHI、多 queue、透明设备恢复、WARP/其他 GPU 支持矩阵、Present/resize/HDR/多窗口、ray tracing 与性能基线。`window` 输入复用 gfx 的窗口路线，但已有 PASS 仅覆盖离屏。

引擎资源与语言/UI 的下一增量见 [资源连接提案](../resources/design-proposal.md)。该设计已完成接口分析，新增类型、原生访问接口和跨模块消费者尚未实现，不属于当前 RHI 能力。
