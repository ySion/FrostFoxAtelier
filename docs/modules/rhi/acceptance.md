# RHI 与 RDG 验收与证据

最近一次 RHI + RDG 重写验收于 2026-11-26 在 `render_engine_rewrite` 工作树的 `dev` 与 `release` 两种配置下通过（门禁记录 `out/acceptance-gate/20261126-112028-482-68324/`：`dev:rhi_acceptance` 3.25 s PASS，`release:rhi_acceptance` 3.19 s PASS，D3D12 Debug Layer + Enhanced Barriers 零警告零错误）。

## 当前保留代表与验证范围

`rhi_acceptance` 运行正式消费者 [main.cpp](../../../acceptance/rhi/main.cpp)：

- 打开真实硬件 D3D12 Ultimate `rhi::Device` 与 `rdg::Executor`，编译 SM 6.6 Bindless 的 Compute、Mesh Shader 与 Vertex Shader 三条管线。
- 在同一帧 `rdg::Graph` 内组织五个通道：`ComputeSetup`（`QueueKind::compute` 异步计算队列写入种子与 Mesh/Vertex 间接绘制参数，经 RDG 跨队列围栏与 Enhanced Barriers 交接）→ `MeshDraw`（`QueueKind::graphics` 间接 Mesh Shader 绘制并写深度）→ `CopyMeshColor`（中间颜色读回）→ `VertexDraw`（间接 Vertex Shader 叠加绘制）→ `FinalReadbacks`（最终颜色、D32 深度与缓冲字读回）。
- 逐像素核对两张 64×32 读回图（4,096 个颜色像素：红 40、绿 80；2,048 个深度像素：精确 `0.25f`）、15 个间接与计算参数字，以及 `rdg::TraceSnapshot` 的 5 个通道记录完整性。
- 销毁临时 `Device` 外壳后，确认存活的 `Pipeline` 继续保活底层执行域与原生 PSO，释放后 D3D12 `ID3D12InfoQueue` 零 warning、零 error。

## 命令与环境

在仓库根目录的 PowerShell 中执行：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target rhi_acceptance
cmake --preset release
cmake --build --preset release --target rhi_acceptance
```

快速门禁设置 `ATELIER_GPU_VALIDATION=debug`，正式消费者报告 `debug-layer=1 gpu-validation=0`；`-Full` 对此目标设置 `gbv`，未设置环境变量的直接运行同样启用 GPU-Based Validation（`debug-layer=1 gpu-validation=1`）。

| 项目 | 已验证条件 |
| --- | --- |
| 平台与硬件 | Windows x64，AMD Radeon RX 6800 XT（vendor `0x1002`，SM 6.8，3 帧槽） |
| 驱动版本 | `32.0.21045.29003` |
| 验证层 | D3D12 Debug Layer + Enhanced Barriers（`-Full` 下启用 GPU-Based Validation） |
| 依赖与工具链 | 版本、归档与 SHA-256 以 [dependencies-lock.json](../../../cmake/dependencies-lock.json)、[toolchain-lock.json](../../../cmake/toolchain-lock.json) 为准 |

## 未覆盖范围

- 真实硬件设备拔除/驱动崩溃后的运行时热恢复、完整原生 COM OOM 注入普查。
- Vulkan 后端编译与运行验证（当前仅锁定并启用 D3D12 Ultimate 后端）。
- 其他 GPU 厂商（NVIDIA / Intel）及不同驱动版本的实机交叉矩阵、HDR10 / scRGB 交换链。
- 窗口交换链呈现与完整光照/材质闭环由 [Renderer 验收](../renderer/acceptance.md) 独立覆盖。
