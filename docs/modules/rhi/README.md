# RHI 与 RDG：显式硬件层与渲染依赖图

当前实现由 [modules/render/rhi](../../../modules/render/rhi/include/rhi/rhi.hpp)（`atelier_rhi`）与 [modules/render/rdg](../../../modules/render/rdg/include/rdg/rdg.hpp)（`atelier_rdg`）共同组成，底层采用固定修订号的 NVIDIA NRI 及可重放补丁 [third_party/nri/atelier.patch](../../../third_party/nri/atelier.patch)。

- `atelier::rhi` 负责进程内唯一 D3D12 Ultimate `Device`、全局 SM 6.6 Bindless 描述符堆（`ResourceDescriptorHeap` + `SamplerDescriptorHeap` + 16-DWORD 根常量 `register(b0, space0)`）、Graphics / Async Compute / Copy 三队列时间线、异步 DXC 管线编译与磁盘缓存、延迟安全回收器，以及轻量多窗口 `IDXGISwapChain4` `WindowSurface`。
- `atelier::rdg` 在 `rhi::Device` 上提供线性 Arena 编译期 Render Dependency Graph：拓扑死通道裁剪、D3D12 Enhanced Barriers 与跨队列“能力最强队列（Most-Capable Queue）”布局转换律、64 KiB 对齐 Placed Resource + Bindless 瞬态别名池、区间并集校准 GPU 计时器，以及无堆分配 `rdg::TraceSnapshot` 命令录制观测。

现行行为以 [contract.md](contract.md) 为准，选型依据见 [decisions.md](decisions.md)，运行证据以 [acceptance.md](acceptance.md) 为准。

## 入口

- [rhi.hpp](../../../modules/render/rhi/include/rhi/rhi.hpp)：`Device::open / info / wait_idle / close`、可多实例并存的 `WindowSurface::open / target / sync_target / wait_frame / close`，以及不可复制、可移动且保活所属执行域的 `Pipeline` / `PipelineJob`。
- [backend.hpp](../../../modules/render/rhi/internal/rhi/backend.hpp)：供渲染子系统内部消费的 `BackendAccess`（NRI 接口表、Bindless 分配、三队列时间线、延迟回收器与交换链获取/呈现）。
- [rdg.hpp](../../../modules/render/rdg/include/rdg/rdg.hpp) 与 [graph.hpp](../../../modules/render/rdg/internal/rdg/graph.hpp)：公开 `TraceSnapshot` 快照结构，以及内部 `LinearArena`、`Graph`、`PassBuilder`、`PassContext` 与 `Executor`。
- [正式消费者](../../../acceptance/rhi/main.cpp)：在 D3D12 debug / GBV 下经 `rdg::Executor` 执行 Compute（Async Compute 队列）→ Mesh → Copy → Vertex → Readback 五通道依赖图，校验间接绘制、颜色/深度/参数字回读、`TraceSnapshot` 完整性，以及 `Device` 外壳释放后 `Pipeline` 对执行域的保活。

## 构建与运行

在仓库根目录的 PowerShell 中执行；各 preset 首次使用先配置：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target rhi_acceptance
cmake --preset release
cmake --build --preset release --target rhi_acceptance
```

`atelier_rhi` 与 `atelier_rdg` 均为静态库；`atelier_rhi_acceptance` 是正式可执行目标。`rhi_acceptance` 编译并在 180 秒超时保护下运行主场景。只编译主消费者使用 `--target atelier_rhi_acceptance`。

依赖版本、归档地址与 SHA-256 的唯一权威是 [dependencies-lock.json](../../../cmake/dependencies-lock.json)，工具链版本唯一权威是 [toolchain-lock.json](../../../cmake/toolchain-lock.json)。[cmake/rhi.cmake](../../../cmake/rhi.cmake) 部署锁定的 DXC、Agility SDK、PIX 运行库及 `licenses/rhi`，并在配置阶段从 pristine 副本重放 [atelier.patch](../../../third_party/nri/atelier.patch)。普通 CPU 分配由项目共享 mimalloc 接管，NRI 内部 CPU 分配回调统一桥接至同一分配器。

## 当前边界

验证过的执行模式是 Windows x64、硬件 D3D12 Ultimate（SM 6.6+）、owner 线程驱动命令图提交与后台工作线程池异步编译 PSO。主窗口与独立原生弹出菜单共用同一 `std::shared_ptr<rhi::Device>`，各自仅创建轻量 `WindowSurface`，不再为弹出窗口重复初始化图形设备或着色器缓存。Vulkan 后端已由 NRI 抽象边界预留，当前构建仅开启并验收 D3D12。RHI 与 RDG 不链接业务 Runtime、JIT、语言或 UI 模块。
