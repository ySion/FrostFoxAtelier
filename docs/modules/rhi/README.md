# RHI：资源与执行入口

当前实现位于 [modules/render/rhi](../../../modules/render/rhi/include/rhi/rhi.hpp)，采用固定 gfx 及可重放补丁。设备资源、反射绑定、descriptor、barrier、命令与退休由 gfx 负责；`atelier::rhi` 负责硬件/验证准入、执行域所有权、管线准备、窗口目标与生命周期。

Compute、Mesh、Vertex 三类管线共用这一执行域，支持离屏执行和现有 Renderer 的窗口呈现。现行行为以 [contract.md](contract.md) 为准，逐项运行证据以 [acceptance.md](acceptance.md) 为准。

Scene Lit 的 RHI 接入已验证 compute inline RayQuery、BLAS/TLAS build/update/退休、FP16 MRT、D32 深度 SRV、非零 mip UAV/SRV、GPU 生成间接派发及带完成帧序号的原始 GPU 时间戳。调用入口、Geometry 接管 range 句柄的所有权、整纹理状态限制见 [Scene Lit 契约](contract.md#scene-litinline-rayquery-与离屏资源)。

窗口尺寸可通过 `sync_window_target()` 在绘制前同步，隐藏但尺寸有效的窗口也可准备新目标；实际变化复用同一 swapchain 的排空和 resize，不产生额外 Present。调用时机、无操作条件及失败语义见[窗口合同](contract.md#窗口与读回)。

阴影 atlas 可通过 `gfxCommandClearImageRects` 局部清除 mip/slice 内的矩形，并通过 `gfxDrawStateSetDepthBias` 设置 Vertex/Mesh 管线的固定光栅 bias。空列表、整批拒绝、状态切换与管线快照语义见[阴影 atlas 契约](contract.md#阴影-atlas-原语)。

## 入口

- [公开头文件](../../../modules/render/rhi/include/rhi/rhi.hpp)：`Context::open / prepare / target / sync_window_target / frame / finish / close`，不可复制、可移动的 `Pipeline`。
- [生产实现](../../../modules/render/rhi/src/rhi.cpp)：选择硬件 adapter，不自动回退 WARP；统一 owner 线程和错误终态。
- [正式消费者](../../../acceptance/rhi/main.cpp)：同一 Interface 上一帧 Compute → Mesh → Vertex、间接参数、颜色/深度回读与提前释放，以及 Context 析构后 Pipeline 保持执行域；当前范围见[验收与证据](acceptance.md#当前保留代表与验证范围)。
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

`atelier_rhi` 是静态库；`atelier_rhi_acceptance` 是正式可执行目标。`rhi_acceptance` 编译并在 180 秒超时保护下运行主场景。只编译主消费者使用 `--target atelier_rhi_acceptance`。

依赖版本/下载与 hash 的唯一权威是 [dependencies-lock.json](../../../cmake/dependencies-lock.json)，工具链是 [toolchain-lock.json](../../../cmake/toolchain-lock.json)。[cmake/rhi.cmake](../../../cmake/rhi.cmake) 部署 DXC、Agility、PIX 运行库及 `licenses/rhi`；[atelier.patch](../../../third_party/gfx/atelier.patch) 可从固定上游源码重新应用。普通 CPU 分配沿项目 mimalloc 接入，GPU 内存由 gfx/D3D12MA 管理。

gfx 源码按锁定修订号放在 `out/<preset>/_deps/gfx-<revision>-src`，补丁的 pristine 副本及嵌套 SDK 留在对应源码目录。更新 gfx 时重新配置 preset 即可；不同修订分开存放，避免父源码重新解压后嵌套 SDK 被删除、旧 FetchContent 记录却仍宣称已完成下载。运行库部署从实际 gfx target 的源码目录取文件。

部署时保留同目录 DXC、Agility、PIX、mimalloc 运行库及许可证，不能只复制 exe。共享根构建与依赖锁的跨模块整合由主持协调；运行场景、预期诊断和限制集中在 [acceptance](acceptance.md)，不另建会话交接规则。

## 当前边界

验证过的业务执行模式是 Windows x64、硬件 D3D12、一个 owner 线程串行驱动 owned context。gfx 内部仍有异步 GPU 工作和多帧槽，不代表并行录制或多 queue。管线准备作业在 Context 内部工作线程上编译并创建 PSO，不录制、不提交。RHI 不链接业务 Runtime/JIT/语言/UI；顶层工程仍配置 JIT，不能据此声称配置工程无需 LLVM。

现有 Renderer 已接入 Present/resize、显式尺寸深度与最终帧结果，并通过 dev/release 的[renderer验收](../renderer/acceptance.md)及[编辑器窗口链](../editor/README.md)。主窗口与原生弹出窗口分别持有自己的 Renderer/Context；现行呈现和失败语义见[renderer合同](../renderer/contract.md)。
未交付或未验证的能力集中列于[验收边界](acceptance.md#未覆盖范围)，包括通用 RDG、业务 Texture 语言接线、并发录制、多 queue、设备恢复和不同 GPU 的支持矩阵。

引擎已有不可变 CPU Mesh/材质图片、UI 图片快照及 Renderer 的上传/退休路径；共享 Runtime Texture 业务对象到语言、布局和绘制的连接仍未实现。已有机制、剩余缺口和生命周期分工见[资源连接提案](../resources/asset-system-plan.md#texture-到-ui-的共同消费待接线)，不会把 Mesh Shader 管线等同于引擎 Mesh 资源。
